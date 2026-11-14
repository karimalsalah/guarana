// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#ifdef _WIN32
#include <windows.h>
#endif
#include <cmath>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include "common/display_hdr.h"
#include "common/dkb_frame_trace.h"
#include "common/logging.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "common/thread.h"
#include "common/vblank_clock.h"
#ifdef _WIN32
#include "common/windows/timer_resolution.h"
#endif
#include "core/frontend/emu_window.h"
#include "video_core/autopilot_feed.h"
#include "video_core/nx2_profile.h"
#ifdef HAS_LSFG
#include "video_core/renderer_vulkan/present/lsfg_common.h"
#endif
#include "video_core/renderer_vulkan/fluid_slot_math.h"
#include "video_core/renderer_vulkan/vk_present_manager.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_surface.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"
#ifdef EGLANTINE_STREAMLINE
#include "video_core/vulkan_common/streamline.h"
#endif

namespace Vulkan {


namespace {

constexpr size_t MAX_FRAMES_IN_FLIGHT = 7;
/// EDEN_NX2_FLUID at a shown rate over 75: a real frame and a prediction for each slot until the
/// next one (six at 120 for 20 real frames), while the last real frame's predictions still wait
/// for their slots. Only extrapolated frames use the frames past MAX_FRAMES_IN_FLIGHT; Lossless
/// Scaling's generation targets stop there (FrameGen::GenerateInto).
constexpr size_t FLUID_HIGH_RATE_FRAMES = 16;
constexpr size_t FLUID_MAX_FRAMES = 16;

/// The rate EDEN_NX2_FLUID shows frames at: EDEN_NX2_SHOWN_RATE=<hz> (30 to 240) if set, else the
/// frame generation target rate, else 60.
[[nodiscard]] f64 FluidShownRate() {
    if (const char* value = std::getenv("EDEN_NX2_SHOWN_RATE")) {
        return static_cast<f64>(std::clamp<long>(std::strtol(value, nullptr, 10), 30L, 240L));
    }
    const u32 rate = Settings::values.frame_gen_target_rate.GetValue();
    return rate != 0 ? static_cast<f64>(rate) : 60.0;
}

/// EDEN_NX2_FLUID_FRAMES=<n> (7 to 16): the FLUID presenter's frame pool, by default 7, or 12 when
/// frames are shown at over 75 a second.
[[nodiscard]] size_t FluidFrameCount() {
    if (const char* value = std::getenv("EDEN_NX2_FLUID_FRAMES")) {
        return std::clamp<size_t>(std::strtoul(value, nullptr, 10), MAX_FRAMES_IN_FLIGHT,
                                  FLUID_MAX_FRAMES);
    }
    return FluidShownRate() > 75.0 ? FLUID_HIGH_RATE_FRAMES : MAX_FRAMES_IN_FLIGHT;
}
constexpr u32 MAX_PRESENT_ATTEMPTS = 3;
#ifdef HAS_LSFG
static_assert(MAX_FRAMES_IN_FLIGHT <= LSFG_MAX_TARGETS);
#endif

// Sleeps until the deadline, without spinning: it may pass by up to a timer tick (about 0.5 ms
// on Windows), well within a refresh, and the next deadline counts from the actual present.
void SleepUntil(std::chrono::steady_clock::time_point deadline) {
    while (true) {
        const auto left = deadline - std::chrono::steady_clock::now();
        if (left <= std::chrono::steady_clock::duration::zero()) {
            return;
        }
#ifdef _WIN32
        Common::Windows::SleepForOneTick();
#else
        std::this_thread::sleep_for(left);
#endif
    }
}

/// EDEN_NX2_FLUID: a slot's frame is presented this long after the vblank before the slot, so a
/// present a few ms early or late still lands on the slot's vblank. EDEN_NX2_FLUID_OFFSET_US=<us>
/// (500 to 8000) moves it: with the DXGI vblank wait the wake already follows the vblank, so about
/// 1000 gives the screen copy 7.3 ms before its flip at 120 Hz instead of 4.8.
[[nodiscard]] std::chrono::nanoseconds FluidPresentOffsetFor() {
    static const std::chrono::nanoseconds offset = [] {
        const char* value = std::getenv("EDEN_NX2_FLUID_OFFSET_US");
        if (value == nullptr) {
            return std::chrono::nanoseconds{3'500'000};
        }
        const long us = std::clamp(std::strtol(value, nullptr, 10), 500L, 8000L);
        LOG_INFO(Render_Vulkan, "EDEN_NX2_FLUID_OFFSET_US={}: slot frames go out {} us after the vblank before them", us, us);
        return std::chrono::nanoseconds{static_cast<s64>(us) * 1000};
    }();
    return offset;
}

/// EDEN_NX2_FLUID_QUEUE_HOST_WAIT=1: with the screen copy on its own queue (EDEN_NX2_FLUID_QUEUE
/// 3 or 5), the present thread waits on the copy's fence before presenting on the graphics queue,
/// so the present's wait on the copy's semaphore is already met and cannot hold the graphics
/// queue's later work.
[[nodiscard]] bool FluidQueueHostWait() {
    static const bool on = [] {
        const char* value = std::getenv("EDEN_NX2_FLUID_QUEUE_HOST_WAIT");
        return value != nullptr && std::strtoul(value, nullptr, 10) != 0;
    }();
    return on;
}

/// EDEN_NX2_FLUID_COPY_AHEAD=1: with the screen copy on its own queue, each slot's frame is copied
/// at the wake one slot before and presented at its own slot's wake, so the copy has about a slot
/// and a half before its flip instead of half a slot (4.8 ms at 120 Hz), at one slot of latency.
[[nodiscard]] bool FluidCopyAhead() {
    static const bool on = [] {
        const char* value = std::getenv("EDEN_NX2_FLUID_COPY_AHEAD");
        return value != nullptr && std::strtoul(value, nullptr, 10) != 0;
    }();
    return on;
}

/// EDEN_NX2_FLUID_HOST_WAIT_US=<us> (500 to 16000): how long that wait lasts at most, by default
/// 12000 at 60 shown frames a second and 4000 above 75 (at 120 a slot is 8.3 ms: a longer wait makes
/// the present thread sleep through the next slot). FluidSlots::CopyWaitNs cuts it to end before
/// the next slot's wake at other refresh rates too.
[[nodiscard]] u64 FluidHostWaitNs(f64 shown_rate) {
    static const u64 limit = [] {
        const char* value = std::getenv("EDEN_NX2_FLUID_HOST_WAIT_US");
        return value == nullptr
                   ? u64{0}
                   : static_cast<u64>(std::clamp(std::strtol(value, nullptr, 10), 500L, 16000L)) *
                         1000;
    }();
    if (limit != 0) {
        return limit;
    }
    return shown_rate > 75.0 ? 4'000'000 : 12'000'000;
}
/// Real frames whose GPU work is watched at once.
constexpr size_t FluidWatches = 8;
/// The watchdog: frames came and none was shown for this long.
constexpr std::chrono::seconds FluidStallLimit{2};
/// A frame that comes this long after the last, with none held, starts the watchdog's count again.
constexpr std::chrono::milliseconds FluidArrivalGap{500};
/// With the dedicated queue: no frame at all this long after the presenter started (a game takes
/// some 10 to 30 s to boot before the first) gives the queue up.
constexpr std::chrono::seconds FluidFirstFrameLimit{60};

[[nodiscard]] s64 CeilDiv(s64 value, s64 divisor) {
    return value >= 0 ? (value + divisor - 1) / divisor : -((-value) / divisor);
}

[[nodiscard]] f64 HertzOf(std::chrono::nanoseconds period) {
    return period.count() > 0 ? 1.0e9 / static_cast<f64>(period.count()) : 0.0;
}

[[nodiscard]] bool FrameDone(const Frame* frame) {
    return frame->present_done.GetStatus() == VK_SUCCESS;
}

/// A frame for a slot, as opposed to one shown in arrival order or not at all.
[[nodiscard]] bool FluidScheduled(const Frame* frame) {
    return frame->fluid_slot != 0 && frame->fluid_slot != FLUID_NOT_SHOWN;
}

/// The slot a frame's slot falls in when slots are every divisor vblanks.
[[nodiscard]] u64 FluidSlotOf(const Frame* frame, u64 divisor) {
    return (frame->fluid_slot + divisor - 1) / divisor * divisor;
}

[[nodiscard]] std::string FluidSlotText(const Frame* frame) {
    if (frame->fluid_slot == 0) {
        return "no slot (arrival order)";
    }
    if (frame->fluid_slot == FLUID_NOT_SHOWN) {
        return "only its predictions shown";
    }
    return fmt::format("slot {}", frame->fluid_slot);
}

void ResetFrame(Frame* frame) {
    frame->present_done.Reset();
    frame->color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    frame->present_spacing = {};
    frame->fluid_slot = 0;
    frame->fluid_batch = 0;
    frame->fluid_final = false;
    frame->fluid_recorded = {};
    frame->gpu_tick = 0;
    frame->shown_kind = VideoCore::Nx2ShownFrames::Kind::Real;
#ifdef EGLANTINE_STREAMLINE
    frame->frame_gen = false;
#endif
}

bool CanStoreToFrame(const vk::PhysicalDevice& physical_device, VkFormat format) {
#ifdef HAS_LSFG
    const VkFormatProperties props{physical_device.GetFormatProperties(format)};
    return (props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
#else
    return false;
#endif
}

bool CanBlitToSwapchain(const vk::PhysicalDevice& physical_device, VkFormat format) {
    const VkFormatProperties props{physical_device.GetFormatProperties(format)};
    return (props.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT);
}

[[nodiscard]] VkImageSubresourceLayers MakeImageSubresourceLayers() {
    return VkImageSubresourceLayers{
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .mipLevel = 0,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
}

[[nodiscard]] VkImageBlit MakeImageBlit(s32 frame_width, s32 frame_height, s32 swapchain_width,
                                        s32 swapchain_height) {
    return VkImageBlit{
        .srcSubresource = MakeImageSubresourceLayers(),
        .srcOffsets =
            {
                {
                    .x = 0,
                    .y = 0,
                    .z = 0,
                },
                {
                    .x = frame_width,
                    .y = frame_height,
                    .z = 1,
                },
            },
        .dstSubresource = MakeImageSubresourceLayers(),
        .dstOffsets =
            {
                {
                    .x = 0,
                    .y = 0,
                    .z = 0,
                },
                {
                    .x = swapchain_width,
                    .y = swapchain_height,
                    .z = 1,
                },
            },
    };
}

[[nodiscard]] VkImageCopy MakeImageCopy(u32 frame_width, u32 frame_height, u32 swapchain_width,
                                        u32 swapchain_height) {
    return VkImageCopy{
        .srcSubresource = MakeImageSubresourceLayers(),
        .srcOffset =
            {
                .x = 0,
                .y = 0,
                .z = 0,
            },
        .dstSubresource = MakeImageSubresourceLayers(),
        .dstOffset =
            {
                .x = 0,
                .y = 0,
                .z = 0,
            },
        .extent =
            {
                .width = (std::min)(frame_width, swapchain_width),
                .height = (std::min)(frame_height, swapchain_height),
                .depth = 1,
            },
    };
}

} // Anonymous namespace

u32 FluidMode() {
    static const u32 mode = [] {
        const char* value = std::getenv("EDEN_NX2_FLUID");
        const u32 parsed = value ? static_cast<u32>(std::strtoul(value, nullptr, 10)) : 0U;
        return parsed == 0 ? 0U : (parsed == 2 ? 2U : 1U);
    }();
    return mode;
}

PresentManager::PresentManager(const vk::Instance& instance_,
                               Core::Frontend::EmuWindow& render_window_,
                               const Device& device_,
                               MemoryAllocator& memory_allocator_,
                               Scheduler& scheduler_,
                               Swapchain& swapchain_,
                               vk::SurfaceKHR& surface_)
    : instance{instance_}
    , render_window{render_window_}
    , device{device_}
    , memory_allocator{memory_allocator_}
    , scheduler{scheduler_}
    , swapchain{swapchain_}
    , surface{surface_}
    , storage_supported{CanStoreToFrame(device.GetPhysical(), swapchain.GetImageFormat())}
    // Frame generation needs the present thread to space generated frames without stalling the
    // emulator.
    , use_present_thread{Settings::values.async_presentation.GetValue() ||
                         Settings::values.frame_gen.GetValue()}
    , fluid_presenter{FluidMode() != 0}
    , shown_capture{device_, memory_allocator_, scheduler_}
{
    SetImageCount();

    auto& dld = device.GetLogical();
    cmdpool = dld.CreateCommandPool({
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags =
            VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = device.GetGraphicsFamily(),
    });
    auto cmdbuffers = cmdpool.Allocate(image_count);
    vk::CommandBuffers fluid_cmdbuffers;
    if (device.HasFluidQueue()) {
        fluid_cmdpool = dld.CreateCommandPool({
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
                     VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = device.GetFluidFamily(),
        });
        fluid_cmdbuffers = fluid_cmdpool.Allocate(image_count);
        // Timestamps need a family that can reset queries in a command buffer (not transfer only).
        const auto families = device.GetPhysical().GetQueueFamilyProperties();
        const u32 fluid_family = device.GetFluidFamily();
        if (std::getenv("EDEN_NX2_SHOWN_STATS") != nullptr && fluid_family < families.size() &&
            (families[fluid_family].queueFlags &
             (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) != 0 &&
            families[fluid_family].timestampValidBits != 0) {
            fluid_copy_queries = dld.CreateQueryPool({
                .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .queryType = VK_QUERY_TYPE_TIMESTAMP,
                .queryCount = 2 * 32,
                .pipelineStatistics = 0,
            });
            fluid_timestamp_ns =
                static_cast<f64>(device.GetPhysical().GetProperties().limits.timestampPeriod);
            const u32 bits = families[fluid_family].timestampValidBits;
            fluid_timestamp_mask = bits >= 64 ? ~u64{0} : (u64{1} << bits) - 1;
        }
    }

    frames.resize(image_count);
    for (u32 i = 0; i < frames.size(); i++) {
        Frame& frame = frames[i];
        frame.index = i;
        frame.cmdbuf = vk::CommandBuffer{cmdbuffers[i], device.GetDispatchLoader()};
        if (i < fluid_cmdbuffers.size()) {
            frame.fluid_cmdbuf = vk::CommandBuffer{fluid_cmdbuffers[i], device.GetDispatchLoader()};
        }
        frame.render_ready = dld.CreateSemaphore({
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
        });
        frame.present_done = dld.CreateFence({
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT,
        });
        frame.storage_capable = storage_supported;
        free_queue.push_back(&frame);
    }

    if (use_present_thread) {
        StartPresentThread();
    }
}

void PresentManager::StartPresentThread() {
    use_present_thread = true;
    present_thread = std::jthread([this](std::stop_token token) { PresentThread(token); });
}

void PresentManager::LogPacing(const Frame* frame, std::chrono::steady_clock::duration took,
                               std::chrono::steady_clock::time_point now) {
    if (!Settings::values.frame_gen.GetValue()) {
        return;
    }
    const bool generated = frame->present_spacing.count() != 0;
    ++(generated ? pacing_log.generated : pacing_log.real);
    pacing_log.spacing += frame->present_spacing;
    pacing_log.present_time += took;
    if (pacing_log.since == std::chrono::steady_clock::time_point{}) {
        pacing_log.since = now;
    } else if (now - pacing_log.since >= std::chrono::seconds(10)) {
        const auto ms = [](auto duration) {
            return std::chrono::duration<double, std::milli>(duration).count();
        };
        const u64 total = pacing_log.real + pacing_log.generated;
        LOG_INFO(Render_Vulkan,
                 "Frame gen pacing (10 s): {} real and {} generated presents, spacing {:.2f} ms "
                 "after each generated frame, {:.2f} ms average wait and present",
                 pacing_log.real, pacing_log.generated,
                 pacing_log.generated ? ms(pacing_log.spacing) / pacing_log.generated : 0.0,
                 total ? ms(pacing_log.present_time) / total : 0.0);
        pacing_log = {.since = now};
    }
}

PresentManager::~PresentManager() {
    // FrameGenSuspend uses state only the present thread touches: stop that thread first.
    if (present_thread.joinable()) {
        present_thread.request_stop();
        present_thread.join();
    }
    if (fluid_presenter) {
        // The FLUID thread shows frames it held for later slots after the renderer last waited
        // for the GPU: their copies must be done before the frames go.
        std::scoped_lock lock{scheduler.submit_mutex};
        void(device.GetLogical().WaitIdle());
    }
#ifdef EGLANTINE_STREAMLINE
    Streamline::FrameGenSuspend();
#endif
}

Frame* PresentManager::GetRenderFrame() {

    VideoCommon::Nx2Profile::Scope scope{VideoCommon::Nx2Profile::Zone::PresentWait};
    // Wait for free presentation frames
    const auto waiting = std::chrono::steady_clock::now();
    std::unique_lock lock{free_mutex};
    free_cv.wait(lock, [this] { return !free_queue.empty(); });

    // Take the frame from the queue
    Frame* frame = free_queue.front();
    if (fluid_presenter) {
        // Frames come back out of order: take one whose copy is done if there is one, and wait
        // without the lock, which the present thread takes to give frames back.
        auto taken = std::find_if(free_queue.begin(), free_queue.end(), FrameDone);
        if (taken == free_queue.end()) {
            taken = free_queue.begin();
        }
        frame = *taken;
        free_queue.erase(taken);
        lock.unlock();
    } else {
        free_queue.pop_front();
    }

    // Wait for the presentation to be finished so all frame resources are free
    frame->present_done.Wait();
    blocked_time.fetch_add((std::chrono::steady_clock::now() - waiting).count(),
                           std::memory_order_relaxed);
    ResetFrame(frame);
    return frame;
}

Frame* PresentManager::TryGetRenderFrame() {
    Frame* frame = nullptr;
    {
        std::scoped_lock lock{free_mutex};
        if (free_queue.size() < 2) {
            return nullptr;
        }
        const auto taken = std::find_if(free_queue.begin(), free_queue.end(), FrameDone);
        if (taken == free_queue.end()) {
            return nullptr;
        }
        frame = *taken;
        free_queue.erase(taken);
    }
    ResetFrame(frame);
    return frame;
}

PresentManager::FluidClock PresentManager::GetFluidClock() const {
    std::scoped_lock lock{fluid_clock_mutex};
    return fluid_clock;
}

std::chrono::steady_clock::time_point PresentManager::FluidClock::SlotTime(u64 slot) const {
    return vblank + refresh * (static_cast<s64>(slot) - static_cast<s64>(vblank_index));
}

u64 PresentManager::FluidClock::FirstSlotPresentedAfter(
    std::chrono::steady_clock::time_point time) const {
    if (!Valid()) {
        return 0;
    }
    // The first vblank at least `lead` after time, then the first slot from it.
    const s64 since =
        std::chrono::duration_cast<std::chrono::nanoseconds>(time + lead - vblank).count();
    const s64 first_vblank = static_cast<s64>(vblank_index) + CeilDiv(since, refresh.count());
    const s64 slot_divisor = static_cast<s64>(divisor);
    return static_cast<u64>(
        (std::max)(slot_divisor, CeilDiv(first_vblank, slot_divisor) * slot_divisor));
}

void PresentManager::Present(Frame* frame) {
    if (!use_present_thread && Settings::values.frame_gen.GetValue()) {
        // Frame generation was turned on from the status bar while a game runs. Earlier frames
        // were presented here, so none are in flight.
        StartPresentThread();
    }
    if (use_present_thread) {
        scheduler.Record([this, frame](vk::CommandBuffer) {
            std::unique_lock lock{queue_mutex};
            present_queue.push_back(frame);
            frame_cv.notify_one();
        });
    } else {
        {
            VideoCommon::Nx2Profile::Scope scope{VideoCommon::Nx2Profile::Zone::PresentWait};
            scheduler.WaitWorker();
        }
        CopyToSwapchain(frame);
        if (VideoCore::Nx2ShownFrames::Armed()) {
            NoteShownCapture(frame, 0, "renderer_thread");
        }
        VideoCore::AutopilotFeed::NoteShown();
        free_queue.push_back(frame);
    }
}

void PresentManager::NoteShownFrame() {
    VideoCore::AutopilotFeed::NoteShown();
    // EDEN_NX2_SHOWN_STATS=1: every 10 s, how evenly frames reached the swapchain (generated ones
    // included): the intervals between presents and the share within 2 ms of the median.
    static const bool enabled = std::getenv("EDEN_NX2_SHOWN_STATS") != nullptr;
    if (!enabled) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (shown_last != std::chrono::steady_clock::time_point{}) {
        shown_intervals.push_back(std::chrono::duration<f32, std::milli>(now - shown_last).count());
    }
    shown_last = now;
    if (shown_window == std::chrono::steady_clock::time_point{}) {
        shown_window = now;
    }
    if (now - shown_window < std::chrono::seconds{10} || shown_intervals.size() < 8) {
        return;
    }
    auto sorted = shown_intervals;
    std::sort(sorted.begin(), sorted.end());
    const auto at = [&sorted](size_t percent) { return sorted[(sorted.size() - 1) * percent / 100]; };
    const f32 median = at(50);
    const auto even = std::count_if(sorted.begin(), sorted.end(), [median](f32 interval) {
        return std::abs(interval - median) <= 2.0f;
    });
    LOG_INFO(Render_Vulkan,
             "Shown frames: {:.1f} per second, interval p5 {:.1f} p50 {:.1f} p95 {:.1f} p99 {:.1f} "
             "max {:.1f} ms, {:.0f}% within 2 ms of the median",
             static_cast<f64>(sorted.size()) /
                 std::chrono::duration<f64>(now - shown_window).count(),
             at(5), median, at(95), at(99), sorted.back(),
             100.0 * static_cast<f64>(even) / static_cast<f64>(sorted.size()));
    shown_intervals.clear();
    shown_window = now;
}

void PresentManager::NoteShownCapture(const Frame* frame, u64 slot, std::string_view presenter) {
    const auto ns = [](std::chrono::steady_clock::time_point point) {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(point.time_since_epoch()).count();
    };
    const auto presented = std::chrono::steady_clock::now();
    s64 scheduled = 0;
    if (slot != 0) {
        const FluidClock clock = GetFluidClock();
        scheduled = clock.Valid() ? ns(clock.SlotTime(slot)) : 0;
    }
    shown_capture.Note(*frame, {
        .kind = frame->shown_kind,
        .slot = slot,
        .batch = frame->fluid_batch,
        .scheduled_ns = scheduled,
        .presented_ns = ns(presented),
        .recorded_ns = frame->fluid_recorded != std::chrono::steady_clock::time_point{}
                           ? ns(frame->fluid_recorded)
                           : 0,
        .presenter = presenter,
        .deferred = fluid_ahead_pending && fluid_ahead_frame == frame,
        .picture = -1,
    });
}

size_t PresentManager::MaxExtraFrames() const {
    return image_count - 1;
}

bool PresentManager::NeedsStorage(const Frame* frame, bool required) const {
    return required && frame->storage_capable && !frame->storage_view;
}

VkFormat PresentManager::GetFrameImageFormat(VkFormat image_view_format) const {
    if (image_view_format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ||
        image_view_format == VK_FORMAT_R16G16B16A16_SFLOAT) {
        return image_view_format;
    }
    // Render SDR through a UNORM view, but preserve the surface's transfer encoding for blits.
    return swapchain.GetSdrImageFormat();
}

void PresentManager::RecreateFrame(Frame* frame, u32 width, u32 height, VkFormat image_view_format,
                                   VkRenderPass rd, bool storage) {
    auto& dld = device.GetLogical();

    frame->width = width;
    frame->height = height;
    frame->format = image_view_format;
    frame->image_format = GetFrameImageFormat(image_view_format);
    frame->storage_capable = CanStoreToFrame(device.GetPhysical(), frame->image_format);

    const bool with_storage = storage && frame->storage_capable;
    VkImageUsageFlags storage_usage = 0;
    if (with_storage) {
        storage_usage = VK_IMAGE_USAGE_STORAGE_BIT;
    }

    std::vector<u32> frame_families{device.GetGraphicsFamily()};
    if (device.HasFluidQueue()) {
        frame_families.push_back(device.GetFluidFamily());
    }

    frame->image = memory_allocator.CreateImage({
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = frame->image_format,
        .extent =
            {
                .width = width,
                .height = height,
                .depth = 1,
            },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | storage_usage,
        // EDEN_NX2_FLUID_QUEUE: the renderer writes the image on the graphics queue and the
        // dedicated queue's family reads it, so it is shared rather than handed over.
        .sharingMode = frame_families.size() > 1 ? VK_SHARING_MODE_CONCURRENT
                                                 : VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = static_cast<u32>(frame_families.size() > 1 ? frame_families.size() : 0),
        .pQueueFamilyIndices = frame_families.size() > 1 ? frame_families.data() : nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    });

    frame->image_view = dld.CreateImageView({
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = *frame->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = image_view_format,
        .components =
            {
                .r = VK_COMPONENT_SWIZZLE_IDENTITY,
                .g = VK_COMPONENT_SWIZZLE_IDENTITY,
                .b = VK_COMPONENT_SWIZZLE_IDENTITY,
                .a = VK_COMPONENT_SWIZZLE_IDENTITY,
            },
        .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
    });

    frame->storage_view = vk::ImageView{};
    if (with_storage) {
        frame->storage_view = dld.CreateImageView({
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .image = *frame->image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = frame->image_format,
            .components =
                {
                    .r = VK_COMPONENT_SWIZZLE_IDENTITY,
                    .g = VK_COMPONENT_SWIZZLE_IDENTITY,
                    .b = VK_COMPONENT_SWIZZLE_IDENTITY,
                    .a = VK_COMPONENT_SWIZZLE_IDENTITY,
                },
            .subresourceRange =
                {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
        });
    }

    const VkImageView image_view{*frame->image_view};
    frame->framebuffer = dld.CreateFramebuffer({
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .renderPass = rd,
        .attachmentCount = 1,
        .pAttachments = &image_view,
        .width = width,
        .height = height,
        .layers = 1,
    });
}

void PresentManager::WaitPresent() {
    if (!use_present_thread) {
        return;
    }

    if (fluid_presenter) {
        // The FLUID present thread holds frames for later slots: it discards them at its next
        // vblank. After its watchdog fell back, it empties the queue as the thread below does.
        std::unique_lock queue_lock{queue_mutex};
        const u64 request = ++fluid_drain_requests;
        frame_cv.wait(queue_lock, [this, request] {
            return fluid_drains >= request ||
                   (fluid_fallback.load(std::memory_order_relaxed) && present_queue.empty());
        });
    } else {
        // Wait for the present queue to be empty
        std::unique_lock queue_lock{queue_mutex};
        frame_cv.wait(queue_lock, [this] { return present_queue.empty(); });
    }

    // The above condition will be satisfied when the last frame is taken from the queue.
    // To ensure that frame has been presented as well take hold of the swapchain
    // mutex.
    std::scoped_lock swapchain_lock{swapchain_mutex};
}

void PresentManager::PresentThread(std::stop_token token) {
    Common::SetCurrentThreadName("VulkanPresent");
    Common::SetCurrentThreadPriority(Common::ThreadPriority::High);
    Common::SetCurrentThreadToPerformanceCores();
    if (fluid_presenter && FluidPresentThread(token)) {
        return;
    }
    while (!token.stop_requested()) {
        std::unique_lock lock{queue_mutex};
        // Wait for presentation frames
        frame_cv.wait(lock, token, [this] { return !present_queue.empty(); });
        if (!token.stop_requested()) {
            // Take the frame and notify anyone waiting
            Frame* frame = present_queue.front();
            present_queue.pop_front();
            frame_cv.notify_one();

            // By exchanging the lock ownership we take the swapchain lock
            // before the queue lock goes out of scope. This way the swapchain
            // lock in WaitPresent is guaranteed to occur after here.
            void(std::exchange(lock, std::unique_lock{swapchain_mutex}));
            const bool trace = Common::DkbFrameTrace::Enabled();
            const auto ready = trace ? std::chrono::steady_clock::now()
                                     : std::chrono::steady_clock::time_point{};
            SleepUntil(next_present);
            CopyToSwapchain(frame);
            const auto presented = std::chrono::steady_clock::now();
            next_present = presented + frame->present_spacing;
            if (trace) {
                LogPacing(frame, presented - ready, presented);
            }
            NoteShownFrame();
            if (VideoCore::Nx2ShownFrames::Armed()) {
                NoteShownCapture(frame, 0, "present_thread");
            }

            // Free the frame for reuse
            std::scoped_lock fl{free_mutex};
            free_queue.push_back(frame);
            free_cv.notify_one();
        }
    }
}

bool PresentManager::FluidPresentThread(std::stop_token token) {
#ifdef _WIN32
    // EDEN_NX2_FLUID_RT=1: the vblank clock's thread wakes on time under a busy CPU: time-critical
    // priority and the MMCSS "Games" task at high priority (avrt.dll, loaded at run time). Its
    // work at each wake is short; it sleeps in between.
    if (const char* rt = std::getenv("EDEN_NX2_FLUID_RT");
        rt != nullptr && std::strtoul(rt, nullptr, 10) != 0) {
        Common::SetCurrentThreadPriority(Common::ThreadPriority::Critical);
        bool mmcss = false;
        if (const HMODULE avrt = LoadLibraryW(L"avrt.dll")) {
            using SetCharacteristics = HANDLE(WINAPI*)(LPCWSTR, LPDWORD);
            using SetPriority = BOOL(WINAPI*)(HANDLE, int);
            const auto set_characteristics = reinterpret_cast<SetCharacteristics>(
                reinterpret_cast<void*>(GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW")));
            const auto set_priority = reinterpret_cast<SetPriority>(
                reinterpret_cast<void*>(GetProcAddress(avrt, "AvSetMmThreadPriority")));
            DWORD task_index = 0;
            if (set_characteristics != nullptr) {
                if (const HANDLE task = set_characteristics(L"Games", &task_index)) {
                    mmcss = true;
                    if (set_priority != nullptr) {
                        set_priority(task, 1); // AVRT_PRIORITY_HIGH
                    }
                }
            }
        }
        LOG_INFO(Render_Vulkan, "EDEN_NX2_FLUID_RT: present thread at time-critical priority{}",
                 mmcss ? ", MMCSS Games task" : "");
    }
#endif
    Common::VblankClock vblank{render_window.GetWindowInfo().render_surface};
    auto source = vblank.CurrentSource();
    LOG_INFO(Render_Vulkan,
             "EDEN_NX2_FLUID {}: frames are shown on a vblank clock ({}, display mode {:.2f} Hz)",
             FluidMode(), Common::VblankClock::SourceName(source),
             HertzOf(vblank.ReportedPeriod()));
    const auto started = std::chrono::steady_clock::now();
    bool start_watched = device.IsFluidQueueActive();
    while (!token.stop_requested()) {
        const auto wake = vblank.Wait();
        if (const auto current = vblank.CurrentSource(); current != source) {
            source = current;
            LOG_INFO(Render_Vulkan, "EDEN_NX2_FLUID: the vblank clock is now {}",
                     Common::VblankClock::SourceName(source));
        }
        const auto period = FluidCountVblanks(wake, vblank.ReportedPeriod());
        VideoCore::AutopilotFeed::SetRefreshPeriod(period);
        const auto offset = (std::min)(FluidPresentOffsetFor(), period * 42 / 100);
        // A screen copy wait from this wake ends in time for the next slot's wake.
        fluid.copy_wait_due = wake + FluidSlots::CopyWaitDue(period, fluid.vblank, fluid.divisor);
        {
            std::scoped_lock lock{fluid_clock_mutex};
            fluid_clock = FluidClock{
                .vblank = wake,
                .vblank_index = fluid.vblank,
                .refresh = period,
                .divisor = fluid.divisor,
                .lead = FluidCopyAhead() && device.IsFluidQueueActive()
                            ? period * static_cast<s64>(1 + fluid.divisor) - offset
                            : period - offset,
                .latency = std::chrono::nanoseconds{static_cast<s64>(fluid.latency_ns)},
            };
        }
        FluidTakeFrames(wake);
        FluidRelease(wake);
        const bool shown = FluidPresentUnscheduled(wake);
        if ((fluid.vblank + 1) % fluid.divisor == 0) {
            // The next vblank is a slot's: its frame goes out mid-refresh, so that being a few ms
            // off either way still lands on it.
            if (!shown) {
                vblank.SleepUntil(wake + offset);
                const auto now = std::chrono::steady_clock::now();
                FluidTakeFrames(now);
                FluidRelease(now);
            }
            if (FluidCopyAhead() && device.IsFluidQueueActive()) {
                FluidPresentAhead();
                FluidPresentSlot(fluid.vblank + 1 + fluid.divisor,
                                 wake + period * static_cast<s64>(1 + fluid.divisor), !shown);
            } else {
                FluidPresentSlot(fluid.vblank + 1, wake + period, !shown);
            }
        }
        const auto now = std::chrono::steady_clock::now();
        FluidLogStats(now, Common::VblankClock::SourceName(source), period);
        if (start_watched && fluid.received != 0) {
            start_watched = false;
        } else if (start_watched && now - started >= FluidFirstFrameLimit) {
            // The producer has handed over nothing: if the dedicated queue is why, copies and
            // presents go back to the graphics queue (this FLUID presenter stays).
            start_watched = false;
            LOG_WARNING(Render_Vulkan,
                        "EDEN_NX2_FLUID_QUEUE: no frame received {:.0f} s after the presenter started, "
                        "GPU at tick {}; copies and presents use the graphics queue from now on",
                        std::chrono::duration<f64>(now - started).count(),
                        scheduler.GetMasterSemaphore().KnownGpuTick());
            device.DisableFluidQueue();
        }
        if (FluidStalled(now)) {
            FluidFallBack(now);
            return false;
        }
    }
    if (fluid_ahead_pending) {
        std::scoped_lock swapchain_lock{swapchain_mutex};
        fluid_ahead_pending = false;
        fluid_ahead_frame = nullptr;
        swapchain.Present(fluid_ahead_semaphore);
    }
    // Nothing is presented from here on: answer WaitPresent.
    {
        std::scoped_lock lock{queue_mutex};
        fluid_drains = fluid_drain_requests;
    }
    frame_cv.notify_all();
    return true;
}

bool PresentManager::FluidStalled(std::chrono::steady_clock::time_point now) const {
    if (fluid.unshown_since == std::chrono::steady_clock::time_point{} ||
        now - fluid.unshown_since < FluidStallLimit) {
        return false;
    }
    // Frames still wait or keep coming, unlike a game that stopped presenting after a frame whose
    // slot passed.
    return !fluid.pending.empty() || now - fluid.last_received < FluidStallLimit;
}

void PresentManager::FluidFallBack(std::chrono::steady_clock::time_point now) {
    // Classic from here on includes the graphics queue for copies and presents.
    device.DisableFluidQueue();
    const Frame* const oldest = fluid.pending.empty() ? nullptr : fluid.pending.front();
    const char* showing = "none";
    if (fluid.showing != nullptr) {
        showing = FrameDone(fluid.showing) ? "copy done" : "copy still pending";
    } else if (fluid.shown != 0) {
        showing = "copy done, frame given back";
    }
    LOG_WARNING(Render_Vulkan,
                "EDEN_NX2_FLUID: no frame shown for {:.1f} s while frames came; presenting in "
                "arrival order from now on. {} frames pending, the oldest with {} and GPU tick "
                "{}; last slot {}, vblank {}, GPU at tick {}; last frame shown: {}; record to GPU "
                "done {:.1f} ms; {} frames received and {} shown since the start",
                std::chrono::duration<f64>(now - fluid.unshown_since).count(),
                fluid.pending.size(), oldest != nullptr ? FluidSlotText(oldest) : "-",
                oldest != nullptr ? oldest->gpu_tick : 0, fluid.last_slot, fluid.vblank,
                scheduler.GetMasterSemaphore().KnownGpuTick(), showing,
                fluid.latency_ns * 1.0e-6, fluid.received, fluid.shown);
    {
        std::scoped_lock lock{queue_mutex};
        // PresentThread takes present_queue in order: the frames held go first.
        present_queue.insert(present_queue.begin(), fluid.pending.begin(), fluid.pending.end());
        fluid_fallback.store(true, std::memory_order_relaxed);
        fluid_drains = fluid_drain_requests;
    }
    fluid.pending.clear();
    frame_cv.notify_all();
    if (fluid.showing != nullptr) {
        // GetRenderFrame waits for its copy before the frame is drawn into again.
        ReturnFrame(fluid.showing);
        fluid.showing = nullptr;
    }
}

std::chrono::nanoseconds PresentManager::FluidCountVblanks(
    std::chrono::steady_clock::time_point wake, std::chrono::nanoseconds reported) {
    const bool first = fluid.last_wake == std::chrono::steady_clock::time_point{};
    const s64 since =
        first ? 0
              : std::chrono::duration_cast<std::chrono::nanoseconds>(wake - fluid.last_wake).count();
    if (!first) {
        fluid.intervals[fluid.interval_next] = since;
        fluid.interval_next = (fluid.interval_next + 1) % fluid.intervals.size();
        fluid.interval_count = (std::min)(fluid.interval_count + 1, fluid.intervals.size());
    }
    // The display mode's period, unless the wakes disagree with it (a dynamic refresh rate).
    std::chrono::nanoseconds period =
        reported.count() > 0 ? reported : std::chrono::nanoseconds{16'666'667};
    if (fluid.interval_count >= 8) {
        auto sorted = fluid.intervals;
        const auto count = static_cast<std::ptrdiff_t>(fluid.interval_count);
        std::nth_element(sorted.begin(), sorted.begin() + count / 2, sorted.begin() + count);
        const s64 median = sorted[static_cast<size_t>(count / 2)];
        const s64 nominal = period.count();
        if (std::abs(median - nominal) * 10 > nominal && median >= 2'000'000 &&
            median <= 50'000'000) {
            period = std::chrono::nanoseconds{median};
        }
    }
    // A wake that came late counts the vblanks it slept through.
    s64 elapsed = 1;
    if (!first) {
        const f64 refreshes = static_cast<f64>(since) / static_cast<f64>(period.count());
        elapsed = (std::max<s64>)(1, static_cast<s64>(std::llround(refreshes)));
    }
    fluid.vblank += static_cast<u64>(elapsed);
    fluid.last_wake = wake;
    // Slots of the frame generation target rate (16.7 ms at 60, a refresh each at 120 on a 120 Hz
    // display): every round(refresh / rate) vblanks, or of the shown rate the autopilot steers
    // towards (EDEN_NX2_AUTOPILOT).
    const f64 slot_rate = VideoCore::AutopilotFeed::SlotRate();
    const f64 per_slot = 1.0e9 / ((slot_rate > 0.0 ? slot_rate : FluidShownRate()) *
                                  static_cast<f64>(period.count()));
    fluid.divisor =
        static_cast<u64>(std::clamp<s64>(static_cast<s64>(std::llround(per_slot)), 1, 8));
    return period;
}

void PresentManager::FluidTakeFrames(std::chrono::steady_clock::time_point now) {
    const size_t received = fluid.pending.size();
    u64 drain = 0;
    {
        std::scoped_lock lock{queue_mutex};
        while (!present_queue.empty()) {
            fluid.pending.push_back(present_queue.front());
            present_queue.pop_front();
        }
        if (fluid_drains != fluid_drain_requests) {
            drain = fluid_drain_requests;
        }
    }
    // A real frame arrives before its predictions, and its tick is the one of their GPU work.
    for (size_t index = received; index < fluid.pending.size(); ++index) {
        const Frame* const frame = fluid.pending[index];
        if (frame->fluid_batch == 0 || frame->fluid_batch == fluid.watched_batch) {
            continue;
        }
        fluid.watched_batch = frame->fluid_batch;
        fluid.newest_batch = (std::max)(fluid.newest_batch, frame->fluid_batch);
        if (fluid.watches.size() >= FluidWatches) {
            fluid.watches.erase(fluid.watches.begin());
        }
        fluid.watches.push_back(FluidWatch{
            .tick = frame->gpu_tick,
            .recorded = frame->fluid_recorded,
            .unready = now,
        });
    }
    if (fluid.pending.size() > received) {
        if (fluid.received == 0) {
            const Frame* const first = fluid.pending[received];
            LOG_INFO(Render_Vulkan,
                     "EDEN_NX2_FLUID: first frame received at vblank {}, with {} and GPU tick {} "
                     "(GPU at tick {})",
                     fluid.vblank, FluidSlotText(first), first->gpu_tick,
                     scheduler.GetMasterSemaphore().KnownGpuTick());
        }
        const u64 arrived = fluid.pending.size() - received;
        fluid.received += arrived;
        fluid_stats.received += arrived;
        // After the game paused, a frame a slot dropped before the pause doesn't count.
        if (fluid.unshown_since == std::chrono::steady_clock::time_point{} ||
            (received == 0 && now - fluid.last_received > FluidArrivalGap)) {
            fluid.unshown_since = now;
        }
        fluid.last_received = now;
    }
    {
        std::scoped_lock swapchain_lock{swapchain_mutex};
        std::erase_if(fluid.pending, [this, drain](Frame* frame) {
            if (drain == 0 && frame->fluid_slot != FLUID_NOT_SHOWN) {
                return false;
            }
            FluidDiscard(frame);
            return true;
        });
    }
    if (drain != 0) {
        {
            std::scoped_lock lock{queue_mutex};
            fluid_drains = drain;
        }
        frame_cv.notify_all();
    }
}

void PresentManager::FluidRelease(std::chrono::steady_clock::time_point now) {
    scheduler.GetMasterSemaphore().Refresh();
    // When a real frame's GPU work is done: halfway between the last look that found it not done
    // and the first that found it done.
    for (auto it = fluid.watches.begin(); it != fluid.watches.end();) {
        if (!scheduler.IsFree(it->tick)) {
            it->unready = now;
            ++it;
            continue;
        }
        const auto done = it->unready + (now - it->unready) / 2;
        // A hitch (a shader built on the GPU thread) says nothing about the next frames.
        const f64 sample = std::clamp(
            std::chrono::duration<f64, std::nano>(done - it->recorded).count(), 0.0, 100.0e6);
        fluid.latency_ns = fluid.latency_ns > 0.0
                               ? fluid.latency_ns + (sample - fluid.latency_ns) * 0.2
                               : sample;
        fluid_stats.latency_ms += sample * 1.0e-6;
        ++fluid_stats.latency_samples;
        it = fluid.watches.erase(it);
    }
    // The last shown frame can be drawn into again once its copy is done. Still not done at its
    // vblank, it was shown late.
    if (fluid.showing != nullptr) {
        if (FrameDone(fluid.showing)) {
            FluidNoteCopyTime(fluid.showing);
            ReturnFrame(fluid.showing);
            fluid.showing = nullptr;
        } else if (!fluid.showing_late && fluid.vblank >= fluid.showing_vblank) {
            fluid.showing_late = true;
            ++fluid_stats.late;
        }
    }
    // Frames whose slot passed, and frames whose slot a newer real frame's done frame takes.
    fluid.freeing.clear();
    for (Frame* const frame : fluid.pending) {
        if (!FluidScheduled(frame)) {
            continue;
        }
        const u64 slot = FluidSlotOf(frame, fluid.divisor);
        const bool replaced =
            frame->fluid_slot <= fluid.last_slot ||
            std::any_of(fluid.pending.begin(), fluid.pending.end(),
                        [this, frame, slot](const Frame* other) {
                            return FluidScheduled(other) &&
                                   other->fluid_batch > frame->fluid_batch &&
                                   FluidSlotOf(other, fluid.divisor) <= slot &&
                                   scheduler.IsFree(other->gpu_tick);
                        });
        if (replaced) {
            fluid.freeing.push_back(frame);
        }
    }
    if (fluid.freeing.empty()) {
        return;
    }
    std::scoped_lock swapchain_lock{swapchain_mutex};
    for (Frame* const frame : fluid.freeing) {
        std::erase(fluid.pending, frame);
        FluidDiscard(frame);
        ++fluid_stats.dropped;
    }
}

bool PresentManager::FluidPresentUnscheduled(std::chrono::steady_clock::time_point now) {
    // Frames without a slot (no vblank clock yet, interpolated frames) are shown as before: in
    // arrival order, after their spacing, one a vblank, and here also once their GPU work is
    // done.
    const auto found = std::find_if(fluid.pending.begin(), fluid.pending.end(),
                                    [](const Frame* frame) { return frame->fluid_slot == 0; });
    if (found == fluid.pending.end()) {
        return false;
    }
    Frame* const frame = *found;
    if (fluid.showing != nullptr || !scheduler.IsFree(frame->gpu_tick) ||
        now + std::chrono::milliseconds{1} < next_present) {
        return false;
    }
    fluid.pending.erase(found);
    std::scoped_lock swapchain_lock{swapchain_mutex};
    if (frame->fluid_batch != 0) {
        // Predictions from older real frames would show the past after it.
        std::erase_if(fluid.pending, [this, frame](Frame* older) {
            if (!FluidScheduled(older) || older->fluid_batch >= frame->fluid_batch) {
                return false;
            }
            FluidDiscard(older);
            ++fluid_stats.dropped;
            return true;
        });
    }
    FluidShow(frame, fluid.vblank + 1);
    next_present = std::chrono::steady_clock::now() + frame->present_spacing;
    ++fluid_stats.in_order;
    return true;
}

void PresentManager::FluidPresentSlot(u64 slot, std::chrono::steady_clock::time_point slot_time,
                                      bool count) {
    constexpr auto MaxRollAge = std::chrono::milliseconds{250};
    const u64 after = fluid.last_slot;
    fluid.last_slot = slot;
    Frame* best = nullptr;
    // The frame to move to the next slot rather than drop: the newest one the slot found not done,
    // or the one a copy still queued kept from being shown.
    Frame* keep = nullptr;
    if (count) {
        if (after != 0 && slot > after + fluid.divisor) {
            // The present thread slept through these.
            const u64 skipped = (slot - after) / fluid.divisor - 1;
            fluid_stats.slots += skipped;
            fluid_stats.overslept += skipped;
        }
        ++fluid_stats.slots;
        FluidPredictHold(slot, after); // EDEN_NX2_PREDICT
        const auto newer = [](const Frame* frame, const Frame* than) {
            return than == nullptr || frame->fluid_batch > than->fluid_batch ||
                   (frame->fluid_batch == than->fluid_batch && frame->fluid_slot > than->fluid_slot);
        };
        // The newest real frame's frame for the slot whose GPU work is done.
        Frame* latest = nullptr;
        for (Frame* const frame : fluid.pending) {
            if (!FluidScheduled(frame) || frame->fluid_slot <= after || frame->fluid_slot > slot) {
                continue;
            }
            if (newer(frame, latest)) {
                latest = frame;
            }
            if (scheduler.IsFree(frame->gpu_tick) && newer(frame, best)) {
                best = frame;
            }
        }
        FluidPredictNote(slot, best); // EDEN_NX2_PREDICT
        if (best == nullptr) {
            ++fluid_stats.no_frame;
            ++(latest != nullptr ? fluid_stats.not_done : fluid_stats.uncovered);
            keep = latest;
        } else if (fluid.showing != nullptr) {
            // The last copy is still queued: presenting this one would queue a second.
            ++fluid_stats.queued;
            keep = best;
            best = nullptr;
        }
        if (keep != nullptr) {
            // Dropped, it would leave the screen on the last frame until the next batch; shown a
            // slot late it is still the newest picture, unless a frame of its batch or a newer
            // one waits for a later slot.
            const bool superseded = std::any_of(
                fluid.pending.begin(), fluid.pending.end(), [&](const Frame* other) {
                    return other != keep && FluidScheduled(other) && other->fluid_slot > slot &&
                           other->fluid_batch >= keep->fluid_batch;
                });
            if (superseded || slot_time - keep->fluid_recorded > MaxRollAge) {
                keep = nullptr;
            }
        }
    }
    const u64 final_batch = best != nullptr && best->fluid_final ? best->fluid_batch : 0;
    std::scoped_lock swapchain_lock{swapchain_mutex};
    if (keep != nullptr) {
        keep->fluid_slot = slot + fluid.divisor;
        ++fluid_stats.rolled;
    }
    // The slot is over for the frames up to it.
    std::erase_if(fluid.pending, [this, slot, best, final_batch](Frame* frame) {
        if (frame == best) {
            return true;
        }
        if (!FluidScheduled(frame)) {
            return false;
        }
        if (final_batch != 0 && frame->fluid_batch < final_batch) {
            // Predicted from the scene before a cut: shown after the cut frame it would flash back.
            ++fluid_stats.stale;
        } else if (frame->fluid_slot > slot) {
            return false;
        }
        FluidDiscard(frame);
        ++fluid_stats.dropped;
        return true;
    });
    if (best == nullptr) {
        return;
    }
    ++fluid_stats.hits;
    fluid_stats.spare += best->fluid_batch < fluid.newest_batch ? 1 : 0;
    fluid_stats.age_ms +=
        std::chrono::duration<f64, std::milli>(slot_time - best->fluid_recorded).count();
    fluid_copy_ahead_request = FluidCopyAhead() && device.IsFluidQueueActive();
    FluidShow(best, slot);
    fluid_copy_ahead_request = false;
    const f64 margin =
        std::chrono::duration<f64, std::milli>(slot_time - std::chrono::steady_clock::now())
            .count();
    fluid_stats.margin_min_ms =
        fluid_stats.margin_samples == 0 ? margin : (std::min)(fluid_stats.margin_min_ms, margin);
    fluid_stats.margin_ms += margin;
    ++fluid_stats.margin_samples;
    fluid_stats.after_vblank += margin < 0.0 ? 1 : 0;
}

void PresentManager::FluidShow(Frame* frame, u64 shown_vblank) {
    CopyToSwapchain(frame);
    if (fluid.shown++ == 0) {
        LOG_INFO(Render_Vulkan,
                 "EDEN_NX2_FLUID: first frame shown at vblank {}, for vblank {}, after {} received",
                 fluid.vblank, shown_vblank, fluid.received);
    }
    // Frames still held count from this show.
    fluid.unshown_since = fluid.pending.empty() ? std::chrono::steady_clock::time_point{}
                                                : std::chrono::steady_clock::now();
    fluid.showing = frame;
    fluid.showing_vblank = shown_vblank;
    fluid.showing_late = false;
    if (fluid.last_shown_vblank != 0 && shown_vblank > fluid.last_shown_vblank) {
        const u64 held = (std::min<u64>)(shown_vblank - fluid.last_shown_vblank, 4);
        ++fluid_stats.held[static_cast<size_t>(held - 1)];
        fluid_stats.one_slot +=
            FluidSlots::HeldOneSlot(shown_vblank, fluid.last_shown_vblank, fluid.divisor) ? 1 : 0;
    }
    fluid.last_shown_vblank = shown_vblank;
    NoteShownFrame();
    if (VideoCore::Nx2ShownFrames::Armed()) {
        NoteShownCapture(frame, frame->fluid_slot != 0 ? shown_vblank : 0, "fluid");
    }
}

void PresentManager::FluidPresentAhead() {
    if (!fluid_ahead_pending) {
        return;
    }
    std::scoped_lock swapchain_lock{swapchain_mutex};
    static const bool shown_stats = std::getenv("EDEN_NX2_SHOWN_STATS") != nullptr;
    if (fluid.showing != nullptr && fluid.showing == fluid_ahead_frame) {
        // Its copy had a slot; a copy still running is late anyway.
        const auto wait_start = std::chrono::steady_clock::now();
        const bool copied =
            fluid_ahead_frame->present_done.Wait(FluidSlots::CopyWaitNs(
                FluidHostWaitNs(FluidShownRate()),
                fluid.copy_wait_due - std::chrono::steady_clock::now())) == VK_SUCCESS;
        if (shown_stats) {
            const f64 waited = std::chrono::duration<f64, std::milli>(
                                   std::chrono::steady_clock::now() - wait_start)
                                   .count();
            ++fluid_stats.copy_waits;
            fluid_stats.copy_wait_timeouts += copied ? 0 : 1;
            fluid_stats.copy_wait_ms += waited;
            fluid_stats.copy_wait_max_ms = (std::max)(fluid_stats.copy_wait_max_ms, waited);
        }
    }
    fluid_ahead_pending = false;
    fluid_ahead_frame = nullptr;
    swapchain.Present(fluid_ahead_semaphore);
    if (shown_stats) {
        const f64 lock = static_cast<f64>(swapchain.LastPresentLockNs()) * 1.0e-6;
        const f64 call = static_cast<f64>(swapchain.LastPresentCallNs()) * 1.0e-6;
        ++fluid_stats.presents;
        fluid_stats.lock_ms += lock;
        fluid_stats.lock_max_ms = (std::max)(fluid_stats.lock_max_ms, lock);
        fluid_stats.call_ms += call;
        fluid_stats.call_max_ms = (std::max)(fluid_stats.call_max_ms, call);
    }
    // The copy is done (or late): give the frame back now rather than at the next wake.
    if (fluid.showing != nullptr && FrameDone(fluid.showing)) {
        FluidNoteCopyTime(fluid.showing);
        ReturnFrame(fluid.showing);
        fluid.showing = nullptr;
    }
}

void PresentManager::FluidNoteCopyTime(Frame* frame) {
    if (!frame->copy_timed) {
        return;
    }
    frame->copy_timed = false;
    std::array<u64, 2> stamps{};
    if (device.GetLogical().GetQueryResults(*fluid_copy_queries, 2 * frame->index, 2,
                                            sizeof(stamps), stamps.data(), sizeof(u64),
                                            VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) {
        return;
    }
    const u64 ticks = (stamps[1] - stamps[0]) & fluid_timestamp_mask;
    const f64 ms = static_cast<f64>(ticks) * fluid_timestamp_ns * 1.0e-6;
    if (ms < 0.0 || ms > 1000.0) {
        return;
    }
    ++fluid_stats.copy_gpu_samples;
    fluid_stats.copy_gpu_ms += ms;
    fluid_stats.copy_gpu_max_ms = (std::max)(fluid_stats.copy_gpu_max_ms, ms);
}

void PresentManager::FluidDiscard(Frame* frame) {
    DiscardFrame(frame);
    ReturnFrame(frame);
}

void PresentManager::ReturnFrame(Frame* frame) {
    std::scoped_lock lock{free_mutex};
    free_queue.push_back(frame);
    free_cv.notify_one();
}

void PresentManager::FluidLogStats(std::chrono::steady_clock::time_point now, const char* source,
                                   std::chrono::nanoseconds period) {
    static const bool enabled = std::getenv("EDEN_NX2_SHOWN_STATS") != nullptr;
    if (!enabled) {
        return;
    }
    auto& stats = fluid_stats;
    if (stats.since == std::chrono::steady_clock::time_point{}) {
        stats.since = now;
        return;
    }
    if (now - stats.since < std::chrono::seconds{10}) {
        return;
    }
    const u64 held = stats.held[0] + stats.held[1] + stats.held[2] + stats.held[3];
    const auto share = [](u64 part, u64 whole) {
        return whole != 0 ? 100.0 * static_cast<f64>(part) / static_cast<f64>(whole) : 0.0;
    };
    const auto mean = [](f64 sum, u64 samples) {
        return samples != 0 ? sum / static_cast<f64>(samples) : 0.0;
    };
    LOG_INFO(Render_Vulkan,
             "Fluid presenter (10 s, {} at {:.2f} Hz, a slot every {} refreshes): {} slots, {} "
             "shown on their slot, {} missed ({} with no frame ready: {} scheduled but their GPU "
             "work not done, {} with nothing scheduled; {} with the last copy still queued, {} "
             "slept through), {} shown in arrival order, {} copies done after their vblank",
             source, HertzOf(period), fluid.divisor, stats.slots, stats.hits,
             stats.no_frame + stats.queued + stats.overslept, stats.no_frame, stats.not_done,
             stats.uncovered, stats.queued, stats.overslept, stats.in_order, stats.late);
    LOG_INFO(Render_Vulkan,
             "Fluid presenter (10 s): {:.0f}% of shown frames held one slot, held 1/2/3/4+ "
             "refreshes {}/{}/{}/{}; real frame age at display {:.1f} ms; GPU work done {:.1f} ms "
             "after recording ({} real frames); {} frames received, {} dropped unshown",
             share(stats.one_slot, held), stats.held[0], stats.held[1], stats.held[2],
             stats.held[3], mean(stats.age_ms, stats.hits),
             mean(stats.latency_ms, stats.latency_samples), stats.latency_samples, stats.received,
             stats.dropped);
    LOG_INFO(Render_Vulkan,
             "Fluid presenter (10 s): {} late or blocked frames shown a slot later instead of "
             "dropped, {} shown frames predicted from an older real frame than the newest, {} "
             "frames dropped as older than a real frame no prediction follows; real frames: {} "
             "recorded, {} not predictable, {} with fewer predictions than slots for lack of a "
             "free frame, {} ended by the prediction limit",
             stats.rolled, stats.spare, stats.stale, fluid_batches.exchange(0),
             fluid_unpredictable.exchange(0), fluid_cut_short.exchange(0),
             fluid_capped.exchange(0));
    LOG_INFO(Render_Vulkan,
             "Fluid presenter (10 s): present thread: {} presents; acquire {:.2f} ms mean, {:.2f} "
             "max; copy wait {:.2f} ms mean, {:.2f} max, {} of {} timed out; submit lock {:.2f} ms "
             "mean, {:.2f} max; present call {:.2f} ms mean, {:.2f} max; slot presents returned "
             "{:.2f} ms before their vblank on average, {:.2f} at the least, {} after it; the copy "
             "ran {:.2f} ms on the GPU once started on average, {:.2f} at most ({} timed)",
             stats.presents, mean(stats.acquire_ms, stats.presents), stats.acquire_max_ms,
             mean(stats.copy_wait_ms, stats.copy_waits), stats.copy_wait_max_ms,
             stats.copy_wait_timeouts, stats.copy_waits, mean(stats.lock_ms, stats.presents),
             stats.lock_max_ms, mean(stats.call_ms, stats.presents), stats.call_max_ms,
             mean(stats.margin_ms, stats.margin_samples), stats.margin_min_ms, stats.after_vblank,
             mean(stats.copy_gpu_ms, stats.copy_gpu_samples), stats.copy_gpu_max_ms,
             stats.copy_gpu_samples);
    stats = {.since = now};
}

void PresentManager::UpdateHdrMetadata() {
    // The values the guest was told it renders for (nx2:vi2 QueryDisplayHdrStatus).
    auto& monitor = Common::DisplayHdrMonitor::Instance();
    monitor.Watch(render_window.GetWindowInfo().render_surface);
    hdr_metadata_generation = monitor.Generation();
    const auto output = Common::ResolveHdrOutput(monitor.Current(), Settings::HdrMode::On,
                                                 Settings::values.hdr_peak_luminance.GetValue(),
                                                 Settings::values.hdr_reference_white.GetValue());
    swapchain.SetHdrMetadata(output.peak_luminance, output.reference_white);
}

// Returns false after discarding a deferred or incompatible frame; creation exceptions propagate.
bool PresentManager::RecreateSwapchain(Frame* frame) {
#ifdef EGLANTINE_STREAMLINE
    Streamline::FrameGenSuspend();
#endif
    if (swapchain.Create(*surface, frame->width, frame->height, frame->format,
                         frame->color_space)) {
        SetImageCount();
        if (frame->color_space == VK_COLOR_SPACE_HDR10_ST2084_EXT) {
            UpdateHdrMetadata();
        }
        ConfigureTransfer(frame);
        if (transfer_mode != TransferMode::Unsupported) {
            return true;
        }
    }
    DiscardFrame(frame);
    return false;
}

void PresentManager::ConfigureTransfer(const Frame* frame) {
    transfer_source_format = frame->image_format;
    transfer_mode = TransferMode::Unsupported;
    if (frame->image_format == GetFrameImageFormat(frame->format)) {
        if (CanBlitToSwapchain(device.GetPhysical(), swapchain.GetImageFormat()) &&
            (device.GetPhysical().GetFormatProperties(frame->image_format).optimalTilingFeatures &
             VK_FORMAT_FEATURE_BLIT_SRC_BIT) != 0) {
            transfer_mode = TransferMode::Blit;
        } else if (frame->image_format == swapchain.GetImageFormat()) {
            transfer_mode = TransferMode::Copy;
        }
    }
}

void PresentManager::SetImageCount() {
    // We cannot have more than 7 images in flight at any given time.
    // FRAMES_IN_FLIGHT is 8, and the cache TICKS_TO_DESTROY is 8.
    // Mali drivers will give us 6.
#ifdef HAS_LSFG
    if (fluid_presenter) {
        // A real frame and a prediction for each slot until the next one is due, while the last
        // real frame's predictions still wait for their slots.
        image_count = FluidFrameCount();
    } else {
        const size_t generations = Settings::FrameGenMaxGenerations();
        const size_t queued_composites = Settings::values.frame_gen_queue_target.GetValue() + 1;
        image_count =
            std::clamp<size_t>((generations + 1) * queued_composites, swapchain.GetImageCount(),
                               MAX_FRAMES_IN_FLIGHT);
    }
#else
    image_count = std::min<size_t>(swapchain.GetImageCount(), MAX_FRAMES_IN_FLIGHT);
#endif
    swapchain_image_count = swapchain.GetImageCount();
    swapchain_image_format = swapchain.GetImageFormat();
}

void PresentManager::DiscardFrame(Frame* frame) {
    static constexpr VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    const VkSemaphore render_ready = *frame->render_ready;
    const VkSubmitInfo submit_info{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = nullptr,
        .waitSemaphoreCount = 1U,
        .pWaitSemaphores = &render_ready,
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 0U,
        .pCommandBuffers = nullptr,
        .signalSemaphoreCount = 0U,
        .pSignalSemaphores = nullptr,
    };

    std::scoped_lock submit_lock{scheduler.submit_mutex};
    void(device.GetGraphicsQueue().Submit(submit_info, *frame->present_done));
}

void PresentManager::CopyToSwapchain(Frame* frame) {
#ifdef EGLANTINE_STREAMLINE
    SCOPE_EXIT {
        if (frame->frame_gen) {
            Streamline::FrameGenClosePresent(frame->frame_gen_frame.index);
        }
    };
#endif
    bool requires_recreation = false;

    for (u32 attempt = 0; attempt < MAX_PRESENT_ATTEMPTS; ++attempt) {
        try {
            // Recreate surface and swapchain if needed.
            if (requires_recreation) {
#ifdef __ANDROID__
                surface = CreateSurface(instance, render_window.GetWindowInfo());
#endif
                if (!RecreateSwapchain(frame)) {
                    return;
                }
            }

            // Draw to swapchain.
            return CopyToSwapchainImpl(frame);
        } catch (const vk::Exception& except) {
            const VkResult result = except.GetResult();
            if (result != VK_ERROR_SURFACE_LOST_KHR && result != VK_INCOMPLETE &&
                result != VK_ERROR_OUT_OF_DATE_KHR && result != VK_ERROR_NATIVE_WINDOW_IN_USE_KHR) {
                throw;
            }

            requires_recreation = true;
        }
    }

    DiscardFrame(frame);
}

bool PresentManager::UseFluidQueue(const Frame* frame, VkExtent2D extent) {
    if (!device.IsFluidQueueActive() || !Fluid()) {
        return false;
    }
    // The dedicated queue's family cannot blit: it copies, which needs the same format and size.
    if (frame->image_format == swapchain.GetImageFormat() && frame->width == extent.width &&
        frame->height == extent.height) {
        return true;
    }
    if (!fluid_queue_mismatch_logged) {
        fluid_queue_mismatch_logged = true;
        LOG_INFO(Render_Vulkan,
                 "EDEN_NX2_FLUID_QUEUE: frame {}x{} format {} and swapchain {}x{} format {} "
                 "differ; those frames are copied on the graphics queue",
                 frame->width, frame->height, static_cast<int>(frame->image_format), extent.width,
                 extent.height, static_cast<int>(swapchain.GetImageFormat()));
    }
    return false;
}

void PresentManager::SubmitFluidCopy(Frame* frame, VkImage image, VkExtent2D extent,
                                     VkSemaphore render_semaphore) {
    const vk::CommandBuffer cmdbuf{frame->fluid_cmdbuf};
    cmdbuf.Begin({
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    });

    // The frame image stays in GENERAL, a valid copy source: the render-done semaphore makes the
    // renderer's writes available and visible, and the fence the copy signals hands the image
    // back, so it needs no barrier or layout change here.
    const VkImageSubresourceRange range{
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };
    const VkImageMemoryBarrier to_destination{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = range,
    };
    const VkImageMemoryBarrier to_present{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = 0,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = range,
    };
    const bool timed = fluid_copy_queries && frame->index < 32;
    if (timed) {
        cmdbuf.ResetQueryPool(*fluid_copy_queries, 2 * frame->index, 2);
        cmdbuf.WriteTimestamp(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, *fluid_copy_queries,
                              2 * frame->index);
    }
    frame->copy_timed = timed;
    // Only stages every queue family supports: the family may be compute or transfer only.
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, {},
                           {}, {}, to_destination);
    cmdbuf.CopyImage(*frame->image, VK_IMAGE_LAYOUT_GENERAL, image,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     MakeImageCopy(frame->width, frame->height, extent.width, extent.height));
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                           {}, {}, {}, to_present);
    if (timed) {
        cmdbuf.WriteTimestamp(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, *fluid_copy_queries,
                              2 * frame->index + 1);
    }
    cmdbuf.End();

    const std::array wait_semaphores = {swapchain.CurrentPresentSemaphore(), *frame->render_ready};
    static constexpr std::array<VkPipelineStageFlags, 2> wait_stage_masks{
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
    };
    const VkSubmitInfo submit_info{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = nullptr,
        .waitSemaphoreCount = 2U,
        .pWaitSemaphores = wait_semaphores.data(),
        .pWaitDstStageMask = wait_stage_masks.data(),
        .commandBufferCount = 1,
        .pCommandBuffers = cmdbuf.address(),
        .signalSemaphoreCount = 1U,
        .pSignalSemaphores = &render_semaphore,
    };
    // Only the present thread uses this queue, so the renderer's submit mutex is not needed.
    switch (const VkResult result =
                device.GetFluidQueue().Submit(submit_info, *frame->present_done)) {
    case VK_SUCCESS:
        break;
    case VK_ERROR_DEVICE_LOST:
        device.ReportLoss();
        [[fallthrough]];
    default:
        vk::Check(result);
        break;
    }
}

void PresentManager::CopyToSwapchainImpl(Frame* frame) {
    if (fluid_ahead_pending) {
        // EDEN_NX2_FLUID_COPY_AHEAD: one acquired image at a time; the one copied ahead goes first.
        fluid_ahead_pending = false;
        fluid_ahead_frame = nullptr;
        swapchain.Present(fluid_ahead_semaphore);
    }

    // Recreate for size changes, SDR/HDR10/scRGB transitions, or a suboptimal swapchain.
    const bool is_suboptimal = swapchain.NeedsRecreation() || swapchain.IsOutDated();
    const bool size_changed =
        swapchain.GetWidth() != frame->width || swapchain.GetHeight() != frame->height;
    const bool format_changed = swapchain.GetRequestedFormat() != frame->format;
    const bool color_space_changed = swapchain.GetRequestedColorSpace() != frame->color_space;
    if (is_suboptimal || size_changed || format_changed || color_space_changed) {
        if (!RecreateSwapchain(frame)) {
            return;
        }
    } else if (frame->color_space == VK_COLOR_SPACE_HDR10_ST2084_EXT &&
               Common::DisplayHdrMonitor::Instance().Generation() != hdr_metadata_generation) {
        // A new calibration or peak while HDR stays on: the game tone maps to the new peak.
        UpdateHdrMetadata();
    }

    // Queued SDR frames can retain an older underlying format after surface recreation.
    if (transfer_source_format != frame->image_format) {
        ConfigureTransfer(frame);
    }
    if (transfer_mode == TransferMode::Unsupported) {
        DiscardFrame(frame);
        return;
    }
    if (!swapchain.IsCreated()) {
        DiscardFrame(frame);
        return;
    }

    // The FLUID present thread times its presents itself (not after its watchdog fell back).
    const bool pace = !Fluid();
    static const bool shown_stats = std::getenv("EDEN_NX2_SHOWN_STATS") != nullptr;
    const bool timed = shown_stats && Fluid();
    const auto ms_since = [](std::chrono::steady_clock::time_point from) {
        return std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - from)
            .count();
    };
    const auto acquire_start = std::chrono::steady_clock::now();
    while (swapchain.AcquireNextImage(!use_present_thread, pace)) {
        if (!RecreateSwapchain(frame)) {
            return;
        }
    }
    if (timed) {
        const f64 acquire = ms_since(acquire_start);
        fluid_stats.acquire_ms += acquire;
        fluid_stats.acquire_max_ms = (std::max)(fluid_stats.acquire_max_ms, acquire);
    }

    const VkImage image{swapchain.CurrentImage()};
    const VkExtent2D extent = swapchain.GetExtent();
    const VkSemaphore render_semaphore = swapchain.CurrentRenderSemaphore();
    // The debug server's shown_frames needs a blit for its pictures, which the dedicated queue's
    // family cannot do: while it captures them the copy runs on the graphics queue.
    if (UseFluidQueue(frame, extent) && !VideoCore::Nx2ShownFrames::PicturesArmed()) {
        SubmitFluidCopy(frame, image, extent, render_semaphore);
        bool streamline_frame = false;
#ifdef EGLANTINE_STREAMLINE
        streamline_frame = frame->frame_gen;
#endif
        if (fluid_copy_ahead_request && !streamline_frame) {
            // Presented at the next slot's wake (FluidPresentAhead).
            fluid_ahead_pending = true;
            fluid_ahead_frame = frame;
            fluid_ahead_semaphore = render_semaphore;
            return;
        }
        if (FluidQueueHostWait()) {
            // A copy still running after this long is late anyway: present and move on.
            const auto wait_start = std::chrono::steady_clock::now();
            const bool copied =
                frame->present_done.Wait(FluidSlots::CopyWaitNs(
                    FluidHostWaitNs(FluidShownRate()), fluid.copy_wait_due - wait_start)) ==
                VK_SUCCESS;
            if (timed) {
                const f64 waited = ms_since(wait_start);
                ++fluid_stats.copy_waits;
                fluid_stats.copy_wait_timeouts += copied ? 0 : 1;
                fluid_stats.copy_wait_ms += waited;
                fluid_stats.copy_wait_max_ms = (std::max)(fluid_stats.copy_wait_max_ms, waited);
            }
        }
    } else {
        const vk::CommandBuffer cmdbuf{frame->cmdbuf};
        cmdbuf.Begin({
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .pNext = nullptr,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
            .pInheritanceInfo = nullptr,
        });

        const std::array pre_barriers{
            VkImageMemoryBarrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = 0,
                .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = image,
                .subresourceRange{
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            },
            VkImageMemoryBarrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = *frame->image,
                .subresourceRange{
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            },
        };
        const std::array post_barriers{
            VkImageMemoryBarrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = image,
                .subresourceRange{
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            },
            VkImageMemoryBarrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                .dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = *frame->image,
                .subresourceRange{
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            },
        };

        cmdbuf.PipelineBarrier(vk::PIPELINE_STAGE_GRAPHICS_COMPUTE_TRANSFER, VK_PIPELINE_STAGE_TRANSFER_BIT, {},
                               {}, {}, pre_barriers);

        if (transfer_mode == TransferMode::Blit) {
            cmdbuf.BlitImage(*frame->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             MakeImageBlit(frame->width, frame->height, extent.width, extent.height),
                             VK_FILTER_LINEAR);
        } else {
            cmdbuf.CopyImage(*frame->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             MakeImageCopy(frame->width, frame->height, extent.width, extent.height));
        }
        if (VideoCore::Nx2ShownFrames::Armed()) {
            shown_capture.Record(cmdbuf, *frame);
        }

        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, {},
                               {}, {}, post_barriers);

        cmdbuf.End();

        const VkSemaphore present_semaphore = swapchain.CurrentPresentSemaphore();
        const std::array wait_semaphores = {present_semaphore, *frame->render_ready};

        static constexpr std::array<VkPipelineStageFlags, 2> wait_stage_masks{
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
        };

        const VkSubmitInfo submit_info{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .pNext = nullptr,
            .waitSemaphoreCount = 2U,
            .pWaitSemaphores = wait_semaphores.data(),
            .pWaitDstStageMask = wait_stage_masks.data(),
            .commandBufferCount = 1,
            .pCommandBuffers = cmdbuf.address(),
            .signalSemaphoreCount = 1U,
            .pSignalSemaphores = &render_semaphore,
        };

        // Submit the image copy/blit to the swapchain
        {
            std::scoped_lock submit_lock{scheduler.submit_mutex};
            switch (const VkResult result =
                        device.GetGraphicsQueue().Submit(submit_info, *frame->present_done)) {
            case VK_SUCCESS:
                break;
            case VK_ERROR_DEVICE_LOST:
                device.ReportLoss();
                [[fallthrough]];
            default:
                vk::Check(result);
                break;
            }
        }
    }

    // Present
#ifdef EGLANTINE_STREAMLINE
    if (frame->frame_gen) {
        Streamline::FrameGenBeforePresent({
            .frame = frame->frame_gen_frame,
            .guest_rect = frame->guest_rect,
            .frame_extent = {frame->width, frame->height},
            .swapchain_extent = extent,
            .swapchain_format = swapchain.GetImageFormat(),
            .swapchain_images = static_cast<u32>(swapchain.GetImageCount()),
        });
    }
#endif
    swapchain.Present(render_semaphore);
    if (timed) {
        const f64 lock = static_cast<f64>(swapchain.LastPresentLockNs()) * 1.0e-6;
        const f64 call = static_cast<f64>(swapchain.LastPresentCallNs()) * 1.0e-6;
        ++fluid_stats.presents;
        fluid_stats.lock_ms += lock;
        fluid_stats.lock_max_ms = (std::max)(fluid_stats.lock_max_ms, lock);
        fluid_stats.call_ms += call;
        fluid_stats.call_max_ms = (std::max)(fluid_stats.call_max_ms, call);
    }
#ifdef EGLANTINE_STREAMLINE
    if (frame->frame_gen) {
        Streamline::FrameGenAfterPresent(frame->frame_gen_frame.index);
    }
#endif
}

} // namespace Vulkan
