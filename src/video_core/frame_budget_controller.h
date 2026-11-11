// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

namespace VideoCore {

/// Holds a frame rate by steering a game's own dynamic resolution: decides the factor the GPU
/// time the game measures is multiplied by (Tegra::AmpereTimeSincePresent). A larger scale makes
/// the game's GPU look slower, so the game renders lower. Pure logic: the caller measures and
/// applies.
class FrameBudgetController {
public:
    struct Config {
        double min_scale = 0.05;
        double max_scale = 4.0;
        double slow_ratio = 1.04;     ///< A miss: frames slower than target * slow_ratio
        double fast_ratio = 0.90;     ///< Headroom: frames faster than target * fast_ratio
        double raise_step = 1.25;     ///< Scale factor of every missed tick: resolution drops
        double lower_step = 0.92;     ///< Scale factor after lower_after ticks with headroom
        u32 lower_after = 4;          ///< Consecutive ticks with headroom before a lower
        double freeze_ratio = 4.0;    ///< No decision on frames slower than target * freeze_ratio
        u32 cooldown_ticks = 30;      ///< Ticks a lower stays above the scale that last missed
        u32 max_cooldown_ticks = 240; ///< The cool-down doubles up to this while misses come back
        u32 max_blind_lowers = 8;     ///< Lowers in a row that leave the frame time alone, 0: any
        double effect_ratio = 1.08;   ///< A lower showed once frames are this much slower
    };

    struct Input {
        double frame_ms{};  ///< Frame time without the pacing wait, 0 without presents
        double target_ms{};
        double ceiling{};   ///< Scales above it change nothing (the GPU clock's divisor), 0: none
        bool hold_lower{};  ///< Something else gives quality back first (coarse shading rates)
    };

    enum class State : u8 {
        Off,       ///< No target
        Frozen,    ///< No presents, or frames too slow to mean anything (a loading screen)
        OnTarget,  ///< Inside the band
        Slow,      ///< A miss: the scale was raised
        Saturated, ///< A miss with the scale at its upper limit
        Fast,      ///< Headroom: counting towards a lower, lowered, or at the lowest scale
        Cooldown,  ///< Headroom, but the next lower scale missed a while ago
        NoEffect,  ///< Headroom, but lowers stopped showing in the frame time
        Waiting,   ///< Headroom, but hold_lower is set
    };
    enum class Action : u8 { Hold, Raise, Lower };
    struct Decision {
        Action action = Action::Hold;
        State state = State::Off;
        double scale{};  ///< The scale after the action, inside the limits
        bool at_limit{}; ///< That scale is the upper limit
    };

    FrameBudgetController() = default;
    explicit FrameBudgetController(Config config_) : config{config_} {}

    /// Feeds one tick, about two a second.
    Decision Update(const Input& input, double current_scale);

    /// Forgets the streaks and the scale that missed.
    void Reset();

    [[nodiscard]] const Config& GetConfig() const {
        return config;
    }

    /// The scale at which the last run of misses began, 0 once forgotten.
    [[nodiscard]] double MissScale() const {
        return miss_scale;
    }
    /// Ticks for which lowers still stay above MissScale.
    [[nodiscard]] u32 CooldownLeft() const {
        return miss_scale > 0.0 && since_miss < cooldown_len ? cooldown_len - since_miss : u32{0};
    }
    [[nodiscard]] u32 FastStreak() const {
        return fast_streak;
    }
    /// Lowers made since the frame time last followed one.
    [[nodiscard]] u32 BlindLowers() const {
        return lower_run;
    }

private:
    Config config;
    u32 fast_streak{};
    bool missing{};        ///< The last decided tick was a miss
    bool probed{};         ///< A lower was made since the last run of misses began
    double miss_scale{};
    u32 since_miss{};      ///< Decided ticks since the last miss, counted while miss_scale is kept
    u32 cooldown_len{};
    u32 lower_run{};
    double lower_ref_ms{}; ///< The fastest frame time of the run of lowers
};

[[nodiscard]] const char* FrameBudgetStateName(FrameBudgetController::State state);

} // namespace VideoCore
