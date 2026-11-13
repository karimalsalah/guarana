// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <compare>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <queue>
#include <source_location>
#include <span>
#include <vector>

#include "common/alignment.h"
#include "common/common_types.h"
#include "common/dkb_frame_trace.h"
#include "common/logging.h"
#include "common/polyfill_thread.h"
#include "common/settings.h"
#include "video_core/gpu_costs.h"
#include "video_core/gpu_split.h"
#include "video_core/renderer_vulkan/fragment_barrier_state.h"
#include "video_core/renderer_vulkan/vk_master_semaphore.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace VideoCommon {
template <typename Trait>
class QueryCacheBase;
}

namespace Vulkan {

class CommandPool;
class Device;
class Framebuffer;
class GraphicsPipeline;
class StateTracker;
class UploadBatch;

struct QueryCacheParams;

/// Emulator-issued GPU work timed by the cost tracer (DKB gpu_work "emu:<name>" rows).
enum class TraceCategory : u8 {
    ImageCopy,
    ImageCopyMsaa,
    ImageReinterpret,
    ImageBlit,
    ImageConvert,
    ImageUpload,
    ImageDownload,
    BufferCopy,        ///< The sum of the BufferCopy* rows: nothing is timed under it
    BufferUploadBatch, ///< The sum of the BufferUploadBatch* rows: nothing is timed under it
    UploadCmdbuf, ///< The whole reordered-upload command buffer of a submission
    ComputeVertexRepack,
    ComputeUint8Index,
    ComputeQuadIndex,
    ComputeCondResolve,
    ComputeQueryPrefixScan,
    ComputeAstc,
    ComputeBcEncode,
    ComputeUnswizzle3D,
    QueryCopy,
    ClearInPass,
    Present,
    /// Buffer copies recorded on their own, by call site (VideoCommon::BufferCopySite order).
    BufferCopyOther,
    BufferCopyUpload,
    BufferCopyInline,
    BufferCopyIndex,
    BufferCopyDma,
    BufferCopyDownload,
    BufferCopyReadDownload,
    BufferCopyFenceDownload,
    BufferCopyListPage,
    BufferCopyJoin,
    BufferCopyGather,
    /// Upload batches by the call site of their copies (VideoCommon::BufferCopySite order).
    BufferUploadBatchOther,
    BufferUploadBatchUpload,
    BufferUploadBatchInline,
    BufferUploadBatchIndex,
    BufferUploadBatchDma,
    BufferUploadBatchDownload,
    BufferUploadBatchReadDownload,
    BufferUploadBatchFenceDownload,
    BufferUploadBatchListPage,
    BufferUploadBatchJoin,
    BufferUploadBatchGather,
    BufferUploadBatchMixed, ///< Copies of more than one call site
    Count,
};

/// The scheduler abstracts command buffer and fence management with an interface that's able to do
/// OpenGL-like operations on Vulkan command buffers.
class Scheduler {
public:
    explicit Scheduler(const Device& device, StateTracker& state_tracker);
    ~Scheduler();

    /// Sends the current execution context to the GPU.
    u64 Flush(VkSemaphore signal_semaphore = {}, VkSemaphore wait_semaphore = {},
              std::source_location location = std::source_location::current());

    /// Sends the current execution context to the GPU and waits for it to complete.
    void Finish(VkSemaphore signal_semaphore = {}, VkSemaphore wait_semaphore = {},
                std::source_location location = std::source_location::current());

    /// On the split GPU thread, a fence or a command list copy submits only if the last
    /// submission is older than a window (EDEN_NX2_FENCE_FLUSH_US, default 500 us, 0 submits at
    /// once); otherwise it goes with the next one. The back submits deferred work before it
    /// sleeps and when another thread asks (WaitSubmitted). True when the flush was deferred.
    bool DeferFlush();
    /// Submits deferred work (the split's back, from its idle hook).
    void FlushDeferred();
    /// Threads other than the split's back: waits until tick is submitted, asking the back to
    /// submit deferred work (logged every 2 s while it waits). False only when the back thread
    /// is not running, so nothing will submit it: the caller then submits it itself.
    bool WaitSubmitted(u64 tick);
    /// Whether everything up to tick has been handed to the worker for submission.
    [[nodiscard]] bool IsSubmitted(u64 tick) const noexcept {
        return tick < master_semaphore->CurrentTick();
    }

