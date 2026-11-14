// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <array>
#include <limits>
#include <mutex>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#ifdef __ANDROID__
#include <android/api-level.h>
#endif

#include "common/display_hdr.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/settings_enums.h"
#include "video_core/renderer_vulkan/fluid_present_mode.h"
#include "video_core/renderer_vulkan/vk_present_manager.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/vulkan_common/vk_enum_string_helper.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"
#include "vulkan/vulkan_core.h"
#ifdef EGLANTINE_STREAMLINE
#include "video_core/vulkan_common/streamline.h"
#include "video_core/vulkan_common/streamline_frame_gen.h"
#endif

namespace Vulkan {

namespace {

/// Streamline's DLSS Frame Generation presents and paces the frames in this format. Turning it
/// on or off changes the present mode, which recreates the swapchain as NVIDIA asks.
bool FrameGenPresents([[maybe_unused]] VkFormat format) {
#ifdef EGLANTINE_STREAMLINE
    return Streamline::FrameGenActive() && Streamline::FrameGenSupportsSwapchain(format);
#else
    return false;
#endif
}

/// DLSS-G is loaded, so Streamline makes the swapchains, whether or not it generates frames.
bool FrameGenLoaded() {
#ifdef EGLANTINE_STREAMLINE
    return Streamline::FrameGenSession();
#else
    return false;
#endif
}

/// EDEN_NX2_FLUID_MAILBOX=<n>, n not 0: the FLUID presenter asks for MAILBOX when the surface
/// lists it. Parsed as EDEN_NX2_FLUID is. Read once.
[[nodiscard]] bool FluidMailboxRequested() {
    static const bool requested = [] {
        const char* value = std::getenv("EDEN_NX2_FLUID_MAILBOX");
        return value != nullptr && std::strtoul(value, nullptr, 10) != 0;
    }();
    return requested;
}

/// EDEN_NX2_FLUID_IMMEDIATE=<n>, n not 0: the FLUID presenter asks for IMMEDIATE when the surface
/// lists it (MAILBOX still wins when EDEN_NX2_FLUID_MAILBOX is on and listed). Parsed as
/// EDEN_NX2_FLUID is. Read once.
[[nodiscard]] bool FluidImmediateRequested() {
    static const bool requested = [] {
        const char* value = std::getenv("EDEN_NX2_FLUID_IMMEDIATE");
        return value != nullptr && std::strtoul(value, nullptr, 10) != 0;
    }();
    return requested;
}

#ifdef EGLANTINE_STREAMLINE
/// Streamline asks for a flushed pipeline and no Vulkan calls while a feature unloads.
void UnloadFrameGen(Scheduler& scheduler, const Device& device, std::string_view reason) {
    const auto paused = scheduler.PauseWorker();
    std::scoped_lock lock{scheduler.submit_mutex};
    const VkResult result = device.GetLogical().WaitIdle();
    if (result == VK_ERROR_DEVICE_LOST) {
        device.ReportLoss();
    }
    vk::Check(result);
    Streamline::DisableFrameGen(reason);
}
#endif

VkSurfaceFormatKHR ChooseSwapSurfaceFormat(vk::Span<VkSurfaceFormatKHR> formats,
                                         VkFormat requested_format,
                                         VkColorSpaceKHR requested_color_space) {
    if (requested_format == VK_FORMAT_R16G16B16A16_SFLOAT ||
        requested_format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ||
        requested_color_space != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
        // A swapchain may only use a pair the surface lists
        // (VUID-VkSwapchainCreateInfoKHR-imageFormat-01273).
        const bool listed = std::ranges::any_of(formats, [&](const VkSurfaceFormatKHR& format) {
            return format.format == requested_format &&
                   format.colorSpace == requested_color_space;
        });
        if (listed) {
            return {
                .format = requested_format,
                .colorSpace = requested_color_space,
            };
        }
        LOG_ERROR(Render_Vulkan, "The surface has no {} {} format, presenting SDR",
                  string_VkFormat(requested_format), string_VkColorSpaceKHR(requested_color_space));
    }
    if (formats.size() == 1 && formats[0].format == VK_FORMAT_UNDEFINED) {
        VkSurfaceFormatKHR format;
        format.format = VK_FORMAT_B8G8R8A8_UNORM;
        format.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        return format;
    }
    const auto& found = std::find_if(formats.begin(), formats.end(), [](const auto& format) {
        return format.format == VK_FORMAT_B8G8R8A8_UNORM &&
               format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    });
    if (found != formats.end()) {
        return *found;
    }
    const auto sdr = std::find_if(formats.begin(), formats.end(), [](const auto& format) {
        return format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    });
    return sdr != formats.end() ? *sdr : formats[0];
}

static VkPresentModeKHR ChooseSwapPresentMode(bool has_imm, bool has_mailbox,
                                              bool has_fifo_relaxed, VkFormat format) {
    // DLSS Frame Generation on Vulkan doesn't support VSync.
    if (FrameGenPresents(format) && (has_imm || has_mailbox)) {
        return has_imm ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_MAILBOX_KHR;
    }
    // Mailbox doesn't lock the application like FIFO (vsync)
    // FIFO present mode locks the framerate to the monitor's refresh rate
    Settings::VSyncMode setting = [has_imm, has_mailbox]() {
        // Choose Mailbox or Immediate if unlocked and those modes are supported
        const auto mode = Settings::values.vsync_mode.GetValue();
        if (FluidMode() != 0) {
            // The FLUID presenter times each present to be shown at the next vblank: FIFO, or
            // MAILBOX with EDEN_NX2_FLUID_MAILBOX, or IMMEDIATE with EDEN_NX2_FLUID_IMMEDIATE,
            // when the surface lists it.
            return ChooseFluidPresentMode(FluidMailboxRequested(), has_mailbox,
                                          FluidImmediateRequested(), has_imm);
        }
        if (Settings::values.frame_gen.GetValue()) {
            return mode == Settings::VSyncMode::FifoRelaxed ? mode : Settings::VSyncMode::Fifo;
        }
        if (Settings::values.use_speed_limit.GetValue() &&
            Settings::values.current_speed_mode.GetValue() != Settings::SpeedMode::Turbo) {
            return mode;
        }
        switch (mode) {
        case Settings::VSyncMode::Fifo:
        case Settings::VSyncMode::FifoRelaxed:
            if (has_mailbox) {
                return Settings::VSyncMode::Mailbox;
            } else if (has_imm) {
                return Settings::VSyncMode::Immediate;
            }
            [[fallthrough]];
        default:
            return mode;
        }
    }();
    if (setting == Settings::VSyncMode::Immediate && !has_imm) {
        setting = Settings::VSyncMode::Mailbox;
    }
    if ((setting == Settings::VSyncMode::Mailbox && !has_mailbox) ||
        (setting == Settings::VSyncMode::FifoRelaxed && !has_fifo_relaxed)) {
        setting = Settings::VSyncMode::Fifo;
    }

    switch (setting) {
    case Settings::VSyncMode::Immediate:
        return VK_PRESENT_MODE_IMMEDIATE_KHR;
    case Settings::VSyncMode::Mailbox:
        return VK_PRESENT_MODE_MAILBOX_KHR;
    case Settings::VSyncMode::Fifo:
        return VK_PRESENT_MODE_FIFO_KHR;
    case Settings::VSyncMode::FifoRelaxed:
        return VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    default:
        return VK_PRESENT_MODE_FIFO_KHR;
    }
}

const char* PresentModeName(VkPresentModeKHR mode) {
    switch (mode) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR:
        return "IMMEDIATE";
    case VK_PRESENT_MODE_MAILBOX_KHR:
        return "MAILBOX";
    case VK_PRESENT_MODE_FIFO_KHR:
        return "FIFO";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR:
        return "FIFO_RELAXED";
    default:
        return "other";
    }
}

VkExtent2D ChooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities, u32 width, u32 height) {
    constexpr auto undefined_size{(std::numeric_limits<u32>::max)()};
    if (capabilities.currentExtent.width != undefined_size) {
        return capabilities.currentExtent;
    }
    VkExtent2D extent;
    extent.width = (std::max)(capabilities.minImageExtent.width,
                            (std::min)(capabilities.maxImageExtent.width, width));
    extent.height = (std::max)(capabilities.minImageExtent.height,
                             (std::min)(capabilities.maxImageExtent.height, height));
    return extent;
}

