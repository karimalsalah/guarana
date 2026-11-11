// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <memory>
#include <optional>
#include <vector>

#include "common/common_types.h"
#include "video_core/ocarina_upscale.h"
#include "video_core/renderer_vulkan/present/extrapolation_source.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class DescriptorPool;
class Device;
class MemoryAllocator;
class Scheduler;
class StagingBufferPool;

/// The temporal upscaler Ocarina uses this session: the settings, EDEN_NX2_DLSS_REPLACEMENT,
/// DLSS availability, and native when FSR 3.1 is unsupported on the device.
[[nodiscard]] VideoCommon::OcarinaUpscaleBackend ResolveOcarinaUpscaler(const Device& device);

struct OcarinaActorMesh {
    std::vector<u8> vertices;
    std::vector<u16> indices;
    std::array<f32, 4> current_origin;
    std::array<f32, 4> previous_origin;
    std::array<f32, 280 * 12> current_bones;
    std::array<f32, 280 * 12> previous_bones;
};

class OcarinaUpscaler {
public:
    OcarinaUpscaler(const Device& device, Scheduler& scheduler, DescriptorPool& pool,
                   ComputePassDescriptorQueue& queue, MemoryAllocator& allocator,
                   StagingBufferPool& staging, VideoCommon::OcarinaUpscaleBackend backend);
    ~OcarinaUpscaler();

    [[nodiscard]] bool EvaluationFailed() const;
    /// Save the 1440p temporal scene copy before the game adds its HUD.
    void SaveSceneReference(VkImageView color);
    /// Composite the HUD from the guest image over the matching 4K scene.
    void Composite4k(VkImageView native_composite, u64 frame);
    /// Copy the scene colour once the fog is drawn, before the glows and particles that have no
    /// motion vectors of their own.
    void SaveEffectsBase(VkImageView scene, VkExtent2D extent, u64 frame);
    /// Mark the pixels those glows and particles changed since SaveEffectsBase, so Record can
    /// tell the temporal upscaler to use this frame's colour there.
    void MarkEffects(VkImageView scene, u64 frame);
    VkImage CompositedImage() const;
    VkImageView CompositedView() const;
    /// The scene SaveSceneReference kept, null before the first one.
    VkImageView SceneReferenceView() const;
    /// Motion and depth of `frame` for frame extrapolation, when Record made them.
    std::optional<ExtrapolationSource> Extrapolation(u64 frame) const;

    /// Queue motion, temporal evaluation and guest writeback for one frame. A false return leaves
    /// the guest dispatch in charge. The scheduler checks EvaluationFailed before skipping it.
    /// `extent` is what the game drew, at the top left of `color` and `depth`. With a
    /// `largest_extent` (EDEN_NX2_OOT_FSR_ANY) the extent may change from frame to frame: FSR 3.1
    /// keeps images of the largest one and its history across the change.
    bool Record(VkImageView color, VkImageView depth, VkImageView target, VkExtent2D extent,
                VkExtent2D output_extent, VkExtent2D target_extent, const std::array<f32, 16>& current,
                const std::array<f32, 16>& previous, const OcarinaActorMesh& mesh,
                VkImage depth_image, u64 frame, std::array<f32, 2> jitter = {},
                VkExtent2D largest_extent = {});

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

/// Motion and depth for frame extrapolation on frames no temporal upscaler replaces (the
/// anti-aliasing network bypass): the half-size guide alone, straight from the scene depth.
class OcarinaMotionGuide {
public:
    OcarinaMotionGuide(const Device& device, Scheduler& scheduler, DescriptorPool& pool,
                       ComputePassDescriptorQueue& queue, MemoryAllocator& allocator);
    ~OcarinaMotionGuide();

    /// Queue the guide of one frame. False when the cameras give no motion.
    bool Record(VkImageView depth, VkExtent2D extent, const std::array<f32, 16>& current,
                const std::array<f32, 16>& previous, const OcarinaActorMesh& mesh, u64 frame,
                std::array<f32, 2> jitter = {});
    /// Motion and depth of `frame`, when Record made them.
    std::optional<ExtrapolationSource> Extrapolation(u64 frame) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Vulkan
