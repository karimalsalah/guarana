// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <span>

#include "common/common_types.h"

/// EDEN_NX2_SHOWN_GOVERNOR: the shown rate follows what the GPU can carry. Every 2 s the FLUID
/// presenter hands the last window's numbers to Decide, which steps the shown rate through the
/// divisors of the display's refresh rate (144, 72, 48 Hz; 120, 60, 40 Hz). The rate is one atomic
/// that FluidCountVblanks (the slot spacing) and FrameGen::UpdateCadence (the cadence divisor, which
/// scales with it so real frames keep their rate and only the in-between frames change) both read.
/// Nothing here touches Vulkan: the decision is a pure function of its inputs.
namespace VideoCore::ShownGovernor {

/// How long a window of statistics lasts.
inline constexpr f64 WindowSeconds = 2.0;
/// The ladder has at most this many rates.
inline constexpr size_t MaxRungs = 3;
/// The lowest rate a rung may have (EDEN_NX2_SHOWN_RATE is clamped to 30 to 240).
inline constexpr u32 MinRateHz = 30;
/// The highest rate EDEN_NX2_SHOWN_RATE is taken as.
inline constexpr u32 MaxRateHz = 240;

/// What EDEN_NX2_SHOWN_RATE said. `set` is false when the variable is unset (nullptr). A set
/// variable is clamped to MinRateHz..MaxRateHz, including empty or non-numeric text (strtol
/// gives 0, which clamps to 30), the same as the presenter did on every vblank.
struct ShownRateEnv {
    bool set{};
    f64 hz{};
};

[[nodiscard]] inline ShownRateEnv ParseShownRateEnv(const char* text) {
    if (text == nullptr) {
        return {};
    }
    const long parsed = std::strtol(text, nullptr, 10);
    return {true, static_cast<f64>(std::clamp(parsed, static_cast<long>(MinRateHz),
                                              static_cast<long>(MaxRateHz)))};
}

/// The override when the variable is set, otherwise the frame-generation target, otherwise 60.
[[nodiscard]] inline f64 ResolveShownRateHz(ShownRateEnv env, u32 frame_gen_target_hz) {
    if (env.set) {
        return env.hz;
    }
    return frame_gen_target_hz != 0 ? static_cast<f64>(frame_gen_target_hz) : 60.0;
}

/// The configured shown rate. The environment is read once: the present thread used to call
/// getenv and strtol on every vblank. The frame-generation setting is still read on each call,
/// so a change in the UI applies without a restart when the environment did not override it.
[[nodiscard]] inline f64 LiveShownRateHz(u32 frame_gen_target_hz) {
    static const ShownRateEnv env = ParseShownRateEnv(std::getenv("EDEN_NX2_SHOWN_RATE"));
    return ResolveShownRateHz(env, frame_gen_target_hz);
}
/// A window with fewer slots than this says nothing (a paused game, a loading screen).
inline constexpr u64 MinSlots = 40;
/// Windows after a step before the next one may be decided; the first ones are the cadence settling
/// to its new divisor.
inline constexpr u32 DwellWindows = 3;
/// Consecutive windows under pressure before the rate steps down.
inline constexpr u32 PressuredWindows = 2;
/// Consecutive windows with room before the rate steps up (20 s).
inline constexpr u32 RoomyWindows = 10;
/// Windows during which no step up is made after a step up failed (60 s).
inline constexpr u32 BlockWindows = 30;
/// A step down within this many windows of a step up means the step up failed.
inline constexpr u32 ProbeWindows = 8;
/// Share of slots that showed a frame on time: under the first is pressure, from the second on
/// there is room to look for more.
inline constexpr f64 HitShareLow = 0.65;
inline constexpr f64 HitShareHigh = 0.90;
/// The GPU's busy share of a real frame's interval: from the first on, the GPU is what limits the
/// frames (below it a low hit share is not the GPU's doing); from the second on it is saturated.
inline constexpr f64 BusyLimited = 0.60;
inline constexpr f64 BusySaturated = 0.90;
/// Real frames per second against the cadence's target: under the first while the GPU is saturated
/// is pressure, from the second on counts as on target, under the third there are no real frames to
/// judge by (loading, a menu).
inline constexpr f64 RealShortfall = 0.92;
inline constexpr f64 RealOnTarget = 0.97;
inline constexpr f64 RealFlowing = 0.50;
/// Without a cadence target, real frames per second that count as flowing.
inline constexpr f64 UnknownTargetFlowingFps = 5.0;
/// A step up is made only if the GPU busy share, grown by StepUpCost, stays under this.
inline constexpr f64 BusyAfterStepUp = 0.95;
/// GPU time per real frame grows by this share of the shown rate's relative increase: one paired
/// measurement on the reference laptop had 33.1 ms at 72 shown and 47.6 ms at 144 (+44%).
inline constexpr f64 CostGrowth = 0.45;

/// The governed shown rate in Hz; 0 while the governor is off or has not decided yet.
inline std::atomic<u32> g_rate{0};
/// The real frames per second the cadence holds, in millihertz; 0 when no cadence holds them.
inline std::atomic<u32> g_real_target_mhz{0};

[[nodiscard]] inline bool Enabled() {
    static const bool on = [] {
        const char* value = std::getenv("EDEN_NX2_SHOWN_GOVERNOR");
        return value != nullptr && std::strtoul(value, nullptr, 10) != 0;
    }();
    return on;
}

[[nodiscard]] inline u32 Rate() {
    return g_rate.load(std::memory_order_relaxed);
}

inline void SetRate(u32 hz) {
    g_rate.store(hz, std::memory_order_relaxed);
}

[[nodiscard]] inline f64 RealTarget() {
    return static_cast<f64>(g_real_target_mhz.load(std::memory_order_relaxed)) / 1000.0;
}

inline void SetRealTarget(f64 fps) {
    g_real_target_mhz.store(fps > 0.0 ? static_cast<u32>(std::llround(fps * 1000.0)) : 0U,
                            std::memory_order_relaxed);
}

/// The rungs of the ladder, highest first: the refresh rate divided by 1, 2, 3, ... (at most
/// MaxRungs), without those above `top_hz` (the shown rate the user configured), under MinRateHz,
/// or, when the cadence holds real frames to `real_fps` (0: none), not a whole multiple of it, so
/// that each rung can keep the real frames at that rate. Returns how many rungs `ladder` holds.
[[nodiscard]] inline size_t BuildLadder(f64 refresh_hz, f64 top_hz, f64 real_fps,
                                        std::array<u32, MaxRungs>& ladder) {
    size_t count = 0;
    if (refresh_hz <= 0.0 || top_hz <= 0.0) {
        return count;
    }
    for (u32 parts = 1; parts <= 6 && count < MaxRungs; ++parts) {
        const f64 rate = std::round(refresh_hz / static_cast<f64>(parts));
        if (rate < static_cast<f64>(MinRateHz)) {
            break;
        }
        if (rate > top_hz * 1.02) {
            continue;
        }
        if (real_fps > 0.0) {
            const f64 shown_per_real = rate / real_fps;
            const f64 whole = std::round(shown_per_real);
            if (whole < 1.0 || std::abs(shown_per_real - whole) > 0.05) {
                continue;
            }
        }
        ladder[count] = static_cast<u32>(rate);
        ++count;
    }
    return count;
}

/// The cadence divisor at shown rate `rate`, when `base_divisor` holds real frames to the rate they
/// have at `base_rate`: 6 at 144 becomes 3 at 72 and 2 at 48. At least 1 and at most 12.
[[nodiscard]] inline u32 ScaledDivisor(f64 base_rate, u32 base_divisor, u32 rate) {
    if (base_rate <= 0.0 || base_divisor == 0 || rate == 0) {
        return base_divisor;
    }
    const f64 scaled =
        std::round(static_cast<f64>(base_divisor) * static_cast<f64>(rate) / base_rate);
    return static_cast<u32>(std::clamp(scaled, 1.0, 12.0));
}

/// The rate the FLUID presenter spaces its slots by, before the configured rate is used for 0: the
/// autopilot's slot rate (0: none) bounded by the governed one (0: none).
[[nodiscard]] constexpr f64 BoundSlotRate(f64 autopilot, f64 governed) {
    if (governed <= 0.0) {
        return autopilot;
    }
    return autopilot > 0.0 ? (std::min)(autopilot, governed) : governed;
}

/// What the presenter saw in one window.
struct WindowStats {
    u64 slots{};          ///< slots that came, those slept through included
    u64 hits{};           ///< of those, slots that showed a frame
    f64 real_fps{};       ///< real frames received a second
    f64 target_fps{};     ///< the real frame rate the cadence holds, 0 when none
    f64 gpu_busy{-1.0};   ///< GPU busy time of a real frame over its interval, 0 to 1, -1 unknown
};

enum class Verdict : u8 {
    NoData,   ///< too few slots, or no real frames to judge by
    Pressure, ///< the GPU limits the frames and slots are missed or real frames fall short
    Room,     ///< slots are met and real frames on target
    Fine,     ///< neither
};

enum class Reason : u8 {
    Hold,
    Start,    ///< the first decision: the top rung
    Ladder,   ///< the ladder changed and the rate was not on it
    Pressure, ///< stepped down
    Headroom, ///< stepped up
};

struct Decision {
    Reason reason{Reason::Hold};
    Verdict verdict{Verdict::NoData};
};

/// Carried from window to window.
struct State {
    u32 rate{};          ///< the shown rate in Hz, 0 before the first decision
    u32 windows_here{};  ///< windows since the last step
    u32 pressured{};     ///< consecutive windows under pressure
    u32 roomy{};         ///< consecutive windows with room for a step up
    u32 block_left{};    ///< windows left in which no step up is made
    u32 probing_left{};  ///< windows left in which a step down means the last step up failed
};

[[nodiscard]] inline Verdict Judge(const WindowStats& window) {
    if (window.slots < MinSlots) {
        return Verdict::NoData;
    }
    const bool target_known = window.target_fps > 0.0;
    const bool flowing = target_known ? window.real_fps >= RealFlowing * window.target_fps
                                      : window.real_fps >= UnknownTargetFlowingFps;
    if (!flowing) {
        return Verdict::NoData;
    }
    const f64 hit_share =
        (std::min)(1.0, static_cast<f64>(window.hits) / static_cast<f64>(window.slots));
    const bool busy_known = window.gpu_busy >= 0.0;
    const bool gpu_limited = !busy_known || window.gpu_busy >= BusyLimited;
    const bool shortfall = target_known && busy_known && window.gpu_busy >= BusySaturated &&
                           window.real_fps < RealShortfall * window.target_fps;
    if (gpu_limited && (hit_share < HitShareLow || shortfall)) {
        return Verdict::Pressure;
    }
    const bool on_target = !target_known || window.real_fps >= RealOnTarget * window.target_fps;
    if (busy_known && hit_share >= HitShareHigh && on_target) {
        return Verdict::Room;
    }
    return Verdict::Fine;
}

/// How much longer a real frame's GPU time gets when the shown rate goes from `from` to `to` Hz.
[[nodiscard]] inline f64 StepUpCost(u32 from, u32 to) {
    if (from == 0) {
        return 1.0;
    }
    return 1.0 + CostGrowth * (static_cast<f64>(to) - static_cast<f64>(from)) /
                     static_cast<f64>(from);
}

/// The next shown rate (state.rate) from the last window's numbers, on `ladder` (highest first, not
/// empty). The first call takes the top rung. A step down needs PressuredWindows windows in a row
/// under pressure, a step up RoomyWindows windows in a row with room for the GPU time the higher
/// rate costs, and neither is decided within DwellWindows windows of a step. A step down within
/// ProbeWindows of a step up marks that step up as failed: no step up for BlockWindows after it.
/// Windows without data change nothing and reset both runs.
[[nodiscard]] inline Decision Decide(State& state, std::span<const u32> ladder,
                                     const WindowStats& window) {
    Decision decision{};
    if (ladder.empty()) {
        return decision;
    }
    if (state.rate == 0) {
        state = State{};
        state.rate = ladder[0];
        decision.reason = Reason::Start;
        return decision;
    }
    size_t index = 0;
    bool found = false;
    for (size_t rung = 0; rung < ladder.size(); ++rung) {
        if (ladder[rung] == state.rate) {
            index = rung;
            found = true;
            break;
        }
    }
    if (!found) {
        // The nearest rung, the higher one on a tie.
        u32 nearest_gap = ~u32{0};
        for (size_t rung = 0; rung < ladder.size(); ++rung) {
            const u32 gap =
                ladder[rung] > state.rate ? ladder[rung] - state.rate : state.rate - ladder[rung];
            if (gap < nearest_gap) {
                nearest_gap = gap;
                index = rung;
            }
        }
        const u32 block = state.block_left;
        state = State{};
        state.rate = ladder[index];
        state.block_left = block;
        decision.reason = Reason::Ladder;
        return decision;
    }

    if (state.block_left > 0) {
        --state.block_left;
    }
    if (state.probing_left > 0) {
        --state.probing_left;
    }
    ++state.windows_here;
    decision.verdict = Judge(window);
    if (state.windows_here < DwellWindows) {
        state.pressured = 0;
        state.roomy = 0;
        return decision;
    }
    switch (decision.verdict) {
    case Verdict::NoData:
    case Verdict::Fine:
        state.pressured = 0;
        state.roomy = 0;
        break;
    case Verdict::Pressure:
        state.roomy = 0;
        state.pressured = (std::min)(state.pressured + 1, PressuredWindows);
        if (state.pressured >= PressuredWindows && index + 1 < ladder.size()) {
            if (state.probing_left > 0) {
                state.block_left = BlockWindows;
            }
            state.rate = ladder[index + 1];
            state.windows_here = 0;
            state.pressured = 0;
            state.probing_left = 0;
            decision.reason = Reason::Pressure;
        }
        break;
    case Verdict::Room:
        state.pressured = 0;
        if (index == 0 ||
            window.gpu_busy * StepUpCost(state.rate, ladder[index - 1]) > BusyAfterStepUp) {
            state.roomy = 0;
            break;
        }
        state.roomy = (std::min)(state.roomy + 1, RoomyWindows);
        if (state.roomy >= RoomyWindows && state.block_left == 0) {
            state.rate = ladder[index - 1];
            state.windows_here = 0;
            state.roomy = 0;
            state.probing_left = ProbeWindows;
            decision.reason = Reason::Headroom;
        }
        break;
    }
    return decision;
}

[[nodiscard]] inline const char* ReasonText(Reason reason) {
    switch (reason) {
    case Reason::Hold:
        break;
    case Reason::Start:
        return "start, the top of the ladder";
    case Reason::Ladder:
        return "the refresh rate changed";
    case Reason::Pressure:
        return "slots missed or real frames short";
    case Reason::Headroom:
        return "room on the GPU";
    }
    return "hold";
}

} // namespace VideoCore::ShownGovernor
