// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <optional>

#include "common/common_types.h"
#include "common/settings_enums.h"
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

    /// blocked: the time the renderer waited for the present thread since the last call.
    [[nodiscard]] size_t WantedGenerations(size_t capacity,
                                           std::chrono::steady_clock::duration blocked = {});

    [[nodiscard]] size_t GeneratedFrameCount() const;

    /// The measured time between real frames in seconds, or 0 while it is unknown.
    [[nodiscard]] f32 FrameInterval() const {
        return pacer.SmoothedInterval();
    }

    /// EDEN_NX2_CADENCE: the time each shown frame keeps the screen while real frames are held to
    /// an even share of the target rate, zero while they are not.
    [[nodiscard]] std::chrono::nanoseconds CadenceSpacing() const;

    /// EDEN_NX2_FLUID: the time between real frames to predict against, in seconds: the interval
    /// the guest is paced to while the measured one is within 10% of it, else the measured one;
    /// 0 while unknown.
    [[nodiscard]] f32 FluidInterval() const;

    /// Whether the frame Process was given can be extrapolated (EDEN_NX2_FLUID).
    [[nodiscard]] bool CanPredict() const;

    /// EDEN_NX2_FLUID: records the frame `advance` frame intervals after the one Process was
    /// given into destination. False when nothing was recorded.
    bool GenerateAt(const Device& device, Frame* destination, f32 advance);

    /// EDEN_NX2_FLUID: counts predicted frames for the status bar.
    void NoteFluidGenerated(size_t count);

    /// EDEN_NX2_CADENCE holds real frames to a share of the target rate.
    [[nodiscard]] bool CadenceActive() const {
        return cadence_divisor != 0;
    }

    /// Generates nothing until Process runs again, and gives back a cadence's frame rate cap
    /// (HDR output: extrapolation writes SDR only).
    void Pause();

    void GenerateInto(const Device& device, Frame* destination, size_t generation);

    [[nodiscard]] VideoCore::FrameGenStatus CurrentStatus() const {
        return {.state = state.load(std::memory_order_relaxed),
                .generated_frames = generated_frames.load(std::memory_order_relaxed)};
    }

private:
    void ProcessExtrapolated(const Device& device, Frame* frame, VkFormat format);
    void UpdateCadence();
    /// Gives the frame budget back the target it had before the cadence held real frames.
    void ReleaseCadence();
    void Rebuild(const Device& device, VkExtent2D extent, VkFormat format, f32 flow_scale,
                 Settings::FrameGenType type);
    /// Updates the status bar state and logs each change with its reason.
    void SetState(VideoCore::FrameGenState new_state, VkFormat format);
    /// Logs every 10 s how many frames were generated, when that changes.
    void LogActivity();
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
    /// This frame has motion to extrapolate (no camera cut).
    bool extrapolation_usable{};
    /// EDEN_NX2_CADENCE: real frames are held to the target rate / cadence_divisor (0: off).
    u32 cadence_divisor{};
    u32 cadence_slow_checks{};
    u32 cadence_fast_checks{};
    std::chrono::steady_clock::time_point cadence_checked{};
    double cadence_saved_target{}; ///< the frame budget's target before the cadence set one
    u32 unusable_streak{}; ///< extrapolated frames in a row without motion data
    FrameGenPacer pacer;
    FrameGenPlan plan{};
    VkExtent2D peak_guest_extent{};
    VkExtent2D built_extent{};
    VkFormat built_format{VK_FORMAT_UNDEFINED};
    Settings::FrameGenType built_type{};
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
    struct {
        std::chrono::steady_clock::time_point since{};
        u64 frames{};
        u64 generated{};
        f32 logged_per_frame{-1.0f};
    } activity;
};

} // namespace Vulkan
