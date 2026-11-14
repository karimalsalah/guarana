// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <variant>

#include "common/dynamic_library.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/renderer_base.h"
#ifdef HAS_LSFG
#include "video_core/renderer_vulkan/present/frame_gen.h"
#endif
#include "video_core/renderer_vulkan/vk_blit_screen.h"
#include "video_core/renderer_vulkan/vk_present_manager.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_state_tracker.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/renderer_vulkan/vk_turbo_mode.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"
#if defined(EGLANTINE_STREAMLINE) && !defined(HAS_LSFG)
#include "video_core/vulkan_common/streamline.h"
#endif

namespace Core::Memory {
class Memory;
}

namespace Tegra {
class GPU;
}

namespace Vulkan {

Device CreateDevice(const vk::Instance& instance, const vk::InstanceDispatch& dld,
                    VkSurfaceKHR surface,
                    const std::function<void(const Device&)>& on_capabilities = {});

class RendererVulkan final : public VideoCore::RendererBase {
public:
    explicit RendererVulkan(Core::Frontend::EmuWindow& emu_window,
                            Tegra::MaxwellDeviceMemoryManager& device_memory_, Tegra::GPU& gpu_,
                            std::unique_ptr<Core::Frontend::GraphicsContext> context_);
    ~RendererVulkan() override;

    void Composite(std::span<const Tegra::FramebufferConfig> framebuffers) override;

    std::vector<u8> GetAppletCaptureBuffer() override;

    VideoCore::RasterizerInterface* ReadRasterizer() override {
        return &rasterizer;
    }

    [[nodiscard]] std::string GetDeviceVendor() const override {
        return device.GetDriverName();
    }

    [[nodiscard]] VideoCore::FrameGenStatus GetFrameGenStatus() const override {
#ifdef HAS_LSFG
        return frame_gen.CurrentStatus();
#else
#ifdef EGLANTINE_STREAMLINE
        if (Streamline::FrameGenActive()) {
            return {.state = VideoCore::FrameGenState::NvidiaFrameGen};
        }
#endif
        return {.state = VideoCore::FrameGenState::UnsupportedBuild};
#endif
    }

#ifdef EGLANTINE_STREAMLINE
    [[nodiscard]] VideoCore::DlssStatus GetDlssStatus() const override;
#endif

    void RequestGpuBusySample() override {
        scheduler.RequestGpuBusySample();
    }

    [[nodiscard]] std::optional<double> GetGpuBusyMs() const override {
        return scheduler.GpuBusyMs();
    }

    bool RequestGpuCostSampling(bool on) override {
        return scheduler.RequestCostSampling(on);
    }

    [[nodiscard]] bool IsGpuCostSampling() const override {
        return scheduler.CostSampling();
    }

    [[nodiscard]] VideoCore::GpuCostSnapshot GetGpuCosts() const override {
        return scheduler.GpuCosts();
    }

    [[nodiscard]] std::optional<VideoCore::RendererBase::PresentStatsView>
    GetPresentStats() const override {
        const PresentManager::PresentStats s = present_manager.GetPresentStats();
        VideoCore::RendererBase::PresentStatsView view{};
        view.fluid = s.fluid;
        view.fallback = s.fallback;
        view.shown_stats = s.shown_stats;
        view.slots = s.slots;
        view.hits = s.hits;
        view.no_frame = s.no_frame;
        view.not_done = s.not_done;
        view.uncovered = s.uncovered;
        view.rolled = s.rolled;
        view.spare = s.spare;
        view.stale = s.stale;
        view.queued = s.queued;
        view.overslept = s.overslept;
        view.in_order = s.in_order;
        view.late = s.late;
        view.dropped = s.dropped;
        view.received = s.received;
        view.held = s.held;
        view.age_ms = s.age_ms;
        view.latency_ms = s.latency_ms;
        view.latency_samples = s.latency_samples;
        view.presents = s.presents;
        view.acquire_ms = s.acquire_ms;
        view.acquire_max_ms = s.acquire_max_ms;
        view.copy_waits = s.copy_waits;
        view.copy_wait_timeouts = s.copy_wait_timeouts;
        view.copy_wait_ms = s.copy_wait_ms;
        view.copy_wait_max_ms = s.copy_wait_max_ms;
        view.lock_ms = s.lock_ms;
        view.lock_max_ms = s.lock_max_ms;
        view.call_ms = s.call_ms;
        view.call_max_ms = s.call_max_ms;
        view.margin_ms = s.margin_ms;
        view.margin_min_ms = s.margin_min_ms;
        view.margin_samples = s.margin_samples;
        view.after_vblank = s.after_vblank;
        view.copy_gpu_samples = s.copy_gpu_samples;
        view.copy_gpu_ms = s.copy_gpu_ms;
        view.copy_gpu_max_ms = s.copy_gpu_max_ms;
        view.refresh_hz = s.refresh_hz;
        view.vblank_index = s.vblank_index;
        view.divisor = s.divisor;
        view.lead_ms = s.lead_ms;
        view.clock_latency_ms = s.clock_latency_ms;
        view.queued_frames = s.queued_frames;
        view.free_frames = s.free_frames;
        view.frame_count = s.frame_count;
        view.swapchain_images = s.swapchain_images;
        return view;
    }

