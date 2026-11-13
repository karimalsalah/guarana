// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <algorithm>
#include <vector>
#include <string_view>
#include <string>
#include <map>
#include <mutex>
#include <thread>
#include <tuple>
#include <utility>

#include <fmt/format.h>

#include "video_core/renderer_vulkan/vk_query_cache.h"

#include "common/logging.h"
#include "common/settings.h"
#include "common/dkb_cpu_trace.h"
#include "common/thread.h"
#include "video_core/auto_shading_rate.h"
#include "video_core/gpu_logging/gpu_logging.h"
#include "video_core/nx2_profile.h"
#include "video_core/nx2_uma.h"
#include "video_core/renderer_vulkan/upload_batch.h"
#include "video_core/renderer_vulkan/vk_command_pool.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_master_semaphore.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_state_tracker.h"
#include "video_core/renderer_vulkan/vk_texture_cache.h"
#include "video_core/texture_cache/formatter.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

namespace {
std::atomic<u64> g_profile_submits{};
std::atomic<u64> g_profile_renderpasses{};
std::atomic<u64> g_profile_finishes{};
std::atomic<u64> g_profile_skipped_flushes{};
std::atomic<u64> g_profile_finish_ns{};
std::atomic<u64> g_profile_window_start{};
std::mutex g_profile_end_mutex;
std::map<std::string, u64> g_profile_ends;

constexpr std::array<std::string_view, static_cast<size_t>(TraceCategory::Count)>
    TRACE_CATEGORY_NAMES{
        "image_copy",
        "image_copy_msaa",
        "image_reinterpret",
        "image_blit",
        "image_convert",
        "image_upload",
        "image_download",
        "buffer_copy",
        "buffer_upload_batch",
        "upload_cmdbuf",
        "compute_vertex_repack",
        "compute_uint8_index",
        "compute_quad_index",
        "compute_cond_resolve",
        "compute_query_prefix_scan",
        "compute_astc",
        "compute_bc_encode",
        "compute_unswizzle_3d",
        "query_copy",
        "clear_in_pass",
        "present",
        "buffer_copy.other",
        "buffer_copy.upload",
        "buffer_copy.inline",
        "buffer_copy.index",
        "buffer_copy.dma",
        "buffer_copy.download",
        "buffer_copy.read_download",
        "buffer_copy.fence_download",
        "buffer_copy.list_page",
        "buffer_copy.join",
        "buffer_copy.gather",
        "buffer_upload_batch.other",
        "buffer_upload_batch.upload",
        "buffer_upload_batch.inline",
        "buffer_upload_batch.index",
        "buffer_upload_batch.dma",
        "buffer_upload_batch.download",
        "buffer_upload_batch.read_download",
        "buffer_upload_batch.fence_download",
        "buffer_upload_batch.list_page",
        "buffer_upload_batch.join",
        "buffer_upload_batch.gather",
        "buffer_upload_batch.mixed",
    };
static_assert(!TRACE_CATEGORY_NAMES.back().empty(), "a TraceCategory without a name");

/// The row a category adds up into besides its own: the call-site rows of buffer copies and
/// upload batches go into buffer_copy and buffer_upload_batch, which keep the meaning they had
/// before the split.
constexpr TraceCategory TraceSumRow(TraceCategory category) {
    if (category >= TraceCategory::BufferCopyOther && category <= TraceCategory::BufferCopyGather) {
        return TraceCategory::BufferCopy;
    }
    if (category >= TraceCategory::BufferUploadBatchOther &&
        category <= TraceCategory::BufferUploadBatchMixed) {
        return TraceCategory::BufferUploadBatch;
    }
    return category;
}

std::string_view TraceBaseName(const char* file) {
    std::string_view name{file ? file : "?"};
    if (const auto slash = name.find_last_of("/\\"); slash != std::string_view::npos) {
        name.remove_prefix(slash + 1);
    }
    return name;
}

/// Why a submission was made, from where Flush or Finish was called.
std::string_view TraceFlushReason(const char* file, const char* function_name) {
    if (file == nullptr) {
        return "unknown";
    }
    const std::string_view name = TraceBaseName(file);
    const std::string_view function{function_name ? function_name : ""};
    const auto in = [&](std::string_view text) {
        return function.find(text) != std::string_view::npos;
    };
    if (name == "vk_rasterizer.cpp") {
        if (in("FlushWork")) {
            return "budget";
        }
        if (in("FlushCommands")) {
            return "list_end";
        }
        if (in("ListCop")) {
            return "list_copy";
        }
        if (in("AccelerateDMA") || in("BufferCopy")) {
            return "dma";
        }
        if (in("Query")) {
            return "query";
        }
        return "rasterizer";
    }
    if (name == "vk_fence_manager.cpp" || name == "vk_fence_manager.h" ||
        name == "fence_manager.h") {
        return "fence";
    }
    if (name == "vk_buffer_cache.cpp" || name == "buffer_cache.h") {
        return "host_read";
    }
    if (name == "renderer_vulkan.cpp") {
        return "present";
    }
    if (name == "vk_scheduler.cpp" || name == "vk_scheduler.h") {
        if (in("FlushDeferred")) {
            return "deferred";
        }
        return in("Wait") ? "wait" : "scheduler";
    }
    if (name == "vk_texture_cache.cpp" || name == "texture_cache.h") {
        return "texture";
    }
    if (name == "vk_query_cache.cpp") {
        return "query";
    }
    return "other";
}

void CountRenderPassEnd(const char* file, u32 line) {
    if (!VideoCommon::Nx2Profile::Enabled()) {
        return;
    }
    std::string_view name{file};
    if (const auto slash = name.find_last_of('/'); slash != std::string_view::npos) {
        name.remove_prefix(slash + 1);
    }
    std::scoped_lock lock{g_profile_end_mutex};
    ++g_profile_ends[fmt::format("{}:{}", name, line)];
}

void ReportSubmitCounts() {
    const u64 now = VideoCommon::Nx2Profile::Now();
    u64 start = g_profile_window_start.load(std::memory_order_relaxed);
    if (start == 0) {
        g_profile_window_start.compare_exchange_strong(start, now);
        return;
    }
    if (now - start < 10'000'000'000ULL ||
        !g_profile_window_start.compare_exchange_strong(start, now)) {
        return;
    }
    const double seconds = static_cast<double>(now - start) / 1e9;
    LOG_INFO(Render_Vulkan,
             "Scheduler per second: {:.0f} submits, {:.0f} empty flushes skipped, {:.0f} render "
             "passes, {:.0f} finishes waiting {:.1f} ms",
             static_cast<double>(g_profile_submits.exchange(0)) / seconds,
             static_cast<double>(g_profile_skipped_flushes.exchange(0)) / seconds,
             static_cast<double>(g_profile_renderpasses.exchange(0)) / seconds,
             static_cast<double>(g_profile_finishes.exchange(0)) / seconds,
             static_cast<double>(g_profile_finish_ns.exchange(0)) / 1e6 / seconds);
    std::vector<std::pair<u64, std::string>> ends;
    {
        std::scoped_lock lock{g_profile_end_mutex};
        for (const auto& [site, count] : g_profile_ends) {
            ends.emplace_back(count, site);
        }
        g_profile_ends.clear();
    }
    std::sort(ends.rbegin(), ends.rend());
    for (size_t i = 0; i < ends.size() && i < 16; ++i) {
        LOG_INFO(Render_Vulkan, "Render pass ends {:.0f}/s at {}",
                 static_cast<double>(ends[i].first) / seconds, ends[i].second);
    }
}
}

Scheduler::PassEnd Scheduler::ClassifyPassEnd(const TraceSite& site, bool render_pass_request) {
    const std::string_view name = TraceBaseName(site.file);
    const std::string_view function{site.function ? site.function : ""};
    const auto in = [&](std::string_view text) {
        return function.find(text) != std::string_view::npos;
    };
    if (name == "blit_image.cpp") {
        // Blits, conversions and MSAA copies done with a draw; clears with a pipeline.
        return in("Clear") ? PassEnd::Clear : PassEnd::Copy;
    }
    if (render_pass_request) {
        return PassEnd::Switch;
    }
    if (name == "vk_texture_cache.cpp") {
        if (in("Upload")) {
            return PassEnd::Upload;
        }
        if (in("Download")) {
            return PassEnd::Download;
        }
        if (in("Barrier") || in("TransitionImageLayout") || in("ClearTiles")) {
            return PassEnd::Other;
        }
        return PassEnd::Copy;
    }
    if (name == "vk_rasterizer.cpp") {
        if (in("WaitForIdle")) {
            return PassEnd::Wfi;
        }
        if (in("Dispatch")) {
            return PassEnd::Dispatch;
        }
        return in("Clear") ? PassEnd::Clear : PassEnd::Other;
    }
    if (name == "vk_query_cache.cpp") {
        return PassEnd::Query;
    }
    if (name == "vk_buffer_cache.cpp") {
        return PassEnd::Buffer;
    }
    if (name == "vk_compute_pass.cpp") {
        return PassEnd::Compute;
    }
    return PassEnd::Other;
}

void Scheduler::CommandChunk::ExecuteAll(vk::CommandBuffer cmdbuf,
                                         vk::CommandBuffer upload_cmdbuf) {
    auto command = first;
    while (command != nullptr) {
        auto next = command->GetNext();
        command->Execute(cmdbuf, upload_cmdbuf);
        command->~Command();
        command = next;
    }
    submit = false;
    command_offset = 0;
    first = nullptr;
    last = nullptr;
}