VkCompositeAlphaFlagBitsKHR ChooseAlphaFlags(const VkSurfaceCapabilitiesKHR& capabilities) {
    if (capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) {
        return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    } else if (capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR) {
        return VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    } else {
        LOG_ERROR(Render_Vulkan, "Unknown composite alpha flags value {:#x}",
                  capabilities.supportedCompositeAlpha);
        return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    }
}

} // Anonymous namespace

std::optional<VkSurfaceFormatKHR> ChooseHdrSurfaceFormat(
    std::span<const VkSurfaceFormatKHR> formats, Settings::HdrSwapchain preferred) {
    constexpr VkSurfaceFormatKHR hdr10{VK_FORMAT_A2B10G10R10_UNORM_PACK32,
                                       VK_COLOR_SPACE_HDR10_ST2084_EXT};
    constexpr VkSurfaceFormatKHR scrgb{VK_FORMAT_R16G16B16A16_SFLOAT,
                                       VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT};
    const auto listed = [&](const VkSurfaceFormatKHR& wanted) {
        return std::ranges::any_of(formats, [&](const VkSurfaceFormatKHR& format) {
            return format.format == wanted.format && format.colorSpace == wanted.colorSpace;
        });
    };
    const auto& first = preferred == Settings::HdrSwapchain::Scrgb ? scrgb : hdr10;
    const auto& second = preferred == Settings::HdrSwapchain::Scrgb ? hdr10 : scrgb;
    if (listed(first)) {
        return first;
    }
    if (listed(second)) {
        return second;
    }
    return std::nullopt;
}