    /// Waits for the worker thread to finish executing everything. After this function returns it's
    /// safe to touch worker resources.
    void WaitWorker();

    /// Keeps the worker from executing chunks while the lock is held. Take it before submit_mutex.
    [[nodiscard]] std::unique_lock<std::mutex> PauseWorker() {
        return std::unique_lock{execution_mutex};
    }

    enum class FrameMark : u8 { None, Start, End, Both };
    /// GPU thread, once per present: the frame-end mark this present gets. EDEN_DKB_FRAME_TRACE
    /// and cost sampling mark every present; otherwise only the two presents after
    /// RequestGpuBusySample.
    [[nodiscard]] FrameMark NextFrameMark();
    void MarkFrameEnd(u64 frame, FrameMark mark);
    /// Any thread: asks for one host GPU timestamp sample of the frame after the next present.
    void RequestGpuBusySample() {
        gpu_busy_marks.store(2, std::memory_order_relaxed);
    }
    /// Host GPU busy time of the last sampled frame, nullopt when none was taken in 3 s.
    [[nodiscard]] std::optional<double> GpuBusyMs() const;
    /// Any thread: turns the per-draw and per-pass timing of one frame a second on or off
    /// without the DKB log lines. False when the query pools were not created at startup
    /// (EDEN_NX2_COST_SAMPLING=1, EDEN_NX2_DEBUG_SERVER or EDEN_DKB_FRAME_TRACE=1).
    bool RequestCostSampling(bool on) {
        if (!trace_work_pool) {
            return false;
        }
        if (cost_sampling.exchange(on, std::memory_order_relaxed) && !on) {
            // One more frame-end mark closes the sample in flight.
            gpu_busy_marks.store(1, std::memory_order_relaxed);
        }
        return true;
    }
    [[nodiscard]] bool CostSampling() const {
        return cost_sampling.load(std::memory_order_relaxed);
    }
    /// Costs of the last sampled frame; frame is 0 when none was taken.
    [[nodiscard]] VideoCore::GpuCostSnapshot GpuCosts() const;
    void TraceGraphicsPipeline(const std::array<u64, 6>& hashes);
    void TraceComputePipeline(u64 hash);
    /// Worker thread, inside a recorded command: timestamps one draw or dispatch of a sampled
    /// submission. A draw passes its vertex and fragment hashes, a dispatch its compute hash.
    /// kind tags other draws: m mesh, e emulated mesh, i indirect count, b indirect byte count.
    template <typename Func>
    void TraceGpuWork(vk::CommandBuffer cmdbuf, u64 hash, u64 fragment_hash,
                      std::array<u32, 3> size, Func&& work, char kind = '\0') {
        if (!trace_gpu_recording || !trace_costs_active) {
            work();
            return;
        }
        if (trace_work_count == TraceMaxWork) {
            ++trace_work_dropped;
            work();
            return;
        }
        const u32 query = trace_work_count * 2;
        trace_work_items[trace_work_count++] = {hash, fragment_hash, size, kind, trace_pass_open};
        cmdbuf.WriteTimestamp(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, *trace_work_pool, query);
        work();
        cmdbuf.WriteTimestamp(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, *trace_work_pool, query + 1);
    }

    /// Worker thread, inside a recorded command: times emulator-issued work (kind 'x').
    template <typename Func>
    void TraceEmuWork(vk::CommandBuffer cmdbuf, TraceCategory category, std::array<u32, 3> size,
                      Func&& work) {
        TraceGpuWork(cmdbuf, static_cast<u64>(category), 0, size, std::forward<Func>(work),
                     TraceEmuKind);
    }

    /// Record() whose command is timed as emulator work in sampled frames. Without the cost
    /// pools (no tracing, sampling or debug server) it is Record().
    template <typename T>
        requires std::is_invocable_v<T, vk::CommandBuffer>
    void RecordEmu(TraceCategory category, std::array<u32, 3> size, T&& c) {
        if (!trace_work_pool) {
            Record(std::move(c));
            return;
        }
        Record([this, category, size, command = std::move(c)](vk::CommandBuffer cmdbuf) {
            TraceEmuWork(cmdbuf, category, size, [&] { command(cmdbuf); });
        });
    }

