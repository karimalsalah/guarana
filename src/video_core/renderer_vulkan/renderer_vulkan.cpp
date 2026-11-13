// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <fmt/ranges.h>

#include "common/display_hdr.h"
#include "common/logging.h"
#include <ranges>
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core_timing.h"
#include "core/game_presets.h"
#include "core/frontend/graphics_context.h"
#include "video_core/capture.h"
#include "video_core/gpu.h"
#include "video_core/nx2.h"
#include "video_core/nx2_profile.h"
#include "video_core/present.h"
#include "video_core/render_trace.h"
#include "video_core/renderer_vulkan/present/util.h"
#ifdef HAS_RESHADE
#include "video_core/post_processing/fx_chain.h"
#endif
#include "video_core/renderer_vulkan/renderer_vulkan.h"
#include "video_core/renderer_vulkan/vk_blit_screen.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_state_tracker.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/textures/decoders.h"
#include "video_core/vulkan_common/vk_enum_string_helper.h"
#include "video_core/vulkan_common/vulkan_debug_callback.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_instance.h"
#include "video_core/vulkan_common/vulkan_library.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_surface.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"
#ifdef EGLANTINE_STREAMLINE
#include "video_core/renderer_vulkan/vk_streamline_dlss.h"
#include "video_core/vulkan_common/streamline.h"
#include "video_core/vulkan_common/streamline_frame_gen.h"
#endif
#ifdef __ANDROID__
#include <jni.h>
#endif
namespace Vulkan {
namespace {

constexpr VkExtent2D CaptureImageSize{
    .width = VideoCore::Capture::LinearWidth,
    .height = VideoCore::Capture::LinearHeight,
};

#ifdef HAS_LSFG
/// EDEN_NX2_FLUID: the real frame is shown as it is for its first slot while the prediction for
/// that slot would carry its motion at most this share of a frame interval.
constexpr f64 FLUID_REAL_ADVANCE = 0.1;
/// Predictions reach at most this many frame intervals ahead.
constexpr f64 FLUID_MAX_ADVANCE = 3.0;
constexpr size_t FLUID_MAX_PREDICTIONS = 6;
/// EDEN_NX2_CADENCE: real frames are paced to be done this long before their slot's present.
constexpr s64 FLUID_PHASE_MARGIN_NS = 3'000'000;

[[nodiscard]] VkExtent2D GuestExtent(std::span<const Tegra::FramebufferConfig> framebuffers) {
    if (framebuffers.empty()) {
        return VkExtent2D{};
    }

    const auto& framebuffer = framebuffers.front();
    if (framebuffer.crop_rect.IsEmpty()) {
        return VkExtent2D{.width = framebuffer.width, .height = framebuffer.height};
    }
    return VkExtent2D{
        .width = static_cast<u32>(framebuffer.crop_rect.GetWidth()),
        .height = static_cast<u32>(framebuffer.crop_rect.GetHeight()),
    };
}
#endif

constexpr VkExtent3D CaptureImageExtent{
    .width = VideoCore::Capture::LinearWidth,
    .height = VideoCore::Capture::LinearHeight,
    .depth = VideoCore::Capture::LinearDepth,
};

constexpr VkFormat CaptureFormat = VK_FORMAT_A8B8G8R8_UNORM_PACK32;

std::string GetReadableVersion(u32 version) {
    return fmt::format("{}.{}.{}", VK_VERSION_MAJOR(version), VK_VERSION_MINOR(version),
                       VK_VERSION_PATCH(version));
}

std::string GetDriverVersion(const Device& device) {
    // Extracted from
    // https://github.com/SaschaWillems/vulkan.gpuinfo.org/blob/5dddea46ea1120b0df14eef8f15ff8e318e35462/functions.php#L308-L314
    const u32 version = device.GetDriverVersion();

    if (device.GetDriverID() == VK_DRIVER_ID_NVIDIA_PROPRIETARY) {
        const u32 major = (version >> 22) & 0x3ff;
        const u32 minor = (version >> 14) & 0x0ff;
        const u32 secondary = (version >> 6) & 0x0ff;
        const u32 tertiary = version & 0x003f;
        return fmt::format("{}.{}.{}.{}", major, minor, secondary, tertiary);
    }
    if (device.GetDriverID() == VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS) {
        const u32 major = version >> 14;
        const u32 minor = version & 0x3fff;
        return fmt::format("{}.{}", major, minor);
    }
    return GetReadableVersion(version);
}

std::string BuildCommaSeparatedExtensions(
    const std::set<std::string, std::less<>>& available_extensions) {
    return fmt::format("{}", fmt::join(available_extensions, ","));
}

// The swapchain format and colour space to present in: an HDR pair the surface lists while HDR
// output is on (Off/On/Auto with the monitor's state), else SDR.
VkSurfaceFormatKHR ChooseOutputFormat(const vk::PhysicalDevice& physical, VkSurfaceKHR surface,
                                      void* window) {
    auto& monitor = Common::DisplayHdrMonitor::Instance();
    monitor.Watch(window);
    const auto output = Common::ResolveHdrOutput(
        monitor.Current(), Settings::values.hdr_mode.GetValue(),
        Settings::values.hdr_peak_luminance.GetValue(),
        Settings::values.hdr_reference_white.GetValue());
    if (output.enabled) {
        const auto formats = physical.GetSurfaceFormatsKHR(surface);
        const auto hdr =
            ChooseHdrSurfaceFormat(formats, Settings::values.hdr_swapchain.GetValue());
        if (hdr) {
            return *hdr;
        }
        LOG_WARNING(Render_Vulkan,
                    "HDR output is on, but the surface lists no HDR10 or scRGB format");
    }
#ifdef __ANDROID__
    return {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
#else
    return {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
#endif
}

// Game presets match on the GPU's capabilities and set values the device and the renderer read
// while they are built.
void ApplyGamePresets(const Device& device) {
    using Core::GameSettings::GPUVendor;
    GPUVendor vendor = GPUVendor::Unknown;
    switch (device.GetDriverID()) {
    case VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS:
    case VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA:
        vendor = GPUVendor::Intel;
        break;
    case VK_DRIVER_ID_NVIDIA_PROPRIETARY:
        vendor = GPUVendor::Nvidia;
        break;
    case VK_DRIVER_ID_AMD_PROPRIETARY:
    case VK_DRIVER_ID_AMD_OPEN_SOURCE:
    case VK_DRIVER_ID_MESA_RADV:
        vendor = GPUVendor::AMD;
        break;
    default:
        break;
    }
    Core::GamePresets::Apply(Settings::GetCurrentProgramID(),
                             Core::GamePresets::GpuInfo{
                                 .vendor = vendor,
                                 .integrated = device.IsIntegrated(),
                                 .software_tensor_math =
                                     !device.IsCooperativeHmmaSupported() &&
                                     !device.IsCooperativeHmma16x16x16Supported(),
                             });
}

} // Anonymous namespace

Device CreateDevice(const vk::Instance& instance, const vk::InstanceDispatch& dld,
                    VkSurfaceKHR surface,
                    const std::function<void(const Device&)>& on_capabilities) {
    const std::vector<VkPhysicalDevice> devices = instance.EnumeratePhysicalDevices();
    const u32 device_index = Settings::values.vulkan_device.GetValue();
    if (device_index >= u32(devices.size())) {
        LOG_ERROR(Render_Vulkan, "Invalid device index {}!", device_index);
        throw vk::Exception(VK_ERROR_INITIALIZATION_FAILED);
    }
    const vk::PhysicalDevice physical_device(devices[device_index], dld);
    return Device(*instance, physical_device, surface, dld, on_capabilities);
}

RendererVulkan::RendererVulkan(Core::Frontend::EmuWindow& emu_window,
                               Tegra::MaxwellDeviceMemoryManager& device_memory_,
                               Tegra::GPU& gpu_,
                               std::unique_ptr<Core::Frontend::GraphicsContext> context_)
try
    : RendererBase(emu_window, std::move(context_))
    , device_memory(device_memory_)
    , gpu(gpu_)
    , library(OpenLibrary(context.get()))
    , dld()
#ifdef EGLANTINE_STREAMLINE
    , streamline_started(Streamline::Start(render_window.GetWindowInfo().render_surface))
#endif
    // Create raw Vulkan instance first
    , instance(CreateInstance(*library,
                            dld,
                            VK_API_VERSION_1_1,
                            render_window.GetWindowInfo().type,
                            Settings::values.renderer_debug.GetValue()))
    // Create debug messenger if debug is enabled
    , debug_messenger(Settings::values.renderer_debug ? CreateDebugUtilsCallback(instance)
                                                    : vk::DebugUtilsMessenger{})
    // Create surface
    , surface(CreateSurface(instance, render_window.GetWindowInfo()))
    , device(CreateDevice(instance, dld, *surface, ApplyGamePresets))
    , memory_allocator(device)
    , state_tracker()
    , scheduler(device, state_tracker)
    , swapchain(PresentationSurface(),
                device,
                scheduler,
               render_window.GetFramebufferLayout().width,
               render_window.GetFramebufferLayout().height,
               ChooseOutputFormat(device.GetPhysical(), *surface,
                                  render_window.GetWindowInfo().render_surface))
    , present_manager(instance,
                      render_window,
                      device,
                      memory_allocator,
                      scheduler,
                      swapchain,
                      surface)
    , blit_swapchain(device_memory,
                   device,
                   memory_allocator,
                   present_manager,
                   scheduler,
                   PresentFiltersForDisplay)
    , blit_capture(device_memory,
                   device,
                   memory_allocator,
                   present_manager,
                   scheduler,
                   PresentFiltersForDisplay)
    , blit_applet(device_memory,
                  device,
                  memory_allocator,
                  present_manager,
                  scheduler,
                  PresentFiltersForAppletCapture)
    , rasterizer(render_window, gpu, device_memory, device, memory_allocator, state_tracker, scheduler)
#ifdef HAS_LSFG
    , frame_gen(memory_allocator, scheduler)
#endif
{

    if (Settings::values.renderer_force_max_clock.GetValue() && device.ShouldBoostClocks()) {
        turbo_mode.emplace(instance, dld);
        scheduler.RegisterOnSubmit([this] { turbo_mode->QueueSubmitted(); });
    }

#ifdef HAS_RESHADE
    VideoCore::FxChain::Instance().LoadFromSettings();
#endif

    device_info = {
        .model = std::string{device.GetModelName()},
        .driver = device.GetDriverVersion() != 0
                      ? fmt::format("{} {}", device.GetVendorName(), GetDriverVersion(device))
                      : std::string{},
        .api = "Vulkan",
        .integrated = device.IsIntegrated(),
    };
    Report();
} catch (const vk::Exception& exception) {
    LOG_ERROR(Render_Vulkan, "Vulkan initialization failed with error: {}", exception.what());
    throw std::runtime_error{fmt::format("Vulkan initialization error {}", exception.what())};
}

RendererVulkan::~RendererVulkan() {
    scheduler.RegisterOnSubmit([] {});
    void(device.GetLogical().WaitIdle());
}

void RendererVulkan::Composite(std::span<const Tegra::FramebufferConfig> framebuffers) {
    const bool trace = Common::DkbFrameTrace::Enabled();
    const auto entered = trace ? std::chrono::steady_clock::now()
                               : std::chrono::steady_clock::time_point{};
    SCOPE_EXIT {
        render_window.OnFrameDisplayed();
    };
#ifdef EGLANTINE_STREAMLINE
    // DLSS Frame Generation: this guest frame ends here, presented or not.
    Streamline::FrameGenBeginFrame();
    SCOPE_EXIT {
        Streamline::FrameGenNextFrame();
    };
#endif

    RenderAppletCaptureLayer(framebuffers);

    if (!render_window.IsShown()) {
        return;
    }

    if (VideoCommon::StutterTrace::Enabled()) {
        VideoCommon::StutterTrace::OnPresent();
    }
    RenderScreenshot(framebuffers);
    const auto acquiring = trace ? std::chrono::steady_clock::now()
                                 : std::chrono::steady_clock::time_point{};
    Frame* frame = [this] {
        const VideoCommon::Nx2Profile::Scope scope{VideoCommon::Nx2Profile::Zone::Present};
        return present_manager.GetRenderFrame();
    }();
    const auto acquired = trace ? std::chrono::steady_clock::now()
                                : std::chrono::steady_clock::time_point{};

    const HdrPresent output = UpdateHdrPresent();
    frame->color_space = output.color_space;

    scheduler.RequestOutsideRenderPassOperationContext();
    {
        const auto& layout = render_window.GetFramebufferLayout();
        scheduler.RecordEmuBegin(TraceCategory::Present, {layout.width, layout.height, 0});
    }
    blit_swapchain.DrawToFrame(device, rasterizer, frame, framebuffers,
                              render_window.GetFramebufferLayout(),
                              present_manager.SwapchainImageCount(), output_format.format, output);
    scheduler.RecordEmuEnd();

#ifdef HAS_LSFG
    // Frames predicted from this one (EDEN_NX2_EXTRAPOLATE): presented after it, unlike
    // interpolated frames. The extrapolation pass writes SDR, so HDR output pauses it.
    std::vector<Frame*> predicted_frames;
    const bool fluid = present_manager.Fluid();
    if (output.IsHdr() && ExtrapolationRequested()) {
        frame_gen.Pause();
        if (fluid) {
            RecordFluidFrames(frame, output, predicted_frames);
        }
    } else {
        if (!output.IsHdr()) {
            const auto& layout = render_window.GetFramebufferLayout();
            frame_gen.SetExtrapolationSource(
                rasterizer.OcarinaExtrapolationSource(),
                VkRect2D{.offset = {static_cast<s32>(layout.screen.left),
                                    static_cast<s32>(layout.screen.top)},
                         .extent = {layout.screen.GetWidth(), layout.screen.GetHeight()}});
        }
        void(frame_gen.WantedGenerations(present_manager.MaxExtraFrames(),
                                         present_manager.TakeBlockedTime()));

        frame_gen.Process(device, frame, frame->image_format, GuestExtent(framebuffers));

        const size_t generated_frames = frame_gen.GeneratedFrameCount();
        const bool predict = frame_gen.Extrapolating();
        if (fluid && (predict || generated_frames == 0)) {
            // EDEN_NX2_FLUID: the present thread shows each frame at its vblank slot.
            RecordFluidFrames(frame, output, predicted_frames);
        } else {
            // Each generated frame keeps the screen for an equal share of the real frame
            // interval (EDEN_NX2_CADENCE: of the target rate). Sent back to back, a high refresh
            // rate display shows it for one refresh only.
            auto spacing = frame_gen.CadenceSpacing();
            if (spacing.count() == 0) {
                spacing = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::duration<f32>(std::min(frame_gen.FrameInterval(), 0.1f) /
                                               static_cast<f32>(generated_frames + 1)));
            }
            for (size_t generation = 0; generation < generated_frames; ++generation) {
                Frame* generated = present_manager.GetRenderFrame();
                generated->color_space = output.color_space;
                generated->present_spacing = spacing;
                blit_swapchain.PrepareFrame(device, generated,
                                            render_window.GetFramebufferLayout());
                frame_gen.GenerateInto(device, generated, generation);
                if (predict) {
                    // Recorded with the real frame's commands, which it reads; shown after it.
                    predicted_frames.push_back(generated);
                    continue;
                }
                generated->gpu_tick = scheduler.Flush(*generated->render_ready);
                present_manager.Present(generated);
            }
            if (!predicted_frames.empty()) {
                // The real frame and each predicted frame but the last hold the screen one
                // spacing. The last leaves the next real frame free to show as soon as it is
                // ready, as the real frame does after interpolated ones: (gens + 1) spacings plus
                // copy time per batch would let the present queue creep behind.
                frame->present_spacing = spacing;
                predicted_frames.back()->present_spacing = {};
            }
        }
    }
#endif

    if (const auto mark = scheduler.NextFrameMark(); mark != Scheduler::FrameMark::None) {
        scheduler.MarkFrameEnd(++trace_frame, mark);
    }
    frame->gpu_tick = scheduler.Flush(*frame->render_ready);
#ifdef EGLANTINE_STREAMLINE
    {
        const Streamline::FrameGenFrame ended = Streamline::FrameGenEndFrame();
        const Common::Rectangle<u32>& screen = render_window.GetFramebufferLayout().screen;
        frame->frame_gen = Streamline::FrameGenSession();
        frame->frame_gen_frame = ended;
        frame->guest_rect = VkRect2D{
            .offset = {static_cast<s32>(screen.left), static_cast<s32>(screen.top)},
            .extent = {screen.GetWidth(), screen.GetHeight()},
        };
    }
#endif

    const auto presenting = trace ? std::chrono::steady_clock::now()
                                  : std::chrono::steady_clock::time_point{};
    {
        const VideoCommon::Nx2Profile::Scope scope{VideoCommon::Nx2Profile::Zone::Present};
        present_manager.Present(frame);
    }
    const auto presented = trace ? std::chrono::steady_clock::now()
                                 : std::chrono::steady_clock::time_point{};
#ifdef HAS_LSFG
    for (Frame* predicted : predicted_frames) {
        predicted->gpu_tick = scheduler.Flush(*predicted->render_ready);
        present_manager.Present(predicted);
    }
    scheduler.DispatchWork();
#endif

    gpu.RendererFrameEndNotify();
    rasterizer.TickFrame();
    if (trace) {
        const auto end = std::chrono::steady_clock::now();
        const auto ms = [](auto duration) {
            return std::chrono::duration<double, std::milli>(duration).count();
        };
        if (trace_cpu_rate.Ready(end)) {
            LOG_INFO(Render_Vulkan,
                     "DKB renderer renderer_frame={} host_pacing_mode={} vsync_mode={} "
                     "cpu_frame_wall_ms={:.3f} "
                     "cpu_composite_wall_ms={:.3f} capture_wall_ms={:.3f} "
                     "acquire_frame_wall_ms={:.3f} record_submit_wall_ms={:.3f} "
                     "present_call_wall_ms={:.3f} frame_end_wall_ms={:.3f}",
                     trace_frame, static_cast<int>(Settings::values.frame_pacing_mode.GetValue()),
                     static_cast<int>(Settings::values.vsync_mode.GetValue()),
                     trace_last_frame == std::chrono::steady_clock::time_point{}
                         ? -1.0 : ms(entered - trace_last_frame),
                     ms(end - entered), ms(acquiring - entered), ms(acquired - acquiring),
                     ms(presenting - acquired), ms(presented - presenting), ms(end - presented));
        }
        trace_last_frame = entered;
    }
}

#ifdef HAS_LSFG
void RendererVulkan::RecordFluidFrames(Frame* frame, const HdrPresent& output,
                                       std::vector<Frame*>& predicted_frames) {
    using std::chrono::duration_cast;
    using std::chrono::nanoseconds;
    const auto recorded = std::chrono::steady_clock::now();
    frame->fluid_batch = ++fluid_batch;
    frame->fluid_recorded = recorded;
    const PresentManager::FluidClock slots = present_manager.GetFluidClock();
    if (!slots.Valid()) {
        // No vblank counted yet: shown in arrival order.
        return;
    }
    // The first slot the frame's GPU work can be done for.
    const u64 first = slots.FirstSlotPresentedAfter(recorded + slots.latency);
    frame->fluid_slot = first;
    const f32 interval = frame_gen.FluidInterval();
    if (interval <= 0.0f || !frame_gen.CanPredict()) {
        return;
    }
    const f32 paced = static_cast<f32>(Tegra::g_nx2_guest_frame_interval_ns.load(
                          std::memory_order_relaxed)) *
                      1.0e-9f;
    if (frame_gen.CadenceActive() && paced > 0.0f &&
        std::abs(frame_gen.FrameInterval() - paced) <= paced * 0.05f) {
        // EDEN_NX2_CADENCE paces the guest's presents, and the game keeps to it: move them, an
        // eighth of the way a frame, towards real frames whose GPU work is done just before
        // their first slot.
        const s64 slot_ns = (slots.refresh * static_cast<s64>(slots.divisor)).count();
        const s64 slack = duration_cast<nanoseconds>(slots.SlotTime(first) - slots.lead -
                                                     (recorded + slots.latency))
                              .count();
        s64 error = slack - FLUID_PHASE_MARGIN_NS;
        if (error > slot_ns / 2) {
            // Nearer the slot before: be earlier.
            error -= slot_ns;
        }
        Tegra::g_nx2_present_phase_shift_ns.fetch_add(
            std::clamp<s64>(error / 8, -1'000'000, 1'000'000), std::memory_order_relaxed);
    }
    const auto real_interval = duration_cast<nanoseconds>(std::chrono::duration<f32>(interval));
    // Through the slot the next real frame is expected for: one spare covers it coming late.
    const u64 last = slots.FirstSlotPresentedAfter(recorded + slots.latency + real_interval);
    // EDEN_NX2_FLUID=2: each prediction is as of its slot less the latency a real frame can be
    // shown at.
    const nanoseconds behind = FluidMode() == 2 ? slots.latency + slots.lead : nanoseconds{};
    size_t predicted = 0;
    for (u64 slot = first; slot <= last && predicted < FLUID_MAX_PREDICTIONS;
         slot += slots.divisor) {
        const nanoseconds ahead =
            duration_cast<nanoseconds>(slots.SlotTime(slot) - recorded - behind);
        const f64 advance =
            static_cast<f64>(ahead.count()) / static_cast<f64>(real_interval.count());
        if (slot == first && advance <= FLUID_REAL_ADVANCE) {
            // The real frame is on time for its slot.
            continue;
        }
        // Never waits: with no frame to spare the batch ends here.
        Frame* const generated = present_manager.TryGetRenderFrame();
        if (generated == nullptr) {
            break;
        }
        generated->color_space = output.color_space;
        blit_swapchain.PrepareFrame(device, generated, render_window.GetFramebufferLayout());
        generated->fluid_batch = frame->fluid_batch;
        generated->fluid_recorded = recorded;
        generated->fluid_slot = FLUID_NOT_SHOWN;
        if (frame_gen.GenerateAt(device, generated,
                                 static_cast<f32>(std::clamp(advance, 0.0, FLUID_MAX_ADVANCE)))) {
            generated->fluid_slot = slot;
            if (slot == first) {
                // The prediction takes the real frame's slot.
                frame->fluid_slot = FLUID_NOT_SHOWN;
            }
            ++predicted;
        }
        predicted_frames.push_back(generated);
    }
    frame_gen.NoteFluidGenerated(predicted);
}
#endif

HdrPresent RendererVulkan::UpdateHdrPresent() {
    const auto& monitor = Common::DisplayHdrMonitor::Instance();
    const u64 key = (monitor.Generation() << 8) |
                    (u64{static_cast<u8>(Settings::values.hdr_mode.GetValue())} << 4) |
                    static_cast<u8>(Settings::values.hdr_swapchain.GetValue());
    const u32 luminance_key = (u32{Settings::values.hdr_peak_luminance.GetValue()} << 16) |
                              Settings::values.hdr_reference_white.GetValue();
    if (key == output_format_key && luminance_key == output_luminance_key) {
        return {output_format.colorSpace, output_reference_white, output_peak};
    }
    output_format_key = key;
    output_luminance_key = luminance_key;
    const auto previous = output_format;
    output_format = ChooseOutputFormat(device.GetPhysical(), *surface,
                                       render_window.GetWindowInfo().render_surface);
    const auto told = CaptureHdrPresent();
    output_reference_white = told.reference_white;
    output_peak = told.peak;
    if (previous.format != output_format.format ||
        previous.colorSpace != output_format.colorSpace) {
        if (output_format.colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            LOG_INFO(Render_Vulkan,
                     "HDR output: {} {}, reference white {} nits, peak {} nits. Anti-aliasing, "
                     "post-processing and upscaling filters are off",
                     string_VkFormat(output_format.format),
                     string_VkColorSpaceKHR(output_format.colorSpace), output_reference_white,
                     output_peak);
        } else {
            LOG_INFO(Render_Vulkan, "HDR output: SDR {}", string_VkFormat(output_format.format));
        }
    }
    return {output_format.colorSpace, output_reference_white, output_peak};
}

HdrPresent RendererVulkan::CaptureHdrPresent() const {
    // The luminances the game was told (nx2:vi2 QueryDisplayHdrStatus / PaperWhite).
    const auto told = Common::ResolveHdrOutput(Common::DisplayHdrMonitor::Instance().Current(),
                                               Settings::HdrMode::On,
                                               Settings::values.hdr_peak_luminance.GetValue(),
                                               Settings::values.hdr_reference_white.GetValue());
    return {VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, static_cast<f32>(told.reference_white),
            static_cast<f32>(told.peak_luminance)};
}

VkSurfaceKHR RendererVulkan::PresentationSurface() {
#ifdef EGLANTINE_STREAMLINE
    // DLSS-G looks the window up by the swapchain's surface: replace the surface made before the
    // device with one made through Streamline, for the same window. Present support stays valid,
    // as it belongs to the window and the queue family.
    const auto& window_info = render_window.GetWindowInfo();
    if (Streamline::FrameGenSession()) {
        const VkSurfaceKHR registered =
            window_info.type == Core::Frontend::WindowSystemType::Windows
                ? Streamline::CreateFrameGenSurface(*instance, window_info.render_surface)
                : VK_NULL_HANDLE;
        if (registered) {
            surface = vk::SurfaceKHR(registered, *instance, instance.Dispatch());
        } else {
            Streamline::DisableFrameGen("Streamline could not make the window's surface");
        }
    }
#endif
    return *surface;
}

void RendererVulkan::Report() const {
    using namespace Common::Literals;

    const std::string api_version = GetReadableVersion(device.ApiVersion());

    const std::string extensions = BuildCommaSeparatedExtensions(device.GetAvailableExtensions());

    const auto available_vram = static_cast<f64>(device.GetDeviceLocalMemory()) / f64{1_GiB};

    LOG_INFO(Render_Vulkan, "Driver: {} {}", device.GetVendorName(), GetDriverVersion(device));
    LOG_INFO(Render_Vulkan, "Device: {}", device_info.model);
    LOG_INFO(Render_Vulkan, "Vulkan: {}", api_version);
    LOG_INFO(Render_Vulkan, "Available VRAM: {:.2f} GiB", available_vram);
    if (VideoCommon::RenderTrace::Enabled()) {
        LOG_INFO(Render_Vulkan, "Render trace enabled: interval=600 frames, log_filter={}",
                 Settings::values.log_filter.GetValue());
    }
    LOG_DEBUG(Render_Vulkan,
              "Render trace capabilities: geometry_shader={} transform_feedback={} "
              "geometry_streams={} mesh_shader={}",
              device.GetPhysical().GetFeatures().geometryShader, device.IsExtTransformFeedbackSupported(),
              device.AreTransformFeedbackGeometryStreamsSupported(), device.IsExtMeshShaderSupported());
}

vk::Buffer RendererVulkan::RenderToBuffer(std::span<const Tegra::FramebufferConfig> framebuffers,
                                          const Layout::FramebufferLayout& layout, VkFormat format,
                                          VkDeviceSize buffer_size) {
    auto frame = [&]() {
        Frame f{};
        f.image =
            CreateWrappedImage(memory_allocator, VkExtent2D{layout.width, layout.height}, format);
        f.image_view = CreateWrappedImageView(device, f.image, format);
        f.framebuffer = blit_capture.CreateFramebuffer(device, layout, *f.image_view, format);
        return f;
    }();

    auto dst_buffer = CreateWrappedBuffer(memory_allocator, buffer_size, MemoryUsage::Download);
    blit_capture.DrawToFrame(device, rasterizer, &frame, framebuffers, layout, 1, format,
                             CaptureHdrPresent());

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([&](vk::CommandBuffer cmdbuf) {
        DownloadColorImage(cmdbuf, *frame.image, *dst_buffer,
                           VkExtent3D{layout.width, layout.height, 1});
    });

    // Ensure the copy is fully completed before saving the capture
    scheduler.Finish();

    // Copy backing image data to the capture buffer
    dst_buffer.Invalidate();
    return dst_buffer;
}

void RendererVulkan::RenderScreenshot(std::span<const Tegra::FramebufferConfig> framebuffers) {
    if (!renderer_settings.screenshot_requested) {
        return;
    }

    const auto screenshot_layers = Tegra::FilterLayerStack(
        framebuffers, renderer_settings.screenshot_layer_stack, screenshot_layer_scratch);

    const auto& layout{renderer_settings.screenshot_framebuffer_layout};
    const auto dst_buffer = RenderToBuffer(screenshot_layers, layout, VK_FORMAT_B8G8R8A8_UNORM,
                                           layout.width * layout.height * 4);

    std::memcpy(renderer_settings.screenshot_bits, dst_buffer.Mapped().data(),
                dst_buffer.Mapped().size());
    renderer_settings.screenshot_complete_callback(false);
    renderer_settings.screenshot_requested = false;
}

std::vector<u8> RendererVulkan::GetAppletCaptureBuffer() {
    using namespace VideoCore::Capture;

    std::vector<u8> out(VideoCore::Capture::TiledSize);

    if (!applet_frame.image) {
        return out;
    }

    const auto dst_buffer =
        CreateWrappedBuffer(memory_allocator, VideoCore::Capture::TiledSize, MemoryUsage::Download);

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([&](vk::CommandBuffer cmdbuf) {
        DownloadColorImage(cmdbuf, *applet_frame.image, *dst_buffer, CaptureImageExtent);
    });

    // Ensure the copy is fully completed before writing the capture
    scheduler.Finish();

    // Swizzle image data to the capture buffer
    dst_buffer.Invalidate();
    Tegra::Texture::SwizzleTexture(out, dst_buffer.Mapped(), BytesPerPixel, LinearWidth,
                                   LinearHeight, LinearDepth, BlockHeight, BlockDepth);

    return out;
}

void RendererVulkan::RenderAppletCaptureLayer(
    std::span<const Tegra::FramebufferConfig> framebuffers) {
    const auto capture_layers = Tegra::FilterLayerStack(
        framebuffers, Service::Nvnflinger::LayerStackId::LastFrame, applet_capture_layers);

    if (capture_layers.empty())
        return;

    if (!applet_frame.image) {
        applet_frame.image = CreateWrappedImage(memory_allocator, CaptureImageSize, CaptureFormat);
        applet_frame.image_view = CreateWrappedImageView(device, applet_frame.image, CaptureFormat);
        applet_frame.framebuffer = blit_applet.CreateFramebuffer(device,
            VideoCore::Capture::Layout, *applet_frame.image_view, CaptureFormat);
    }

    scheduler.RequestOutsideRenderPassOperationContext();
    blit_applet.DrawToFrame(device, rasterizer, &applet_frame, capture_layers, VideoCore::Capture::Layout, 1,
                            CaptureFormat, CaptureHdrPresent());
}

#ifdef EGLANTINE_STREAMLINE
VideoCore::DlssStatus RendererVulkan::GetDlssStatus() const {
    return StreamlineDlssUpscaler::Status();
}
#endif

} // namespace Vulkan