Swapchain::Swapchain(
    VkSurfaceKHR_T* surface_,
    const Device& device_,
    Scheduler& scheduler_,
    u32 width_,
    u32 height_,
    VkSurfaceFormatKHR requested)
    : surface(surface_)
    , device{device_}
    , scheduler{scheduler_}
{
    if (!Create(surface, width_, height_, requested.format, requested.colorSpace)) {
        LOG_ERROR(Render_Vulkan, "Initial swapchain creation failed: surface has zero extent");
        throw vk::Exception(VK_ERROR_INITIALIZATION_FAILED);
    }
}

Swapchain::~Swapchain() {
    Common::DisplayHdrMonitor::Instance().SetPresentingHdr(false);
}

bool Swapchain::Create(
    VkSurfaceKHR_T* surface_,
    u32 width_,
    u32 height_,
    VkFormat requested_format_,
    VkColorSpaceKHR requested_color_space_)
{
    is_outdated = false;
    // Keep recreation pending until creation succeeds, including zero-extent surfaces.
    is_suboptimal = true;
    width = width_;
    height = height_;
    surface = surface_;
    requested_format = requested_format_;
    requested_color_space = requested_color_space_;

    const auto physical_device = device.GetPhysical();
    const auto capabilities{physical_device.GetSurfaceCapabilitiesKHR(VkSurfaceKHR(surface))};
    if (!PresentPolicy::HasExtent(capabilities.maxImageExtent,
                                  ChooseSwapExtent(capabilities, width, height))) {
        return false;
    }

    // Submitted work may still wait on the old acquire/present semaphores; destroying them under
    // it loses the device on MoltenVK (window moved to a display with a different scale).
    if (swapchain) {
        std::scoped_lock lock{scheduler.submit_mutex};
        const VkResult result = device.GetLogical().WaitIdle();
        if (result == VK_ERROR_DEVICE_LOST) {
            device.ReportLoss();
        }
        vk::Check(result);
    }
    Destroy();

    if (!CreateSwapchain(capabilities)) {
        return false;
    }
    CreateSemaphores();
    is_suboptimal = false;

    resource_ticks.clear();
    resource_ticks.resize(image_count);
    return true;
}

