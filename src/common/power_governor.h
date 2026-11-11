// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>

#include "common/common_types.h"

namespace Common {

/// Decides the spin scale (PowerTuning::spin_percent) from host power, GPU load and frame time:
/// on a package power limit shared with an integrated GPU, watts the CPU spends spinning are
/// watts the GPU does not get. Pure logic: the caller measures and applies.
class SpinGovernor {
public:
    /// The scale moves along these, one step a decision.
    static constexpr std::array<u32, 4> Steps{100, 50, 25, 0};

    struct Config {
        double gpu_bound_busy = 0.90; ///< GPU-bound above this busy fraction, when over target
        double cpu_bound_busy = 0.70; ///< CPU-bound below this busy fraction
        double cpu_share = 0.50;      ///< Lower only while cpu_w / package_w is above this
        double over_ratio = 1.03;     ///< Over target above target_ms * over_ratio
        u32 lower_after = 2;          ///< Consecutive GPU-bound samples before a step down
        u32 raise_after = 2;          ///< Consecutive CPU-bound samples before a step up
        u32 ceiling = 100;            ///< Never raised above this (the scale the session began at)
    };

    struct Input {
        double package_w{};
        double cpu_w{};
        bool power_valid{};
        double gpu_busy{-1.0}; ///< GPU busy time over frame time, negative when unknown
        double frame_ms{};
        double target_ms{};
    };

    enum class State : u8 { NoData, Balanced, GpuBound, CpuBound };
    enum class Action : u8 { Hold, Lower, Raise };
    struct Decision {
        Action action = Action::Hold;
        State state = State::NoData;
        u32 spin_percent{}; ///< The scale after the action
    };

    SpinGovernor() = default;
    explicit SpinGovernor(Config config_) : config{config_} {}

    /// Feeds one sample, about one a second.
    Decision Update(const Input& input, u32 current_percent);

    void Reset();

    [[nodiscard]] const Config& GetConfig() const {
        return config;
    }

private:
    Config config;
    u32 lower_streak{};
    u32 raise_streak{};
};

[[nodiscard]] const char* SpinGovernorStateName(SpinGovernor::State state);

} // namespace Common