    /// Times everything recorded between the two calls as one emulator work item. Spans do not
    /// nest and do not cross a submission; items timed inside one are counted in both.
    void RecordEmuBegin(TraceCategory category, std::array<u32, 3> size);
    void RecordEmuEnd();

    /// Sends currently recorded work to the worker thread.
    void DispatchWork();

    /// Requests to begin a renderpass.
    void RequestRenderpass(const Framebuffer* framebuffer,
                           std::source_location location = std::source_location::current());

    /// As RequestRenderpass, for a pass that samples nothing or a draw whose sampled images went
    /// through ReleaseDepthForSampling: another pass's depth image may stay in its attachment
    /// layout.
    void RequestDrawRenderpass(const Framebuffer* framebuffer,
                               std::source_location location = std::source_location::current());

    /// EDEN_NX2_DEPTH_OPTIMAL. 0: depth attachments stay in GENERAL. 1: single-sample depth
    /// attachments are in DEPTH_STENCIL_ATTACHMENT_OPTIMAL during their passes. 2: and stay
    /// there after the pass until something else needs the image (ReleaseRestingDepth).
    [[nodiscard]] static u32 DepthOptimalMode();

    /// Whether `image` (not null) is the active pass's depth attachment in its attachment layout
    /// or the one the last pass left there.
    [[nodiscard]] bool IsDepthInAttachmentLayout(VkImage image) const noexcept {
        return image == resting_depth_image || image == pass_depth_image;
    }

    /// Returns the depth image the last pass left in DEPTH_STENCIL_ATTACHMENT_OPTIMAL to GENERAL,
    /// ending the render pass: all the code outside render passes expects GENERAL. With `image`,
    /// only when it is that image.
    void ReleaseRestingDepth(VkImage image = VK_NULL_HANDLE);

    /// Before a draw samples `image`: ends the pass that has it as its depth attachment and
    /// returns it to GENERAL.
    void ReleaseDepthForSampling(VkImage image);

    /// Defers a full-attachment color clear so it becomes the next render pass.
    bool DeferColorClear(const Framebuffer* framebuffer, u32 rt_slot, const VkClearValue& value);

    /// Defers a full depth/stencil clear so it becomes the next render pass.
    bool DeferDepthStencilClear(const Framebuffer* framebuffer, const VkClearValue& value);

    /// Realizes any pending deferred clear before its framebuffer can be moved or freed.
    void FlushDeferredClear();

    /// Requests the current execution context to be able to execute operations only allowed outside
    /// of a renderpass.
    void RequestOutsideRenderPassOperationContext(
        std::source_location location = std::source_location::current());

    /// The same for work that reads and writes no image.
    void RequestOutsideRenderPassBufferContext(
        std::source_location location = std::source_location::current());

    /// The same for work whose only image is `image`.
    void RequestOutsideRenderPassImageContext(
        VkImage image, std::source_location location = std::source_location::current());

    void DeferFragmentBarrier() {
        fragment_barrier.Defer(state.renderpass != VK_NULL_HANDLE);
    }

    bool HasFragmentBarrier() const {
        return fragment_barrier.Pending();
    }

    void NoteRenderPassStores() {
        render_pass_has_stores = true;
    }

    bool RenderPassHasStores() const {
        return render_pass_has_stores;
    }

    /// A dispatch was recorded: the next render pass must see what it wrote.
    void NoteComputeWrites() {
        compute_writes_pending = true;
    }

    /// Returns true when a render pass is currently active in the scheduler state.
    bool IsRenderPassActive() const {
        return state.renderpass != VK_NULL_HANDLE;
    }

    /// Changes whenever a render pass begins or ends.
    u64 RenderPassSerial() const {
        return render_pass_serial;
    }

    /// The graphics pipeline bound in the current command buffer, if the scheduler knows it.
    const GraphicsPipeline* BoundGraphicsPipeline() const {
        return state.graphics_pipeline;
    }

    /// Update the pipeline to the current execution context.
    bool UpdateGraphicsPipeline(GraphicsPipeline* pipeline);

    /// A recorded draw bound other pipelines than the one UpdateGraphicsPipeline was told of
    /// (alpha-test prepass split): the next draw binds its pipeline again.
    void ForgetGraphicsPipeline() {
        state.graphics_pipeline = nullptr;
    }