Scheduler::Scheduler(const Device& device_, StateTracker& state_tracker_)
    : device{device_}, state_tracker{state_tracker_},
      master_semaphore{std::make_unique<MasterSemaphore>(device)},
      command_pool{std::make_unique<CommandPool>(*master_semaphore, device)},
      upload_batch{std::make_unique<UploadBatch>()},
      reorder_batch{std::make_unique<UploadBatch>()} {

    if (VideoCore::AutoShadingRate::TargetFps() > 0.0 &&
        !device.IsKhrFragmentShadingRateSupported()) {
        LOG_WARNING(Render_Vulkan, "EDEN_NX2_VRS_AUTO: the device has no per-draw shading rate");
        VideoCore::AutoShadingRate::SetTargetFps(0.0);
    }
    // The submission timestamps also feed the performance overlay's GPU %, so they exist
    // without EDEN_DKB_FRAME_TRACE; nothing is written to them until a sample is asked for.
    const auto physical = device.GetPhysical();
    trace_timestamp_bits =
        physical.GetQueueFamilyProperties()[device.GetGraphicsFamily()].timestampValidBits;
    trace_timestamp_period = physical.GetProperties().limits.timestampPeriod;
    if (trace_timestamp_bits != 0 && trace_timestamp_bits <= 64 && trace_timestamp_period > 0) {
        try {
            trace_query_pool = device.GetLogical().CreateQueryPool({
                .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .queryType = VK_QUERY_TYPE_TIMESTAMP,
                .queryCount = TraceMaxSubmissions * 2,
                .pipelineStatistics = 0,
            });
            trace_submissions.resize(TraceMaxSubmissions);
            // The per-draw pools are made here or never: the recording thread tests them
            // unlocked. The debug server gets them so gpu_costs can start sampling live.
            const char* const cost_env = std::getenv("EDEN_NX2_COST_SAMPLING");
            const bool cost_on = cost_env && std::strcmp(cost_env, "1") == 0;
            const char* const server_env = std::getenv("EDEN_NX2_DEBUG_SERVER");
            // EDEN_NX2_VRS_AUTO: the governor lives on the snapshots.
            const bool vrs_auto = VideoCore::AutoShadingRate::TargetFps() > 0.0;
            if (Common::DkbFrameTrace::Enabled() || cost_on || vrs_auto ||
                (server_env && *server_env != '\0')) {
                trace_work_pool = device.GetLogical().CreateQueryPool({
                    .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                    .pNext = nullptr,
                    .flags = 0,
                    .queryType = VK_QUERY_TYPE_TIMESTAMP,
                    .queryCount = TraceMaxWork * 2,
                    .pipelineStatistics = 0,
                });
                trace_work_items.resize(TraceMaxWork);
                trace_work_ms.resize(TraceMaxWork);
                trace_pass_items.resize(TraceMaxPasses);
                trace_pass_reasons.resize(TraceMaxPasses);
                trace_pass_draw_ms.resize(TraceMaxPasses);
                trace_pass_emu_ms.resize(TraceMaxPasses);
                trace_pass_pool = device.GetLogical().CreateQueryPool({
                    .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                    .pNext = nullptr,
                    .flags = 0,
                    .queryType = VK_QUERY_TYPE_TIMESTAMP,
                    .queryCount = TraceMaxPasses * 4,
                    .pipelineStatistics = 0,
                });
                cost_sampling = cost_on || vrs_auto;
            }
        } catch (const vk::Exception& error) {
            LOG_WARNING(Render_Vulkan, "DKB gpu_timestamp unavailable: {}", error.what());
        }
    } else if (Common::DkbFrameTrace::Enabled()) {
        LOG_INFO(Render_Vulkan, "DKB gpu_timestamp unsupported: valid_bits={} period_ns={}",
                 trace_timestamp_bits, trace_timestamp_period);
    }

    AcquireNewChunk();
    AllocateWorkerCommandBuffer();
    worker_thread = std::jthread([this](std::stop_token token) { WorkerThread(token); });
}

Scheduler::~Scheduler() {
    if (trace_query_pool) {
        WaitWorker();
        worker_thread.request_stop();
        worker_thread.join();
        if (trace_gpu_tick != 0) {
            master_semaphore->Wait(trace_gpu_tick);
        }
    }
}

u64 Scheduler::Flush(VkSemaphore signal_semaphore, VkSemaphore wait_semaphore,
                     std::source_location location) {
    if (state.renderpass) {
        CountRenderPassEnd(location.file_name(), location.line());
        pass_end_reason = PassEnd::Submit;
    }
    if (VideoCommon::GpuSplit::check_owner) [[unlikely]] {
        VideoCommon::GpuSplit::CheckOwner("Scheduler::Flush");
    }
    // When flushing, we only send data to the worker thread; no waiting is necessary.
    const u64 signal_value =
        SubmitExecution(signal_semaphore, wait_semaphore, MakeTraceSite(location));
    AllocateNewContext();
    submitted_serial = record_serial;
    last_flush = std::chrono::steady_clock::now();
    flush_pending = false;
    return signal_value;
}

void Scheduler::CountSkippedFlush() {
    if (VideoCommon::Nx2Profile::Enabled()) {
        g_profile_skipped_flushes.fetch_add(1, std::memory_order_relaxed);
    }
}

bool Scheduler::DeferFlush() {
    // Fences come one per compute release: ~11,500 a second in Hyrule Warriors: Age of
    // Imprisonment fights, each a queue submission (~22,000 a second with the others), which
    // kept the Vulkan worker busy submitting. Batched within 500 us: ~10-15% more fps there.
    // Command list copies after dispatches were ~9,000 more a second, of which the front waited
    // for about 5%.
    static const s64 interval_us = [] {
        const char* value = std::getenv("EDEN_NX2_FENCE_FLUSH_US");
        if (value != nullptr) {
            return static_cast<s64>(std::atoll(value));
        }
        return s64{500};
    }();
    if (interval_us <= 0 || !VideoCommon::GpuSplit::OnBackThread() ||
        std::chrono::steady_clock::now() - last_flush >= std::chrono::microseconds{interval_us}) {
        return false;
    }
    flush_pending = true;
    return true;
}

void Scheduler::FlushDeferred() {
    if (flush_pending) {
        Flush();
    }
}

bool Scheduler::WaitSubmitted(u64 tick) {
    if (IsSubmitted(tick)) {
        return true;
    }
    auto& split = VideoCommon::GpuSplit::Get();
    split.RequestIdleHook();
    // No time limit: the back can spend seconds on one entry (a large shader's translation),
    // and a fence reported as submitted before it is would release its downloads too early.
    const auto start = std::chrono::steady_clock::now();
    auto next_log = start + std::chrono::seconds{2};
    for (u32 spins = 0; !IsSubmitted(tick); ++spins) {
        if (!split.BackRunning()) {
            return IsSubmitted(tick);
        }
        if (const auto now = std::chrono::steady_clock::now(); now >= next_log) {
            next_log = now + std::chrono::seconds{2};
            LOG_WARNING(Render_Vulkan, "Waited {} s for the GPU thread to submit tick {}",
                        std::chrono::duration_cast<std::chrono::seconds>(now - start).count(),
                        tick);
            split.RequestIdleHook();
        }
        if (spins < 64) {
            std::this_thread::yield();
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds{50});
        }
    }
    return true;
}

void Scheduler::Finish(VkSemaphore signal_semaphore, VkSemaphore wait_semaphore,
                       std::source_location location) {
    if (state.renderpass) {
        CountRenderPassEnd(location.file_name(), location.line());
        pass_end_reason = PassEnd::Submit;
    }
    if (VideoCommon::GpuSplit::check_owner) [[unlikely]] {
        VideoCommon::GpuSplit::CheckOwner("Scheduler::Finish");
    }
    // When finishing, we need to wait for the submission to have executed on the device.
    const VideoCommon::StutterTrace::SiteScope site{VideoCommon::StutterTrace::Site::SchedulerFinish};
    const u64 presubmit_tick = CurrentTick();
    const bool profile = VideoCommon::Nx2Profile::Enabled();
    const u64 start = profile ? VideoCommon::Nx2Profile::Now() : 0;
    SubmitExecution(signal_semaphore, wait_semaphore, MakeTraceSite(location));
    Wait(presubmit_tick);
    AllocateNewContext();
    submitted_serial = record_serial;
    last_flush = std::chrono::steady_clock::now();
    flush_pending = false;
    if (profile) {
        g_profile_finishes.fetch_add(1, std::memory_order_relaxed);
        g_profile_finish_ns.fetch_add(VideoCommon::Nx2Profile::Now() - start,
                                      std::memory_order_relaxed);
    }
}

void Scheduler::WaitWorker() {
    DispatchWork();

    // Ensure the queue is drained.
    {
        std::unique_lock ql{queue_mutex};
        event_cv.wait(ql, [this] { return work_queue.empty(); });
    }

    // Now wait for execution to finish.
    std::scoped_lock el{execution_mutex};
}

Scheduler::FrameMark Scheduler::NextFrameMark() {
    if (TraceCostsWanted()) {
        return FrameMark::Both;
    }
    u32 marks = gpu_busy_marks.load(std::memory_order_relaxed);
    if (marks == 0 ||
        !gpu_busy_marks.compare_exchange_strong(marks, marks - 1, std::memory_order_relaxed)) {
        return FrameMark::None;
    }
    return marks == 2 ? FrameMark::Start : FrameMark::End;
}

void Scheduler::MarkFrameEnd(u64 frame, FrameMark mark) {
    Record([this, frame, mark](vk::CommandBuffer) {
        trace_frame_end = frame;
        trace_frame_mark = mark;
    });
}

std::optional<double> Scheduler::GpuBusyMs() const {
    std::scoped_lock lock{gpu_busy_mutex};
    if (!gpu_busy || std::chrono::steady_clock::now() - gpu_busy->taken > std::chrono::seconds{3}) {
        return std::nullopt;
    }
    return gpu_busy->ms;
}

VideoCore::GpuCostSnapshot Scheduler::GpuCosts() const {
    std::scoped_lock lock{gpu_costs_mutex};
    return gpu_costs;
}

void Scheduler::DispatchWork() {
    if (VideoCommon::GpuSplit::check_owner) [[unlikely]] {
        VideoCommon::GpuSplit::CheckOwner("Scheduler::DispatchWork");
    }
    if (uploads_pending) {
        // The worker must receive everything recorded so far.
        FlushUploads();
    }
    if (chunk && !chunk->Empty()) {
        // Waking the worker is a system call (~2% of the GPU thread at one dispatch per 8
        // draws): skip it while the worker is still busy, it pops the queue before waiting.
        bool wake;
        {
            std::scoped_lock ql{queue_mutex};
            work_queue.push(std::move(chunk));
            wake = worker_waiting;
        }
        if (wake) {
            event_cv.notify_all();
        }
        AcquireNewChunk();
    }
}

