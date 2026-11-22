// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstddef>
#include <span>
#include <utility>

#include "common/logging.h"
#include "video_core/renderer_vulkan/vk_present_manager.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shown_capture.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"

namespace Vulkan {

namespace {

namespace Shown = VideoCore::Nx2ShownFrames;

/// How long the last frame's copy may take before the pictures are given up.
constexpr u64 READBACK_WAIT_NS = 500'000'000;

[[nodiscard]] bool IsSrgb(VkFormat format) {
    return format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_R8G8B8A8_SRGB ||
           format == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
}

[[nodiscard]] VkImageSubresourceLayers ColorLayer() {
    return VkImageSubresourceLayers{
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .mipLevel = 0,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
}

} // Anonymous namespace

ShownCapture::ShownCapture(const Device& device_, MemoryAllocator& memory_allocator_,
                           Scheduler& scheduler_)
    : device{device_}, memory_allocator{memory_allocator_}, scheduler{scheduler_} {}

ShownCapture::~ShownCapture() {
    {
        // The presenter stops during a capture (the game stops, or the renderer is recreated):
        // answer it, or the process-wide request stays running and armed for good.
        std::scoped_lock lock{mutex};
        if (request) {
            AddNote("the presenter stopped before all frames were shown");
            for (Shown::Shown& frame : result.frames) {
                frame.picture = -1;
            }
            Shown::FinishCapture(request->id, std::move(result));
            request.reset();
        }
    }
    if (image || buffer || !retired_images.empty() || !retired_buffers.empty()) {
        // A capture's copies may still run (one was cut short by shutdown, or timed out).
        std::scoped_lock lock{scheduler.submit_mutex};
        void(device.GetLogical().WaitIdle());
    }
}

void ShownCapture::AddNote(std::string_view note) {
    if (result.picture_note.find(note) != std::string::npos) {
        return;
    }
    if (!result.picture_note.empty()) {
        result.picture_note += "; ";
    }
    result.picture_note += note;
}

bool ShownCapture::Begin() {
    if (request) {
        return true;
    }
    request = Shown::TakeCapture();
    if (!request) {
        return false;
    }
    result = {};
    recorded = nullptr;
    any_picture = false;
    pictures = request->height != 0;
    if (!pictures) {
        AddNote("not asked for (pictures: false)");
    }
    return true;
}

bool ShownCapture::Allocate(const Frame& frame) {
    height = request->height;
    // The frame's aspect ratio at the asked height.
    const u64 frame_height = (std::max)(frame.height, 1U);
    width = static_cast<u32>(std::clamp<u64>(
        (u64{frame.width} * height + frame_height / 2) / frame_height, 1, u64{height} * 4));
    image_srgb = IsSrgb(frame.image_format);
    const VkFormat format = image_srgb ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_B8G8R8A8_UNORM;
    const auto& physical = device.GetPhysical();
    // The blit filters linearly, which the source format must also support.
    constexpr VkFormatFeatureFlags source_features =
        VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if ((physical.GetFormatProperties(frame.image_format).optimalTilingFeatures &
         source_features) != source_features ||
        (physical.GetFormatProperties(format).optimalTilingFeatures &
         VK_FORMAT_FEATURE_BLIT_DST_BIT) == 0) {
        AddNote("the device cannot blit the frame's format");
        pictures = false;
        return false;
    }
    image = memory_allocator.TryCreateImage({
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent =
            {
                .width = width,
                .height = height,
                .depth = 1,
            },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    });
    if (image) {
        try {
            buffer = memory_allocator.CreateBuffer(
                VkBufferCreateInfo{
                    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                    .pNext = nullptr,
                    .flags = 0,
                    .size = VkDeviceSize{request->count} * width * height * 4,
                    .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                    .queueFamilyIndexCount = 0,
                    .pQueueFamilyIndices = nullptr,
                },
                MemoryUsage::Download);
        } catch (const vk::Exception& exception) {
            LOG_WARNING(Render_Vulkan, "shown_frames: no read-back buffer: {}", exception.what());
        }
    }
    if (!image || !buffer || !buffer.IsHostVisible()) {
        image.reset();
        buffer.reset();
        AddNote("no memory for the pictures");
        pictures = false;
        return false;
    }
    return true;
}

void ShownCapture::Record(const vk::CommandBuffer& cmdbuf, const Frame& frame) {
    std::scoped_lock lock{mutex};
    if (!Begin() || !pictures) {
        return;
    }
    const u32 index = static_cast<u32>(result.frames.size());
    if (index >= request->count || (!image && !Allocate(frame))) {
        return;
    }
    if (IsSrgb(frame.image_format) != image_srgb) {
        AddNote("the output switched between sRGB and linear encoding during the capture");
        return;
    }
    const VkImageSubresourceRange range{
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
    // The previous frame's copy out of the image is done before the blit writes it again.
    const VkImageMemoryBarrier to_destination{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = *image,
        .subresourceRange = range,
    };
    const VkImageMemoryBarrier to_source{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = *image,
        .subresourceRange = range,
    };
    const VkImageBlit blit{
        .srcSubresource = ColorLayer(),
        .srcOffsets =
            {
                {
                    .x = 0,
                    .y = 0,
                    .z = 0,
                },
                {
                    .x = static_cast<s32>(frame.width),
                    .y = static_cast<s32>(frame.height),
                    .z = 1,
                },
            },
        .dstSubresource = ColorLayer(),
        .dstOffsets =
            {
                {
                    .x = 0,
                    .y = 0,
                    .z = 0,
                },
                {
                    .x = static_cast<s32>(width),
                    .y = static_cast<s32>(height),
                    .z = 1,
                },
            },
    };
    const VkDeviceSize picture_bytes = VkDeviceSize{width} * height * 4;
    const VkBufferImageCopy copy{
        .bufferOffset = VkDeviceSize{index} * picture_bytes,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = ColorLayer(),
        .imageOffset =
            {
                .x = 0,
                .y = 0,
                .z = 0,
            },
        .imageExtent =
            {
                .width = width,
                .height = height,
                .depth = 1,
            },
    };
    const VkBufferMemoryBarrier to_host{
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = *buffer,
        .offset = copy.bufferOffset,
        .size = picture_bytes,
    };
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                           to_destination);
    cmdbuf.BlitImage(*frame.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, *image,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, blit, VK_FILTER_LINEAR);
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                           to_source);
    cmdbuf.CopyImageToBuffer(*image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, *buffer, copy);
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                           to_host);
    recorded = &frame;
}

