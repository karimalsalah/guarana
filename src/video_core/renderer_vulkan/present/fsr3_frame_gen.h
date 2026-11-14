// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <optional>
#include <vector>

#include "common/common_types.h"
#include "video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class Scheduler;

/// EDEN_NX2_FG_PINGPONG: the chain's two frame images swap roles every real frame, so only the
/// new frame is copied, into the image that held the older one. Image 0 is the one created as
/// "current", image 1 the one created as "history". Flow descriptor set 0 is written with image 0
/// at binding 0 (current) and image 1 at binding 1 (history), set 1 with the two exchanged, so the
/// set to bind is the parity of the number of swaps.
struct Fsr3PingPong {
    struct Roles {
        u32 current;
        u32 history;
    };

    /// The images flow descriptor set `set` reads; fixed for the life of the chain.
    [[nodiscard]] static constexpr Roles SetRoles(u32 set) noexcept {
        return (set & 1U) == 0 ? Roles{0U, 1U} : Roles{1U, 0U};
    }

    /// One swap of roles: the image that held the older frame becomes current.
    constexpr void Advance() noexcept {
        parity ^= 1U;
    }

    /// The flow descriptor set whose bindings match the images' roles now.
    [[nodiscard]] constexpr u32 FlowSet() const noexcept {
        return parity;
    }

    [[nodiscard]] constexpr Roles Now() const noexcept {
        return SetRoles(parity);
    }

    u32 parity{};
};

/// AMD FidelityFX Super Resolution 3 (FSR 3) Frame Generation pipeline.
/// Uses compute-based optical flow and bilateral frame interpolation
/// to synthesize intermediate frames without requiring engine motion vectors.
///
/// Optimizations:
///  - Quarter-resolution optical flow (16├ù fewer compute invocations)
///  - Shared-memory tile caching in both shaders
///  - Early-out for static pixels (skip diamond search)
///  - Duplicate-frame detection (skip optical flow entirely)
///  - Precise pipeline barriers instead of full-pipeline TransitionImageLayout
///  - Descriptor-write caching (only update when image views change)
class Fsr3FrameGen {
public:
    explicit Fsr3FrameGen(const Device& device, MemoryAllocator& memory_allocator,
                          VkExtent2D extent, VkFormat format);
    ~Fsr3FrameGen();

    void Process(const Device& device, Scheduler& scheduler, VkImage current_frame,
                 VkImageView current_view, u64 frame_count);

    void Generate(const Device& device, Scheduler& scheduler, VkImage destination,
                  VkImageView destination_storage_view, float blend_factor, float flow_scale);

    [[nodiscard]] bool IsReady() const {
        return m_initialized && m_has_history && m_flow_valid;
    }

private:
    void CreateResources(const Device& device);
    void CreateShaders(const Device& device);
    void CreatePipelines(const Device& device);
    void ProcessPingPong(const Device& device, Scheduler& scheduler, VkImage current_frame,
                         u64 frame_count);

    MemoryAllocator& m_allocator;
    VkExtent2D m_extent;
    VkExtent2D m_flow_extent;   // half-res for optical flow
    VkFormat m_format;

    vk::Sampler m_linear_sampler;
    vk::ShaderModule m_flow_shader;
    vk::ShaderModule m_interp_shader;

    vk::DescriptorSetLayout m_flow_desc_layout;
    vk::PipelineLayout m_flow_pipe_layout;
    vk::Pipeline m_flow_pipeline;

    vk::DescriptorSetLayout m_interp_desc_layout;
    vk::PipelineLayout m_interp_pipe_layout;
    vk::Pipeline m_interp_pipeline;

    vk::DescriptorPool m_desc_pool;
    vk::DescriptorSets m_flow_desc_sets;
    std::optional<DescriptorPool> m_interp_pool;
    DescriptorAllocator m_interp_allocator;

    // History and motion vector images
    vk::Image m_history_image;
    vk::ImageView m_history_view;

    vk::Image m_motion_image;
    vk::ImageView m_motion_view;

    // Store the last current-frame view for descriptor caching
    vk::Image m_current_image;
    vk::ImageView m_current_view;

    bool m_initialized{};
    bool m_has_history{};

    // Duplicate-frame detection: skip optical flow when the frame hasn't changed
    u64 m_last_frame_count{};
    bool m_flow_valid{};

    bool m_flow_descriptors_initialized{};

    // EDEN_NX2_FG_PINGPONG, read once at construction. The roles are never reset: the two flow
    // descriptor sets are tied to the parity of the swaps since the images were created.
    bool m_pingpong{};
    Fsr3PingPong m_roles{};
};

} // namespace Vulkan