void Scheduler::RecordUpload(VkBuffer src, VkBuffer dst, std::span<const VkBufferCopy> copies,
                             TraceCategory category) {
    if (VideoCommon::GpuSplit::check_owner) {
        VideoCommon::GpuSplit::CheckOwner("Scheduler::RecordUpload");
    }
    // Check each region so even a single upload larger than the capacity stays bounded, and
    // overlapping regions within that upload cannot enter the same batch.
    for (const VkBufferCopy& copy : copies) {
        const std::span<const VkBufferCopy> region{&copy, 1};
        if (uploads_pending && !upload_batch->CanAdd(src, dst, region)) {
            FlushUploads();
        }
        upload_batch->Add(src, dst, region);
        if (!uploads_pending) {
            upload_batch_category = category;
        } else if (upload_batch_category != category) {
            upload_batch_category = TraceCategory::BufferUploadBatchMixed;
        }
        uploads_pending = true;
    }
}

void Scheduler::FlushUploads() {
    if (VideoCommon::Nx2Uma::Has(VideoCommon::Nx2Uma::Coalesce)) {
        std::vector<VkBufferCopy> merged_copies;
        std::vector<UploadBatch::Group> merged_groups;
        upload_batch->Coalesce(merged_copies, merged_groups);
        const u32 merged_num_copies = static_cast<u32>(merged_copies.size());
        const u32 merged_num_groups = static_cast<u32>(merged_groups.size());
        auto merged_command = [copies = std::move(merged_copies),
                               groups = std::move(merged_groups)](vk::CommandBuffer cmdbuf) {
            UploadBatch::Record(cmdbuf, copies, groups);
        };
        upload_batch->copies.clear();
        upload_batch->groups.clear();
        uploads_pending = false;
        RecordEmu(upload_batch_category, {merged_num_copies, merged_num_groups, 0},
                  std::move(merged_command));
        return;
    }
    // Copy the vectors so the recording thread retains their capacity while the worker uses them.
    const u32 num_copies = static_cast<u32>(upload_batch->copies.size());
    const u32 num_groups = static_cast<u32>(upload_batch->groups.size());
    auto command = [copies = upload_batch->copies, groups = upload_batch->groups](
                       vk::CommandBuffer cmdbuf) { UploadBatch::Record(cmdbuf, copies, groups); };
    upload_batch->copies.clear();
    upload_batch->groups.clear();
    uploads_pending = false;
    RecordEmu(upload_batch_category, {num_copies, num_groups, 0}, std::move(command));
}

void Scheduler::RecordReorderedUpload(VkBuffer src, VkBuffer dst,
                                      std::span<const VkBufferCopy> copies) {
    if (VideoCommon::GpuSplit::check_owner) {
        VideoCommon::GpuSplit::CheckOwner("Scheduler::RecordReorderedUpload");
    }
    // As RecordUpload: a region that overwrites a pending one is recorded after it.
    for (const VkBufferCopy& copy : copies) {
        const std::span<const VkBufferCopy> region{&copy, 1};
        if (reorders_pending && !reorder_batch->CanAdd(src, dst, region)) {
            FlushReorderedUploads();
        }
        reorder_batch->Add(src, dst, region);
        reorders_pending = true;
    }
}

void Scheduler::FlushReorderedUploads() {
    std::vector<VkBufferCopy> merged_copies;
    std::vector<UploadBatch::Group> merged_groups;
    reorder_batch->Coalesce(merged_copies, merged_groups);
    reorder_batch->copies.clear();
    reorder_batch->groups.clear();
    reorders_pending = false;
    RecordWithUploadBuffer([copies = std::move(merged_copies), groups = std::move(merged_groups)](
                               vk::CommandBuffer, vk::CommandBuffer upload_cmdbuf) {
        for (const UploadBatch::Group& group : groups) {
            upload_cmdbuf.CopyBuffer(
                group.src, group.dst,
                vk::Span<VkBufferCopy>(copies.data() + group.first, group.count));
        }
    });
}

void Scheduler::RecordEmuBegin(TraceCategory category, std::array<u32, 3> size) {
    if (!trace_work_pool) {
        return;
    }
    Record([this, category, size](vk::CommandBuffer cmdbuf) {
        if (!trace_gpu_recording || !trace_costs_active || trace_span_item != TraceNoPass) {
            return;
        }
        if (trace_work_count == TraceMaxWork) {
            ++trace_work_dropped;
            return;
        }
        trace_span_item = trace_work_count++;
        trace_work_items[trace_span_item] = {static_cast<u64>(category), 0, size, TraceEmuKind,
                                             TraceNoPass, 0};
        cmdbuf.WriteTimestamp(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, *trace_work_pool,
                              trace_span_item * 2);
    });
}

void Scheduler::RecordEmuEnd() {
    if (!trace_work_pool) {
        return;
    }
    Record([this](vk::CommandBuffer cmdbuf) {
        if (trace_span_item == TraceNoPass) {
            return;
        }
        cmdbuf.WriteTimestamp(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, *trace_work_pool,
                              trace_span_item * 2 + 1);
        trace_work_items[trace_span_item].span_end = trace_work_count;
        trace_span_item = TraceNoPass;
    });
}

void Scheduler::BeginRenderPassImpl(const Framebuffer* framebuffer, VkRenderPass renderpass,
                                    const VkClearValue* clear_values, u32 clear_value_count) {
    if (VideoCommon::Nx2Profile::Enabled()) {
        g_profile_renderpasses.fetch_add(1, std::memory_order_relaxed);
    }
    const VkFramebuffer framebuffer_handle = framebuffer->Handle();
    const VkExtent2D render_area = framebuffer->RenderArea();
    ++render_pass_serial;
    state.renderpass = renderpass;
    state.framebuffer = framebuffer_handle;
    state.render_area = render_area;

    if (GPU::Logging::IsActive() && Settings::values.gpu_log_vulkan_calls.GetValue()) {
        const std::string render_pass_info =
            fmt::format("renderArea={}x{}, numImages={}", render_area.width, render_area.height,
                        framebuffer->NumImages());
        GPU::Logging::GPULogger::GetInstance().LogRenderPassBegin(render_pass_info);
    }

    std::array<VkClearValue, 9> values{};
    for (u32 i = 0; i < clear_value_count && i < values.size(); ++i) {
        values[i] = clear_values[i];
    }
    if (trace_pass_pool && TraceCostsWanted()) {
        trace_pass_begun = true;
        const RenderPassKey& key = framebuffer->RenderPassKeyBase();
        PassTrace pass{
            .width = render_area.width,
            .height = render_area.height,
            .color_format = static_cast<u8>(VideoCore::Surface::PixelFormat::Invalid),
            .num_colors = 0,
            .depth_format = static_cast<u8>(key.depth_format),
            .samples = static_cast<u8>(key.samples),
        };
        for (const VideoCore::Surface::PixelFormat format : key.color_formats) {
            if (format == VideoCore::Surface::PixelFormat::Invalid) {
                continue;
            }
            if (pass.num_colors++ == 0) {
                pass.color_format = static_cast<u8>(format);
            }
        }
        Record([this, pass](vk::CommandBuffer cmdbuf) { TracePassBegin(cmdbuf, pass); });
    }
    const bool after_compute = std::exchange(compute_writes_pending, false);
    Record([renderpass, framebuffer_handle, render_area, values, clear_value_count,
            after_compute](vk::CommandBuffer cmdbuf) {
        const VkRenderPassBeginInfo renderpass_bi{
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .pNext = nullptr,
            .renderPass = renderpass,
            .framebuffer = framebuffer_handle,
            .renderArea =
                {
                    .offset = {.x = 0, .y = 0},
                    .extent = render_area,
                },
            .clearValueCount = clear_value_count,
            .pClearValues = clear_value_count != 0 ? values.data() : nullptr,
        };
        if (after_compute) {
            // Draws read compute output as indirect arguments, vertices and resources; nothing
            // else orders those reads after the dispatch.
            static constexpr VkMemoryBarrier COMPUTE_TO_GRAPHICS{
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            };
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                   VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, 0, COMPUTE_TO_GRAPHICS);
        }
        cmdbuf.BeginRenderPass(renderpass_bi, VK_SUBPASS_CONTENTS_INLINE);
    });
    if (trace_pass_begun) {
        Record([this](vk::CommandBuffer cmdbuf) { TracePassStamp(cmdbuf, 1); });
    }
    num_renderpass_images = framebuffer->NumImages();
    renderpass_images = framebuffer->Images();
    renderpass_image_ranges = framebuffer->ImageRanges();
    framebuffer->MarkResolveShadowsUpToDate();
}

void Scheduler::RealizeDeferredClear() {
    if (deferred_clear.framebuffer == nullptr) {
        return;
    }
    const DeferredClear dc = deferred_clear;
    deferred_clear = {};

    std::array<VkClearValue, 9> clear_values{};
    u32 count = 0;
    const RenderPassKey& base = dc.framebuffer->RenderPassKeyBase();
    for (u32 slot = 0; slot < 8; ++slot) {
        if (base.color_formats[slot] == VideoCore::Surface::PixelFormat::Invalid) {
            continue;
        }
        clear_values[count++] = dc.color_values[slot];
    }
    if (base.depth_format != VideoCore::Surface::PixelFormat::Invalid) {
        clear_values[count++] = dc.depth_stencil_value;
    }
    const u32 color_discard_mask =
        dc.framebuffer->DiscardsMsaaColor() ? dc.color_clear_mask : 0u;
    const bool depth_stencil_discard =
        dc.depth_stencil && dc.framebuffer->DiscardsMsaaDepthStencil();
    // No pass is open while a clear is parked: keep the caller's reason for the pass begun here.
    const PassEnd reason = pass_end_reason;
    EndRenderPass();
    const VkRenderPass renderpass = dc.framebuffer->RenderPassVariant(
        dc.color_clear_mask, dc.depth_stencil, color_discard_mask, depth_stencil_discard,
        PickDepthLayout(dc.framebuffer));
    BeginRenderPassImpl(dc.framebuffer, renderpass, clear_values.data(), count);
    pass_end_reason = reason;
}