void ShownCapture::Note(const Frame& frame, Shown::Shown shown) {
    std::scoped_lock lock{mutex};
    if (!Begin()) {
        return;
    }
    const u32 index = static_cast<u32>(result.frames.size());
    if (recorded == &frame && pictures) {
        shown.picture = static_cast<s32>(index);
        any_picture = true;
    } else if (pictures) {
        AddNote("some frames have none: their screen copy was recorded before the capture "
                "began, or did not reach the swapchain");
    }
    recorded = nullptr;
    result.hdr |= frame.color_space != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    result.frames.push_back(shown);
    if (result.frames.size() >= request->count) {
        Finish(frame);
    }
}

void ShownCapture::Finish(const Frame& last) {
    if (any_picture) {
        // Every copy into the buffer was submitted to the graphics queue no later than the last
        // frame's screen copy (or its discard), whose fence therefore waits for them all. The
        // frame is not given back for reuse before this returns, so the fence is still its own.
        if (last.present_done.Wait(READBACK_WAIT_NS) == VK_SUCCESS) {
            buffer.Invalidate();
            const std::span<const u8> mapped = std::as_const(buffer).Mapped();
            const size_t bytes = (std::min)(
                mapped.size(), size_t{request->count} * width * height * 4);
            result.pixels.assign(mapped.begin(), mapped.begin() + static_cast<std::ptrdiff_t>(bytes));
            result.width = width;
            result.height = height;
            image.reset();
            buffer.reset();
        } else {
            AddNote("the copies were not done within 500 ms, so no picture was read");
            for (Shown::Shown& frame : result.frames) {
                frame.picture = -1;
            }
            // Still in use by the GPU: freed at destruction, after waiting for the device.
            retired_images.push_back(std::move(image));
            retired_buffers.push_back(std::move(buffer));
        }
    } else {
        image.reset();
        buffer.reset();
    }
    Shown::FinishCapture(request->id, std::move(result));
    request.reset();
    result = {};
    recorded = nullptr;
    any_picture = false;
    pictures = false;
}

} // namespace Vulkan