PresentPolicy::Acquire Swapchain::AcquireNextImage(bool may_flush, bool pace) {
    u32 acquired_index = 0;
    const VkResult result = device.GetLogical().AcquireNextImageKHR(
        *swapchain, (std::numeric_limits<u64>::max)(), *present_semaphores[frame_index],
        VK_NULL_HANDLE, &acquired_index);
    const auto action = PresentPolicy::ClassifyAcquire(result);
    switch (action) {
    case PresentPolicy::Acquire::Ready:
        image_index = acquired_index;
        if (result == VK_SUBOPTIMAL_KHR) {
            is_suboptimal = true;
        }
        break;
    case PresentPolicy::Acquire::Recreate:
        is_outdated = true;
        return action;
    case PresentPolicy::Acquire::Drop:
        return action;
    case PresentPolicy::Acquire::DeviceLost:
        device.ReportLoss();
        [[fallthrough]];
    case PresentPolicy::Acquire::Error:
        throw vk::Exception(result);
    }

    const auto wait_with_frame_pacing = [this, may_flush] {
    switch (Settings::values.frame_pacing_mode.GetValue()) {
    case Settings::FramePacingMode::Target_Auto:
        scheduler.Wait(resource_ticks[image_index], 0.0, may_flush);
        break;
    case Settings::FramePacingMode::Target_30:
        scheduler.Wait(resource_ticks[image_index], 30.0, may_flush);
        break;
    case Settings::FramePacingMode::Target_60:
        scheduler.Wait(resource_ticks[image_index], 60.0, may_flush);
        break;
    case Settings::FramePacingMode::Target_90:
        scheduler.Wait(resource_ticks[image_index], 90.0, may_flush);
        break;
    case Settings::FramePacingMode::Target_120:
        scheduler.Wait(resource_ticks[image_index], 120.0, may_flush);
        break;
    }
    };

    // The FLUID presenter keeps its own time, and the last copy to an image is done before it
    // presents again.
    if (pace) {
#ifdef __ANDROID__
        if (android_get_device_api_level() >= 30) {
            scheduler.Wait(resource_ticks[image_index], 0.0, may_flush);
        } else {
            wait_with_frame_pacing();
        }
#else
        wait_with_frame_pacing();
#endif
    }

    resource_ticks[image_index] = scheduler.CurrentTick();

    // A suboptimal acquire still acquired an image and signals its semaphore: present it, and let
    // NeedsRecreation() recreate at the next frame.
    return PresentPolicy::Acquire::Ready;
}

void Swapchain::Present(VkSemaphore render_semaphore) {
    const auto present_queue{device.GetPresentQueue()};
    const VkPresentInfoKHR present_info{
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .pNext = nullptr,
        .waitSemaphoreCount = render_semaphore ? 1U : 0U,
        .pWaitSemaphores = &render_semaphore,
        .swapchainCount = 1,
        .pSwapchains = swapchain.address(),
        .pImageIndices = &image_index,
        .pResults = nullptr,
    };
    const auto lock_start = std::chrono::steady_clock::now();
    std::scoped_lock lock{scheduler.submit_mutex};
    const auto locked = std::chrono::steady_clock::now();
    const VkResult present_result = present_queue.Present(present_info);
    last_present_lock_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(locked - lock_start).count();
    last_present_call_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now() - locked)
                               .count();
    switch (const VkResult result = present_result) {
    case VK_SUCCESS:
        break;
    case VK_SUBOPTIMAL_KHR:
        LOG_DEBUG(Render_Vulkan, "Suboptimal swapchain");
        break;
    case VK_ERROR_OUT_OF_DATE_KHR:
    case VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT:
        is_outdated = true;
        break;
    case VK_ERROR_SURFACE_LOST_KHR:
        vk::Check(result);
        break;
    case VK_ERROR_DEVICE_LOST:
        device.ReportLoss();
        throw vk::Exception(result);
    default:
        LOG_CRITICAL(Render_Vulkan, "Failed to present with error {}", string_VkResult(result));
        break;
    }
    ++frame_index;
    if (frame_index >= image_count) {
        frame_index = 0;
    }
}