bool Scheduler::DeferColorClear(const Framebuffer* framebuffer, u32 rt_slot,
                                const VkClearValue& value) {
    if (IsRenderPassActive()) {
        return false;
    }
    if (deferred_clear.framebuffer != nullptr && deferred_clear.framebuffer != framebuffer) {
        RealizeDeferredClear();
        EndRenderPass();
    }
    deferred_clear.framebuffer = framebuffer;
    deferred_clear.color_clear_mask |= 1u << rt_slot;
    deferred_clear.color_values[rt_slot] = value;
    return true;
}

bool Scheduler::DeferDepthStencilClear(const Framebuffer* framebuffer, const VkClearValue& value) {
    if (IsRenderPassActive()) {
        return false;
    }
    if (deferred_clear.framebuffer != nullptr && deferred_clear.framebuffer != framebuffer) {
        RealizeDeferredClear();
        EndRenderPass();
    }
    deferred_clear.framebuffer = framebuffer;
    deferred_clear.depth_stencil = true;
    deferred_clear.depth_stencil_value = value;
    return true;
}

void Scheduler::FlushDeferredClear() {
    if (deferred_clear.framebuffer == nullptr) {
        return;
    }
    RealizeDeferredClear();
    EndRenderPass();
}

void Scheduler::RequestRenderpass(const Framebuffer* framebuffer, std::source_location location) {
    if (deferred_clear.framebuffer == framebuffer) {
        RealizeDeferredClear();
        return;
    }
    const VkFramebuffer framebuffer_handle = framebuffer->Handle();
    const VkExtent2D render_area = framebuffer->RenderArea();
    // The active pass can't go on with its depth attachment in the attachment layout once a draw
    // samples it, nor may a pass that didn't check what it samples leave a resting depth image.
    const bool depth_restart =
        (pass_depth_image != VK_NULL_HANDLE && framebuffer->DepthSampledInPass()) ||
        (resting_depth_image != VK_NULL_HANDLE && !keep_resting_depth);
    // A draw continues the active pass whenever the same framebuffer and area are bound. The
    // render-pass *variant* (a deferred clear's loadOp=CLEAR form) is only meaningful at begin -
    // loadOps are applied once, and the base/variant passes are render-pass-compatible (only
    // loadOp/storeOp differ). Comparing the exact handle here made every draw after a deferred
    // clear restart the pass and re-LOAD what was just CLEARed (~118 MB of attachment traffic per
    // world-target turn). The framebuffer handle already fixes the attachment structure and key,
    // so it is the correct identity to compare.
    if (state.renderpass && framebuffer_handle == state.framebuffer &&
        render_area.width == state.render_area.width &&
        render_area.height == state.render_area.height && !depth_restart) {
        return;
    }
    if (state.renderpass) {
        CountRenderPassEnd(location.file_name(), location.line());
        if (trace_pass_begun) {
            pass_end_reason = ClassifyPassEnd(MakeTraceSite(location), true);
        }
    }
    // Ends any active pass and realizes a deferred clear
    EndRenderPass();
    const VkRenderPass renderpass =
        framebuffer->RenderPassVariant(0, false, 0, false, PickDepthLayout(framebuffer));
    BeginRenderPassImpl(framebuffer, renderpass, nullptr, 0);
}

void Scheduler::RequestDrawRenderpass(const Framebuffer* framebuffer,
                                      std::source_location location) {
    keep_resting_depth = true;
    RequestRenderpass(framebuffer, location);
    keep_resting_depth = false;
}

u32 Scheduler::DepthOptimalMode() {
    static const u32 mode = [] {
        const char* value = std::getenv("EDEN_NX2_DEPTH_OPTIMAL");
        const u32 result = value != nullptr ? static_cast<u32>(std::atoi(value)) : u32{0};
        if (result != 0) {
            LOG_INFO(Render_Vulkan, "EDEN_NX2_DEPTH_OPTIMAL={}: depth attachments use "
                                    "DEPTH_STENCIL_ATTACHMENT_OPTIMAL{}",
                     result, result >= 2 ? " and rest in it between passes" : " in their passes");
        }
        return result;
    }();
    return mode;
}

u8 Scheduler::PickDepthLayout(const Framebuffer* framebuffer) {
    const u32 mode = DepthOptimalMode();
    if (mode == 0) {
        return 0;
    }
    const VkImage depth_image = framebuffer->DepthImage();
    const bool optimal = depth_image != VK_NULL_HANDLE &&
                         framebuffer->Samples() == VK_SAMPLE_COUNT_1_BIT &&
                         !framebuffer->DepthSampledInPass();
    bool resumes = false;
    if (resting_depth_image != VK_NULL_HANDLE) {
        if (optimal && depth_image == resting_depth_image) {
            const VkImageSubresourceRange& range = framebuffer->DepthImageRange();
            resumes = range.aspectMask == resting_depth_range.aspectMask &&
                      range.baseMipLevel == resting_depth_range.baseMipLevel &&
                      range.levelCount == resting_depth_range.levelCount &&
                      range.baseArrayLayer == resting_depth_range.baseArrayLayer &&
                      range.layerCount == resting_depth_range.layerCount;
        }
        if (resumes) {
            resting_depth_image = VK_NULL_HANDLE;
        } else {
            // Only one image rests at a time, and none that this pass has as an attachment.
            const std::array<VkImage, 9>& attachments = framebuffer->Images();
            bool is_attachment = false;
            for (u32 index = 0; index < framebuffer->NumImages(); ++index) {
                is_attachment |= attachments[index] == resting_depth_image;
            }
            if (optimal || is_attachment || !keep_resting_depth) {
                ReleaseRestingDepth();
            }
        }
    }
    if (!optimal) {
        return 0;
    }
    pass_depth_image = depth_image;
    pass_depth_range = framebuffer->DepthImageRange();
    pass_depth_final_layout = mode >= 2 ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                                        : VK_IMAGE_LAYOUT_GENERAL;
    if (resumes) {
        return u8{3};
    }
    return mode >= 2 ? u8{2} : u8{1};
}

void Scheduler::ReleaseRestingDepth(VkImage image) {
    if (resting_depth_image == VK_NULL_HANDLE ||
        (image != VK_NULL_HANDLE && image != resting_depth_image)) {
        return;
    }
    if (state.renderpass) {
        // The active pass has no depth attachment in the attachment layout (PickDepthLayout),
        // so ending it leaves the resting image as it is.
        EndRenderPass();
    }
    Record([resting = resting_depth_image,
            resting_range = resting_depth_range](vk::CommandBuffer cmdbuf) {
        const VkImageMemoryBarrier barrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                             VK_ACCESS_TRANSFER_WRITE_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = resting,
            .subresourceRange = resting_range,
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                   VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, barrier);
    });
    resting_depth_image = VK_NULL_HANDLE;
}

void Scheduler::ReleaseDepthForSampling(VkImage image) {
    if (image == VK_NULL_HANDLE) {
        return;
    }
    if (image == pass_depth_image && state.renderpass) {
        EndRenderPass();
    }
    ReleaseRestingDepth(image);
}

void Scheduler::EndRenderPassForOutsideWork(const std::source_location& location) {
    if (state.renderpass) {
        CountRenderPassEnd(location.file_name(), location.line());
        if (trace_pass_begun) {
            pass_end_reason = ClassifyPassEnd(MakeTraceSite(location), false);
        }
    }
    EndRenderPass();
}

void Scheduler::RequestOutsideRenderPassOperationContext(std::source_location location) {
    EndRenderPassForOutsideWork(location);
    ReleaseRestingDepth();
}

void Scheduler::RequestOutsideRenderPassBufferContext(std::source_location location) {
    EndRenderPassForOutsideWork(location);
}

void Scheduler::RequestOutsideRenderPassImageContext(VkImage image,
                                                     std::source_location location) {
    EndRenderPassForOutsideWork(location);
    ReleaseRestingDepth(image);
}

bool Scheduler::UpdateGraphicsPipeline(GraphicsPipeline* pipeline) {
    if (state.graphics_pipeline == pipeline) {
        if (pipeline && pipeline->UsesExtendedDynamicState() &&
            state.needs_state_enable_refresh) {
            state_tracker.InvalidateStateEnableFlag();
            state.needs_state_enable_refresh = false;
        }
        return false;
    }

    state.graphics_pipeline = pipeline;

    if (!pipeline) {
        return true;
    }

    if (!pipeline->UsesExtendedDynamicState()) {
        state.needs_state_enable_refresh = true;
    } else if (state.needs_state_enable_refresh) {
        state_tracker.InvalidateStateEnableFlag();
        state.needs_state_enable_refresh = false;
    }

    return true;
}

bool Scheduler::UpdateRescaling(bool is_rescaling, bool mesh_layout) {
    if (state.rescaling_defined && is_rescaling == state.is_rescaling &&
        mesh_layout == state.mesh_layout) {
        return false;
    }
    state.rescaling_defined = true;
    state.is_rescaling = is_rescaling;
    state.mesh_layout = mesh_layout;
    return true;
}

bool Scheduler::UpdateDescriptorBufferChunk(u32 descriptor_chunk) {
    if (state.descriptor_buffer_bound && descriptor_chunk == state.descriptor_buffer_chunk) {
        return false;
    }
    state.descriptor_buffer_bound = true;
    state.descriptor_buffer_chunk = descriptor_chunk;
    return true;
}