    /// Worker thread only: whether the bound graphics pipeline failed to build, so the draws
    /// recorded after it must be dropped instead of reaching the driver without a pipeline.
    void SetGraphicsPipelineMissing(bool missing) {
        graphics_pipeline_missing = missing;
    }
    [[nodiscard]] bool GraphicsPipelineMissing() const {
        return graphics_pipeline_missing;
    }

    /// Returns true when the fragment shading rate has to be set in the command buffer.
    bool UpdateFragmentShadingRate(VkExtent2D rate) {
        if (state.fragment_shading_rate.width == rate.width &&
            state.fragment_shading_rate.height == rate.height) {
            return false;
        }
        state.fragment_shading_rate = rate;
        return true;
    }

    /// Update the rescaling state. Returns true if the state has to be updated.
    bool UpdateRescaling(bool is_rescaling, bool mesh_layout);

    /// Returns true when the descriptor buffer chunk has to be bound into the command buffer.
    bool UpdateDescriptorBufferChunk(u32 descriptor_chunk);

    /// Invalidates current command buffer state except for render passes
    void InvalidateState();

    /// Assigns the query cache.
    void SetQueryCache(VideoCommon::QueryCacheBase<QueryCacheParams>& query_cache_) {
        query_cache = &query_cache_;
    }

    // Registers a callback to perform on queue submission.
    void RegisterOnSubmit(std::function<void()>&& func) {
        on_submit = std::move(func);
    }

    /// Consecutive in-order staging uploads share barriers until their destinations overlap,
    /// a source depends on a pending copy, or the batch is full. Call outside a render pass.
    /// A batch is timed under the category of its copies (BufferUploadBatchMixed when they
    /// differ).
    void RecordUpload(VkBuffer src, VkBuffer dst, std::span<const VkBufferCopy> copies,
                      TraceCategory category = TraceCategory::BufferUploadBatchOther);

    /// EDEN_NX2_UMA bit 1. Uploads that go in the upload command buffer, ahead of everything
    /// recorded in this submission: they wait for the submission, or for an upload that writes
    /// the same bytes, and are recorded as one vkCmdCopyBuffer per destination.
    void RecordReorderedUpload(VkBuffer src, VkBuffer dst, std::span<const VkBufferCopy> copies);

    /// Send work to a separate thread.
    template <typename T>
        requires std::is_invocable_v<T, vk::CommandBuffer, vk::CommandBuffer>
    void RecordWithUploadBuffer(T&& command) {
        if (VideoCommon::GpuSplit::check_owner) [[unlikely]] {
            VideoCommon::GpuSplit::CheckOwner("Scheduler::Record");
        }
        if (uploads_pending) {
            // Pending uploads precede every other recorded command, including upload-buffer work.
            FlushUploads();
        }
        ++record_serial;
        if (chunk->Record(command)) {
            return;
        }
        DispatchWork();
        (void)chunk->Record(command);
    }

    template <typename T>
        requires std::is_invocable_v<T, vk::CommandBuffer>
    void Record(T&& c) {
        this->RecordWithUploadBuffer(
            [command = std::move(c)](vk::CommandBuffer cmdbuf, vk::CommandBuffer) {
                command(cmdbuf);
            });
    }

    /// Whether anything was recorded since the last Flush or Finish, or waits to be: batched
    /// uploads and a deferred clear reach the command buffer only at the next record or submit.
    [[nodiscard]] bool RecordedSinceSubmit() const noexcept {
        return record_serial != submitted_serial || uploads_pending || reorders_pending ||
               deferred_clear.framebuffer != nullptr;
    }
    /// Counts a flush skipped because nothing was recorded (GPU profile).
    static void CountSkippedFlush();

    /// Whether the recording thread submitted `tick` within 20 ms.
    [[nodiscard]] bool WaitForSubmission(u64 tick) const {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{20};
        while (tick >= master_semaphore->CurrentTick()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::microseconds{100});
        }
        return true;
    }

    /// Returns the current command buffer tick.
    [[nodiscard]] u64 CurrentTick() const noexcept {
        return master_semaphore->CurrentTick();
    }