bool Swapchain::CreateSwapchain(const VkSurfaceCapabilitiesKHR& capabilities) {
    const auto physical_device{device.GetPhysical()};
    const auto formats{physical_device.GetSurfaceFormatsKHR(VkSurfaceKHR(surface))};
    const auto present_modes = physical_device.GetSurfacePresentModesKHR(VkSurfaceKHR(surface));

    // Keep the SDR choice available even while the active swapchain is HDR.
    sdr_image_format = ChooseSwapSurfaceFormat(formats, VK_FORMAT_B8G8R8A8_UNORM,
                                              VK_COLOR_SPACE_SRGB_NONLINEAR_KHR).format;

    has_mailbox = std::find(present_modes.begin(), present_modes.end(), VK_PRESENT_MODE_MAILBOX_KHR)
                  != present_modes.end();
    has_imm = std::find(present_modes.begin(), present_modes.end(),
                        VK_PRESENT_MODE_IMMEDIATE_KHR) != present_modes.end();
    has_fifo_relaxed = std::find(present_modes.begin(), present_modes.end(),
                                 VK_PRESENT_MODE_FIFO_RELAXED_KHR) != present_modes.end();

    const VkCompositeAlphaFlagBitsKHR alpha_flags{ChooseAlphaFlags(capabilities)};
    surface_format = ChooseSwapSurfaceFormat(formats, requested_format, requested_color_space);
    Common::DisplayHdrMonitor::Instance().SetPresentingHdr(surface_format.colorSpace !=
                                                           VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);
#ifdef EGLANTINE_STREAMLINE
    // The old swapchain is gone, so DLSS-G unloads before the new one is made.
    if (Streamline::FrameGenSession() &&
        !Streamline::FrameGenSupportsSwapchain(surface_format.format)) {
        UnloadFrameGen(scheduler, device,
                       "scRGB HDR output; set the HDR format to HDR10 to use it");
    }
#endif
    present_mode =
        ChooseSwapPresentMode(has_imm, has_mailbox, has_fifo_relaxed, surface_format.format);

    u32 requested_image_count{capabilities.minImageCount + 1};
    // Ensure Triple buffering if possible.
    if (capabilities.maxImageCount > 0) {
        if (requested_image_count > capabilities.maxImageCount) {
            requested_image_count = capabilities.maxImageCount;
        } else {
            requested_image_count =
                (std::max)(requested_image_count, (std::min)(3U, capabilities.maxImageCount));
        }
    } else {
        requested_image_count = (std::max)(requested_image_count, 3U);
    }
    // EDEN_NX2_FLUID showing more than 75 frames a second: five images. With three, the copy for
    // a 120 Hz slot waited for a free image (Arc 140T: slots on time 790-801 of 1200 with three,
    // 1134-1137 with five, copies done after their vblank about 500 per 10 s to 0).
    // EDEN_NX2_FLUID_COPY_AHEAD keeps an image acquired across a slot: at least four.
    const auto env_on = [](const char* name) {
        const char* value = std::getenv(name);
        return value != nullptr && std::strtoul(value, nullptr, 10) != 0;
    };
    if (env_on("EDEN_NX2_FLUID")) {
        const char* shown = std::getenv("EDEN_NX2_SHOWN_RATE");
        const u32 rate = shown != nullptr ? static_cast<u32>(std::strtoul(shown, nullptr, 10))
                                          : Settings::FrameGenTargetRate();
        const u32 least = rate > 75 ? 5U : env_on("EDEN_NX2_FLUID_COPY_AHEAD") ? 4U : 0U;
        const u32 most = capabilities.maxImageCount > 0 ? capabilities.maxImageCount : 16U;
        requested_image_count = (std::min)((std::max)(requested_image_count, least), most);
    }
    // EDEN_NX2_SWAPCHAIN_IMAGES=<n> (2 to 16): ask for n images, within what the surface allows.
    if (const char* images = std::getenv("EDEN_NX2_SWAPCHAIN_IMAGES")) {
        const u32 asked = std::clamp<u32>(static_cast<u32>(std::strtoul(images, nullptr, 10)), 2U, 16U);
        const u32 most = capabilities.maxImageCount > 0 ? capabilities.maxImageCount : 16U;
        requested_image_count = std::clamp(asked, capabilities.minImageCount, most);
    }
    // EDEN_NX2_FLUID_COMPOSE: the FLUID presenter's slot pass writes the images through storage
    // views. Asked for only with a dedicated queue that can compute, a present thread, a surface
    // that lists the usage and a format that can be stored to.
    bool with_storage = FluidComposeMode() != 0 && FluidMode() != 0 &&
                        device.FluidFamilyCanCompute() && !FrameGenLoaded() &&
                        (Settings::values.async_presentation.GetValue() ||
                         Settings::values.frame_gen.GetValue()) &&
                        surface_format.format == VK_FORMAT_B8G8R8A8_UNORM &&
                        surface_format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    if (with_storage &&
        ((capabilities.supportedUsageFlags & VK_IMAGE_USAGE_STORAGE_BIT) == 0 ||
         (physical_device.GetFormatProperties(surface_format.format).optimalTilingFeatures &
          VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) == 0)) {
        static bool logged = false;
        if (!std::exchange(logged, true)) {
            LOG_INFO(Render_Vulkan,
                     "EDEN_NX2_FLUID_COMPOSE: the surface offers no storage images in {}; frames "
                     "are shown by the screen copy",
                     string_VkFormat(surface_format.format));
        }
        with_storage = false;
    }
    VkSwapchainCreateInfoKHR swapchain_ci{
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .pNext = nullptr,
        .flags = 0,
        .surface = VkSurfaceKHR(surface),
        .minImageCount = requested_image_count,
        .imageFormat = surface_format.format,
        .imageColorSpace = surface_format.colorSpace,
        .imageExtent = {},
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
#ifdef __ANDROID__
        // On Android, do not allow surface rotation to deviate from the frontend.
        .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
#else
        .preTransform = capabilities.currentTransform,
#endif
        .compositeAlpha = alpha_flags,
        .presentMode = present_mode,
        .clipped = VK_FALSE,
        .oldSwapchain = VkSwapchainKHR{},
    };
    if (with_storage) {
        swapchain_ci.imageUsage |= VK_IMAGE_USAGE_STORAGE_BIT;
    }
    const u32 graphics_family{device.GetGraphicsFamily()};
    const u32 present_family{device.GetPresentFamily()};
    // The families that touch the images: the renderer's, the presenting one and, with
    // EDEN_NX2_FLUID_QUEUE, the one that copies to the screen.
    std::array<u32, 3> queue_indices{graphics_family};
    u32 queue_index_count = 1;
    for (const u32 family : {present_family, device.HasFluidQueue() ? device.GetFluidFamily()
                                                                    : graphics_family}) {
        if (std::find(queue_indices.begin(), queue_indices.begin() + queue_index_count, family) ==
            queue_indices.begin() + queue_index_count) {
            queue_indices[queue_index_count++] = family;
        }
    }
    if (queue_index_count > 1) {
        swapchain_ci.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        swapchain_ci.queueFamilyIndexCount = queue_index_count;
        swapchain_ci.pQueueFamilyIndices = queue_indices.data();
    }
    // According to Vulkan spec, when using VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR,
    // the base format (imageFormat) MUST be included in pViewFormats
    const std::array view_formats{
        swapchain_ci.imageFormat,  // Base format MUST be first
        VK_FORMAT_B8G8R8A8_UNORM,
        VK_FORMAT_B8G8R8A8_SRGB,
#ifdef __ANDROID__
        VK_FORMAT_R8G8B8A8_UNORM,  // Android may use RGBA
        VK_FORMAT_R8G8B8A8_SRGB,
#endif
    };
    VkImageFormatListCreateInfo format_list{
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO_KHR,
        .pNext = nullptr,
        .viewFormatCount = static_cast<u32>(view_formats.size()),
        .pViewFormats = view_formats.data(),
    };
    if (device.IsKhrSwapchainMutableFormatEnabled() &&
        surface_format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
        format_list.pNext = std::exchange(swapchain_ci.pNext, &format_list);
        swapchain_ci.flags |= VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR;
    }
    // A storage image cannot be viewed as sRGB: with storage usage the list holds the UNORM formats.
    const std::array<VkFormat, 2> storage_view_formats{swapchain_ci.imageFormat,
                                                       VK_FORMAT_B8G8R8A8_UNORM};
    if (with_storage) {
        format_list.viewFormatCount = static_cast<u32>(storage_view_formats.size());
        format_list.pViewFormats = storage_view_formats.data();
    }
    // Request the size again to reduce the possibility of a TOCTOU race condition.
    const auto updated_capabilities = physical_device.GetSurfaceCapabilitiesKHR(VkSurfaceKHR(surface));
    swapchain_ci.imageExtent = ChooseSwapExtent(updated_capabilities, width, height);
    if (!PresentPolicy::HasExtent(updated_capabilities.maxImageExtent, swapchain_ci.imageExtent)) {
        return false;
    }
    // Don't add code within this and the swapchain creation.
    try {
        try {
            swapchain = device.GetLogical().CreateSwapchainKHR(swapchain_ci);
        } catch (const vk::Exception& storage_exception) {
            // EDEN_NX2_FLUID_COMPOSE: a driver that refuses the storage usage still gets the
            // swapchain main makes, and the frames are shown by the screen copy.
            const VkResult storage_result = storage_exception.GetResult();
            if (!with_storage || storage_result == VK_ERROR_SURFACE_LOST_KHR ||
                storage_result == VK_ERROR_NATIVE_WINDOW_IN_USE_KHR ||
                storage_result == VK_ERROR_OUT_OF_DATE_KHR ||
                storage_result == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT) {
                throw;
            }
            LOG_WARNING(Render_Vulkan,
                        "EDEN_NX2_FLUID_COMPOSE: the swapchain cannot be made with storage usage "
                        "({}); frames are shown by the screen copy",
                        storage_exception.what());
            with_storage = false;
            swapchain_ci.imageUsage &= ~static_cast<VkImageUsageFlags>(VK_IMAGE_USAGE_STORAGE_BIT);
            format_list.viewFormatCount = static_cast<u32>(view_formats.size());
            format_list.pViewFormats = view_formats.data();
            swapchain = device.GetLogical().CreateSwapchainKHR(swapchain_ci);
        }
    } catch ([[maybe_unused]] const vk::Exception& exception) {
        // A lost surface or a window still in use is transient: the present loop retries those.
        const VkResult result = exception.GetResult();
        if (!FrameGenLoaded() || result == VK_ERROR_SURFACE_LOST_KHR ||
            result == VK_ERROR_NATIVE_WINDOW_IN_USE_KHR) {
            throw;
        }
#ifdef EGLANTINE_STREAMLINE
        // Keep the game running without frame generation: DLSS-G unloaded, the hooks reach the
        // driver's swapchain.
        UnloadFrameGen(scheduler, device,
                       fmt::format("Streamline's swapchain failed ({})", exception.what()));
#endif
        present_mode =
            ChooseSwapPresentMode(has_imm, has_mailbox, has_fifo_relaxed, surface_format.format);
        swapchain_ci.presentMode = present_mode;
        swapchain = device.GetLogical().CreateSwapchainKHR(swapchain_ci);
    }

    extent = swapchain_ci.imageExtent;

    images = swapchain.GetImages();
    image_count = static_cast<u32>(images.size());
    if (with_storage) {
        // One storage view of each image, in the format it has, for the slot pass.
        try {
            for (const VkImage image : images) {
                storage_views.push_back(device.GetLogical().CreateImageView({
                    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                    .pNext = nullptr,
                    .flags = 0,
                    .image = image,
                    .viewType = VK_IMAGE_VIEW_TYPE_2D,
                    .format = surface_format.format,
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
                }));
                storage_view_handles.push_back(*storage_views.back());
            }
        } catch (const vk::Exception& exception) {
            LOG_WARNING(Render_Vulkan,
                        "EDEN_NX2_FLUID_COMPOSE: no storage view of a swapchain image ({}); frames "
                        "are shown by the screen copy",
                        exception.what());
            storage_view_handles.clear();
            storage_views.clear();
            with_storage = false;
        }
    }
    storage_enabled.store(with_storage, std::memory_order_relaxed);
    ++storage_generation;
    if (present_mode != logged_present_mode || image_count != logged_image_count) {
        logged_present_mode = present_mode;
        logged_image_count = image_count;
        LOG_INFO(Render_Vulkan,
                 "Swapchain present mode {} with {} images (vsync setting {}, frame generation "
                 "{}, EDEN_NX2_FLUID {})",
                 PresentModeName(present_mode), image_count,
                 Settings::CanonicalizeEnum(Settings::values.vsync_mode.GetValue()),
                 Settings::values.frame_gen.GetValue() ? "on" : "off", FluidMode());
    }
    if (FluidMode() != 0 && FluidMailboxRequested()) {
        LOG_INFO(Render_Vulkan, "EDEN_NX2_FLUID_MAILBOX: present mode {} (surface {} MAILBOX)",
                 PresentModeName(present_mode), has_mailbox ? "offers" : "does not offer");
    }
    if (FluidMode() != 0 && FluidImmediateRequested()) {
        LOG_INFO(Render_Vulkan, "EDEN_NX2_FLUID_IMMEDIATE: present mode {} (surface {} IMMEDIATE)",
                 PresentModeName(present_mode), has_imm ? "offers" : "does not offer");
    }
#ifdef __ANDROID__
    // Android is already ordered the same as Switch.
    image_view_format = VK_FORMAT_R8G8B8A8_UNORM;
#else
    image_view_format = VK_FORMAT_B8G8R8A8_UNORM;
#endif
    if (surface_format.colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
        image_view_format = surface_format.format;
    }

    // Frame generation asks for FIFO (or FIFO relaxed) whatever the vsync setting, unless DLSS-G
    // presents the frames itself.
    std::string supported_modes;
    for (const VkPresentModeKHR mode : present_modes) {
        if (!supported_modes.empty()) {
            supported_modes += ", ";
        }
        supported_modes += string_VkPresentModeKHR(mode);
    }
    LOG_INFO(Render_Vulkan,
             "Swapchain: {}, {} images ({} asked, surface min {} max {}), {}x{} {} {}; surface "
             "present modes: {}; vsync setting {}, frame generation {}{}",
             string_VkPresentModeKHR(present_mode), image_count, requested_image_count,
             capabilities.minImageCount, capabilities.maxImageCount, extent.width, extent.height,
             string_VkFormat(surface_format.format),
             string_VkColorSpaceKHR(surface_format.colorSpace), supported_modes,
             Settings::CanonicalizeEnum(Settings::values.vsync_mode.GetValue()),
             Settings::values.frame_gen.GetValue() ? "on" : "off",
             FrameGenPresents(surface_format.format) ? ", DLSS-G presents" : "");
    return true;
}