    // Enhanced platform-specific initialization
    void InitializePlatformSpecific();

private:
    Common::DkbFrameTrace::RateLimit trace_cpu_rate;
    std::chrono::steady_clock::time_point trace_last_frame{};
    u64 trace_frame{};

    void InterpolateFrames(Frame* prev_frame, Frame* curr_frame);
    Frame* previous_frame = nullptr;  // Store the previous frame for interpolation
    VkCommandBuffer BeginSingleTimeCommands();
    void EndSingleTimeCommands(VkCommandBuffer command_buffer);
    void Report() const;
    /// The surface the swapchain is made for, once the device exists.
    VkSurfaceKHR PresentationSurface();

#ifdef HAS_LSFG
    /// EDEN_NX2_FLUID: gives the real frame the first slot its GPU work can be done for, and when
    /// its motion can be extrapolated, records a prediction for each slot until the next real
    /// frame is due and one more, each as of its slot's time.
    void RecordFluidFrames(Frame* frame, const HdrPresent& output,
                           std::vector<Frame*>& predicted_frames,
                           bool& real_flushed);
    /// EDEN_NX2_FLUID_INTERP: records the in-between frames of the last two real frames, each on
    /// the slot before the next, and gives the real frame the slot after them.
    void RecordFluidInterpolated(Frame* frame, const HdrPresent& output, size_t generations,
                                 std::vector<Frame*>& interpolated_frames);
    u64 fluid_batch{}; ///< real frames recorded for the FLUID presenter
#endif

    /// The encoding and luminances this frame is presented with; picks the swapchain format
    /// again when the HDR settings or the monitor's state changed.
    HdrPresent UpdateHdrPresent();
    /// SDR, with the luminances the game was told: screenshots and captures tone map HDR layers.
    HdrPresent CaptureHdrPresent() const;

    vk::Buffer RenderToBuffer(std::span<const Tegra::FramebufferConfig> framebuffers,
                              const Layout::FramebufferLayout& layout, VkFormat format,
                              VkDeviceSize buffer_size);
    void RenderScreenshot(std::span<const Tegra::FramebufferConfig> framebuffers);
    void RenderAppletCaptureLayer(std::span<const Tegra::FramebufferConfig> framebuffers);

    std::vector<Tegra::FramebufferConfig> applet_capture_layers;
    std::vector<Tegra::FramebufferConfig> screenshot_layer_scratch;

    Tegra::MaxwellDeviceMemoryManager& device_memory;
    Tegra::GPU& gpu;

    std::shared_ptr<Common::DynamicLibrary> library;
    vk::InstanceDispatch dld;
#ifdef EGLANTINE_STREAMLINE
    bool streamline_started; ///< NVIDIA Streamline starts before the instance.
#endif

    // Keep original handles for compatibility with existing code
    vk::Instance instance;

    vk::DebugUtilsMessenger debug_messenger;

    vk::SurfaceKHR surface;

    Device device;
    MemoryAllocator memory_allocator;
    StateTracker state_tracker;
    Scheduler scheduler;
    Swapchain swapchain;
    PresentManager present_manager;
    BlitScreen blit_swapchain;
    BlitScreen blit_capture;
    BlitScreen blit_applet;
    RasterizerVulkan rasterizer;
#ifdef HAS_LSFG
    FrameGen frame_gen;
#endif
    std::optional<TurboMode> turbo_mode;

    Frame applet_frame;

    u64 output_format_key{~u64{}};
    u32 output_luminance_key{};
    VkSurfaceFormatKHR output_format{};
    f32 output_reference_white{};
    f32 output_peak{};
};

} // namespace Vulkan
