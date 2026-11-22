// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "video_core/host_shaders/fluid_compose_comp_spv.h"
#include "video_core/renderer_vulkan/present/fluid_compose.h"
#include "video_core/renderer_vulkan/present/util.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {

FluidCompose::FluidCompose(const Device& device, std::size_t frame_count) {
    filter_mode = FluidComposeMath::ModeOf(std::getenv("EDEN_NX2_FLUID_COMPOSE_NATIVE")) != 0
                      ? 0U
                      : FluidComposeMath::FilterModeOf(std::getenv("EDEN_NX2_FLUID_COMPOSE_FILTER"));
    const auto& dld = device.GetLogical();
    shader = BuildShader(device, FLUID_COMPOSE_COMP_SPV);

    sampler = dld.CreateSampler(VkSamplerCreateInfo{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .mipLodBias = 0.0f,
        .anisotropyEnable = VK_FALSE,
        .maxAnisotropy = 0.0f,
        .compareEnable = VK_FALSE,
        .compareOp = VK_COMPARE_OP_NEVER,
        .minLod = 0.0f,
        .maxLod = 0.0f,
        .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
        .unnormalizedCoordinates = VK_FALSE,
    });

    // Set 0: the frame, through the immutable sampler.
    const VkSampler immutable_sampler = *sampler;
    const VkDescriptorSetLayoutBinding frame_binding{
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .pImmutableSamplers = &immutable_sampler,
    };
    frame_layout = dld.CreateDescriptorSetLayout(VkDescriptorSetLayoutCreateInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .bindingCount = 1,
        .pBindings = &frame_binding,
    });
    // Set 1: the swapchain image, as a storage image.
    const VkDescriptorSetLayoutBinding swapchain_binding{
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .pImmutableSamplers = nullptr,
    };
    swapchain_layout = dld.CreateDescriptorSetLayout(VkDescriptorSetLayoutCreateInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .bindingCount = 1,
        .pBindings = &swapchain_binding,
    });

    const std::array<VkDescriptorSetLayout, 2> set_layouts{*frame_layout, *swapchain_layout};
    const VkPushConstantRange push_range{
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = sizeof(FluidComposeMath::PushConstants),
    };
    pipeline_layout = dld.CreatePipelineLayout(VkPipelineLayoutCreateInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .setLayoutCount = static_cast<u32>(set_layouts.size()),
        .pSetLayouts = set_layouts.data(),
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_range,
    });
    pipeline = dld.CreateComputePipeline(VkComputePipelineCreateInfo{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .stage =
            {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                .module = *shader,
                .pName = "main",
                .pSpecializationInfo = nullptr,
            },
        .layout = *pipeline_layout,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex = 0,
    });

    // One set per frame, written by SetFrameView.
    const VkDescriptorPoolSize frame_pool_size{
        .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = static_cast<u32>(frame_count),
    };
    frame_pool = dld.CreateDescriptorPool(VkDescriptorPoolCreateInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .maxSets = static_cast<u32>(frame_count),
        .poolSizeCount = 1,
        .pPoolSizes = &frame_pool_size,
    });
    const std::vector<VkDescriptorSetLayout> frame_set_layouts(frame_count, *frame_layout);
    frame_sets = CreateWrappedDescriptorSets(frame_pool, frame_set_layouts);
    if (frame_sets.IsOutOfPoolMemory()) {
        throw vk::Exception(VK_ERROR_OUT_OF_POOL_MEMORY);
    }
}

FluidCompose::~FluidCompose() = default;

void FluidCompose::SetFrameView(const Device& device, u32 frame_index, VkImageView view) {
    if (frame_index >= frame_sets.size()) {
        return;
    }
    // The sampler is the layout's immutable one; the image is read in GENERAL layout, where the
    // frames stay.
    const VkDescriptorImageInfo image_info{
        .sampler = VK_NULL_HANDLE,
        .imageView = view,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
    };
    const VkWriteDescriptorSet write{
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .pNext = nullptr,
        .dstSet = frame_sets[frame_index],
        .dstBinding = 0,
        .dstArrayElement = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &image_info,
        .pBufferInfo = nullptr,
        .pTexelBufferView = nullptr,
    };
    device.GetLogical().UpdateDescriptorSets(write, {});
}

void FluidCompose::SetSwapchainViews(const Device& device, std::span<const VkImageView> views) {
    // The old sets go with their pool.
    swapchain_sets = vk::DescriptorSets{};
    swapchain_pool = vk::DescriptorPool{};
    if (views.empty()) {
        return;
    }
    const auto& dld = device.GetLogical();
    const VkDescriptorPoolSize pool_size{
        .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        .descriptorCount = static_cast<u32>(views.size()),
    };
    swapchain_pool = dld.CreateDescriptorPool(VkDescriptorPoolCreateInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .maxSets = static_cast<u32>(views.size()),
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size,
    });
    const std::vector<VkDescriptorSetLayout> set_layouts(views.size(), *swapchain_layout);
    swapchain_sets = CreateWrappedDescriptorSets(swapchain_pool, set_layouts);
    if (swapchain_sets.IsOutOfPoolMemory()) {
        swapchain_sets = vk::DescriptorSets{};
        throw vk::Exception(VK_ERROR_OUT_OF_POOL_MEMORY);
    }
    std::vector<VkDescriptorImageInfo> image_infos(views.size());
    std::vector<VkWriteDescriptorSet> writes(views.size());
    for (std::size_t index = 0; index < views.size(); ++index) {
        image_infos[index] = VkDescriptorImageInfo{
            .sampler = VK_NULL_HANDLE,
            .imageView = views[index],
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
        writes[index] = VkWriteDescriptorSet{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext = nullptr,
            .dstSet = swapchain_sets[index],
            .dstBinding = 0,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .pImageInfo = &image_infos[index],
            .pBufferInfo = nullptr,
            .pTexelBufferView = nullptr,
        };
    }
    dld.UpdateDescriptorSets(writes, {});
}

void FluidCompose::Record(const vk::CommandBuffer& cmdbuf, u32 frame_index, u32 image_index,
                          const FluidComposeMath::PushConstants& job) const {
    const std::array<VkDescriptorSet, 2> sets{frame_sets[frame_index],
                                              swapchain_sets[image_index]};
    cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *pipeline);
    cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, *pipeline_layout, 0, sets, {});
    auto constants = job;
    constants.filter_mode = static_cast<s32>(filter_mode);
    cmdbuf.PushConstants(*pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, constants);
    cmdbuf.Dispatch(FluidComposeMath::GroupCount(static_cast<u32>(job.out_width)),
                    FluidComposeMath::GroupCount(static_cast<u32>(job.out_height)), 1);
}

} // namespace Vulkan
