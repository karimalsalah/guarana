// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <string>

#include "common/common_types.h"

/// EDEN_NX2_TARGET_FPS=<fps>: holds a Switch 2 title at that frame rate. Guest presents are held
/// to its interval (the nx2:vi2 present pacer), and twice a second a FrameBudgetController moves
/// the GPU time scale so that the game's own dynamic resolution fits the frame into the interval.
/// The shading-rate governor (AutoShadingRate) takes the same target and may coarsen only while
/// the scale is at its upper limit. Without a target only the scale exists: one atomic load a
/// present.
namespace VideoCore::FrameBudget {

using TimePoint = std::chrono::steady_clock::time_point;

/// Any thread, one relaxed load: what the GPU time a Switch 2 title measures is multiplied by.
/// GPU::Impl::RendererFrameEndNotify latches it into the Tegra::AmpereAnchor of every present.
/// Starts from EDEN_NX2_GPU_TIME_SCALE, 1 without it; with a target the controller moves it.
[[nodiscard]] f64 GpuTimeScale();
/// Any thread: sets the scale by hand (a positive number). With a target the controller goes on
/// from it; without one it stays.
void SetGpuTimeScale(f64 scale);

/// Any thread, one relaxed load: the target in frames per second, 0 when off. Starts from
/// EDEN_NX2_TARGET_FPS.
[[nodiscard]] double TargetFps();
/// Any thread: sets the target (1 to 1000). 0 turns the controller off, puts the scale back to
/// the one set by hand and lets the shading-rate governor act on its own again.
void SetTargetFps(double fps);
/// Any thread: the interval guest presents are held to, zero without a target.
[[nodiscard]] std::chrono::nanoseconds PresentInterval();

/// The guest's present thread, every present: when the present began its pacing wait and when it
/// returned to the game. The time from the last return to pace_begin is the frame without the
/// wait; every half second of presents is one tick, and the time four in five of its frames stay
/// under is the controller's input.
void OnGuestPresent(TimePoint pace_begin, TimePoint pace_end);

/// Any thread, one relaxed load: that input of the last tick in ms. 0 without a target or with
/// too few presents.
[[nodiscard]] double RecentWorkMs();

struct Status {
    double target_fps{};
    double target_ms{};
    double frame_ms{};     ///< Median present interval at the last tick, 0 when unknown
    double work_ms{};      ///< 80th percentile without the pacing wait: the controller's input
    u32 samples{};         ///< Presents of that tick
    f64 scale{};
    f64 latched_scale{};   ///< The scale as an anchor keeps it
    f64 manual_scale{};    ///< The scale without a target
    double min_scale{};
    double max_scale{};    ///< With the GPU clock's divisor: a larger scale changes nothing
    std::string state;     ///< FrameBudgetStateName of the last tick
    u64 ticks{};
    u64 raises{};
    u64 lowers{};
    double miss_scale{};   ///< The scale lowers stay above, 0 when none
    u32 cooldown_ticks{};  ///< For this many more ticks
    u32 fast_streak{};
    u32 blind_lowers{};
    bool shading_allowed{}; ///< The shading-rate governor may coarsen (the scale is at its limit)
    std::string last_change; ///< The last raise or lower
};
[[nodiscard]] Status GetStatus();

} // namespace VideoCore::FrameBudget