void Scheduler::WorkerThread(std::stop_token stop_token) {
    Common::SetCurrentThreadName("VulkanWorker");
    const Common::DkbCpuTrace::Registration trace_thread{Common::DkbCpuTrace::Role::VulkanWorker};
    const VideoCommon::StutterTrace::ThreadScope stutter_thread{
        VideoCommon::StutterTrace::Thread::Worker};
    Common::SetCurrentThreadPriority(Common::ThreadPriority::Critical);
    Common::SetCurrentThreadToPerformanceCores();

    const auto TryPopQueue{[this](auto& work) -> bool {
        if (work_queue.empty()) {
            return false;
        }

        work = std::move(work_queue.front());
        work_queue.pop();
        event_cv.notify_all();
        return true;
    }};

    while (!stop_token.stop_requested()) {
        std::unique_ptr<CommandChunk> work;

        {
            std::unique_lock lk{queue_mutex};

            // Wait for work.
            {
                const VideoCommon::Nx2Profile::Scope scope{VideoCommon::Nx2Profile::Zone::WorkerIdle};
                event_cv.wait(lk, stop_token, [&] {
                    worker_waiting = !TryPopQueue(work);
                    return !worker_waiting;
                });
            }
            worker_waiting = false;

            // If we've been asked to stop, we're done.
            if (stop_token.stop_requested()) {
                return;
            }

            // Exchange lock ownership so that we take the execution lock before
            // the queue lock goes out of scope. This allows us to force execution
            // to complete in the next step.
            void(std::exchange(lk, std::unique_lock{execution_mutex}));

            // Perform the work, tracking whether the chunk was a submission
            // before executing.
            const bool has_submit = work->HasSubmit();
            work->ExecuteAll(current_cmdbuf, current_upload_cmdbuf);

            // If the chunk was a submission, reallocate the command buffer.
            if (has_submit) {
                AllocateWorkerCommandBuffer();
            }
        }

        {
            std::scoped_lock rl{reserve_mutex};

            // Recycle the chunk back to the reserve.
            chunk_reserve.emplace_back(std::move(work));
        }
        VideoCommon::Nx2Profile::Report("Vulkan worker");
    }
}

void Scheduler::AllocateWorkerCommandBuffer() {
    current_cmdbuf = vk::CommandBuffer(command_pool->Commit(), device.GetDispatchLoader());
    current_cmdbuf.Begin({
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    });
    current_upload_cmdbuf = vk::CommandBuffer(command_pool->Commit(), device.GetDispatchLoader());
    current_upload_cmdbuf.Begin({
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    });
    if (trace_query_pool && !trace_gpu_failed && (trace_gpu_pending || trace_gpu_sampling)) {
        TraceGpuSubmission();
    }
}

void Scheduler::TraceShader(u64 hash, u32 stage) {
    if (hash == 0) {
        return;
    }
    for (u32 i = 0; i < trace_work.num_shaders; ++i) {
        if (trace_work.shaders[i].hash == hash && trace_work.shaders[i].stage == stage) {
            return;
        }
    }
    if (trace_work.num_shaders == trace_work.shaders.size()) {
        ++trace_work.untracked_uses;
        return;
    }
    trace_work.shaders[trace_work.num_shaders++] = {hash, stage};
}

void Scheduler::TraceGraphicsPipeline(const std::array<u64, 6>& hashes) {
    // Shader lists per submission only feed the DKB log lines.
    if (!trace_work_pool || !Common::DkbFrameTrace::Enabled()) {
        return;
    }
    Record([this, hashes](vk::CommandBuffer) {
        if (!trace_gpu_recording) {
            return;
        }
        ++trace_work.draw_attempts;
        for (u32 stage = 0; stage < hashes.size(); ++stage) {
            TraceShader(hashes[stage], stage);
        }
    });
}

void Scheduler::TraceComputePipeline(u64 hash) {
    if (!trace_work_pool || !Common::DkbFrameTrace::Enabled()) {
        return;
    }
    Record([this, hash](vk::CommandBuffer) {
        if (!trace_gpu_recording) {
            return;
        }
        ++trace_work.dispatch_attempts;
        TraceShader(hash, 6);
    });
}

