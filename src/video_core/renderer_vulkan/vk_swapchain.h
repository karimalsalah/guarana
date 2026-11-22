// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

#include "common/common_types.h"
#include "common/settings_enums.h"
#include "video_core/renderer_vulkan/present_trace.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"
#include "video_core/renderer_vulkan/present_error_policy.h"

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

    /// Acquires the next image, returning Ready, Recreate or Drop; fatal results throw.
    /// `may_flush`: false on the async present thread (see Scheduler::Wait).
    /// `pace`: false skips the wait for the image's last GPU tick and the frame pacing sleep.
    /// `game_frame_presents`: how many presents show this game frame, its generated frames
    /// included; Frame Pacing Mode targets game frames.
    PresentPolicy::Acquire AcquireNextImage(bool may_flush = true, bool pace = true,
                                            u32 game_frame_presents = 1,
                                            PresentTrace* trace = nullptr);

    /// Presents the rendered image to the swapchain. A non-zero `present_id` is chained as the
    /// present's id when the swapchain has ids (HasPresentIds). Returns whether the present was
    /// accepted (VK_SUCCESS or VK_SUBOPTIMAL_KHR): only then can its id be waited for.
    bool Present(VkSemaphore render_semaphore, u64 present_id = 0);

    /// Asks for presents that carry ids and can be waited for. A swapchain made without them is
    /// recreated for it once (NeedsRecreation); nothing is recreated to drop them.
    void RequestPresentIds(bool requested) {
        present_ids_requested = requested;
    }

    /// Presents of this swapchain carry ids and can be waited for (VK_KHR_present_id2 and
    /// VK_KHR_present_wait2 where the surface supports them, else the first versions).
    bool HasPresentIds() const {
        return present_ids;
    }

    /// A present waits in the swapchain's queue for the display (every mode but MAILBOX, which
    /// replaces the waiting present), so the presenter can hold the next one until it is shown.
    bool WaitsForDisplay() const {
        return present_ids && present_mode != VK_PRESENT_MODE_MAILBOX_KHR;
    }

    /// vkWaitForPresentKHR (or vkWaitForPresent2KHR) on the swapchain of `generation`, for the
    /// present wait thread. Vulkan allows no other use of a swapchain during that wait, so it
    /// holds the lock AcquireNextImage, Present and Create take: keep `timeout_ns` short (they
    /// wait for it to end) and call again after VK_TIMEOUT. VK_ERROR_OUT_OF_DATE_KHR once that
    /// swapchain is gone.
    VkResult WaitForPresent(u64 generation, u64 present_id, u64 timeout_ns);

    /// Changes each time the swapchain is destroyed (present ids are per swapchain).
    u64 Generation() const {
        return generation.load(std::memory_order_acquire);
    }

    /// Returns true when the swapchain needs to be recreated.
    bool NeedsRecreation() const {
        return !swapchain || IsSubOptimal() || NeedsPresentModeUpdate() ||
               (present_ids_requested && !present_ids && !present_ids_refused);
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

    /// The present mode of the swapchain the last Create made.
    VkPresentModeKHR GetPresentMode() const {
        return present_mode;
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
    bool CreateSwapchain(const VkSurfaceCapabilitiesKHR& capabilities);
    void CreateSemaphores();
    void CreateImageViews();

    void Destroy();

    bool NeedsPresentModeUpdate() const;

    /// Locks the swapchain for an acquire, a present or its recreation, ahead of the present wait
    /// thread (WaitForPresent).
    [[nodiscard]] std::unique_lock<std::mutex> LockForUse();

    /// The surface takes VK_KHR_present_id2 ids and VK_KHR_present_wait2 waits.
    bool SurfaceSupportsPresentWait2() const;

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
    bool present_ids_requested{}; ///< RequestPresentIds
    bool present_ids{};           ///< Presents carry ids and can be waited for.
    bool present_ids2{};          ///< Through VK_KHR_present_id2/wait2, not the first versions.
    bool present_ids_refused{};   ///< Requested, but this swapchain can have none.
    bool present_ids2_failed{};   ///< No swapchain could be made with the second versions.
    bool present_wait_stalled{};  ///< A present wait ignored its timeout (guarded by use_mutex).

    bool is_outdated{};
    bool is_suboptimal{};

    /// Held for each use of the swapchain handle that Vulkan wants synchronized: acquires,
    /// presents, present waits and its destruction.
    std::mutex use_mutex;
    std::condition_variable use_cv;
    std::atomic<u32> use_requests{}; ///< Threads in LockForUse; WaitForPresent lets them in first.
    std::atomic<u64> generation{};
};

} // namespace Vulkan
