// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>

#include "common/common_types.h"
#include "common/dynamic_library.h"
#include "core/frontend/emu_window.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

/**
 * Create a Vulkan instance
 *
 * @param library            Dynamic library to load the Vulkan instance from
 * @param dld                Dispatch table to load function pointers into
 * @param required_version   Required Vulkan version (for example, VK_API_VERSION_1_1)
 * @param window_type        Window system type's enabled extension
 * @param enable_validation  Whether to enable Vulkan validation layers or not
 *
 * @return A new Vulkan instance
 * @throw vk::Exception on failure
 */
[[nodiscard]] vk::Instance CreateInstance(
    const Common::DynamicLibrary& library, vk::InstanceDispatch& dld, u32 required_version,
    Core::Frontend::WindowSystemType window_type = Core::Frontend::WindowSystemType::Headless,
    bool enable_validation = false);

/**
 * Starts creating the instance on another thread, to be taken by the next CreateInstance of the
 * same window type and validation setting (the renderer's, later in the boot). Does nothing
 * unless EDEN_NX2_BOOT_ASYNC=1 or EDEN_NX2_VULKAN_INSTANCE_ONCE is on, and never on Android or
 * with Streamline. A failure or a different setting leaves CreateInstance creating its own.
 */
void PrecreateInstance(Core::Frontend::WindowSystemType window_type, bool enable_validation);

/// EDEN_NX2_VULKAN_INSTANCE_ONCE: on when set to a number other than 0 (unset, empty, 0 and false
/// are off). Always off on Android and with Streamline.
[[nodiscard]] bool InstanceOnceEnabled();

using InstanceUse = std::function<void(const vk::Instance&, const vk::InstanceDispatch&)>;

/**
 * EDEN_NX2_VULKAN_INSTANCE_ONCE: calls `use` with the instance made ahead (started here when none
 * is under way), and leaves it for the renderer's CreateInstance to take, so the frontend's
 * device list and the renderer share one vkCreateInstance instead of paying for two. `use` runs
 * with the instance locked: the renderer waits for it, and it must not keep the instance.
 *
 * @return False, without calling `use`, when the switch is off, when what was made ahead is for
 *         another window type or validation setting, when its creation failed, or when the
 *         renderer took it already; the caller then creates its own instance as before.
 * @throw whatever `use` throws
 */
[[nodiscard]] bool UseEarlyInstance(Core::Frontend::WindowSystemType window_type,
                                    bool enable_validation, const InstanceUse& use);

} // namespace Vulkan
