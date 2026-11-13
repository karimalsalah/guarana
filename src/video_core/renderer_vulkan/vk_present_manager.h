// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <vector>
#include <boost/container/deque.hpp>

#include "common/common_types.h"
#include "common/polyfill_thread.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"
#ifdef EGLANTINE_STREAMLINE
#include "video_core/vulkan_common/streamline_frame_gen.h"
#endif

namespace Core::Frontend {
class EmuWindow;
} // namespace Core::Frontend

namespace Vulkan {

class Device;
class Scheduler;
class Swapchain;

/// EDEN_NX2_FLUID. 0: off. 1: the present thread shows frames on a vblank clock, one per slot of
/// round(refresh / 60) vblanks (16.7 ms at 60 or 120 Hz), and frame extrapolation records a
/// prediction for each slot until the next real frame is due, as of the slot's own time. 2: the
/// same, but each prediction is as of its slot less the latency a real frame can be shown at, so
/// the real frame itself is shown when it is on time and predictions reach less far.
[[nodiscard]] u32 FluidMode();

struct Frame {
    u32 width;
    u32 height;
    u32 index;
    // Requested presentation view format and underlying image format may differ for SDR.
    VkFormat format{VK_FORMAT_UNDEFINED};
    VkFormat image_format{VK_FORMAT_UNDEFINED};
    VkColorSpaceKHR color_space{VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    vk::Image image;
    vk::ImageView image_view;
    vk::ImageView storage_view;
    vk::Framebuffer framebuffer;
    vk::CommandBuffer cmdbuf;
    vk::Semaphore render_ready;
    vk::Fence present_done;
    bool storage_capable{};
    /// Time the present thread leaves before presenting the next frame. Frame generation sets it
    /// on generated frames, so they share the screen time evenly with the real ones.
    std::chrono::nanoseconds present_spacing{};
    /// EDEN_NX2_FLUID: the slot (a vblank, PresentManager::FluidClock numbering) the frame is
    /// shown at. 0: none, shown in arrival order. FLUID_NOT_SHOWN: a real frame only its
    /// predictions show.
    u64 fluid_slot{};
    /// The real frame it is or was predicted from; a newer one's frame for a slot replaces it.
    u64 fluid_batch{};
    /// When that real frame was recorded.
    std::chrono::steady_clock::time_point fluid_recorded{};
    /// The scheduler tick whose completion finishes the frame's GPU work.
    u64 gpu_tick{};
#ifdef EGLANTINE_STREAMLINE
    // DLSS Frame Generation: the guest frame presented and where the guest image is in the frame.
    bool frame_gen{};
    Streamline::FrameGenFrame frame_gen_frame{};
    VkRect2D guest_rect{};
#endif
};

inline constexpr u64 FLUID_NOT_SHOWN = ~u64{0};

class PresentManager {
public:
    /// EDEN_NX2_FLUID: when the present thread shows frames, as of the vblank it last woke for.
    /// Slots are the vblanks whose number is a multiple of divisor.
    struct FluidClock {
        std::chrono::steady_clock::time_point vblank{}; ///< when the present thread woke for it
        u64 vblank_index{};
        std::chrono::nanoseconds refresh{}; ///< zero until the present thread counted a vblank
        u64 divisor{1};
        std::chrono::nanoseconds lead{}; ///< a slot's frame is presented this long before it
        std::chrono::nanoseconds latency{}; ///< from recording a real frame to its GPU work done

        [[nodiscard]] bool Valid() const {
            return refresh.count() > 0;
        }

        /// When the slot's vblank comes.
        [[nodiscard]] std::chrono::steady_clock::time_point SlotTime(u64 slot) const;

        /// The first slot whose frame is presented at or after time.
        [[nodiscard]] u64 FirstSlotPresentedAfter(std::chrono::steady_clock::time_point time) const;
    };

    PresentManager(const vk::Instance& instance,
                   Core::Frontend::EmuWindow& render_window,
                   const Device& device,
                   MemoryAllocator& memory_allocator,
                   Scheduler& scheduler,
                   Swapchain& swapchain,
                   vk::SurfaceKHR& surface);
    ~PresentManager();

    /// Returns the last used presentation frame
    Frame* GetRenderFrame();

