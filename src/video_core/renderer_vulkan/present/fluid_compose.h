// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <span>

#include "common/common_types.h"
#include "video_core/renderer_vulkan/fluid_compose_math.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;

/// EDEN_NX2_FLUID_COMPOSE: the compute pipeline of the FLUID presenter's slot pass
/// (host_shaders/fluid_compose.comp). It reads a frame through one descriptor set per frame and
/// writes a swapchain image through one set per swapchain image; both are written outside the
/// slots (when the frame's image or the swapchain is created), so a slot only binds them.
class FluidCompose {
public:
    /// Builds the pipeline and a descriptor set for each of `frame_count` frames.
    /// Throws vk::Exception when the pipeline or the sets cannot be made.
    explicit FluidCompose(const Device& device, std::size_t frame_count);
    ~FluidCompose();

    FluidCompose(const FluidCompose&) = delete;
    FluidCompose& operator=(const FluidCompose&) = delete;

    /// Points the frame's set at the view of its image, which is sampled in GENERAL layout. The
    /// last slot pass that read the frame must be done.
    void SetFrameView(const Device& device, u32 frame_index, VkImageView view);

    /// Replaces the sets of the swapchain's images, one storage view each. The GPU must be idle.
    void SetSwapchainViews(const Device& device, std::span<const VkImageView> views);

    /// Binds the sets and dispatches the pass over `job.out_width` by `job.out_height` pixels.
    void Record(const vk::CommandBuffer& cmdbuf, u32 frame_index, u32 image_index,
                const FluidComposeMath::PushConstants& job) const;

    /// How many swapchain images have a set.
    [[nodiscard]] std::size_t SwapchainImageCount() const {
        return swapchain_sets.size();
    }

private:
    vk::ShaderModule shader;
    vk::Sampler sampler;
    vk::DescriptorSetLayout frame_layout;
    vk::DescriptorSetLayout swapchain_layout;
    vk::PipelineLayout pipeline_layout;
    vk::Pipeline pipeline;
    vk::DescriptorPool frame_pool;
    vk::DescriptorSets frame_sets;
    vk::DescriptorPool swapchain_pool;
    vk::DescriptorSets swapchain_sets;
};

} // namespace Vulkan
