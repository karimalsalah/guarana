// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "common/power_governor.h"

namespace {

using Common::SpinGovernor;
using Action = SpinGovernor::Action;
using State = SpinGovernor::State;

constexpr double Target = 33.3;

/// GPU-bound on a package the CPU takes most of.
SpinGovernor::Input GpuBound() {
    return {.package_w = 28.0,
            .cpu_w = 19.0,
            .power_valid = true,
            .gpu_busy = 0.97,
            .frame_ms = 40.0,
            .target_ms = Target};
}

SpinGovernor::Input CpuBound() {
    return {.package_w = 28.0,
            .cpu_w = 19.0,
            .power_valid = true,
            .gpu_busy = 0.5,
            .frame_ms = 40.0,
            .target_ms = Target};
}

/// Feeds the same input until an action comes out, at most `limit` times.
SpinGovernor::Decision Run(SpinGovernor& governor, const SpinGovernor::Input& input, u32& percent,
                           u32 limit = 8) {
    SpinGovernor::Decision decision;
    for (u32 i = 0; i < limit; ++i) {
        decision = governor.Update(input, percent);
        percent = decision.spin_percent;
        if (decision.action != Action::Hold) {
            break;
        }
    }
    return decision;
}

} // namespace

TEST_CASE("Spin governor steps the scale down while GPU-bound and CPU-heavy",
          "[common][power_governor]") {
    SpinGovernor governor;
    u32 percent = 100;
    const SpinGovernor::Decision first = governor.Update(GpuBound(), percent);
    REQUIRE(first.action == Action::Hold);
    REQUIRE(first.state == State::GpuBound);
    REQUIRE(first.spin_percent == 100);
    const SpinGovernor::Decision second = governor.Update(GpuBound(), percent);
    REQUIRE(second.action == Action::Lower);
    REQUIRE(second.spin_percent == 50);
    percent = 50;
    REQUIRE(Run(governor, GpuBound(), percent).spin_percent == 25);
    REQUIRE(Run(governor, GpuBound(), percent).spin_percent == 0);
    const SpinGovernor::Decision floor = Run(governor, GpuBound(), percent);
    REQUIRE(floor.action == Action::Hold);
    REQUIRE(floor.state == State::GpuBound);
    REQUIRE(percent == 0);
}

TEST_CASE("Spin governor leaves the scale alone unless the CPU takes the package",
          "[common][power_governor]") {
    SpinGovernor governor;
    u32 percent = 100;
    SpinGovernor::Input light = GpuBound();
    light.cpu_w = 9.0;
    REQUIRE(Run(governor, light, percent).action == Action::Hold);
    SpinGovernor::Input unmeasured = GpuBound();
    unmeasured.power_valid = false;
    REQUIRE(Run(governor, unmeasured, percent).action == Action::Hold);
    // Busy but on target: not GPU-bound.
    SpinGovernor::Input on_target = GpuBound();
    on_target.frame_ms = Target;
    const SpinGovernor::Decision decision = Run(governor, on_target, percent);
    REQUIRE(decision.action == Action::Hold);
    REQUIRE(decision.state == State::Balanced);
    REQUIRE(percent == 100);
}

TEST_CASE("Spin governor raises the scale back while CPU-bound, up to its ceiling",
          "[common][power_governor]") {
    SpinGovernor governor;
    u32 percent = 0;
    const SpinGovernor::Decision first = Run(governor, CpuBound(), percent);
    REQUIRE(first.action == Action::Raise);
    REQUIRE(first.state == State::CpuBound);
    REQUIRE(percent == 25);
    REQUIRE(Run(governor, CpuBound(), percent).spin_percent == 50);
    REQUIRE(Run(governor, CpuBound(), percent).spin_percent == 100);
    REQUIRE(Run(governor, CpuBound(), percent).action == Action::Hold);
    REQUIRE(percent == 100);

    SpinGovernor::Config config;
    config.ceiling = 50;
    SpinGovernor capped{config};
    percent = 25;
    REQUIRE(Run(capped, CpuBound(), percent).spin_percent == 50);
    REQUIRE(Run(capped, CpuBound(), percent).action == Action::Hold);
    REQUIRE(percent == 50);
}

TEST_CASE("Spin governor needs a streak and data", "[common][power_governor]") {
    SpinGovernor governor;
    const u32 percent = 100;
    REQUIRE(governor.Update(GpuBound(), percent).action == Action::Hold);
    // A sample without GPU load starts the streak over.
    SpinGovernor::Input blind = GpuBound();
    blind.gpu_busy = -1.0;
    const SpinGovernor::Decision none = governor.Update(blind, percent);
    REQUIRE(none.state == State::NoData);
    REQUIRE(none.spin_percent == 100);
    REQUIRE(governor.Update(GpuBound(), percent).action == Action::Hold);
    // So does one between the two bounds.
    SpinGovernor::Input between = GpuBound();
    between.gpu_busy = 0.8;
    REQUIRE(governor.Update(between, percent).state == State::Balanced);
    REQUIRE(governor.Update(GpuBound(), percent).action == Action::Hold);
    REQUIRE(governor.Update(GpuBound(), percent).action == Action::Lower);
}

TEST_CASE("Spin governor moves a scale between steps to the next step",
          "[common][power_governor]") {
    SpinGovernor governor;
    u32 percent = 60;
    REQUIRE(Run(governor, GpuBound(), percent).spin_percent == 50);
    percent = 60;
    REQUIRE(Run(governor, CpuBound(), percent).spin_percent == 100);
}
