// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2014 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "common/common_funcs.h"
#include "common/common_types.h"
#include "common/settings_enums.h"
#include "core/frontend/framebuffer_layout.h"
#include "video_core/gpu.h"
#include "video_core/gpu_costs.h"
#include "video_core/rasterizer_interface.h"

namespace Core::Frontend {
class EmuWindow;
class GraphicsContext;
} // namespace Core::Frontend

namespace VideoCore {

struct RendererDeviceInfo {
    std::string model;
    std::string driver;
    std::string api;
    bool integrated{}; ///< An integrated GPU: it shares the package power budget with the CPU
    /// Vulkan only, for bug reports: the API version, device-local memory, the device's
    /// extensions and the features Lichii checks.
    std::string api_version;
    u64 device_local_memory{};
    std::vector<std::string> extensions;
    std::vector<std::pair<std::string, bool>> features;
};

enum class FrameGenState : u8 {
    Pending,
    Running,
    UnsupportedRenderer,
    UnsupportedBuild,
    UnsupportedGpu,
    UnsupportedSurface,
    MissingLosslessDll,
    NvidiaFrameGen,
};

struct FrameGenStatus {
    FrameGenState state{FrameGenState::UnsupportedRenderer};
    u64 generated_frames{};
};

struct DlssStatus {
    u64 evaluations{};
    /// The last evaluation's settings: the mode it ran in, and the mode and preset the user chose
    Settings::NvidiaDlssQuality mode{};
    Settings::NvidiaDlssQuality requested{};
    Settings::NvidiaDlssPreset preset{};
};

struct RendererSettings {
    // Screenshot
    std::atomic<bool> screenshot_requested{false};
    void* screenshot_bits{};
    std::function<void(bool)> screenshot_complete_callback;
    Layout::FramebufferLayout screenshot_framebuffer_layout;
    Service::Nvnflinger::LayerStackId screenshot_layer_stack{Service::Nvnflinger::LayerStackId::Default};
};

class RendererBase {
public:
    LICHII_NON_COPYABLE(RendererBase);
    LICHII_NON_MOVEABLE(RendererBase);

    explicit RendererBase(Core::Frontend::EmuWindow& window,
                          std::unique_ptr<Core::Frontend::GraphicsContext> context);
    virtual ~RendererBase();

    /// Finalize rendering the guest frame and draw into the presentation texture
    virtual void Composite(std::span<const Tegra::FramebufferConfig> layers) = 0;

    /// Get the tiled applet layer capture buffer
    virtual std::vector<u8> GetAppletCaptureBuffer() = 0;

    [[nodiscard]] virtual RasterizerInterface* ReadRasterizer() = 0;

    [[nodiscard]] virtual std::string GetDeviceVendor() const = 0;

    /// Whether frame generation can run and how many frames it has generated so far
    [[nodiscard]] virtual FrameGenStatus GetFrameGenStatus() const {
        return {};
    }

    /// How many NVIDIA DLSS evaluations ran so far and the mode of the last one
    [[nodiscard]] virtual DlssStatus GetDlssStatus() const {
        return {};
    }

    /// Asks for one host GPU time sample of an upcoming frame
    virtual void RequestGpuBusySample() {}

    /// Host GPU busy time of the last sampled frame, nullopt when it isn't measured
    [[nodiscard]] virtual std::optional<double> GetGpuBusyMs() const {
        return std::nullopt;
    }

    /// Turns per-draw host GPU cost sampling on or off; false when the renderer cannot sample
    virtual bool RequestGpuCostSampling([[maybe_unused]] bool on) {
        return false;
    }

    [[nodiscard]] virtual bool IsGpuCostSampling() const {
        return false;
    }

    /// Host GPU time of the last sampled frame by shaders and by render target; frame 0 when
    /// none was sampled
    [[nodiscard]] virtual GpuCostSnapshot GetGpuCosts() const {
        return {};
    }

