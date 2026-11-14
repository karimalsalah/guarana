// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <optional>
#include <span>
#include <vector>

#include "common/common_types.h"
#include "common/settings_enums.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Layout {
struct FramebufferLayout;
}

namespace Vulkan {

class Device;
class Scheduler;

/// The HDR swapchain pair the surface lists: the preferred encoding, else the other one, else
/// nullopt (present SDR).
std::optional<VkSurfaceFormatKHR> ChooseHdrSurfaceFormat(
    std::span<const VkSurfaceFormatKHR> formats, Settings::HdrSwapchain preferred);

class Swapchain {
public:
    /// Creates the initial swapchain in the requested format (SDR unless HDR output is active).
    /// Throws on startup failure rather than constructing an unusable presenter.
    explicit Swapchain(
        VkSurfaceKHR_T* surface,
        const Device& device,
        Scheduler& scheduler,
        u32 width,
        u32 height,
        VkSurfaceFormatKHR requested = {VK_FORMAT_B8G8R8A8_UNORM,
                                        VK_COLOR_SPACE_SRGB_NONLINEAR_KHR});
    ~Swapchain();

    /// Creates (or recreates) the swapchain; returns false when deferred for zero extent.
    bool Create(
        VkSurfaceKHR_T* surface,
        u32 width,
        u32 height,
        VkFormat requested_format = VK_FORMAT_B8G8R8A8_UNORM,
        VkColorSpaceKHR requested_color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);

    /// Acquires the next image in the swapchain, waits as needed. Returns true when no image was
    /// acquired (out of date) and the swapchain has to be recreated first.
    /// `may_flush`: false on the async present thread (see Scheduler::Wait).
    /// `pace`: false skips the wait for the image's last GPU tick and the frame pacing sleep.
    bool AcquireNextImage(bool may_flush = true, bool pace = true);

    /// Presents the rendered image to the swapchain.
    void Present(VkSemaphore render_semaphore);

    /// Returns true when the swapchain needs to be recreated.
    bool NeedsRecreation() const {
        return !swapchain || IsSubOptimal() || NeedsPresentModeUpdate();
    }

    bool IsCreated() const {
        return static_cast<bool>(swapchain);
    }

    /// Returns true when the swapchain is outdated.
    bool IsOutDated() const {
        return is_outdated;
    }

    /// Returns true when the swapchain is suboptimal.
    bool IsSubOptimal() const {
        return is_suboptimal;
    }

    VkExtent2D GetSize() const {
        return extent;
    }

    std::size_t GetImageCount() const {
        return image_count;
    }

    VkImage CurrentImage() const {
        return images[image_index];
    }

    VkFormat GetImageFormat() const {
        return surface_format.format;
    }

    /// Hands the display the HDR10 mastering metadata (VK_EXT_hdr_metadata) of an HDR10 swapchain.
    void SetHdrMetadata(u16 peak_luminance, u16 reference_white) const;

    /// Returns the cached SDR surface format, including while an HDR swapchain is active.
    VkFormat GetSdrImageFormat() const {
        return sdr_image_format.load();
    }

    /// Returns the producer format requested by the latest creation attempt.
    VkFormat GetRequestedFormat() const {
        return requested_format;
    }

    VkColorSpaceKHR GetRequestedColorSpace() const {
        return requested_color_space;
    }

    /// The index of the acquired image.
    u32 CurrentImageIndex() const {
        return image_index;
    }

    /// EDEN_NX2_FLUID_COMPOSE: whether the images were created with STORAGE usage and have the
    /// storage views StorageViews lists. Any thread.
    bool HasStorage() const {
        return storage_enabled.load(std::memory_order_relaxed);
    }

    /// Counts the swapchains made, so the descriptors of their storage views are written again.
    u64 StorageGeneration() const {
        return storage_generation;
    }

    /// A storage view of each image, in the format of the swapchain; empty without HasStorage.
    std::span<const VkImageView> StorageViews() const {
        return storage_view_handles;
    }

    VkSemaphore CurrentPresentSemaphore() const {
        return *present_semaphores[frame_index];
    }

    VkSemaphore CurrentRenderSemaphore() const {
        return *render_semaphores[image_index];
    }

    /// The last Present: nanoseconds waiting for the submit lock, then in vkQueuePresentKHR.
    s64 LastPresentLockNs() const {
        return last_present_lock_ns;
    }
    s64 LastPresentCallNs() const {
        return last_present_call_ns;
    }

    u32 GetWidth() const {
        return width;
    }

    u32 GetHeight() const {
        return height;
    }

    VkExtent2D GetExtent() const {
        return extent;
    }

private:
    void CreateSwapchain(const VkSurfaceCapabilitiesKHR& capabilities);
    void CreateSemaphores();
    void CreateImageViews();

    void Destroy();

    bool NeedsPresentModeUpdate() const;

    VkSurfaceKHR_T* surface;

    const Device& device;
    Scheduler& scheduler;

    vk::SwapchainKHR swapchain;

    std::size_t image_count{};
    std::vector<VkImage> images;
    std::vector<u64> resource_ticks;
    std::vector<vk::Semaphore> present_semaphores;
    std::vector<vk::Semaphore> render_semaphores;
    std::vector<vk::ImageView> storage_views;
    std::vector<VkImageView> storage_view_handles;
    std::atomic<bool> storage_enabled{};
    u64 storage_generation{};

    u32 width;
    u32 height;

    u32 image_index{};
    u32 frame_index{};
    s64 last_present_lock_ns{};
    s64 last_present_call_ns{};

    VkFormat image_view_format{};
    VkExtent2D extent{};
    VkPresentModeKHR present_mode{};
    VkPresentModeKHR logged_present_mode{VK_PRESENT_MODE_MAX_ENUM_KHR};
    std::size_t logged_image_count{};
    VkSurfaceFormatKHR surface_format{};
    std::atomic<VkFormat> sdr_image_format{VK_FORMAT_B8G8R8A8_UNORM};
    VkFormat requested_format{VK_FORMAT_B8G8R8A8_UNORM};
    VkColorSpaceKHR requested_color_space{VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    bool has_imm{false};
    bool has_mailbox{false};
    bool has_fifo_relaxed{false};

    bool is_outdated{};
    bool is_suboptimal{};
};

} // namespace Vulkan
