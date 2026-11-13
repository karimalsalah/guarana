// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>

#include <atomic>
#include <optional>

#include "common/common_types.h"
#include "video_core/renderer_base.h"
#include "video_core/renderer_vulkan/present/frame_extrapolation.h"
#include "video_core/renderer_vulkan/present/frame_gen_pacer.h"
#include "video_core/renderer_vulkan/present/lsfg_chain.h"
#include "video_core/renderer_vulkan/present/lsfg_shaders.h"
#include "video_core/renderer_vulkan/present/fsr3_frame_gen.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"

namespace Vulkan {

class Device;
class Scheduler;
struct Frame;

class FrameGen {
public:
    explicit FrameGen(MemoryAllocator& memory_allocator, Scheduler& scheduler);
    ~FrameGen();

    /// Motion data of the frame Process is called with next and the rectangle of the frame the
    /// scene covers. Once a frame had any, generated frames are predicted after the real frame
    /// instead of interpolated before it (EDEN_NX2_EXTRAPOLATE).
    void SetExtrapolationSource(const std::optional<ExtrapolationSource>& source, VkRect2D screen);

    /// Generated frames follow the real frame: present it first.
    [[nodiscard]] bool Extrapolating() const {
        return extrapolating;
    }

    void Process(const Device& device, Frame* frame, VkFormat format, VkExtent2D guest_extent);

    /// Retains resources while invalidating pacing and history across a presentation gap.
    void Pause();

    [[nodiscard]] size_t WantedGenerations(size_t capacity);

    [[nodiscard]] size_t GeneratedFrameCount() const;

    /// Time between shown frames while frames are extrapolated, zero when they are not spaced.
    [[nodiscard]] std::chrono::nanoseconds ShownFrameSpacing() const;

    void GenerateInto(const Device& device, Frame* destination, size_t generation);

    [[nodiscard]] VideoCore::FrameGenStatus CurrentStatus() const {
        return {.state = state.load(std::memory_order_relaxed),
                .generated_frames = generated_frames.load(std::memory_order_relaxed)};
    }

private:
    void ProcessExtrapolated(const Device& device, Frame* frame);
    void UpdateCadence();
    void Rebuild(const Device& device, VkExtent2D extent, VkFormat format, f32 flow_scale);
    void DumpDebugImages(u64 count);

    MemoryAllocator& memory_allocator;
    Scheduler& scheduler;

    std::optional<LsfgShaders> shaders;
    std::optional<LsfgChain> chain;
    std::optional<Fsr3FrameGen> fsr3_chain;
    std::optional<FrameExtrapolation> extrapolation;
    std::optional<ExtrapolationSource> extrapolation_source;
    VkRect2D extrapolation_screen{};
    Frame* extrapolation_frame{}; ///< the real frame of this composite
    u64 extrapolation_cuts{};
    u64 extrapolation_missing{};
    u64 extrapolation_frames{};
    bool extrapolating{};
    /// EDEN_NX2_CADENCE: real frames are held to the target rate / cadence_divisor (0: off).
    u32 cadence_divisor{};
    u32 cadence_slow_checks{};
    u32 cadence_fast_checks{};
    std::chrono::steady_clock::time_point cadence_checked{};
    std::chrono::steady_clock::time_point real_frame_time{};
    f32 real_interval_ms{}; ///< smoothed time between real frames
    FrameGenPacer pacer;
    FrameGenPlan plan{};
    VkExtent2D peak_guest_extent{};
    VkExtent2D built_extent{};
    VkFormat built_format{VK_FORMAT_UNDEFINED};
    f32 built_flow_scale{};
    u64 frame_count{};
    u64 last_count{};
    size_t last_generations{};
    u32 warm_streak{};
    bool generated{};
    bool unavailable{};
    bool dumped{};
    std::atomic<VideoCore::FrameGenState> state{VideoCore::FrameGenState::Pending};
    std::atomic<u64> generated_frames{};
};

} // namespace Vulkan