    /// EDEN_NX2_GPU_PERF: one hardware performance counter of a gpu_perf sample,
    /// normalized to percentages/MHz where the counter's unit allows it.
    struct GpuPerfSample {
        std::string name;
        double value{};
        std::string unit;
    };
    /// Whether the gpu_perf counter sampler can run on this renderer/device.
    [[nodiscard]] virtual bool IsGpuPerfSupported() const {
        return false;
    }
    /// Why gpu_perf cannot sample (unsupported renderer, extension, lock, ...).
    [[nodiscard]] virtual std::string GpuPerfReason() const {
        return "not the Vulkan renderer";
    }
    /// Samples the next `frames` submissions (0 stops); false when unsupported or the
    /// profiling lock could not be acquired.
    virtual bool RequestGpuPerfSampling([[maybe_unused]] u32 frames) {
        return false;
    }
    [[nodiscard]] virtual bool IsGpuPerfSampling() const {
        return false;
    }
    /// Counters averaged over the submissions collected so far; empty until then.
    [[nodiscard]] virtual std::vector<GpuPerfSample> ReadGpuPerf() {
        return {};
    }
    [[nodiscard]] virtual u64 GpuPerfFramesSampled() const {
        return 0;
    }
    /// Diagnostic: {pending slots, frames left, known GPU tick}.
    [[nodiscard]] virtual std::tuple<u64, u64, u64> GpuPerfDiag() const {
        return {0, 0, 0};
    }

    /// The Vulkan present thread's counters (present_stats); null on other renderers.
    struct PresentStatsView {
        bool fluid{};
        bool fallback{};
        bool shown_stats{};
        u64 slot_count{};
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
        std::array<u64, 4> held{};
        double age_ms{};
        double latency_ms{};
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
        double refresh_hz{};
        u64 vblank_index{};
        u64 divisor{};
        double lead_ms{};
        double clock_latency_ms{};
        std::size_t queued_frames{};
        std::size_t free_frames{};
        std::size_t frame_count{};
        std::size_t swapchain_images{};
    };
    [[nodiscard]] virtual std::optional<PresentStatsView> GetPresentStats() const {
        return std::nullopt;
    }

    [[nodiscard]] const RendererDeviceInfo& GetDeviceInfo() const {
        return device_info;
    }

    // Getter/setter functions:
    // ------------------------

    [[nodiscard]] f32 GetCurrentFPS() const {
        return m_current_fps;
    }

    [[nodiscard]] int GetCurrentFrame() const {
        return m_current_frame;
    }

    [[nodiscard]] Core::Frontend::GraphicsContext& Context() {
        return *context;
    }

    [[nodiscard]] const Core::Frontend::GraphicsContext& Context() const {
        return *context;
    }

    [[nodiscard]] Core::Frontend::EmuWindow& GetRenderWindow() {
        return render_window;
    }

    [[nodiscard]] const Core::Frontend::EmuWindow& GetRenderWindow() const {
        return render_window;
    }

    [[nodiscard]] RendererSettings& Settings() {
        return renderer_settings;
    }

    [[nodiscard]] const RendererSettings& Settings() const {
        return renderer_settings;
    }

    /// Refreshes the settings common to all renderers
    void RefreshBaseSettings();

    /// Returns true if a screenshot is being processed
    bool IsScreenshotPending() const;

    /// Request a screenshot of the next frame.
    void RequestScreenshot(void* data, std::function<void(bool)> callback,
                           const Layout::FramebufferLayout& layout,
                           Service::Nvnflinger::LayerStackId layer_stack =
                               Service::Nvnflinger::LayerStackId::Default);

protected:
    Core::Frontend::EmuWindow& render_window; ///< Reference to the render window handle.
    std::unique_ptr<Core::Frontend::GraphicsContext> context;
    f32 m_current_fps = 0.0f; ///< Current framerate, should be set by the renderer
    int m_current_frame = 0;  ///< Current frame, should be set by the renderer

    RendererSettings renderer_settings;
    RendererDeviceInfo device_info;

private:
    /// Updates the framebuffer layout of the contained render window handle.
    void UpdateCurrentFramebufferLayout();
};

} // namespace VideoCore