    /// EDEN_NX2_FLUID: a frame to predict into without waiting, or null when every free frame is
    /// still in use by the GPU or only one is left (for the next real frame).
    [[nodiscard]] Frame* TryGetRenderFrame();

    /// Whether the present thread shows frames on the vblank clock (EDEN_NX2_FLUID).
    [[nodiscard]] bool Fluid() const {
        return fluid_presenter && use_present_thread;
    }

    /// Any thread: the vblank clock as of the present thread's last wake.
    [[nodiscard]] FluidClock GetFluidClock() const;

    /// The time GetRenderFrame calls waited for a frame since the last call of this (the
    /// present thread holding the renderer back).
    [[nodiscard]] std::chrono::steady_clock::duration TakeBlockedTime() {
        return std::chrono::steady_clock::duration{
            blocked_time.exchange(0, std::memory_order_relaxed)};
    }

    /// Pushes a frame for presentation
    void Present(Frame* frame);

    /// Recreates the present frame to match the provided parameters
    void RecreateFrame(Frame* frame, u32 width, u32 height, VkFormat image_view_format,
                       VkRenderPass rd, bool storage);

    [[nodiscard]] bool NeedsStorage(const Frame* frame, bool required) const;

    /// Selects the backing image format, preserving the cached SDR format or explicit HDR format.
    [[nodiscard]] VkFormat GetFrameImageFormat(VkFormat image_view_format) const;

    /// Waits for the present thread to finish presenting all queued frames.
    void WaitPresent();

    /// How many additional frames can be queued without stalling the render thread
    [[nodiscard]] size_t MaxExtraFrames() const;

    [[nodiscard]] std::size_t SwapchainImageCount() const { return swapchain_image_count; }
    [[nodiscard]] VkFormat SwapchainImageFormat() const { return swapchain_image_format; }

private:
    enum class TransferMode { Unsupported, Copy, Blit };

    void StartPresentThread();

    void PresentThread(std::stop_token token);

    /// EDEN_NX2_FLUID: at every vblank, takes the frames sent, frees those no slot shows and
    /// shows the frames without a slot in arrival order; at the vblank before each slot, shows
    /// the newest real frame's frame for the slot whose GPU work is done.
    void FluidPresentThread(std::stop_token token);

    /// Counts the vblanks since the last wake; returns the refresh period.
    std::chrono::nanoseconds FluidCountVblanks(std::chrono::steady_clock::time_point wake,
                                               std::chrono::nanoseconds reported);

    /// Takes the frames sent, frees those only predictions show and answers WaitPresent.
    void FluidTakeFrames(std::chrono::steady_clock::time_point now);

    /// Watches the GPU work of real frames, gives the last shown frame back once its copy is
    /// done, and frees frames whose slot passed or that a newer real frame's frame replaces.
    void FluidRelease(std::chrono::steady_clock::time_point now);

    /// Shows the oldest frame without a slot if it can be shown now; true if it was.
    bool FluidPresentUnscheduled(std::chrono::steady_clock::time_point now);

    /// Shows the slot's frame (none when !count, which passes the slot) and frees the rest up to
    /// the slot.
    void FluidPresentSlot(u64 slot, std::chrono::steady_clock::time_point slot_time, bool count);

    /// Copies the frame to the swapchain; it stays the frame showing until its copy is done.
    /// Holds swapchain_mutex.
    void FluidShow(Frame* frame, u64 shown_vblank);

    /// Frees a frame that is not shown. Holds swapchain_mutex.
    void FluidDiscard(Frame* frame);

    void ReturnFrame(Frame* frame);

    /// EDEN_NX2_SHOWN_STATS: logs the slots of every 10 s.
    void FluidLogStats(std::chrono::steady_clock::time_point now, const char* source,
                       std::chrono::nanoseconds period);

    void NoteShownFrame();

    void CopyToSwapchain(Frame* frame);

    void CopyToSwapchainImpl(Frame* frame);

    // Returns false after discarding the frame if creation or transfer cannot proceed.
    bool RecreateSwapchain(Frame* frame);

    /// Caches a copy, blit, or unsupported transfer decision for this frame and active swapchain.
    void ConfigureTransfer(const Frame* frame);

    void DiscardFrame(Frame* frame);

    /// Sends the HDR10 metadata of the monitor's current state to an HDR10 swapchain.
    void UpdateHdrMetadata();

