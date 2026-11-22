// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <vulkan/vulkan_core.h>

namespace Vulkan::PresentPolicy {
enum class Acquire { Ready, Recreate, Drop, DeviceLost, Error };

constexpr Acquire ClassifyAcquire(VkResult result) {
    switch (result) {
    case VK_SUCCESS:
    case VK_SUBOPTIMAL_KHR:
        return Acquire::Ready;
    case VK_ERROR_OUT_OF_DATE_KHR:
    case VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT:
        return Acquire::Recreate;
    case VK_TIMEOUT:
    case VK_NOT_READY:
        return Acquire::Drop;
    case VK_ERROR_DEVICE_LOST:
        return Acquire::DeviceLost;
    default:
        return Acquire::Error;
    }
}

constexpr bool HasExtent(VkExtent2D maximum, VkExtent2D chosen) {
    return maximum.width != 0 && maximum.height != 0 && chosen.width != 0 && chosen.height != 0;
}
} // namespace Vulkan::PresentPolicy
