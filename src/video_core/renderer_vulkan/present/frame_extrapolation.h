// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>

#include "common/common_types.h"
#include "video_core/renderer_vulkan/present/extrapolation_source.h"
#include "video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class Scheduler;

/// Predicts frames after the presented one by moving its pixels along the game's motion vectors.
class FrameExtrapolation {
public:
    explicit FrameExtrapolation(const Device& device);
    ~FrameExtrapolation();

    /// Writes the frame `advance` of a frame interval after `color` into `destination`. `screen`
    /// is the rectangle of the frame the scene covers.
    void Generate(const Device& device, Scheduler& scheduler, const ExtrapolationSource& source,
                  VkImageView color, VkImage destination, VkImageView destination_storage_view,
                  VkExtent2D extent, VkRect2D screen, f32 advance);

private:
    vk::Sampler linear_sampler;
    vk::Sampler nearest_sampler;
    vk::ShaderModule shader;
    vk::DescriptorSetLayout descriptor_layout;
    vk::PipelineLayout pipeline_layout;
    vk::Pipeline pipeline;
    std::optional<DescriptorPool> descriptor_pool;
    DescriptorAllocator descriptor_allocator;
};

} // namespace Vulkan
