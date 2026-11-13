// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <thread>
#include <vector>

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
#include "video_core/nx2_profile.h"
#ifdef HAS_LSFG
#include "video_core/renderer_vulkan/present/lsfg_common.h"
#endif
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
/// present a few ms early or late still lands on the slot's vblank.
constexpr std::chrono::nanoseconds FluidPresentOffset{3'500'000};
/// Real frames whose GPU work is watched at once.
constexpr size_t FluidWatches = 8;

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

void ResetFrame(Frame* frame) {
    frame->present_done.Reset();
    frame->color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    frame->present_spacing = {};
    frame->fluid_slot = 0;
    frame->fluid_batch = 0;
    frame->fluid_recorded = {};
    frame->gpu_tick = 0;
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

    frames.resize(image_count);
    for (u32 i = 0; i < frames.size(); i++) {
        Frame& frame = frames[i];
        frame.index = i;
        frame.cmdbuf = vk::CommandBuffer{cmdbuffers[i], device.GetDispatchLoader()};
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
        free_queue.push_back(frame);
    }
}

void PresentManager::NoteShownFrame() {
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
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
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
        // vblank.
        std::unique_lock queue_lock{queue_mutex};
        const u64 request = ++fluid_drain_requests;
        frame_cv.wait(queue_lock, [this, request] { return fluid_drains >= request; });
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
    if (fluid_presenter) {
        FluidPresentThread(token);
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

            // Free the frame for reuse
            std::scoped_lock fl{free_mutex};
            free_queue.push_back(frame);
            free_cv.notify_one();
        }
    }
}

