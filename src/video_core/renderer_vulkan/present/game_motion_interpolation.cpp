// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "video_core/host_shaders/game_motion_interpolation_comp_spv.h"
#include "video_core/renderer_vulkan/present/game_motion_interpolation.h"
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
constexpr u32 FLAG_HUD_DILATE = 32;
constexpr u32 FLAG_HUD_FILL = 64;
constexpr u32 SAMPLED_BINDINGS = 6;
constexpr u32 OUTPUT_BINDING = 6;
constexpr u32 GROUP_SIZE = 8;
/// The guides' format (vk_ocarina_camera_motion.cpp, GUIDE_FORMAT).
constexpr VkFormat GUIDE_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;

struct GameMotionPushConstants {
    f32 screen_x;
    f32 screen_y;
    f32 screen_width;
    f32 screen_height;
    f32 t;
    u32 flags;
};

[[nodiscard]] bool GameMotionFlag(const char* name, bool fallback) {
    const char* value = std::getenv(name);
    if (!value || *value == '\0') {
        return fallback;
    }
    return *value != '0';
}

vk::Sampler CreateGameMotionSampler(const Device& device, VkFilter filter) {
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

VkImageMemoryBarrier GuideBarrier(VkImage image, VkAccessFlags src_access, VkAccessFlags dst_access,
                                  VkImageLayout old_layout, VkImageLayout new_layout) {
    return VkImageMemoryBarrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
}

} // Anonymous namespace

