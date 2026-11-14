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
#include "video_core/renderer_vulkan/vk_shown_capture.h"
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

/// EDEN_NX2_FLUID_COMPOSE. 0: off. 1: with the FLUID presenter and a dedicated queue that can
/// compute, the slot's frame is scaled into the swapchain image by a compute pass, and the frames
/// are drawn at the game's internal size (EDEN_NX2_FLUID_COMPOSE_NATIVE keeps them at the window's).
[[nodiscard]] u32 FluidComposeMode();

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
    /// EDEN_NX2_FLUID_QUEUE: the screen copy recorded for the dedicated queue.
    vk::CommandBuffer fluid_cmdbuf;
    vk::Semaphore render_ready;
    vk::Fence present_done;
    /// EDEN_NX2_SHOWN_STATS: the screen copy wrote GPU timestamps (queries 2 index and 2 index + 1).
    bool copy_timed{};
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
    /// A real frame no prediction follows (a camera cut, no motion, HDR): frames predicted from
    /// older real frames are stale once it is shown.
    bool fluid_final{};
    /// When that real frame was recorded.
    std::chrono::steady_clock::time_point fluid_recorded{};
    /// The scheduler tick whose completion finishes the frame's GPU work.
    u64 gpu_tick{};
    /// A real frame, a prediction or an interpolated frame (the debug server's shown_frames).
    VideoCore::Nx2ShownFrames::Kind shown_kind{};
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

    /// Whether the present thread shows frames on the vblank clock (EDEN_NX2_FLUID); false once
    /// its watchdog fell back to presenting in arrival order.
    [[nodiscard]] bool Fluid() const {
        return fluid_presenter && use_present_thread &&
               !fluid_fallback.load(std::memory_order_relaxed);
    }

    /// Renderer thread: one real frame's predictions were recorded (counted for the Fluid
    /// presenter line). `predictable`: the frame could be predicted from; `cut_short`: the frame
    /// pool had none to spare for a slot the batch covers; `capped`: the prediction limit ended
    /// the batch before its last slot.
    void NoteFluidBatch(bool predictable, bool cut_short, bool capped) {
        ++fluid_batches;
        fluid_unpredictable += predictable ? 0 : 1;
        fluid_cut_short += cut_short ? 1 : 0;
        fluid_capped += capped ? 1 : 0;
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

    /// The FLUID presenter's counters for the debug server's present_stats tool: a copy of
    /// fluid_stats (EDEN_NX2_SHOWN_STATS fills it; zero without it) plus the vblank clock and
    /// the present queue's depth. Any thread.
    struct PresentStats {
        bool fluid{};          ///< The present thread shows frames on the vblank clock
        bool fallback{};       ///< Its watchdog fell back to arrival order
        bool shown_stats{};    ///< EDEN_NX2_SHOWN_STATS: the counters below fill
        u64 slots{};
        u64 hits{};
        u64 no_frame{};
        u64 not_done{};
        u64 uncovered{};
        u64 rolled{};
        u64 spare{};
        u64 stale{};
        u64 queued{};
        u64 overslept{};
        u64 in_order{};
        u64 late{};
        u64 dropped{};
        u64 received{};
        std::array<u64, 4> held{}; ///< Frames shown for 1, 2, 3 and 4+ refreshes
        double age_ms{};           ///< Sum of real-frame ages at display (divide by hits)
        double latency_ms{};       ///< Sum of record-to-GPU-done times (divide by samples)
        u64 latency_samples{};
        u64 presents{};
        double acquire_ms{}, acquire_max_ms{};
        u64 copy_waits{}, copy_wait_timeouts{};
        double copy_wait_ms{}, copy_wait_max_ms{};
        double lock_ms{}, lock_max_ms{};
        double call_ms{}, call_max_ms{};
        double margin_ms{}, margin_min_ms{};
        u64 margin_samples{}, after_vblank{};
        u64 copy_gpu_samples{};
        double copy_gpu_ms{}, copy_gpu_max_ms{};
        /// The vblank clock as of the present thread's last wake (zero until it counted one).
        double refresh_hz{};
        u64 vblank_index{};
        u64 divisor{};
        double lead_ms{};
        double clock_latency_ms{};
        /// Frames handed to the present thread and not shown yet.
        std::size_t queued_frames{};
        std::size_t free_frames{};
        std::size_t frame_count{};
        std::size_t swapchain_images{};
    };
    [[nodiscard]] PresentStats GetPresentStats() const;

private:
    enum class TransferMode { Unsupported, Copy, Blit };

    void StartPresentThread();

    void PresentThread(std::stop_token token);

    /// EDEN_NX2_FLUID: at every vblank, takes the frames sent, frees those no slot shows and
    /// shows the frames without a slot in arrival order; at the vblank before each slot, shows
    /// the newest real frame's frame for the slot whose GPU work is done. False when its watchdog
    /// gave up: the thread then presents in arrival order as without FLUID.
    bool FluidPresentThread(std::stop_token token);

    /// Frames came but none was shown for FluidStallLimit, and frames still wait or keep coming.
    [[nodiscard]] bool FluidStalled(std::chrono::steady_clock::time_point now) const;

    /// Logs why, then hands the frames held to the arrival-order loop, for the rest of the session.
    void FluidFallBack(std::chrono::steady_clock::time_point now);

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

    /// EDEN_NX2_PREDICT (vk_present_predict.cpp): before the slot's frame is chosen, waits a
    /// moment for a real frame that is nearly done; and counts the slot's outcome.
    void FluidPredictHold(u64 slot, u64 after);
    void FluidPredictNote(u64 slot, const Frame* best);

    /// Copies the frame to the swapchain; it stays the frame showing until its copy is done.
    /// Holds swapchain_mutex.
    void FluidShow(Frame* frame, u64 shown_vblank);
    /// EDEN_NX2_FLUID_COPY_AHEAD: presents the frame copied at the last slot's wake, if any.
    void FluidPresentAhead();

    /// Frees a frame that is not shown. Holds swapchain_mutex.
    void FluidDiscard(Frame* frame);

    void ReturnFrame(Frame* frame);

    /// EDEN_NX2_SHOWN_STATS: logs the slots of every 10 s.
    void FluidLogStats(std::chrono::steady_clock::time_point now, const char* source,
                       std::chrono::nanoseconds period);

    void NoteShownFrame();

    /// The debug server's shown_frames: notes the frame just presented (`slot` 0 when shown in
    /// arrival order). Called only while VideoCore::Nx2ShownFrames::Armed().
    void NoteShownCapture(const Frame* frame, u64 slot, std::string_view presenter);

    void CopyToSwapchain(Frame* frame);

    void CopyToSwapchainImpl(Frame* frame);

    /// EDEN_NX2_FLUID_QUEUE: whether this frame's screen copy runs on the dedicated queue.
    [[nodiscard]] bool UseFluidQueue(const Frame* frame, VkExtent2D extent);

    /// Copies the frame to the acquired swapchain image on the dedicated queue, waiting for the
    /// acquire and render-done semaphores and signalling render_semaphore and the frame's fence.
    void SubmitFluidCopy(Frame* frame, VkImage image, VkExtent2D extent,
                         VkSemaphore render_semaphore);

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
    vk::CommandPool fluid_cmdpool; ///< EDEN_NX2_FLUID_QUEUE: pool of the dedicated queue family
    /// EDEN_NX2_SHOWN_STATS with the dedicated queue: timestamps around each screen copy.
    vk::QueryPool fluid_copy_queries;
    f64 fluid_timestamp_ns{};
    u64 fluid_timestamp_mask{};
    bool fluid_queue_mismatch_logged{};
    std::vector<Frame> frames;
    std::vector<f32> shown_intervals; ///< EDEN_NX2_SHOWN_STATS, present thread only
    std::chrono::steady_clock::time_point shown_last{};
    std::chrono::steady_clock::time_point shown_window{};
    boost::container::deque<Frame*> present_queue;
    boost::container::deque<Frame*> free_queue;
    std::condition_variable_any frame_cv;
    std::condition_variable free_cv;
    mutable std::mutex swapchain_mutex;
    mutable std::mutex queue_mutex;
    mutable std::mutex free_mutex;
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
    /// EDEN_NX2_FLUID's watchdog fell back to arrival order. Set under queue_mutex.
    std::atomic<bool> fluid_fallback{};
    std::atomic<u64> fluid_batches{};
    std::atomic<u64> fluid_unpredictable{};
    std::atomic<u64> fluid_cut_short{};
    std::atomic<u64> fluid_capped{};

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
        /// A screen copy wait ends by then, so the next slot's wake is not slept through.
        std::chrono::steady_clock::time_point copy_wait_due{};
        std::array<s64, 15> intervals{}; ///< between the last wakes, in ns
        size_t interval_count{};
        size_t interval_next{};
        u64 last_slot{}; ///< the last slot handled
        u64 last_shown_vblank{};
        u64 watched_batch{};
        u64 newest_batch{}; ///< the newest real frame's batch received
        std::vector<FluidWatch> watches;
        f64 latency_ns{};
        /// Since when frames wait unshown (the watchdog): the first received after the last show,
        /// or that show if frames were still held; zero when none.
        std::chrono::steady_clock::time_point unshown_since{};
        std::chrono::steady_clock::time_point last_received{};
        u64 received{}; ///< since the thread started
        u64 shown{};
    } fluid;
    /// EDEN_NX2_SHOWN_STATS with EDEN_NX2_FLUID, present thread only.
    struct {
        std::chrono::steady_clock::time_point since{};
        u64 slots{};
        u64 hits{};
        u64 no_frame{};
        u64 not_done{};   ///< of no_frame: a frame was scheduled for the slot, its GPU work was not done
        u64 uncovered{};  ///< of no_frame: no frame was scheduled for the slot
        u64 rolled{};     ///< late or blocked frames moved to the next slot instead of dropped
        u64 spare{};      ///< shown frames predicted from an older real frame than the newest
        u64 stale{};      ///< frames dropped as older than a real frame no prediction follows
        u64 queued{};
        u64 overslept{};
        u64 in_order{};
        u64 late{};
        u64 dropped{};
        u64 received{};
        std::array<u64, 4> held{}; ///< frames shown for 1, 2, 3 and 4+ refreshes
        u64 one_slot{};            ///< of those, frames shown for exactly one slot's refreshes
        f64 age_ms{};
        f64 latency_ms{};
        u64 latency_samples{};
        // Where the present thread's time goes, in ms: sums and maxima over `presents`.
        u64 presents{};
        f64 acquire_ms{}, acquire_max_ms{};
        u64 copy_waits{}, copy_wait_timeouts{};
        f64 copy_wait_ms{}, copy_wait_max_ms{};
        f64 lock_ms{}, lock_max_ms{};
        f64 call_ms{}, call_max_ms{};
        // How long before its slot's vblank each slot present returned; negative after it.
        f64 margin_ms{}, margin_min_ms{};
        u64 margin_samples{}, after_vblank{};
        // GPU time the screen copy ran for once it started (its timestamps), in ms.
        u64 copy_gpu_samples{};
        f64 copy_gpu_ms{}, copy_gpu_max_ms{};
    } fluid_stats;
    void FluidNoteCopyTime(Frame* frame);
    /// EDEN_NX2_FLUID_COPY_AHEAD: the next copy is for the slot after next and is presented at the
    /// next slot's wake; a copy is pending presentation.
    bool fluid_copy_ahead_request{};
    bool fluid_ahead_pending{};
    Frame* fluid_ahead_frame{};
    VkSemaphore fluid_ahead_semaphore{};
    /// The debug server's shown_frames: pictures and timing of the frames shown.
    ShownCapture shown_capture;
};

} // namespace Vulkan
