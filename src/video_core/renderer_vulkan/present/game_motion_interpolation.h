// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <optional>

#include "common/common_types.h"
#include "video_core/renderer_vulkan/present/extrapolation_source.h"
#include "video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class Scheduler;

/// EDEN_NX2_FG_GAME_MOTION: in-between frames warped with the game's motion (the guides of the
/// two real frames around them) instead of optical flow, with what one frame no longer shows
/// filled from the other by depth (game_motion_warp.h, host_shaders/game_motion_interpolation.comp).
class GameMotionInterpolation {
public:
    explicit GameMotionInterpolation(const Device& device, MemoryAllocator& memory_allocator);
    ~GameMotionInterpolation();

    /// The newest real frame's guide, or none: copied, since the next frame overwrites the source.
    /// The guide before it becomes the previous one. Call once per real frame, in its recording.
    void Push(const Device& device, Scheduler& scheduler,
              const std::optional<ExtrapolationSource>& source);

    /// Both real frames of the interval have a guide of the same size and no camera cut separates
    /// them.
    [[nodiscard]] bool Ready() const;

    /// Writes the frame `t` of the way from `previous_color` (0) to `newest_color` (1) into
    /// `destination`. `screen` is the rectangle of the frame the scene covers. Call after Push of
    /// the newest frame, while its source's HUD images are still this frame's.
    void Generate(const Device& device, Scheduler& scheduler, VkImageView newest_color,
                  VkImageView previous_color, VkImage destination,
                  VkImageView destination_storage_view, VkExtent2D extent, VkRect2D screen,
                  f32 t);

private:
    struct Guide {
        vk::Image image;
        vk::ImageView view;
        VkExtent2D extent{};
        bool valid{};
    };

    MemoryAllocator& memory_allocator;
    std::array<Guide, 2> guides;
    u32 newest{}; ///< index of the newest real frame's guide in `guides`
    bool newest_cut{};
    std::optional<ExtrapolationSource> newest_source;

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