GameMotionInterpolation::GameMotionInterpolation(const Device& device,
                                                 MemoryAllocator& memory_allocator_)
    : memory_allocator{memory_allocator_},
      linear_sampler{CreateGameMotionSampler(device, VK_FILTER_LINEAR)},
      nearest_sampler{CreateGameMotionSampler(device, VK_FILTER_NEAREST)},
      shader{BuildShader(device, GAME_MOTION_INTERPOLATION_COMP_SPV)} {
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
        .size = sizeof(GameMotionPushConstants),
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

GameMotionInterpolation::~GameMotionInterpolation() = default;

void GameMotionInterpolation::Push(const Device& device, Scheduler& scheduler,
                                   const std::optional<ExtrapolationSource>& source) {
    newest ^= 1U;
    Guide& slot = guides[newest];
    newest_source = source;
    newest_cut = source && source->cut;
    if (!source || source->guide_image == VK_NULL_HANDLE || source->guide_extent.width == 0 ||
        source->guide_extent.height == 0) {
        slot.valid = false;
        return;
    }
    const VkExtent2D extent = source->guide_extent;
    bool fresh = false;
    if (!slot.image || slot.extent.width != extent.width || slot.extent.height != extent.height) {
        if (slot.image) {
            scheduler.Finish(); // an in-between frame may still read the old image
        }
        slot.image = CreateWrappedImage(memory_allocator, extent, GUIDE_FORMAT);
        slot.view = CreateWrappedImageView(device, slot.image, GUIDE_FORMAT);
        slot.extent = extent;
        fresh = true;
    }
    slot.valid = true;
    // The provider rewrites its guide for the next real frame: keep this one.
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([source_image = source->guide_image, destination = *slot.image, extent,
                      fresh](vk::CommandBuffer cmdbuf) {
        const VkImageCopy region{
            .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .srcOffset = {0, 0, 0},
            .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .dstOffset = {0, 0, 0},
            .extent = {extent.width, extent.height, 1},
        };
        const std::array before{
            GuideBarrier(source_image,
                         VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                         VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
            GuideBarrier(destination, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                         fresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT, 0, {}, {}, before);
        cmdbuf.CopyImage(source_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, region);
        const std::array after{
            GuideBarrier(source_image, VK_ACCESS_TRANSFER_READ_BIT,
                         VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                             VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
            GuideBarrier(destination, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                               0, {}, {}, after);
    });
}

bool GameMotionInterpolation::Ready() const {
    const Guide& latest = guides[newest];
    const Guide& before = guides[newest ^ 1U];
    return latest.valid && before.valid && !newest_cut &&
           latest.extent.width == before.extent.width &&
           latest.extent.height == before.extent.height;
}

void GameMotionInterpolation::Generate(const Device& device, Scheduler& scheduler,
                                       VkImageView newest_color, VkImageView previous_color,
                                       VkImage destination, VkImageView destination_storage_view,
                                       VkExtent2D extent, VkRect2D screen, f32 t) {
    // The extrapolation's debug and HUD switches apply here too.
    static const bool debug = [] {
        const char* value = std::getenv("EDEN_NX2_EXTRAPOLATE_DEBUG");
        return value != nullptr && std::strtoul(value, nullptr, 10) != 0;
    }();
    static const bool dilate_hud = GameMotionFlag("EDEN_NX2_EXTRAPOLATE_HUD_DILATE", true);
    static const bool fill_hud = GameMotionFlag("EDEN_NX2_EXTRAPOLATE_HUD_FILL", false);
    static const bool keep_hud = GameMotionFlag("EDEN_NX2_EXTRAPOLATE_HUD", true);

    if (!descriptor_pool) {
        descriptor_pool.emplace(device, scheduler);
        descriptor_allocator = descriptor_pool->Allocator(
            device, scheduler, *descriptor_layout,
            DescriptorBankInfo{.textures = SAMPLED_BINDINGS, .images = 1});
    }

    const VkImageView latest_guide = *guides[newest].view;
    const VkImageView previous_guide = *guides[newest ^ 1U].view;
    const bool has_hud = keep_hud && newest_source && newest_source->hud_reference &&
                         newest_source->hud_composite;
    // Unused HUD bindings still need an image; the shader never samples them without FLAG_HUD.
    const VkImageView hud_reference = has_hud ? newest_source->hud_reference : latest_guide;
    const VkImageView hud_composite = has_hud ? newest_source->hud_composite : latest_guide;

    const VkDescriptorSet set = descriptor_allocator.Commit();
    std::vector<VkDescriptorImageInfo> image_infos;
    image_infos.reserve(SAMPLED_BINDINGS);
    std::vector<VkWriteDescriptorSet> updates{
        CreateWriteDescriptorSet(image_infos, *linear_sampler, newest_color, set, 0),
        CreateWriteDescriptorSet(image_infos, *linear_sampler, previous_color, set, 1),
        CreateWriteDescriptorSet(image_infos, *nearest_sampler, latest_guide, set, 2),
        CreateWriteDescriptorSet(image_infos, *nearest_sampler, previous_guide, set, 3),
        CreateWriteDescriptorSet(image_infos, *linear_sampler, hud_reference, set, 4),
        CreateWriteDescriptorSet(image_infos, *linear_sampler, hud_composite, set, 5),
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

    const GameMotionPushConstants constants{
        .screen_x = static_cast<f32>(screen.offset.x),
        .screen_y = static_cast<f32>(screen.offset.y),
        .screen_width = static_cast<f32>(screen.extent.width),
        .screen_height = static_cast<f32>(screen.extent.height),
        .t = t,
        .flags = (has_hud ? FLAG_HUD : 0) | (debug ? FLAG_DEBUG : 0) |
                 (has_hud && dilate_hud ? FLAG_HUD_DILATE : 0) |
                 (has_hud && fill_hud ? FLAG_HUD_FILL : 0) |
                 (has_hud && newest_source->hud_reference_linear ? FLAG_HUD_LINEAR : 0) |
                 (has_hud && newest_source->hud_exact ? FLAG_HUD_EXACT : 0),
    };

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([this, constants, set, destination, extent](vk::CommandBuffer cmdbuf) {
        // The frames were copied and the guides written earlier this frame.
        const VkMemoryBarrier inputs{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        };
        const VkImageMemoryBarrier output = GuideBarrier(
            destination, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_GENERAL);
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