    void SetImageCount();

    /// With EDEN_DKB_FRAME_TRACE=1 and frame generation on, logs the presents of every 10 s.
    void LogPacing(const Frame* frame, std::chrono::steady_clock::duration took,
                   std::chrono::steady_clock::time_point now);

private:
    const vk::Instance& instance;
    Core::Frontend::EmuWindow& render_window;
    const Device& device;
    MemoryAllocator& memory_allocator;
    Scheduler& scheduler;
    Swapchain& swapchain;
    vk::SurfaceKHR& surface;
    vk::CommandPool cmdpool;
    std::vector<Frame> frames;
    std::vector<f32> shown_intervals; ///< EDEN_NX2_SHOWN_STATS, present thread only
    std::chrono::steady_clock::time_point shown_last{};
    std::chrono::steady_clock::time_point shown_window{};
    boost::container::deque<Frame*> present_queue;
    boost::container::deque<Frame*> free_queue;
    std::condition_variable_any frame_cv;
    std::condition_variable free_cv;
    std::mutex swapchain_mutex;
    std::mutex queue_mutex;
    std::mutex free_mutex;
    std::jthread present_thread;
    VkFormat transfer_source_format{VK_FORMAT_UNDEFINED};
    TransferMode transfer_mode{TransferMode::Unsupported};
    u64 hdr_metadata_generation{}; ///< DisplayHdrMonitor generation of the metadata sent
    bool storage_supported;
    bool use_present_thread;
    const bool fluid_presenter; ///< EDEN_NX2_FLUID: the present thread runs FluidPresentThread
    std::atomic<std::size_t> image_count{};
    std::atomic<std::size_t> swapchain_image_count{};
    std::atomic<VkFormat> swapchain_image_format{};
    /// The present thread presents no frame before this (Frame::present_spacing).
    std::chrono::steady_clock::time_point next_present{};
    struct {
        std::chrono::steady_clock::time_point since{};
        u64 real{};
        u64 generated{};
        std::chrono::nanoseconds spacing{};
        std::chrono::steady_clock::duration present_time{};
    } pacing_log;
    /// See TakeBlockedTime. Atomic: a composite can also run from a syncpoint action on
    /// another thread (GPU::RequestComposite).
    std::atomic<std::chrono::steady_clock::rep> blocked_time{};

    mutable std::mutex fluid_clock_mutex;
    FluidClock fluid_clock; ///< guarded by fluid_clock_mutex
    u64 fluid_drain_requests{}; ///< guarded by queue_mutex: WaitPresent calls
    u64 fluid_drains{};         ///< guarded by queue_mutex: those the present thread answered

    struct FluidWatch {
        u64 tick;
        std::chrono::steady_clock::time_point recorded;
        std::chrono::steady_clock::time_point unready; ///< the last look found it not done
    };
    /// EDEN_NX2_FLUID, present thread only.
    struct {
        std::vector<Frame*> pending; ///< received, neither shown nor freed
        std::vector<Frame*> freeing;
        Frame* showing{}; ///< the last frame shown, until its copy is done
        u64 showing_vblank{};
        bool showing_late{};
        u64 vblank{}; ///< vblanks counted, the last one woken for
        u64 divisor{1};
        std::chrono::steady_clock::time_point last_wake{};
        std::array<s64, 15> intervals{}; ///< between the last wakes, in ns
        size_t interval_count{};
        size_t interval_next{};
        u64 last_slot{}; ///< the last slot handled
        u64 last_shown_vblank{};
        u64 watched_batch{};
        std::vector<FluidWatch> watches;
        f64 latency_ns{};
    } fluid;
    /// EDEN_NX2_SHOWN_STATS with EDEN_NX2_FLUID, present thread only.
    struct {
        std::chrono::steady_clock::time_point since{};
        u64 slots{};
        u64 hits{};
        u64 no_frame{};
        u64 queued{};
        u64 overslept{};
        u64 in_order{};
        u64 late{};
        u64 dropped{};
        std::array<u64, 4> held{}; ///< frames shown for 1, 2, 3 and 4+ refreshes
        f64 age_ms{};
        f64 latency_ms{};
        u64 latency_samples{};
    } fluid_stats;
};

} // namespace Vulkan