u32 Scheduler::FetchWorkTimes() {
    const u32 count = std::exchange(trace_work_count, 0);
    std::ranges::fill(trace_pass_draw_ms, 0.0);
    std::ranges::fill(trace_pass_emu_ms, 0.0);
    if (count == 0) {
        return 0;
    }
    struct Result {
        u64 timestamp;
        u64 available;
    };
    std::vector<Result> results(count * 2);
    const auto result = device.GetLogical().GetQueryResults(
        *trace_work_pool, 0, count * 2, results.size() * sizeof(Result), results.data(),
        sizeof(Result), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    // VK_NOT_READY: a span cut by a submission never wrote its end; its item is skipped below.
    if (result != VK_SUCCESS && result != VK_NOT_READY) {
        return 0;
    }
    for (u32 i = 0; i < count; ++i) {
        trace_work_ms[i] = -1.0;
        if (results[i * 2].available == 0 || results[i * 2 + 1].available == 0) {
            continue;
        }
        trace_work_ms[i] = static_cast<double>(Common::DkbFrameTrace::TimestampDelta(
                               results[i * 2].timestamp, results[i * 2 + 1].timestamp,
                               trace_timestamp_bits)) *
                           trace_timestamp_period / 1e6;
    }
    for (u32 i = 0; i < count; ++i) {
        const WorkTrace& item = trace_work_items[i];
        double& ms = trace_work_ms[i];
        if (ms < 0.0) {
            continue;
        }
        // A span reports its own time: what was timed inside it has rows of its own.
        for (u32 inner = i + 1; inner < item.span_end && inner < count; ++inner) {
            if (trace_work_ms[inner] > 0.0) {
                ms = (std::max)(0.0, ms - trace_work_ms[inner]);
            }
        }
        if (item.pass < TraceMaxPasses) {
            (item.kind == TraceEmuKind ? trace_pass_emu_ms : trace_pass_draw_ms)[item.pass] += ms;
        }
    }
    return count;
}

void Scheduler::ReportWorkTimes(u32 count, double submission_sum_ms,
                                VideoCore::GpuCostSnapshot& costs) {
    const u32 dropped = std::exchange(trace_work_dropped, 0);
    if (count == 0) {
        return;
    }
    struct Totals {
        double total_ms{};
        double max_ms{};
        u32 count{};
        u32 small_draws{};
        u32 side_draws{};
        std::array<u32, 3> max_size{};
    };
    std::map<std::tuple<u64, u64, char>, Totals> by_shaders;
    std::array<Totals, static_cast<size_t>(TraceCategory::Count)> by_category{};
    double total_ms{};
    double emu_ms{};
    double dispatch_ms{};
    // Draw time by render target, to tell the main scene target from the side ones.
    std::map<PassTrace, double> by_target;
    for (u32 i = 0; i < count; ++i) {
        const double ms = trace_work_ms[i];
        const WorkTrace& item = trace_work_items[i];
        if (ms < 0.0 || item.kind == TraceEmuKind) {
            continue;
        }
        if (item.fragment_hash != 0 && item.pass < trace_pass_count) {
            by_target[trace_pass_items[item.pass]] += ms;
        }
    }
    const u8 no_format = static_cast<u8>(VideoCore::Surface::PixelFormat::Invalid);
    const PassTrace* main_target{};
    double main_ms{};
    for (const auto& [target, target_ms] : by_target) {
        if (target.depth_format != no_format && target.num_colors != 0 && target_ms > main_ms) {
            main_target = &target;
            main_ms = target_ms;
        }
    }
    costs.has_main_target = main_target != nullptr;
    for (u32 i = 0; i < count; ++i) {
        const double ms = trace_work_ms[i];
        if (ms < 0.0) {
            continue;
        }
        const WorkTrace& item = trace_work_items[i];
        const bool is_emu = item.kind == TraceEmuKind;
        if (is_emu && item.hash >= by_category.size()) {
            continue;
        }
        Totals& totals = is_emu ? by_category[item.hash]
                                : by_shaders[{item.hash, item.fragment_hash, item.kind}];
        totals.total_ms += ms;
        ++totals.count;
        totals.small_draws +=
            item.kind == '\0' && item.size[0] != 0 && item.size[0] <= 6 && item.size[1] == 1;
        totals.side_draws += main_target && item.fragment_hash != 0 &&
                             item.pass < trace_pass_count &&
                             trace_pass_items[item.pass] != *main_target;
        if (ms > totals.max_ms || totals.count == 1) {
            totals.max_ms = ms;
            totals.max_size = item.size;
        }
        if (is_emu) {
            emu_ms += ms;
            continue;
        }
        total_ms += ms;
        if (item.fragment_hash == 0) {
            dispatch_ms += ms;
        }
    }
    // The call-site rows add up into their sum rows (TraceSumRow). Rows only: emu_ms counted each
    // item once.
    for (size_t category = 0; category < by_category.size(); ++category) {
        const size_t sum_row =
            static_cast<size_t>(TraceSumRow(static_cast<TraceCategory>(category)));
        const Totals& part = by_category[category];
        if (sum_row == category || part.count == 0) {
            continue;
        }
        Totals& sum = by_category[sum_row];
        if (part.max_ms > sum.max_ms || sum.count == 0) {
            sum.max_ms = part.max_ms;
            sum.max_size = part.max_size;
        }
        sum.total_ms += part.total_ms;
        sum.count += part.count;
    }
    std::vector<std::pair<std::tuple<u64, u64, char>, Totals>> sorted(by_shaders.begin(),
                                                                      by_shaders.end());
    std::ranges::sort(sorted, std::greater<>{},
                      [](const auto& entry) { return entry.second.total_ms; });
    std::vector<std::pair<size_t, Totals>> emu_sorted;
    for (size_t category = 0; category < by_category.size(); ++category) {
        if (by_category[category].count != 0) {
            emu_sorted.emplace_back(category, by_category[category]);
        }
    }
    std::ranges::sort(emu_sorted, std::greater<>{},
                      [](const auto& entry) { return entry.second.total_ms; });
    costs.timed_sum_ms = total_ms;
    costs.emu_sum_ms = emu_ms;
    for (const auto& [category, totals] : emu_sorted) {
        costs.emu.push_back({
            .category = std::string{TRACE_CATEGORY_NAMES[category]},
            .count = totals.count,
            .total_ms = totals.total_ms,
            .max_ms = totals.max_ms,
        });
    }
    for (const auto& [shaders, totals] : sorted) {
        if (costs.work.size() == VideoCore::GpuCostSnapshot::MaxWork) {
            break;
        }
        costs.work.push_back({
            .vertex_hash = std::get<0>(shaders),
            .fragment_hash = std::get<1>(shaders),
            .kind = std::get<2>(shaders),
            .draws = totals.count,
            .small_draws = totals.small_draws,
            .side_draws = totals.side_draws,
            .total_ms = totals.total_ms,
            .max_ms = totals.max_ms,
        });
    }
    if (!Common::DkbFrameTrace::Enabled()) {
        return;
    }
    if (sorted.size() > 24) {
        sorted.resize(24);
    }
    std::string slowest;
    for (const auto& [shaders, totals] : sorted) {
        const auto& [hash, fragment_hash, kind] = shaders;
        slowest += fragment_hash == 0
                       ? fmt::format(" s6:{:016x}", hash)
                       : fmt::format(" s1:{:016x}+s5:{:016x}", hash, fragment_hash);
        if (kind != '\0') {
            slowest += fmt::format("#{}", kind);
        }
        slowest += fmt::format("={:.3f}ms/{}x(max {:.3f}ms size {}x{}x{})", totals.total_ms,
                               totals.count, totals.max_ms, totals.max_size[0],
                               totals.max_size[1], totals.max_size[2]);
    }
    std::string emu_rows;
    for (const auto& [category, totals] : emu_sorted) {
        emu_rows += fmt::format(" emu:{}={:.3f}ms/{}x(max {:.3f}ms size {}x{}x{})",
                                TRACE_CATEGORY_NAMES[category], totals.total_ms, totals.count,
                                totals.max_ms, totals.max_size[0], totals.max_size[1],
                                totals.max_size[2]);
    }
    // timed_sum_ms is guest draws and dispatches only; untimed_ms no longer holds emu_ms.
    LOG_INFO(Render_Vulkan,
             "DKB gpu_work renderer_frame={} timed={} dropped={} timed_sum_ms={:.3f} "
             "submission_sum_ms={:.3f} untimed_ms={:.3f} emu_ms={:.3f} guest_dispatch_ms={:.3f} "
             "emu:{} slowest:{}",
             trace_sample_frame, count, dropped, total_ms, submission_sum_ms,
             submission_sum_ms - total_ms - emu_ms, emu_ms, dispatch_ms,
             emu_rows.empty() ? " -" : emu_rows, slowest);
}

void Scheduler::TracePassBegin(vk::CommandBuffer cmdbuf, const PassTrace& pass) {
    if (!trace_gpu_recording || !trace_costs_active) {
        return;
    }
    if (trace_pass_count == TraceMaxPasses) {
        ++trace_pass_dropped;
        return;
    }
    trace_pass_open = trace_pass_count++;
    trace_pass_items[trace_pass_open] = pass;
    trace_pass_reasons[trace_pass_open] = PassEnd::Other;
    cmdbuf.WriteTimestamp(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, *trace_pass_pool,
                          trace_pass_open * 4);
}

void Scheduler::TracePassStamp(vk::CommandBuffer cmdbuf, u32 stamp) {
    if (trace_pass_open == TraceNoPass) {
        return;
    }
    cmdbuf.WriteTimestamp(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, *trace_pass_pool,
                          trace_pass_open * 4 + stamp);
}

void Scheduler::TracePassEnd(vk::CommandBuffer cmdbuf, PassEnd reason) {
    if (trace_pass_open == TraceNoPass) {
        return;
    }
    cmdbuf.WriteTimestamp(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, *trace_pass_pool,
                          trace_pass_open * 4 + 3);
    trace_pass_reasons[trace_pass_open] = reason;
    trace_pass_open = TraceNoPass;
}

void Scheduler::ReportPassTimes(VideoCore::GpuCostSnapshot& costs) {
    const u32 count = std::exchange(trace_pass_count, 0);
    const u32 dropped = std::exchange(trace_pass_dropped, 0);
    if (count == 0) {
        return;
    }
    struct Result {
        u64 timestamp;
        u64 available;
    };
    std::vector<Result> results(count * 4);
    const auto result = device.GetLogical().GetQueryResults(
        *trace_pass_pool, 0, count * 4, results.size() * sizeof(Result), results.data(),
        sizeof(Result), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    if (result != VK_SUCCESS && result != VK_NOT_READY) {
        return;
    }
    static constexpr std::array<std::string_view, static_cast<size_t>(PassEnd::Count)> end_names{
        "copy",     "wfi",    "query",   "submit",   "switch", "upload",
        "download", "buffer", "compute", "dispatch", "clear",  "other",
    };
    // The histogram keeps the documented order: other is printed last.
    using Ends = std::array<u32, static_cast<size_t>(PassEnd::Count)>;
    const auto format_ends = [](const Ends& ends) {
        std::string text;
        for (size_t reason = 0; reason < ends.size(); ++reason) {
            text += fmt::format(" {}={}", end_names[reason], ends[reason]);
        }
        return text;
    };
    struct Totals {
        double total_ms{};
        double max_ms{};
        u32 count{};
        double load_ms{};
        double store_ms{};
        double draw_ms{};
        double emu_ms{};
        Ends ends{};

        [[nodiscard]] double OtherMs() const {
            return total_ms - load_ms - store_ms - draw_ms - emu_ms;
        }
    };
    std::map<PassTrace, Totals> by_target;
    Totals all;
    u32 timed{};
    const auto delta_ms = [&](u32 begin, u32 end) {
        if (results[begin].available == 0 || results[end].available == 0) {
            return 0.0;
        }
        return static_cast<double>(Common::DkbFrameTrace::TimestampDelta(
                   results[begin].timestamp, results[end].timestamp, trace_timestamp_bits)) *
               trace_timestamp_period / 1e6;
    };
    for (u32 i = 0; i < count; ++i) {
        if (results[i * 4].available == 0 || results[i * 4 + 3].available == 0) {
            continue;
        }
        const double ms = delta_ms(i * 4, i * 4 + 3);
        const double load_ms = delta_ms(i * 4, i * 4 + 1);
        const double store_ms = delta_ms(i * 4 + 2, i * 4 + 3);
        const size_t reason =
            (std::min)(static_cast<size_t>(trace_pass_reasons[i]), end_names.size() - 1);
        for (Totals* const totals : {&by_target[trace_pass_items[i]], &all}) {
            totals->total_ms += ms;
            totals->max_ms = (std::max)(totals->max_ms, ms);
            ++totals->count;
            totals->load_ms += load_ms;
            totals->store_ms += store_ms;
            totals->draw_ms += trace_pass_draw_ms[i];
            totals->emu_ms += trace_pass_emu_ms[i];
            ++totals->ends[reason];
        }
        ++timed;
    }
    const double total_ms = all.total_ms;
    std::vector<std::pair<PassTrace, Totals>> sorted(by_target.begin(), by_target.end());
    std::ranges::sort(sorted, std::greater<>{},
                      [](const auto& entry) { return entry.second.total_ms; });
    const size_t targets = sorted.size();
    const auto format_name = [](u8 format) {
        const auto pixel_format = static_cast<VideoCore::Surface::PixelFormat>(format);
        return pixel_format == VideoCore::Surface::PixelFormat::Invalid
                   ? std::string{"-"}
                   : fmt::format("{}", pixel_format);
    };
    for (const auto& [pass, totals] : sorted) {
        if (costs.passes.size() == VideoCore::GpuCostSnapshot::MaxPasses) {
            break;
        }
        costs.passes.push_back({
            .width = pass.width,
            .height = pass.height,
            .color_format = format_name(pass.color_format),
            .num_colors = pass.num_colors,
            .depth_format = format_name(pass.depth_format),
            .samples = pass.samples,
            .passes = totals.count,
            .total_ms = totals.total_ms,
            .max_ms = totals.max_ms,
        });
    }
    if (!Common::DkbFrameTrace::Enabled()) {
        return;
    }
    if (sorted.size() > 12) {
        sorted.resize(12);
    }
    std::string top;
    for (const auto& [pass, totals] : sorted) {
        top += fmt::format(" {}x{}/{}/{}/{}/{}={:.3f}ms/{}x(max {:.3f}ms)", pass.width,
                           pass.height, format_name(pass.color_format), u32{pass.num_colors},
                           format_name(pass.depth_format), u32{pass.samples}, totals.total_ms,
                           totals.count, totals.max_ms);
    }
    // other_ms: state changes, in-pass clears and whatever else no timer covers.
    LOG_INFO(Render_Vulkan,
             "DKB gpu_pass renderer_frame={} passes={} timed={} dropped={} targets={} "
             "pass_sum_ms={:.3f} load_ms={:.3f} store_ms={:.3f} draw_ms={:.3f} emu_ms={:.3f} "
             "other_ms={:.3f} ends:{} top:{}",
             trace_sample_frame, count, timed, dropped, targets, total_ms, all.load_ms,
             all.store_ms, all.draw_ms, all.emu_ms, all.OtherMs(), format_ends(all.ends), top);
    for (size_t rank = 0; rank < sorted.size() && rank < 4; ++rank) {
        const auto& [pass, totals] = sorted[rank];
        LOG_INFO(Render_Vulkan,
                 "DKB gpu_pass_target renderer_frame={} rank={} target={}x{}/{}/{}/{}/{} "
                 "passes={} pass_ms={:.3f} max_ms={:.3f} load_ms={:.3f} store_ms={:.3f} "
                 "draw_ms={:.3f} emu_ms={:.3f} other_ms={:.3f} ends:{}",
                 trace_sample_frame, rank, pass.width, pass.height,
                 format_name(pass.color_format), u32{pass.num_colors},
                 format_name(pass.depth_format), u32{pass.samples}, totals.count,
                 totals.total_ms, totals.max_ms, totals.load_ms, totals.store_ms, totals.draw_ms,
                 totals.emu_ms, totals.OtherMs(), format_ends(totals.ends));
    }
}

void Scheduler::TraceGpuSubmission() {
    if (trace_gpu_pending) {
        master_semaphore->Refresh();
        if (!master_semaphore->IsFree(trace_gpu_tick)) {
            return;
        }
        struct Result {
            u64 timestamp;
            u64 available;
        };
        std::array<Result, TraceMaxSubmissions * 2> results{};
        const auto result = device.GetLogical().GetQueryResults(
            *trace_query_pool, 0, trace_gpu_count * 2, trace_gpu_count * 2 * sizeof(Result),
            results.data(), sizeof(Result),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        if (result == VK_NOT_READY) {
            return;
        }
        if (result != VK_SUCCESS) {
            LOG_WARNING(Render_Vulkan, "DKB gpu_timestamp disabled: query result {}",
                        static_cast<int>(result));
            trace_gpu_failed = true;
            return;
        }
        // A submission can begin before the one ahead of it ends: the shorter way round the
        // counter gives the sign.
        const auto signed_ms = [this](u64 begin, u64 end) {
            const u64 forward =
                Common::DkbFrameTrace::TimestampDelta(begin, end, trace_timestamp_bits);
            const u64 backward =
                Common::DkbFrameTrace::TimestampDelta(end, begin, trace_timestamp_bits);
            const double ticks = forward <= backward ? static_cast<double>(forward)
                                                     : -static_cast<double>(backward);
            return ticks * trace_timestamp_period / 1e6;
        };
        static constexpr std::array<double, 6> histogram_limits{0.25, 0.5, 1.0, 2.0, 5.0, 10.0};
        std::array<u32, 7> histogram{};
        // Per submission: its GPU time and the gap since the one before it ended.
        std::vector<std::pair<double, double>> times(trace_gpu_count);
        double total_ms{};
        double gap_sum_ms{};
        double gap_max_ms{};
        double overlap_sum_ms{};
        u32 gaps{};
        u32 overlaps{};
        std::array<std::pair<double, u32>, 4> slowest{};
        // Each submission's start and end, in ticks after the first one started.
        std::vector<std::pair<u64, u64>> intervals;
        intervals.reserve(trace_gpu_count);
        for (u32 i = 0; i < trace_gpu_count * 2; i += 2) {
            if (results[i].available == 0 || results[i + 1].available == 0) {
                return;
            }
            const auto ticks = Common::DkbFrameTrace::TimestampDelta(
                results[i].timestamp, results[i + 1].timestamp, trace_timestamp_bits);
            const double duration_ms = static_cast<double>(ticks) * trace_timestamp_period / 1e6;
            total_ms += duration_ms;
            if (duration_ms >= 10.0 && duration_ms > slowest.back().first) {
                slowest.back() = {duration_ms, i / 2};
                std::ranges::sort(slowest, std::greater<>{});
            }
            u32 bucket = 0;
            while (bucket < histogram_limits.size() && duration_ms >= histogram_limits[bucket]) {
                ++bucket;
            }
            ++histogram[bucket];
            const double gap_ms =
                i == 0 ? 0.0 : signed_ms(results[i - 1].timestamp, results[i].timestamp);
            if (gap_ms > 0.0) {
                gap_sum_ms += gap_ms;
                gap_max_ms = (std::max)(gap_max_ms, gap_ms);
                ++gaps;
            } else if (gap_ms < 0.0) {
                overlap_sum_ms -= gap_ms;
                ++overlaps;
            }
            times[i / 2] = {duration_ms, gap_ms};
            const u64 start = Common::DkbFrameTrace::TimestampDelta(
                results[0].timestamp, results[i].timestamp, trace_timestamp_bits);
            intervals.emplace_back(start, start + ticks);
        }
        // Busy time is the union of the submissions: idle gaps between them are not counted,
        // and overlapping submissions are counted once.
        std::ranges::sort(intervals);
        u64 busy_ticks{};
        u64 busy_start = intervals.front().first;
        u64 busy_end = intervals.front().second;
        for (const auto& [start, end] : intervals) {
            if (start > busy_end) {
                busy_ticks += busy_end - busy_start;
                busy_start = start;
            }
            busy_end = std::max(busy_end, end);
        }
        busy_ticks += busy_end - busy_start;
        const double busy_ms = static_cast<double>(busy_ticks) * trace_timestamp_period / 1e6;
        if (trace_gpu_publish && trace_gpu_skipped == 0) {
            std::scoped_lock lock{gpu_busy_mutex};
            gpu_busy = GpuBusySample{busy_ms, std::chrono::steady_clock::now()};
        }
        const double span_ms = trace_gpu_skipped == 0
            ? static_cast<double>(Common::DkbFrameTrace::TimestampDelta(
                  results[0].timestamp, results[trace_gpu_count * 2 - 1].timestamp,
                  trace_timestamp_bits)) * trace_timestamp_period / 1e6
            : -1.0;
        const u32 work_count = FetchWorkTimes();
        if (Common::DkbFrameTrace::Enabled()) {
            LOG_INFO(Render_Vulkan,
                     "DKB gpu_frame renderer_frame={} last_submission_tick={} "
                     "gpu_frame_span_ms={:.3f} gpu_submission_sum_ms={:.3f} gpu_busy_ms={:.3f} "
                     "measured_submissions={} dropped_submissions={} "
                     "valid_bits={} period_ns={}",
                     trace_sample_frame, trace_gpu_tick, span_ms, total_ms, busy_ms,
                     trace_gpu_count, trace_gpu_skipped, trace_timestamp_bits,
                     trace_timestamp_period);
        LOG_INFO(Render_Vulkan,
                 "DKB gpu_hist renderer_frame={} submissions={} lt0.25={} lt0.5={} lt1={} lt2={} "
                 "lt5={} lt10={} ge10={} idle_gap_sum_ms={:.3f} idle_gaps={} "
                 "max_idle_gap_ms={:.3f} overlap_sum_ms={:.3f} overlaps={}",
                 trace_sample_frame, trace_gpu_count, histogram[0], histogram[1], histogram[2],
                 histogram[3], histogram[4], histogram[5], histogram[6], gap_sum_ms, gaps,
                 gap_max_ms, overlap_sum_ms, overlaps);
        static const bool all_submissions = [] {
            const char* value = std::getenv("EDEN_NX2_TRACE_ALL_SUBMISSIONS");
            return value && std::strcmp(value, "1") == 0;
        }();
        for (u32 index = 0; all_submissions && index < trace_gpu_count; ++index) {
            const auto& work = trace_submissions[index];
            const u32 listed = (std::min)(work.num_shaders, u32{4});
            std::string shaders;
            for (u32 i = 0; i < listed; ++i) {
                const auto& shader = work.shaders[i];
                shaders += fmt::format("{}s{}:{:016x}", shaders.empty() ? "" : ",",
                                       shader.stage, shader.hash);
            }
            double timed_ms{};
            double emu_ms{};
            for (u32 i = work.work_begin; i < work.work_end && i < work_count; ++i) {
                if (trace_work_ms[i] > 0.0) {
                    (trace_work_items[i].kind == TraceEmuKind ? emu_ms : timed_ms) +=
                        trace_work_ms[i];
                }
            }
            LOG_INFO(Render_Vulkan,
                     "DKB gpu_sub renderer_frame={} index={} tick={} gpu_ms={:.3f} gap_ms={:.3f} "
                     "draw_attempts={} dispatch_attempts={} shaders={} more_shaders={} "
                     "timed_ms={:.3f} emu_ms={:.3f} untimed_ms={:.3f} passes={} reason={}@{}:{}",
                     trace_sample_frame, index, work.tick, times[index].first,
                     times[index].second, work.draw_attempts, work.dispatch_attempts,
                     shaders.empty() ? "-" : shaders,
                     work.num_shaders - listed + work.untracked_uses, timed_ms, emu_ms,
                     times[index].first - timed_ms - emu_ms, work.pass_end - work.pass_begin,
                     TraceFlushReason(work.flush_file, work.flush_function),
                     TraceBaseName(work.flush_file), work.flush_line);
            }
        }
        for (const auto& [duration_ms, index] : slowest) {
            if (duration_ms < 10.0 || !Common::DkbFrameTrace::Enabled()) {
                continue;
            }
            const auto& work = trace_submissions[index];
            std::string shaders;
            for (u32 i = 0; i < work.num_shaders; ++i) {
                const auto& shader = work.shaders[i];
                shaders += fmt::format("{}s{}:{:016x}", shaders.empty() ? "" : ",",
                                       shader.stage, shader.hash);
            }
            LOG_INFO(Render_Vulkan,
                     "DKB gpu_submission renderer_frame={} tick={} gpu_ms={:.3f} "
                     "draw_attempts={} dispatch_attempts={} shaders={} untracked_uses={}",
                     trace_sample_frame, work.tick, duration_ms, work.draw_attempts,
                     work.dispatch_attempts, shaders, work.untracked_uses);
        }
        VideoCore::GpuCostSnapshot costs;
        costs.frame = trace_sample_frame;
        costs.frame_span_ms = span_ms;
        costs.submission_sum_ms = total_ms;
        ReportWorkTimes(work_count, total_ms, costs);
        ReportPassTimes(costs);
        if (trace_costs_active) {
            costs.when = std::chrono::steady_clock::now();
            // Worker thread, about once a second. The governor only writes the shading-rate
            // registry, which draws read through a generation counter.
            VideoCore::AutoShadingRate::OnSnapshot(costs);
            std::scoped_lock lock{gpu_costs_mutex};
            gpu_costs = std::move(costs);
        }
        trace_gpu_tick = 0;
        trace_gpu_count = 0;
        trace_gpu_skipped = 0;
        trace_gpu_pending = false;
        trace_gpu_rate.next = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    }
    if (!trace_gpu_sampling || trace_gpu_count == TraceMaxSubmissions) {
        return;
    }
    current_upload_cmdbuf.ResetQueryPool(*trace_query_pool, trace_gpu_count * 2, 2);
    if (trace_gpu_count == 0 && trace_work_pool && trace_costs_active) {
        // Draws are timed inside render passes, where queries cannot be reset.
        current_upload_cmdbuf.ResetQueryPool(*trace_work_pool, 0, TraceMaxWork * 2);
    }
    if (trace_gpu_count == 0 && trace_pass_pool && trace_costs_active) {
        current_upload_cmdbuf.ResetQueryPool(*trace_pass_pool, 0, TraceMaxPasses * 4);
    }
    current_upload_cmdbuf.WriteTimestamp(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, *trace_query_pool,
                                        trace_gpu_count * 2);
    trace_sub_work_begin = trace_work_count;
    trace_sub_pass_begin = trace_pass_count;
    trace_upload_item = TraceNoPass;
    if (trace_work_pool && trace_costs_active && trace_work_count < TraceMaxWork) {
        // The reordered uploads run in their own command buffer, ahead of the main one.
        trace_upload_item = trace_work_count++;
        trace_work_items[trace_upload_item] = {static_cast<u64>(TraceCategory::UploadCmdbuf),
                                               0,
                                               {},
                                               TraceEmuKind,
                                               TraceNoPass,
                                               0};
        current_upload_cmdbuf.WriteTimestamp(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                            *trace_work_pool, trace_upload_item * 2);
    }
    trace_gpu_recording = true;
}

u64 Scheduler::SubmitExecution(VkSemaphore signal_semaphore, VkSemaphore wait_semaphore,
                               TraceSite site) {
    if (reorders_pending) {
        // The upload command buffer ends with this submission.
        FlushReorderedUploads();
    }
    if (uploads_pending) {
        // Keep the copies in the tick that protects their staging regions and buffer lifetimes.
        FlushUploads();
    }
    if (VideoCommon::Nx2Profile::Enabled()) {
        g_profile_submits.fetch_add(1, std::memory_order_relaxed);
        ReportSubmitCounts();
    }
    EndPendingOperations();
    InvalidateState();

    const u64 signal_value = master_semaphore->NextTick();
    RecordWithUploadBuffer([signal_semaphore, wait_semaphore, signal_value, site,
                            this](vk::CommandBuffer cmdbuf, vk::CommandBuffer upload_cmdbuf) {
        static constexpr VkMemoryBarrier WRITE_BARRIER{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        };
        upload_cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, WRITE_BARRIER);
        if (trace_gpu_recording && trace_upload_item != TraceNoPass) {
            upload_cmdbuf.WriteTimestamp(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, *trace_work_pool,
                                        trace_upload_item * 2 + 1);
        }
        upload_cmdbuf.End();
        if (trace_gpu_recording) {
            cmdbuf.WriteTimestamp(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, *trace_query_pool,
                                 trace_gpu_count * 2 + 1);
            trace_gpu_tick = signal_value;
            trace_work.tick = signal_value;
            trace_work.work_begin = trace_sub_work_begin;
            trace_work.work_end = trace_work_count;
            trace_work.pass_begin = trace_sub_pass_begin;
            trace_work.pass_end = trace_pass_count;
            trace_work.flush_file = site.file;
            trace_work.flush_function = site.function;
            trace_work.flush_line = site.line;
            trace_span_item = TraceNoPass;
            trace_upload_item = TraceNoPass;
            trace_submissions[trace_gpu_count] = trace_work;
            ++trace_gpu_count;
            trace_gpu_recording = false;
            trace_pass_open = TraceNoPass;
        } else if (trace_gpu_sampling) {
            ++trace_gpu_skipped;
        }
        if (trace_work_pool) {
            trace_work = {};
        }
        cmdbuf.End();

        if (on_submit) {
            on_submit();
        }

        std::scoped_lock lock{submit_mutex};
        switch (const VkResult result = master_semaphore->SubmitQueue(
                    cmdbuf, upload_cmdbuf, signal_semaphore, wait_semaphore, signal_value)) {
        case VK_SUCCESS:
            // Log successful queue submission
            if (GPU::Logging::IsActive() &&
                Settings::values.gpu_log_vulkan_calls.GetValue()) {
                GPU::Logging::GPULogger::GetInstance().LogVulkanCall(
                    "vkQueueSubmit", "", VK_SUCCESS);
            }
            break;
        case VK_ERROR_DEVICE_LOST:
            device.ReportLoss();
            [[fallthrough]];
        default:
            vk::Check(result);
            break;
        }
        if (trace_frame_end != 0) {
            if (trace_gpu_sampling) {
                trace_gpu_pending = true;
                trace_gpu_sampling = false;
                // A start mark while sampling means the end mark was lost: the sample spans
                // more than one frame.
                trace_gpu_publish = trace_frame_mark != FrameMark::Start;
            }
            if (trace_query_pool && !trace_gpu_failed && !trace_gpu_pending &&
                trace_frame_mark != FrameMark::End &&
                trace_gpu_rate.Ready(std::chrono::steady_clock::now())) {
                trace_sample_frame = trace_frame_end + 1;
                trace_gpu_sampling = true;
                trace_costs_active = trace_work_pool && TraceCostsWanted();
            }
            trace_frame_end = 0;
        }
    });
    chunk->MarkSubmit();
    DispatchWork();
    return signal_value;
}

void Scheduler::AllocateNewContext() {
    // Enable counters once again. These are disabled when a command buffer is finished.
}

void Scheduler::InvalidateState() {
    ++recording_epoch;
    state.graphics_pipeline = nullptr;
    state.fragment_shading_rate = {0, 0};
    state.rescaling_defined = false;
    state.descriptor_buffer_bound = false;
    state_tracker.InvalidateCommandBufferState();
}

void Scheduler::EndPendingOperations() {
    // Pause, don't reset: a submission can land between a guest's counter reset and its report,
    // and resetting here would drop the samples counted so far (the report then reads too low).
    query_cache->CounterClose(VideoCommon::QueryType::ZPassPixelCount64);
    EndRenderPass();
    // No image rests across a submission: the texture cache may free it once this one is done.
    ReleaseRestingDepth();
}

void Scheduler::EndRenderPass()
    {
        RealizeDeferredClear();
        if (!state.renderpass) {
            pass_end_reason = PassEnd::Other;
            return;
        }
        const PassEnd end_reason = std::exchange(pass_end_reason, PassEnd::Other);
        ++recording_epoch;
        fragment_barrier.Reset();
        render_pass_has_stores = false;
        ++render_pass_serial;

        query_cache->CounterClose(VideoCommon::QueryType::StreamingByteCount);

        // Log render pass end
        if (GPU::Logging::IsActive() &&
            Settings::values.gpu_log_vulkan_calls.GetValue()) {
            GPU::Logging::GPULogger::GetInstance().LogRenderPassEnd();
        }

        query_cache->CounterEnable(VideoCommon::QueryType::ZPassPixelCount64, false);
        query_cache->NotifySegment(false);

        if (trace_pass_begun) {
            Record([this](vk::CommandBuffer cmdbuf) { TracePassStamp(cmdbuf, 2); });
        }
        Record([num_images = num_renderpass_images,
                       images = renderpass_images,
                       ranges = renderpass_image_ranges,
                       depth_layout = pass_depth_final_layout,
                       has_transform_feedback = device.IsExtTransformFeedbackSupported()](
                          vk::CommandBuffer cmdbuf) {
            std::array<VkImageMemoryBarrier, 9> barriers;
            for (size_t i = 0; i < num_images; ++i) {
                const VkImageSubresourceRange& range = ranges[i];
                const bool is_color = (range.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) != 0;
                const bool is_depth_stencil = (range.aspectMask
                                              & (VK_IMAGE_ASPECT_DEPTH_BIT
                                                 | VK_IMAGE_ASPECT_STENCIL_BIT)) !=0;

                VkAccessFlags src_access = 0;

                if (is_color)
                    src_access |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                else if (is_depth_stencil)
                    src_access |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                else
                    src_access |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                                  | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

                // The pass leaves its depth attachment in depth_layout (EDEN_NX2_DEPTH_OPTIMAL).
                const VkImageLayout layout =
                    !is_color && is_depth_stencil ? depth_layout : VK_IMAGE_LAYOUT_GENERAL;
                barriers[i] = VkImageMemoryBarrier{
                        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                        .pNext = nullptr,
                        .srcAccessMask = src_access,
                        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT
                                         | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT
                                         | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                                         | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                                         | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                        .oldLayout = layout,
                        .newLayout = layout,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .image = images[i],
                        .subresourceRange = range,
                };
            }
            cmdbuf.EndRenderPass();
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, vk::PIPELINE_STAGE_GRAPHICS_COMPUTE,
                                   0, nullptr, nullptr, vk::Span(barriers.data(), num_images));
            if (has_transform_feedback) {
                static constexpr VkMemoryBarrier XFB_OUTPUT_BARRIER{
                    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                    .pNext = nullptr,
                    .srcAccessMask = VK_ACCESS_TRANSFORM_FEEDBACK_WRITE_BIT_EXT,
                    .dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                };
                cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT,
                                       VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                                       0, XFB_OUTPUT_BARRIER);
            }
        });
        if (std::exchange(trace_pass_begun, false)) {
            Record([this, end_reason](vk::CommandBuffer cmdbuf) {
                TracePassEnd(cmdbuf, end_reason);
            });
        }

        state.renderpass = VkRenderPass{};
        num_renderpass_images = 0;
        if (pass_depth_image != VK_NULL_HANDLE) {
            if (pass_depth_final_layout != VK_IMAGE_LAYOUT_GENERAL) {
                resting_depth_image = pass_depth_image;
                resting_depth_range = pass_depth_range;
            }
            pass_depth_image = VK_NULL_HANDLE;
            pass_depth_final_layout = VK_IMAGE_LAYOUT_GENERAL;
        }
    }


void Scheduler::AcquireNewChunk() {
    std::scoped_lock rl{reserve_mutex};

    if (chunk_reserve.empty()) {
        // If we don't have anything reserved, we need to make a new chunk.
        chunk = std::make_unique<CommandChunk>();
    } else {
        // Otherwise, we can just take from the reserve.
        chunk = std::move(chunk_reserve.back());
        chunk_reserve.pop_back();
    }
}

} // namespace Vulkan
