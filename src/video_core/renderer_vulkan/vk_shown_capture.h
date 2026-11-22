// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <mutex>
#include <optional>
#include <string_view>
#include <vector>

#include "common/common_types.h"
#include "video_core/nx2_shown_frames.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class MemoryAllocator;
class Scheduler;
struct Frame;

/// The presenter's side of the debug server's shown_frames (video_core/nx2_shown_frames.h).
/// Each shown frame's screen copy also blits the frame into one small image and copies that into
/// the frame's part of a host-visible buffer; after the last frame the buffer is read back once
/// that frame's copy is done. Used by whichever thread presents, so it keeps its own lock; it is
/// only entered while VideoCore::Nx2ShownFrames::Armed().
class ShownCapture {
public:
    explicit ShownCapture(const Device& device_, MemoryAllocator& memory_allocator_,
                          Scheduler& scheduler_);
    ~ShownCapture();

    ShownCapture(const ShownCapture&) = delete;
    ShownCapture& operator=(const ShownCapture&) = delete;

    /// The screen copy on the graphics queue, with the frame's image in TRANSFER_SRC_OPTIMAL:
    /// records the frame's picture into the copy's command buffer.
    void Record(const vk::CommandBuffer& cmdbuf, const Frame& frame);

    /// The frame was presented and is not given back for reuse yet: adds it to the capture, and
    /// after the last frame reads the pictures back and answers the server.
    void Note(const Frame& frame, VideoCore::Nx2ShownFrames::Shown shown);

private:
    /// Takes the queued capture if none runs. Caller holds mutex.
    bool Begin();
    /// The small image and the buffer, sized from the first frame. Caller holds mutex.
    bool Allocate(const Frame& frame);
    /// Reads the pictures back and answers. Caller holds mutex.
    void Finish(const Frame& last);
    void AddNote(std::string_view note);

    const Device& device;
    MemoryAllocator& memory_allocator;
    Scheduler& scheduler;
    std::mutex mutex;
    std::optional<VideoCore::Nx2ShownFrames::Request> request;
    VideoCore::Nx2ShownFrames::Result result;
    vk::Image image;
    vk::Buffer buffer;
    u32 width{};
    u32 height{};
    bool image_srgb{};
    /// Pictures may still be taken in this capture (false after a failure).
    bool pictures{};
    /// The frame whose picture the last Record wrote, until its Note.
    const Frame* recorded{};
    bool any_picture{};
    /// Kept until destruction: copies into them did not finish in time.
    std::vector<vk::Image> retired_images;
    std::vector<vk::Buffer> retired_buffers;
};

} // namespace Vulkan
