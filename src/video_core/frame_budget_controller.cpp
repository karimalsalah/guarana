// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>

#include "video_core/frame_budget_controller.h"

namespace VideoCore {

namespace {
/// The caller may round the scale it applies (Tegra::AmpereLatchedScale): limits compare with
/// this much slack.
constexpr double Slack = 0.999;
} // Anonymous namespace

FrameBudgetController::Decision FrameBudgetController::Update(const Input& input,
                                                              double current_scale) {
    if (input.target_ms <= 0.0) {
        Reset();
        return {Action::Hold, State::Off, current_scale, false};
    }
    const double low = (std::max)(config.min_scale, 1e-6);
    double high = (std::max)(config.max_scale, low);
    if (input.ceiling > 0.0) {
        high = (std::max)((std::min)(high, input.ceiling), low);
    }
    const double scale = (std::clamp)(current_scale, low, high);
    const bool at_limit = scale >= high * Slack;
    const double raise_step = (std::max)(config.raise_step, 1.01);
    const double lower_step = (std::clamp)(config.lower_step, 0.5, 0.99);

    if (input.frame_ms <= 0.0 || input.frame_ms > input.target_ms * config.freeze_ratio) {
        // A loading screen or a stall says nothing about the scene.
        fast_streak = 0;
        return {Action::Hold, State::Frozen, scale, at_limit};
    }

    if (input.frame_ms > input.target_ms * config.slow_ratio) {
        fast_streak = 0;
        lower_run = 0;
        if (!missing) {
            // The scale that let frames miss: lowers stay above it for a cool-down, twice as
            // long each time a lower made since the miss before brings a miss back.
            if (miss_scale <= 0.0) {
                cooldown_len = config.cooldown_ticks;
            } else if (probed) {
                cooldown_len = (std::min)(cooldown_len * 2, (std::max)(config.max_cooldown_ticks,
                                                                       config.cooldown_ticks));
            }
            miss_scale = scale;
            missing = true;
            probed = false;
        }
        since_miss = 0;
        if (at_limit) {
            return {Action::Hold, State::Saturated, scale, true};
        }
        const double next = (std::min)(scale * raise_step, high);
        return {Action::Raise, State::Slow, next, next >= high * Slack};
    }
    missing = false;
    // The scale that missed is forgotten after two cool-downs without another miss.
    if (miss_scale > 0.0 && ++since_miss >= cooldown_len * 2) {
        miss_scale = 0.0;
        since_miss = 0;
    }

    if (input.frame_ms >= input.target_ms * config.fast_ratio) {
        fast_streak = 0;
        lower_run = 0;
        return {Action::Hold, State::OnTarget, scale, at_limit};
    }

    const u32 lower_after = (std::max)(config.lower_after, u32{1});
    if (fast_streak < lower_after && ++fast_streak < lower_after) {
        return {Action::Hold, State::Fast, scale, at_limit};
    }
    if (input.hold_lower) {
        return {Action::Hold, State::Waiting, scale, at_limit};
    }
    const double next = (std::max)(scale * lower_step, low);
    if (next >= scale * Slack) {
        // Already at the lowest scale.
        return {Action::Hold, State::Fast, scale, at_limit};
    }
    if (miss_scale > 0.0 && since_miss < cooldown_len && next < miss_scale / lower_step * Slack) {
        return {Action::Hold, State::Cooldown, scale, at_limit};
    }
    // A game at its highest resolution, or one that does not follow the scale, would let the
    // scale fall for nothing and take as long to come back: lowers stop once a run of them left
    // the frame time alone.
    if (lower_run == 0 || input.frame_ms > lower_ref_ms * config.effect_ratio) {
        lower_run = 0;
        lower_ref_ms = input.frame_ms;
    } else {
        lower_ref_ms = (std::min)(lower_ref_ms, input.frame_ms);
    }
    if (config.max_blind_lowers != 0 && lower_run >= config.max_blind_lowers) {
        return {Action::Hold, State::NoEffect, scale, at_limit};
    }
    ++lower_run;
    fast_streak = 0;
    probed = true;
    return {Action::Lower, State::Fast, next, next >= high * Slack};
}

void FrameBudgetController::Reset() {
    fast_streak = 0;
    missing = false;
    probed = false;
    miss_scale = 0.0;
    since_miss = 0;
    cooldown_len = 0;
    lower_run = 0;
    lower_ref_ms = 0.0;
}

const char* FrameBudgetStateName(FrameBudgetController::State state) {
    switch (state) {
    case FrameBudgetController::State::Frozen:
        return "frozen";
    case FrameBudgetController::State::OnTarget:
        return "on_target";
    case FrameBudgetController::State::Slow:
        return "slow";
    case FrameBudgetController::State::Saturated:
        return "saturated";
    case FrameBudgetController::State::Fast:
        return "fast";
    case FrameBudgetController::State::Cooldown:
        return "cooldown";
    case FrameBudgetController::State::NoEffect:
        return "no_effect";
    case FrameBudgetController::State::Waiting:
        return "waiting";
    case FrameBudgetController::State::Off:
        break;
    }
    return "off";
}

} // namespace VideoCore