    /// Changes whenever recorded command buffer state may have been reset (InvalidateState, the
    /// end of a render pass), so a binding recorded at the same epoch is still bound.
    [[nodiscard]] u64 RecordingEpoch() const noexcept {
        return recording_epoch;
    }

    /// Returns true when a tick has been triggered by the GPU.
    [[nodiscard]] bool IsFree(u64 tick) const noexcept {
        return master_semaphore->IsFree(tick);
    }

    /// Waits for the given GPU tick, optionally pacing frames. Threads other than the one that
    /// records (the async present thread) pass may_flush = false: flushing from there records
    /// into the current chunk while that thread does (corrupt command buffers, crashes). They
    /// wait for the recording thread to submit the tick instead, and give up after 20 ms (it may
    /// be waiting for them).
    void Wait(u64 tick, double target_fps = 0.0, bool may_flush = true) {
        const bool trace = Common::DkbFrameTrace::Enabled() && target_fps > 0.0;
        const auto trace_start = trace ? std::chrono::steady_clock::now()
                                       : std::chrono::steady_clock::time_point{};
        if (tick > 0 && (may_flush || WaitForSubmission(tick))) {
            if (tick >= master_semaphore->CurrentTick()) {
                Flush();
            }
            master_semaphore->Wait(tick);
        }
        const auto trace_waited = trace ? std::chrono::steady_clock::now()
                                        : std::chrono::steady_clock::time_point{};
        double trace_requested_ms{};
        double trace_sleep_ms{};
        double trace_slept_ms{};
        if (Settings::values.use_speed_limit.GetValue() && target_fps > 0.0) {
            auto now = std::chrono::steady_clock::now();
            if (last_target_fps != target_fps) {
                frame_interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(1.0 / target_fps));
                max_frame_count = static_cast<int>(0.1 * target_fps);
                last_target_fps = target_fps;
                frame_counter = 0;
                start_time = now;
            }
            frame_counter++;
            auto target_time = start_time + frame_interval * frame_counter;
            if (target_time >= now) {
                constexpr auto spin_tail = std::chrono::milliseconds(1);
                auto sleep_time = target_time - now;
                if (trace) {
                    trace_requested_ms =
                        std::chrono::duration<double, std::milli>(sleep_time).count();
                }
                if (sleep_time > spin_tail * 2) {
                    if (trace) {
                        trace_sleep_ms =
                            std::chrono::duration<double, std::milli>(sleep_time - spin_tail).count();
                    }
                    const auto sleep_start = trace ? std::chrono::steady_clock::now()
                                                   : std::chrono::steady_clock::time_point{};
                    std::this_thread::sleep_for(sleep_time - spin_tail);
                    if (trace) {
                        trace_slept_ms = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - sleep_start).count();
                    }
                }
                while (std::chrono::steady_clock::now() < target_time) {
                    std::this_thread::yield();
                }
            } else if (frame_counter > max_frame_count) {
                frame_counter = 0;
                start_time = now;
            }
        }
        if (trace) {
            static thread_local Common::DkbFrameTrace::RateLimit rate;
            const auto end = std::chrono::steady_clock::now();
            if (rate.Ready(end)) {
                LOG_INFO(Render_Vulkan,
                         "DKB host_pace target_fps={} enabled={} may_flush={} "
                         "cpu_wait_wall_ms={:.3f} pace_requested_ms={:.3f} "
                         "sleep_requested_ms={:.3f} sleep_actual_wall_ms={:.3f} "
                         "pace_actual_wall_ms={:.3f}",
                         target_fps, Settings::values.use_speed_limit.GetValue(), may_flush,
                         std::chrono::duration<double, std::milli>(trace_waited - trace_start).count(),
                         trace_requested_ms, trace_sleep_ms, trace_slept_ms,
                         std::chrono::duration<double, std::milli>(end - trace_waited).count());
            }
        }
    }

    /// Returns the master timeline semaphore.
    [[nodiscard]] MasterSemaphore& GetMasterSemaphore() const noexcept {
        return *master_semaphore;
    }

    std::mutex submit_mutex;

