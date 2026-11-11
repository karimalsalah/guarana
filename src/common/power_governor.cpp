// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>

#include "common/power_governor.h"

namespace Common {

SpinGovernor::Decision SpinGovernor::Update(const Input& input, u32 current_percent) {
    if (input.gpu_busy < 0.0 || input.frame_ms <= 0.0 || input.target_ms <= 0.0) {
        Reset();
        return {Action::Hold, State::NoData, current_percent};
    }
    const bool over_target = input.frame_ms > input.target_ms * config.over_ratio;
    if (input.gpu_busy > config.gpu_bound_busy && over_target) {
        raise_streak = 0;
        const bool cpu_heavy = input.power_valid && input.package_w > 0.0 &&
                               input.cpu_w / input.package_w > config.cpu_share;
        if (!cpu_heavy) {
            // The CPU is not what takes the package's watts: spinning less would not help.
            lower_streak = 0;
            return {Action::Hold, State::GpuBound, current_percent};
        }
        if (++lower_streak < (std::max)(config.lower_after, u32{1})) {
            return {Action::Hold, State::GpuBound, current_percent};
        }
        // Steps are in falling order: the first one below the current scale.
        const auto step = std::ranges::find_if(
            Steps, [current_percent](u32 value) { return value < current_percent; });
        if (step == Steps.end()) {
            return {Action::Hold, State::GpuBound, current_percent};
        }
        lower_streak = 0;
        return {Action::Lower, State::GpuBound, *step};
    }
    if (input.gpu_busy < config.cpu_bound_busy) {
        lower_streak = 0;
        if (++raise_streak < (std::max)(config.raise_after, u32{1})) {
            return {Action::Hold, State::CpuBound, current_percent};
        }
        if (current_percent >= config.ceiling) {
            return {Action::Hold, State::CpuBound, current_percent};
        }
        u32 next = config.ceiling;
        for (const u32 value : Steps) {
            if (value > current_percent) {
                next = (std::min)(value, config.ceiling);
            }
        }
        raise_streak = 0;
        return {Action::Raise, State::CpuBound, next};
    }
    lower_streak = 0;
    raise_streak = 0;
    return {Action::Hold, State::Balanced, current_percent};
}

void SpinGovernor::Reset() {
    lower_streak = 0;
    raise_streak = 0;
}

const char* SpinGovernorStateName(SpinGovernor::State state) {
    switch (state) {
    case SpinGovernor::State::Balanced:
        return "balanced";
    case SpinGovernor::State::GpuBound:
        return "gpu_bound";
    case SpinGovernor::State::CpuBound:
        return "cpu_bound";
    case SpinGovernor::State::NoData:
        break;
    }
    return "no_data";
}

} // namespace Common
