// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

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
 * unless EDEN_NX2_BOOT_ASYNC=1, on Android or with Streamline. A failure or a different setting
 * leaves CreateInstance creating its own.
 */
void PrecreateInstance(Core::Frontend::WindowSystemType window_type, bool enable_validation);

} // namespace Vulkan