private:
    std::chrono::steady_clock::time_point last_flush{};
    bool flush_pending{};
    u64 record_serial{};    ///< Commands recorded
    u64 submitted_serial{}; ///< record_serial at the last Flush or Finish

    class Command {
    public:
        virtual ~Command() = default;

        virtual void Execute(vk::CommandBuffer cmdbuf, vk::CommandBuffer upload_cmdbuf) const = 0;

        Command* GetNext() const {
            return next;
        }

        void SetNext(Command* next_) {
            next = next_;
        }

    private:
        Command* next = nullptr;
    };

    template <typename T>
    class TypedCommand final : public Command {
    public:
        explicit TypedCommand(T&& command_) : command{std::move(command_)} {}
        ~TypedCommand() override = default;

        TypedCommand(TypedCommand&&) = delete;
        TypedCommand& operator=(TypedCommand&&) = delete;

        void Execute(vk::CommandBuffer cmdbuf, vk::CommandBuffer upload_cmdbuf) const override {

            command(cmdbuf, upload_cmdbuf);
        }

    private:
        T command;
    };

    class CommandChunk final {
    public:
        void ExecuteAll(vk::CommandBuffer cmdbuf, vk::CommandBuffer upload_cmdbuf);

        template <typename T>
        bool Record(T& command) {
            using FuncType = TypedCommand<T>;
            static_assert(sizeof(FuncType) < sizeof(data), "Lambda is too large");

            command_offset = Common::AlignUp(command_offset, alignof(FuncType));
            if (command_offset > sizeof(data) - sizeof(FuncType)) {
                return false;
            }
            Command* const current_last = last;
            last = new (data.data() + command_offset) FuncType(std::move(command));

            if (current_last) {
                current_last->SetNext(last);
            } else {
                first = last;
            }
            command_offset += sizeof(FuncType);
            return true;
        }

        void MarkSubmit() {
            submit = true;
        }

        bool Empty() const {
            return command_offset == 0;
        }

        bool HasSubmit() const {
            return submit;
        }

    private:
        Command* first = nullptr;
        Command* last = nullptr;

        size_t command_offset = 0;
        bool submit = false;
        alignas(std::max_align_t) std::array<u8, 0x8000> data{};
    };

    struct State {
        VkRenderPass renderpass{};
        VkFramebuffer framebuffer{};
        VkExtent2D render_area = {0, 0};
        GraphicsPipeline* graphics_pipeline = nullptr;
        VkExtent2D fragment_shading_rate = {0, 0}; ///< {0, 0}: not set in this command buffer
        bool is_rescaling = false;
        bool rescaling_defined = false;
        bool mesh_layout = false;
        bool needs_state_enable_refresh = false;
        u32 descriptor_buffer_chunk = 0;
        bool descriptor_buffer_bound = false;
    };

    struct DeferredClear {
        const Framebuffer* framebuffer = nullptr;
        u32 color_clear_mask = 0;
        std::array<VkClearValue, 8> color_values{};
        bool depth_stencil = false;
        VkClearValue depth_stencil_value{};
    };

    /// Begins a render pass for the given framebuffer, optionally with clear values.
    void BeginRenderPassImpl(const Framebuffer* framebuffer, VkRenderPass renderpass,
                             const VkClearValue* clear_values, u32 clear_value_count);

    /// If a deferred clear is pending.
    void RealizeDeferredClear();

    void WorkerThread(std::stop_token stop_token);

    void AllocateWorkerCommandBuffer();

    void TraceGpuSubmission();
    /// Reads the work timestamps into trace_work_ms and trace_pass_*_ms; the item count.
    u32 FetchWorkTimes();
    void ReportWorkTimes(u32 count, double submission_sum_ms, VideoCore::GpuCostSnapshot& costs);
    void ReportPassTimes(VideoCore::GpuCostSnapshot& costs);
    /// Recording thread: whether draws and passes are to be timed in sampled frames.
    [[nodiscard]] bool TraceCostsWanted() const {
        return Common::DkbFrameTrace::Enabled() || cost_sampling.load(std::memory_order_relaxed);
    }
    void TraceShader(u64 hash, u32 stage);

    /// Where a flush or a render pass end was asked from (static strings; null when unknown).
    struct TraceSite {
        const char* file{};
        const char* function{};
        u32 line{};
    };
    static TraceSite MakeTraceSite(const std::source_location& location) {
        return {location.file_name(), location.function_name(), location.line()};
    }
    enum class PassEnd : u8 {
        Copy,
        Wfi,
        Query,
        Submit,
        Switch,
        Upload,
        Download,
        Buffer,
        Compute,
        Dispatch,
        Clear,
        Other,
        Count,
    };
    static PassEnd ClassifyPassEnd(const TraceSite& site, bool render_pass_request);

    u64 SubmitExecution(VkSemaphore signal_semaphore, VkSemaphore wait_semaphore,
                        TraceSite site);

    void AllocateNewContext();

    void EndPendingOperations();

    void EndRenderPass();

    /// Ends the render pass for work asked from `location`.
    void EndRenderPassForOutsideWork(const std::source_location& location);

    /// Picks RenderPassKey::depth_layout for the pass about to begin on `framebuffer` (none is
    /// active) and returns the resting depth image to GENERAL when that pass can't take it over.
    [[nodiscard]] u8 PickDepthLayout(const Framebuffer* framebuffer);

    /// The active pass's depth attachment when it is not in GENERAL, and its layout afterwards.
    VkImage pass_depth_image{};
    VkImageSubresourceRange pass_depth_range{};
    VkImageLayout pass_depth_final_layout = VK_IMAGE_LAYOUT_GENERAL;
    /// The depth image the last pass left in DEPTH_STENCIL_ATTACHMENT_OPTIMAL (mode 2). Never set
    /// while a pass with a pass_depth_image is active.
    VkImage resting_depth_image{};
    VkImageSubresourceRange resting_depth_range{};
    bool keep_resting_depth{};

    void AcquireNewChunk();

    const Device& device;
    StateTracker& state_tracker;

    std::unique_ptr<MasterSemaphore> master_semaphore;
    FragmentBarrierState fragment_barrier;
    bool render_pass_has_stores{};
    bool compute_writes_pending{};
    bool graphics_pipeline_missing{};
    u64 render_pass_serial{};
    std::unique_ptr<CommandPool> command_pool;

    std::unique_ptr<UploadBatch> upload_batch;
    bool uploads_pending{};
    TraceCategory upload_batch_category{TraceCategory::BufferUploadBatchOther};
    /// Records pending uploads before another command, worker handoff, or submission tick.
    void FlushUploads();

    std::unique_ptr<UploadBatch> reorder_batch; ///< RecordReorderedUpload's pending copies
    bool reorders_pending{};
    /// Records the pending reordered uploads in the upload command buffer.
    void FlushReorderedUploads();

    vk::QueryPool trace_query_pool;
    static constexpr u32 TraceMaxSubmissions = 512;
    struct SubmissionTrace {
        struct ShaderUse {
            u64 hash{};
            u32 stage{};
        };
        u64 tick{};
        u32 draw_attempts{};
        u32 dispatch_attempts{};
        u32 num_shaders{};
        u32 untracked_uses{};
        std::array<ShaderUse, 32> shaders{};
        /// Work and pass items recorded in this submission: [begin, end).
        u32 work_begin{};
        u32 work_end{};
        u32 pass_begin{};
        u32 pass_end{};
        const char* flush_file{};
        const char* flush_function{};
        u32 flush_line{};
    };
    SubmissionTrace trace_work;
    std::vector<SubmissionTrace> trace_submissions;
    static constexpr u32 TraceMaxWork = 8192;
    static constexpr u32 TraceMaxPasses = 2048;
    static constexpr u32 TraceNoPass = ~u32{0};
    static constexpr char TraceEmuKind = 'x'; ///< WorkTrace::kind of emulator work
    struct WorkTrace {
        u64 hash{}; ///< A TraceCategory for emulator work
        u64 fragment_hash{};
        /// Dispatch grid, or vertex and instance counts of a draw; zero when indirect.
        std::array<u32, 3> size{};
        char kind{};
        u32 pass{TraceNoPass}; ///< The traced render pass it ran in
        u32 span_end{};        ///< RecordEmuBegin items: the item count at RecordEmuEnd
    };
    vk::QueryPool trace_work_pool;
    std::vector<WorkTrace> trace_work_items;
    std::vector<double> trace_work_ms; ///< Per item, negative when not measured
    u32 trace_work_count{};
    u32 trace_work_dropped{};
    u32 trace_span_item{TraceNoPass};   ///< Worker: the open RecordEmuBegin item
    u32 trace_upload_item{TraceNoPass}; ///< Worker: this submission's UploadCmdbuf item
    u32 trace_sub_work_begin{};         ///< Worker: work and pass counts when this
    u32 trace_sub_pass_begin{};         ///< submission started
    /// What DKB gpu_pass groups render passes by. Formats are PixelFormat values.
    struct PassTrace {
        auto operator<=>(const PassTrace&) const = default;

        u32 width{};
        u32 height{};
        u8 color_format{}; ///< First color target's, Invalid without one
        u8 num_colors{};
        u8 depth_format{};
        u8 samples{};
    };
    /// Worker thread: timestamps around a render pass of a sampled submission. Four per pass:
    /// before and after vkCmdBeginRenderPass, before vkCmdEndRenderPass and after its barrier.
    void TracePassBegin(vk::CommandBuffer cmdbuf, const PassTrace& pass);
    void TracePassStamp(vk::CommandBuffer cmdbuf, u32 stamp);
    void TracePassEnd(vk::CommandBuffer cmdbuf, PassEnd reason);
    vk::QueryPool trace_pass_pool;
    std::vector<PassTrace> trace_pass_items;
    std::vector<PassEnd> trace_pass_reasons;
    std::vector<double> trace_pass_draw_ms; ///< Guest work timed inside each pass
    std::vector<double> trace_pass_emu_ms;  ///< Emulator work timed inside each pass
    /// Recording thread: why the open traced pass is about to end (set by the caller of
    /// EndRenderPass, Other otherwise).
    PassEnd pass_end_reason{PassEnd::Other};
    u32 trace_pass_count{};
    u32 trace_pass_dropped{};
    u32 trace_pass_open{TraceNoPass};
    bool trace_pass_begun{}; ///< Recording thread: the open render pass recorded TracePassBegin
    Common::DkbFrameTrace::RateLimit trace_gpu_rate;
    u64 trace_gpu_tick{};
    u64 trace_frame_end{};
    u64 trace_sample_frame{};
    u32 trace_gpu_count{};
    u64 trace_gpu_skipped{};
    u32 trace_timestamp_bits{};
    float trace_timestamp_period{};
    bool trace_gpu_recording{};
    bool trace_gpu_sampling{};
    bool trace_gpu_pending{};
    bool trace_gpu_failed{};
    FrameMark trace_frame_mark{};
    /// The pending sample covers exactly one frame, so its busy time can be shown.
    bool trace_gpu_publish{};
    /// Worker thread: the sample in flight times draws and passes (latched when it starts).
    bool trace_costs_active{};

    std::atomic<bool> cost_sampling{};
    mutable std::mutex gpu_costs_mutex;
    VideoCore::GpuCostSnapshot gpu_costs;

    std::atomic<u32> gpu_busy_marks{};
    struct GpuBusySample {
        double ms;
        std::chrono::steady_clock::time_point taken;
    };
    mutable std::mutex gpu_busy_mutex;
    std::optional<GpuBusySample> gpu_busy;

    VideoCommon::QueryCacheBase<QueryCacheParams>* query_cache = nullptr;

    vk::CommandBuffer current_cmdbuf;
    vk::CommandBuffer current_upload_cmdbuf;

    DeferredClear deferred_clear;

    std::unique_ptr<CommandChunk> chunk;
    std::function<void()> on_submit;

    State state;
    u64 recording_epoch = 0;

    u32 num_renderpass_images = 0;
    std::array<VkImage, 9> renderpass_images{};
    std::array<VkImageSubresourceRange, 9> renderpass_image_ranges{};

    std::queue<std::unique_ptr<CommandChunk>> work_queue;
    std::vector<std::unique_ptr<CommandChunk>> chunk_reserve;
    std::mutex execution_mutex;
    std::mutex reserve_mutex;
    std::mutex queue_mutex;
    std::condition_variable_any event_cv;
    bool worker_waiting{}; ///< The worker is waiting for work (guarded by queue_mutex)
    std::jthread worker_thread;

    std::chrono::steady_clock::duration frame_interval{};
    std::chrono::steady_clock::time_point start_time{};
    double last_target_fps{};
    u64 max_frame_count{};
    u64 frame_counter{};
};

} // namespace Vulkan