void PresentManager::FluidPresentThread(std::stop_token token) {
    Common::VblankClock vblank{render_window.GetWindowInfo().render_surface};
    auto source = vblank.CurrentSource();
    LOG_INFO(Render_Vulkan,
             "EDEN_NX2_FLUID {}: frames are shown on a vblank clock ({}, display mode {:.2f} Hz)",
             FluidMode(), Common::VblankClock::SourceName(source),
             HertzOf(vblank.ReportedPeriod()));
    while (!token.stop_requested()) {
        const auto wake = vblank.Wait();
        if (const auto current = vblank.CurrentSource(); current != source) {
            source = current;
            LOG_INFO(Render_Vulkan, "EDEN_NX2_FLUID: the vblank clock is now {}",
                     Common::VblankClock::SourceName(source));
        }
        const auto period = FluidCountVblanks(wake, vblank.ReportedPeriod());
        const auto offset = (std::min)(FluidPresentOffset, period * 42 / 100);
        {
            std::scoped_lock lock{fluid_clock_mutex};
            fluid_clock = FluidClock{
                .vblank = wake,
                .vblank_index = fluid.vblank,
                .refresh = period,
                .divisor = fluid.divisor,
                .lead = period - offset,
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
            FluidPresentSlot(fluid.vblank + 1, wake + period, !shown);
        }
        FluidLogStats(std::chrono::steady_clock::now(), Common::VblankClock::SourceName(source),
                      period);
    }
    // Nothing is presented from here on: answer WaitPresent.
    {
        std::scoped_lock lock{queue_mutex};
        fluid_drains = fluid_drain_requests;
    }
    frame_cv.notify_all();
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
    // Slots of about 16.7 ms: every round(refresh / 60) vblanks.
    const f64 per_slot = 1.0e9 / (60.0 * static_cast<f64>(period.count()));
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
        if (fluid.watches.size() >= FluidWatches) {
            fluid.watches.erase(fluid.watches.begin());
        }
        fluid.watches.push_back(FluidWatch{
            .tick = frame->gpu_tick,
            .recorded = frame->fluid_recorded,
            .unready = now,
        });
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
        const f64 sample =
            (std::max)(0.0, std::chrono::duration<f64, std::nano>(done - it->recorded).count());
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
                                   FluidSlotOf(other, fluid.divisor) == slot &&
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
    const u64 after = fluid.last_slot;
    fluid.last_slot = slot;
    Frame* best = nullptr;
    if (count) {
        if (after != 0 && slot > after + fluid.divisor) {
            // The present thread slept through these.
            const u64 skipped = (slot - after) / fluid.divisor - 1;
            fluid_stats.slots += skipped;
            fluid_stats.overslept += skipped;
        }
        ++fluid_stats.slots;
        // The newest real frame's frame for the slot whose GPU work is done.
        for (Frame* const frame : fluid.pending) {
            if (!FluidScheduled(frame) || frame->fluid_slot <= after || frame->fluid_slot > slot ||
                !scheduler.IsFree(frame->gpu_tick)) {
                continue;
            }
            if (best == nullptr || frame->fluid_batch > best->fluid_batch ||
                (frame->fluid_batch == best->fluid_batch && frame->fluid_slot > best->fluid_slot)) {
                best = frame;
            }
        }
        if (best == nullptr) {
            ++fluid_stats.no_frame;
        } else if (fluid.showing != nullptr) {
            // The last copy is still queued: presenting this one would queue a second.
            ++fluid_stats.queued;
            best = nullptr;
        }
    }
    std::scoped_lock swapchain_lock{swapchain_mutex};
    // The slot is over for the frames up to it.
    std::erase_if(fluid.pending, [this, slot, best](Frame* frame) {
        if (frame == best) {
            return true;
        }
        if (!FluidScheduled(frame) || frame->fluid_slot > slot) {
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
    fluid_stats.age_ms +=
        std::chrono::duration<f64, std::milli>(slot_time - best->fluid_recorded).count();
    FluidShow(best, slot);
}

void PresentManager::FluidShow(Frame* frame, u64 shown_vblank) {
    CopyToSwapchain(frame);
    fluid.showing = frame;
    fluid.showing_vblank = shown_vblank;
    fluid.showing_late = false;
    if (fluid.last_shown_vblank != 0 && shown_vblank > fluid.last_shown_vblank) {
        const u64 held = (std::min<u64>)(shown_vblank - fluid.last_shown_vblank, 4);
        ++fluid_stats.held[static_cast<size_t>(held - 1)];
    }
    fluid.last_shown_vblank = shown_vblank;
    NoteShownFrame();
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
    const u64 one_slot = fluid.divisor <= stats.held.size() ? stats.held[fluid.divisor - 1] : 0;
    const auto share = [](u64 part, u64 whole) {
        return whole != 0 ? 100.0 * static_cast<f64>(part) / static_cast<f64>(whole) : 0.0;
    };
    const auto mean = [](f64 sum, u64 samples) {
        return samples != 0 ? sum / static_cast<f64>(samples) : 0.0;
    };
    LOG_INFO(Render_Vulkan,
             "Fluid presenter (10 s, {} at {:.2f} Hz, a slot every {} refreshes): {} slots, {} "
             "shown on their slot, {} missed ({} with no frame ready, {} with the last copy still "
             "queued, {} slept through), {} shown in arrival order, {} copies done after their "
             "vblank",
             source, HertzOf(period), fluid.divisor, stats.slots, stats.hits,
             stats.no_frame + stats.queued + stats.overslept, stats.no_frame, stats.queued,
             stats.overslept, stats.in_order, stats.late);
    LOG_INFO(Render_Vulkan,
             "Fluid presenter (10 s): {:.0f}% of shown frames held one slot, held 1/2/3/4+ "
             "refreshes {}/{}/{}/{}; real frame age at display {:.1f} ms; GPU work done {:.1f} ms "
             "after recording ({} real frames); {} frames dropped unshown",
             share(one_slot, held), stats.held[0], stats.held[1], stats.held[2], stats.held[3],
             mean(stats.age_ms, stats.hits), mean(stats.latency_ms, stats.latency_samples),
             stats.latency_samples, stats.dropped);
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
        image_count = MAX_FRAMES_IN_FLIGHT;
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

void PresentManager::CopyToSwapchainImpl(Frame* frame) {

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

    // The FLUID present thread times its presents itself.
    const bool pace = !(fluid_presenter && use_present_thread);
    while (swapchain.AcquireNextImage(!use_present_thread, pace)) {
        if (!RecreateSwapchain(frame)) {
            return;
        }
    }

    const vk::CommandBuffer cmdbuf{frame->cmdbuf};
    cmdbuf.Begin({
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    });

    const VkImage image{swapchain.CurrentImage()};
    const VkExtent2D extent = swapchain.GetExtent();
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

    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, {},
                           {}, {}, post_barriers);

    cmdbuf.End();

    const VkSemaphore present_semaphore = swapchain.CurrentPresentSemaphore();
    const VkSemaphore render_semaphore = swapchain.CurrentRenderSemaphore();
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
#ifdef EGLANTINE_STREAMLINE
    if (frame->frame_gen) {
        Streamline::FrameGenAfterPresent(frame->frame_gen_frame.index);
    }
#endif
}

} // namespace Vulkan