void Swapchain::CreateSemaphores() {
    present_semaphores.resize(image_count);
    std::ranges::generate(present_semaphores,
                          [this] { return device.GetLogical().CreateSemaphore(); });
    render_semaphores.resize(image_count);
    std::ranges::generate(render_semaphores,
                          [this] { return device.GetLogical().CreateSemaphore(); });
}

void Swapchain::Destroy() {
    frame_index = 0;
    image_index = 0;
    present_semaphores.clear();
    render_semaphores.clear();
    // The views go before the images they view.
    storage_enabled.store(false, std::memory_order_relaxed);
    storage_view_handles.clear();
    storage_views.clear();
    swapchain.reset();
}

void Swapchain::SetHdrMetadata(u16 peak_luminance, u16 reference_white) const {
    if (!device.IsExtHdrMetadataEnabled() ||
        surface_format.colorSpace != VK_COLOR_SPACE_HDR10_ST2084_EXT) {
        return;
    }
    // The game tone maps to the peak it was told about, so that is the brightest the stream
    // gets. Mastering primaries are BT.2020 with a D65 white (the colour space's own); the frame
    // average is not known.
    const VkHdrMetadataEXT metadata{
        .sType = VK_STRUCTURE_TYPE_HDR_METADATA_EXT,
        .pNext = nullptr,
        .displayPrimaryRed = {0.708f, 0.292f},
        .displayPrimaryGreen = {0.170f, 0.797f},
        .displayPrimaryBlue = {0.131f, 0.046f},
        .whitePoint = {0.3127f, 0.3290f},
        .maxLuminance = static_cast<float>(peak_luminance),
        .minLuminance = 0.0f,
        .maxContentLightLevel = static_cast<float>(peak_luminance),
        .maxFrameAverageLightLevel = 0.0f,
    };
    device.GetLogical().SetHdrMetadataEXT(*swapchain, metadata);
    LOG_INFO(Render_Vulkan, "HDR10 metadata: peak {} nits (reference white {} nits)",
             peak_luminance, reference_white);
}

bool Swapchain::NeedsPresentModeUpdate() const {
    const auto requested_mode =
        ChooseSwapPresentMode(has_imm, has_mailbox, has_fifo_relaxed, surface_format.format);
    return present_mode != requested_mode;
}

} // namespace Vulkan
