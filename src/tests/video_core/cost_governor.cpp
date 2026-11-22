// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "video_core/cost_governor.h"

namespace {

using VideoCore::CostGovernor;
using VideoCore::GpuCostSnapshot;
using Action = CostGovernor::Action;

constexpr double Target = 16.6;
constexpr double Slow = 20.0;
constexpr double Fast = 12.0;
constexpr double Steady = 16.0;

GpuCostSnapshot::Entry Work(u64 vertex, u64 fragment, double total_ms) {
    GpuCostSnapshot::Entry entry;
    entry.vertex_hash = vertex;
    entry.fragment_hash = fragment;
    entry.draws = 1;
    entry.total_ms = total_ms;
    entry.max_ms = total_ms;
    return entry;
}

GpuCostSnapshot Snapshot() {
    GpuCostSnapshot snapshot;
    snapshot.frame = 1;
    snapshot.work.push_back(Work(0x10, 0, 9.0)); // A dispatch
    snapshot.work.push_back(Work(0x11, 0xA, 2.0));
    snapshot.work.push_back(Work(0x12, 0xB, 1.5));
    snapshot.work.push_back(Work(0x13, 0xB, 1.0));
    snapshot.work.push_back(Work(0x14, 0xC, 0.4));
    return snapshot;
}

u32 Level(const CostGovernor::RateTable& rates, u64 hash) {
    const auto it = rates.find(hash);
    return it == rates.end() ? 0 : u32{it->second};
}

} // namespace

TEST_CASE("Cost governor raises the costliest fragment shader after a streak",
          "[video_core][cost_governor]") {
    CostGovernor governor;
    CostGovernor::RateTable rates;
    const GpuCostSnapshot snapshot = Snapshot();
    REQUIRE(governor.Update(snapshot, Slow, Target, rates).action == Action::Hold);
    REQUIRE(rates.empty());
    // 0xB costs 2.5 ms over its two vertex shaders, more than 0xA.
    auto decision = governor.Update(snapshot, Slow, Target, rates);
    REQUIRE(decision.action == Action::Raise);
    REQUIRE(decision.fragment_hash == 0xB);
    REQUIRE(u32{decision.level} == 1);
    REQUIRE(governor.Update(snapshot, Slow, Target, rates).action == Action::Hold);
    decision = governor.Update(snapshot, Slow, Target, rates);
    REQUIRE(decision.action == Action::Raise);
    REQUIRE(decision.fragment_hash == 0xB);
    REQUIRE(u32{decision.level} == 2);
    // 0xB is at the default maximum: 0xA is next, the dispatch and the 0.4 ms shader never.
    for (u32 i = 0; i < 4; ++i) {
        governor.Update(snapshot, Slow, Target, rates);
    }
    REQUIRE(rates.size() == 2);
    REQUIRE(Level(rates, 0xA) == 2);
    REQUIRE(Level(rates, 0xB) == 2);
    for (u32 i = 0; i < 8; ++i) {
        REQUIRE(governor.Update(snapshot, Slow, Target, rates).action == Action::Hold);
    }
    REQUIRE(rates.size() == 2);
    REQUIRE(Level(rates, 0xA) == 2);
    REQUIRE(Level(rates, 0xB) == 2);
}

TEST_CASE("Cost governor holds inside the band and on broken streaks",
          "[video_core][cost_governor]") {
    CostGovernor governor;
    CostGovernor::RateTable rates;
    const GpuCostSnapshot snapshot = Snapshot();
    for (u32 i = 0; i < 16; ++i) {
        REQUIRE(governor.Update(snapshot, Steady, Target, rates).action == Action::Hold);
    }
    // A frame inside the band between two slow ones restarts the streak.
    for (u32 i = 0; i < 8; ++i) {
        REQUIRE(governor.Update(snapshot, Slow, Target, rates).action == Action::Hold);
        REQUIRE(governor.Update(snapshot, Steady, Target, rates).action == Action::Hold);
    }
    REQUIRE(rates.empty());
    // No target, no decision.
    REQUIRE(governor.Update(snapshot, Slow, 0.0, rates).action == Action::Hold);
    REQUIRE(governor.Update(snapshot, Slow, 0.0, rates).action == Action::Hold);
    REQUIRE(rates.empty());
}

TEST_CASE("Cost governor lowers the most recent raise first", "[video_core][cost_governor]") {
    CostGovernor::Config config;
    config.raise_after = 1;
    config.lower_after = 2;
    config.max_level = 1;
    CostGovernor governor{config};
    CostGovernor::RateTable rates;
    const GpuCostSnapshot snapshot = Snapshot();
    REQUIRE(governor.Update(snapshot, Slow, Target, rates).fragment_hash == 0xB);
    REQUIRE(governor.Update(snapshot, Slow, Target, rates).fragment_hash == 0xA);
    REQUIRE(rates.size() == 2);

    REQUIRE(governor.Update(snapshot, Fast, Target, rates).action == Action::Hold);
    auto decision = governor.Update(snapshot, Fast, Target, rates);
    REQUIRE(decision.action == Action::Lower);
    REQUIRE(decision.fragment_hash == 0xA);
    REQUIRE(u32{decision.level} == 0);
    REQUIRE(rates.size() == 1);
    REQUIRE(Level(rates, 0xB) == 1);
    // The streak starts over after each step.
    REQUIRE(governor.Update(snapshot, Fast, Target, rates).action == Action::Hold);
    decision = governor.Update(snapshot, Fast, Target, rates);
    REQUIRE(decision.action == Action::Lower);
    REQUIRE(decision.fragment_hash == 0xB);
    REQUIRE(rates.empty());
    for (u32 i = 0; i < 4; ++i) {
        REQUIRE(governor.Update(snapshot, Fast, Target, rates).action == Action::Hold);
    }
}

TEST_CASE("Cost governor honours the deny list and the level cap",
          "[video_core][cost_governor]") {
    CostGovernor::Config config;
    config.raise_after = 1;
    config.max_level = 3;
    config.deny.insert(0xB);
    CostGovernor governor{config};
    CostGovernor::RateTable rates;
    const GpuCostSnapshot snapshot = Snapshot();
    for (u32 level = 1; level <= 3; ++level) {
        const auto decision = governor.Update(snapshot, Slow, Target, rates);
        REQUIRE(decision.action == Action::Raise);
        REQUIRE(decision.fragment_hash == 0xA);
        REQUIRE(u32{decision.level} == level);
    }
    REQUIRE(governor.Update(snapshot, Slow, Target, rates).action == Action::Hold);
    REQUIRE(rates.size() == 1);
    REQUIRE(Level(rates, 0xA) == 3);
}
