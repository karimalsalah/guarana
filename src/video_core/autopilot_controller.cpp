// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cmath>

#include "video_core/autopilot_controller.h"

namespace VideoCore {

namespace {
/// A scale counts as at a limit within this much of it.
constexpr double Slack = 0.999;
constexpr u32 SinceMax = 1'000'000;

double Percentile(const std::vector<double>& sorted, size_t percent) {
    return sorted[(sorted.size() - 1) * percent / 100];
}
} // Anonymous namespace

AutopilotController::AutopilotController() = default;

AutopilotController::AutopilotController(const Config& config_) : config{config_} {}

void AutopilotController::Reset() {
    *this = AutopilotController{config};
}

std::vector<u32> AutopilotController::Candidates(double refresh_hz) const {
    std::vector<u32> result;
    if (!std::isfinite(refresh_hz) || refresh_hz < 1.0) {
        return result;
    }
    const u32 refresh = static_cast<u32>(std::lround(refresh_hz));
    for (u32 k = 1; k <= refresh; ++k) {
        if (refresh % k != 0) {
            continue;
        }
        const double rate = static_cast<double>(refresh / k);
        if (rate > config.max_target_fps) {
            continue;
        }
        if (rate < config.min_target_fps) {
            break;
        }
        result.push_back(refresh / k);
    }
    return result;
}

u32 AutopilotController::PickFrom(const std::vector<u32>& candidates, double real_fps,
                                  double margin) const {
    for (const u32 candidate : candidates) {
        if (real_fps >= static_cast<double>(candidate) / config.max_ratio * margin) {
            return candidate;
        }
    }
    return 0;
}

u32 AutopilotController::BestTarget(double refresh_hz, double real_fps, double margin) const {
    return PickFrom(Candidates(refresh_hz), real_fps, margin);
}

AutopilotController::ShownSummary AutopilotController::Summarize(std::vector<double> intervals_ms) {
    ShownSummary summary;
    summary.samples = static_cast<u32>(intervals_ms.size());
    if (intervals_ms.empty()) {
        return summary;
    }
    std::ranges::sort(intervals_ms);
    summary.p5_ms = Percentile(intervals_ms, 5);
    summary.p50_ms = Percentile(intervals_ms, 50);
    summary.p95_ms = Percentile(intervals_ms, 95);
    const auto even = std::ranges::count_if(intervals_ms, [&summary](double interval) {
        return std::abs(interval - summary.p50_ms) <= 2.0;
    });
    summary.even_percent =
        100.0 * static_cast<double>(even) / static_cast<double>(intervals_ms.size());
    return summary;
}

AutopilotController::Decision AutopilotController::Update(const Input& input) {
    Decision decision;
    fg_since = (std::min)(fg_since + 1, SinceMax);
    resolution_since = (std::min)(resolution_since + 1, SinceMax);
    if (blocked_left > 0 && --blocked_left == 0) {
        blocked_target = 0;
    }
    if (hold_left > 0) {
        --hold_left;
    }
    DecidePower(input, decision);
    const bool no_data = !std::isfinite(input.real_fps) || input.real_fps < config.min_real_fps;
    if (no_data) {
        // A loading screen or a stall says nothing about the scene.
        judder_run = 0;
        headroom_run = 0;
        return decision;
    }
    DecideFrameGen(input, decision);
    DecideResolution(input, decision);
    return decision;
}

u32 AutopilotController::StepTarget(const std::vector<u32>& candidates, u32 current,
                                    double real_fps) const {
    const auto at = std::ranges::find(candidates, current);
    const auto minimum = [this](u32 rate) { return static_cast<double>(rate) / config.max_ratio; };
    if (at == candidates.end()) {
        // Not a divisor of the refresh: the best one in a single step.
        const u32 best = PickFrom(candidates, real_fps, 1.0);
        return best != 0 ? best : candidates.back();
    }
    const size_t index = static_cast<size_t>(at - candidates.begin());
    if (real_fps < minimum(current) * config.down_margin && index + 1 < candidates.size()) {
        return candidates[index + 1];
    }
    if (index > 0) {
        const u32 higher = candidates[index - 1];
        if (real_fps >= minimum(higher) * config.up_margin &&
            (blocked_target == 0 || higher < blocked_target)) {
            return higher;
        }
    }
    return current;
}

void AutopilotController::DecideFrameGen(const Input& input, Decision& decision) {
    if (!input.fg_on) {
        judder_run = 0;
        virtual_target = 0;
        return;
    }
    const std::vector<u32> candidates = Candidates(input.refresh_hz);
    if (candidates.empty()) {
        // The refresh is unknown, or no divisor of it is in range: nothing is steered.
        return;
    }
    if (input.fg_target != 0) {
        const u32 current = input.fg_target;
        const auto at = std::ranges::find(candidates, current);
        if (at != candidates.end() && input.shown_samples >= config.judder_min_samples &&
            input.shown_even_percent < config.judder_even_percent) {
            ++judder_run;
        } else {
            judder_run = 0;
        }
        if (judder_run >= config.judder_windows && at != candidates.end() &&
            at + 1 != candidates.end() && fg_since >= config.down_dwell) {
            decision.fg_target = *(at + 1);
            decision.fg_reason = "shown frames are uneven";
            blocked_target = current;
            blocked_left = config.judder_block;
            judder_run = 0;
            fg_since = 0;
            return;
        }
        const u32 next = StepTarget(candidates, current, input.real_fps);
        if (next == current) {
            return;
        }
        if (at == candidates.end()) {
            decision.fg_reason = "target is not a divisor of the refresh";
        } else if (next < current) {
            if (fg_since < config.down_dwell) {
                return;
            }
            decision.fg_reason = "real fps is too low for the target";
        } else {
            if (fg_since < config.up_dwell) {
                return;
            }
            decision.fg_reason = "real fps carries a higher target";
        }
        decision.fg_target = next;
        fg_since = 0;
        return;
    }

    // A fixed multiplier: the target is only the shown rate the multiplier aims at.
    if (virtual_target == 0 ||
        std::ranges::find(candidates, virtual_target) == candidates.end()) {
        const u32 best = PickFrom(candidates, input.real_fps, 1.0);
        virtual_target = best != 0 ? best : candidates.back();
    } else {
        const u32 next = StepTarget(candidates, virtual_target, input.real_fps);
        if (next != virtual_target &&
            fg_since >= (next < virtual_target ? config.down_dwell : config.up_dwell)) {
            virtual_target = next;
        }
    }
    const u32 limit =
        (std::max)((std::min)(config.max_multiplier, input.fg_multiplier_limit),
                   config.min_multiplier);
    const u32 current = (std::clamp)(input.fg_multiplier, config.min_multiplier, limit);
    const double ratio = static_cast<double>(virtual_target) / input.real_fps;
    if (std::abs(ratio - static_cast<double>(current)) <= config.multiplier_band) {
        return;
    }
    const u32 wanted =
        (std::clamp)(static_cast<u32>(std::lround(ratio)), config.min_multiplier, limit);
    if (wanted == current) {
        return;
    }
    const u32 next = wanted > current ? current + 1 : current - 1;
    if (fg_since < (next > current ? config.down_dwell : config.up_dwell)) {
        return;
    }
    decision.fg_multiplier = next;
    decision.fg_reason = next > current ? "real fps is too low for the multiplier"
                                        : "real fps carries a smaller multiplier";
    fg_since = 0;
}

void AutopilotController::DecideResolution(const Input& input, Decision& decision) {
    if (!input.resolution_available || input.gpu_busy < 0.0) {
        headroom_run = 0;
        in_run = false;
        no_effect_run = 0;
        return;
    }
    const double goal = input.fg_on && input.fg_target > 0
                            ? static_cast<double>(input.fg_target) / 2.0
                            : config.goal_fps;
    decision.goal_fps = goal;
    const double floor_scale = input.scale_base;
    const double cap =
        (std::max)((std::min)(input.scale_base * config.max_scale_factor, input.scale_limit),
                   floor_scale);
    const double step = (std::max)(config.scale_step, 1.01);
    const bool at_goal = input.real_fps >= goal * config.goal_slack;
    const bool gpu_bound = input.gpu_busy >= config.busy_high && !at_goal;

    if (gpu_bound) {
        headroom_run = 0;
        if (hold_left > 0 || input.scale >= cap * Slack ||
            resolution_since < config.resolution_dwell) {
            return;
        }
        if (in_run) {
            no_effect_run = input.real_fps >= run_fps * config.effect_gain ? 0 : no_effect_run + 1;
            if (no_effect_run >= config.no_effect_limit) {
                // The game does not follow: give back what the run took, in one step.
                in_run = false;
                no_effect_run = 0;
                hold_left = config.no_effect_hold;
                resolution_since = 0;
                if (input.scale > run_start_scale / Slack) {
                    decision.resolution = ResolutionAction::Raise;
                    decision.scale = (std::max)(run_start_scale, floor_scale);
                    decision.resolution_reason = "lowering the resolution did not raise real fps";
                }
                return;
            }
        } else {
            in_run = true;
            no_effect_run = 0;
            run_start_scale = input.scale;
        }
        run_fps = input.real_fps;
        decision.resolution = ResolutionAction::Lower;
        decision.scale = (std::min)(input.scale * step, cap);
        decision.resolution_reason = "GPU bound below the real fps goal";
        resolution_since = 0;
        return;
    }

    if (at_goal) {
        in_run = false;
        no_effect_run = 0;
    }
    headroom_run = at_goal && input.gpu_busy < config.busy_low ? headroom_run + 1 : 0;
    if (input.scale > floor_scale / Slack && headroom_run >= config.headroom_windows &&
        resolution_since >= config.resolution_dwell) {
        decision.resolution = ResolutionAction::Raise;
        decision.scale = (std::max)(input.scale / step, floor_scale);
        decision.resolution_reason = "GPU headroom";
        resolution_since = 0;
        headroom_run = 0;
    }
}

void AutopilotController::DecidePower(const Input& input, Decision& decision) {
    if (!input.power_available || input.power == PowerSource::Unknown) {
        power_seen = PowerSource::Unknown;
        power_run = 0;
        return;
    }
    if (input.power == power_seen) {
        power_run = (std::min)(power_run + 1, SinceMax);
    } else {
        power_seen = input.power;
        power_run = 1;
    }
    if (power_run < config.power_windows) {
        return;
    }
    if (input.power == PowerSource::Battery && !input.power_applied && !power_gave_up) {
        decision.power = PowerAction::Apply;
        decision.power_reason = "on battery";
    } else if (input.power == PowerSource::Ac && input.power_applied) {
        decision.power = PowerAction::Restore;
        decision.power_reason = "on AC";
    }
}

} // namespace VideoCore
