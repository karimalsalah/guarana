// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstdlib>
#include <vector>

#include "video_core/host_shaders/frame_extrapolation_comp_spv.h"
#include "video_core/renderer_vulkan/present/frame_extrapolation.h"
#include "video_core/renderer_vulkan/present/util.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {

namespace {

constexpr u32 FLAG_HUD = 1;
constexpr u32 FLAG_DEBUG = 2;
constexpr u32 FLAG_HUD_LINEAR = 4;
constexpr u32 FLAG_HUD_EXACT = 8;
constexpr u32 SAMPLED_BINDINGS = 4;
constexpr u32 OUTPUT_BINDING = 4;
constexpr u32 GROUP_SIZE = 8;

struct ExtrapolationPushConstants {
    f32 screen_x;
    f32 screen_y;
    f32 screen_width;
    f32 screen_height;
    f32 advance;
    u32 flags;
};

[[nodiscard]] bool EnvironmentFlag(const char* name, bool fallback) {
    const char* value = std::getenv(name);
    if (!value || *value == '\0') {
        return fallback;
    }
    return *value != '0';
}

vk::Sampler CreateClampedSampler(const Device& device, VkFilter filter) {
    return device.GetLogical().CreateSampler(VkSamplerCreateInfo{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = filter,
        .minFilter = filter,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxAnisotropy = 1.0f,
    });
}

} // Anonymous namespace

FrameExtrapolation::FrameExtrapolation(const Device& device)
    : linear_sampler{CreateClampedSampler(device, VK_FILTER_LINEAR)},
      nearest_sampler{CreateClampedSampler(device, VK_FILTER_NEAREST)},
      shader{BuildShader(device, FRAME_EXTRAPOLATION_COMP_SPV)} {
    std::array<VkDescriptorSetLayoutBinding, SAMPLED_BINDINGS + 1> bindings{};
    for (u32 i = 0; i < SAMPLED_BINDINGS; ++i) {
        bindings[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                       VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    }
    bindings[OUTPUT_BINDING] = {OUTPUT_BINDING, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                                VK_SHADER_STAGE_COMPUTE_BIT, nullptr};

    descriptor_layout = device.GetLogical().CreateDescriptorSetLayout(VkDescriptorSetLayoutCreateInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    });

    const VkPushConstantRange push_range{
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = sizeof(ExtrapolationPushConstants),
    };
    pipeline_layout = device.GetLogical().CreatePipelineLayout(VkPipelineLayoutCreateInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = descriptor_layout.address(),
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_range,
    });

    pipeline = device.GetLogical().CreateComputePipeline(VkComputePipelineCreateInfo{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = *shader,
            .pName = "main",
        },
        .layout = *pipeline_layout,
    });
}

FrameExtrapolation::~FrameExtrapolation() = default;

void FrameExtrapolation::Generate(const Device& device, Scheduler& scheduler,
                                  const ExtrapolationSource& source, VkImageView color,
                                  VkImage destination, VkImageView destination_storage_view,
                                  VkExtent2D extent, VkRect2D screen, f32 advance) {
    static const bool debug = EnvironmentFlag("EDEN_NX2_EXTRAPOLATE_DEBUG", false);
    static const bool keep_hud = EnvironmentFlag("EDEN_NX2_EXTRAPOLATE_HUD", true);

    if (!descriptor_pool) {
        descriptor_pool.emplace(device, scheduler);
        descriptor_allocator = descriptor_pool->Allocator(
            device, scheduler, *descriptor_layout,
            DescriptorBankInfo{.textures = SAMPLED_BINDINGS, .images = 1});
    }

    // Unused HUD bindings still need an image; the shader never samples them without FLAG_HUD.
    const bool has_hud = keep_hud && source.hud_reference && source.hud_composite;
    const VkImageView hud_reference = has_hud ? source.hud_reference : source.guide;
    const VkImageView hud_composite = has_hud ? source.hud_composite : source.guide;

    const VkDescriptorSet set = descriptor_allocator.Commit();
    std::vector<VkDescriptorImageInfo> image_infos;
    image_infos.reserve(SAMPLED_BINDINGS);
    std::vector<VkWriteDescriptorSet> updates{
        CreateWriteDescriptorSet(image_infos, *linear_sampler, color, set, 0),
        CreateWriteDescriptorSet(image_infos, *nearest_sampler, source.guide, set, 1),
        CreateWriteDescriptorSet(image_infos, *linear_sampler, hud_reference, set, 2),
        CreateWriteDescriptorSet(image_infos, *linear_sampler, hud_composite, set, 3),
    };
    const VkDescriptorImageInfo destination_info{
        .imageView = destination_storage_view,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
    };
    updates.push_back(VkWriteDescriptorSet{
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = set,
        .dstBinding = OUTPUT_BINDING,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        .pImageInfo = &destination_info,
    });
    device.GetLogical().UpdateDescriptorSets(updates, {});

    const ExtrapolationPushConstants constants{
        .screen_x = static_cast<f32>(screen.offset.x),
        .screen_y = static_cast<f32>(screen.offset.y),
        .screen_width = static_cast<f32>(screen.extent.width),
        .screen_height = static_cast<f32>(screen.extent.height),
        .advance = advance,
        .flags = (has_hud ? FLAG_HUD : 0) | (debug ? FLAG_DEBUG : 0) |
                 (has_hud && source.hud_reference_linear ? FLAG_HUD_LINEAR : 0) |
                 (has_hud && source.hud_exact ? FLAG_HUD_EXACT : 0),
    };

    scheduler.Record([this, constants, set, destination, extent](vk::CommandBuffer cmdbuf) {
        // The frame was just drawn, the guide and the HUD images were written earlier this frame.
        const VkMemoryBarrier inputs{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        };
        const VkImageMemoryBarrier output{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = destination,
            .subresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, inputs, {}, output);
        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *pipeline);
        cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, *pipeline_layout, 0, set, {});
        cmdbuf.PushConstants(*pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, constants);
        cmdbuf.Dispatch((extent.width + GROUP_SIZE - 1) / GROUP_SIZE,
                        (extent.height + GROUP_SIZE - 1) / GROUP_SIZE, 1);
    });
}

} // namespace Vulkan
