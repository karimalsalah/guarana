// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cmath>
#include <span>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "video_core/shown_governor.h"

namespace {

using namespace VideoCore::ShownGovernor;

constexpr std::array<u32, 3> Ladder144{144, 72, 48};
constexpr f64 SlotsPerWindow = 288.0;

/// A window of 288 slots (144 a second for 2 s): `hit_share` of them on time, `real_fps` real
/// frames a second against a target of 24, and a GPU busy share (-1: not sampled).
WindowStats Window(f64 hit_share, f64 real_fps, f64 busy) {
    WindowStats window;
    window.slots = static_cast<u64>(SlotsPerWindow);
    window.hits = static_cast<u64>(std::llround(hit_share * SlotsPerWindow));
    window.real_fps = real_fps;
    window.target_fps = 24.0;
    window.gpu_busy = busy;
    return window;
}

/// The reference laptop at 144 shown: half the slots on time, real frames short, GPU saturated.
WindowStats Pressured() {
    return Window(0.50, 21.0, 1.0);
}

/// 95% of the slots on time, real frames on target, the GPU half busy.
WindowStats Roomy() {
    return Window(0.95, 24.0, 0.5);
}

/// 74% of the slots on time (72 shown on the reference laptop): neither pressure nor room.
WindowStats Fine() {
    return Window(0.74, 24.0, 0.79);
}

/// Feeds `windows` equal windows; the decisions that changed the rate or the ladder.
u32 CountChanges(State& state, std::span<const u32> ladder, const WindowStats& window,
                 u32 windows) {
    u32 changes = 0;
    for (u32 index = 0; index < windows; ++index) {
        changes += Decide(state, ladder, window).reason != Reason::Hold ? 1U : 0U;
    }
    return changes;
}

/// A state at `rate` as if it had settled there: the dwell is over.
State Settled(u32 rate) {
    State state;
    state.rate = rate;
    state.windows_here = DwellWindows;
    return state;
}

} // Anonymous namespace

TEST_CASE("Shown governor: the ladder is the refresh rate's divisors", "[shown_governor]") {
    std::array<u32, MaxRungs> ladder{};
    SECTION("144 Hz with 24 real frames: 144, 72, 48") {
        REQUIRE(BuildLadder(144.0, 144.0, 24.0, ladder) == 3);
        CHECK(ladder[0] == 144);
        CHECK(ladder[1] == 72);
        CHECK(ladder[2] == 48);
    }
    SECTION("120 Hz with 20 real frames: 120, 60, 40") {
        REQUIRE(BuildLadder(120.0, 120.0, 20.0, ladder) == 3);
        CHECK(ladder[0] == 120);
        CHECK(ladder[1] == 60);
        CHECK(ladder[2] == 40);
    }
    SECTION("a refresh rate that is a fraction off rounds to the whole one") {
        REQUIRE(BuildLadder(143.97, 144.0, 24.0, ladder) == 3);
        CHECK(ladder[0] == 144);
        CHECK(ladder[1] == 72);
        CHECK(ladder[2] == 48);
    }
    SECTION("rungs that cannot keep the real frame rate are left out") {
        // 24 real frames at 120 Hz: 60 and 40 shown frames are not a whole number per real frame.
        REQUIRE(BuildLadder(120.0, 120.0, 24.0, ladder) == 1);
        CHECK(ladder[0] == 120);
    }
    SECTION("without a cadence every divisor counts") {
        REQUIRE(BuildLadder(144.0, 144.0, 0.0, ladder) == 3);
        CHECK(ladder[2] == 48);
    }
    SECTION("rungs above the configured rate are left out") {
        REQUIRE(BuildLadder(144.0, 100.0, 0.0, ladder) == 3);
        CHECK(ladder[0] == 72);
        CHECK(ladder[1] == 48);
        CHECK(ladder[2] == 36);
    }
    SECTION("rungs under 30 Hz are left out") {
        REQUIRE(BuildLadder(60.0, 144.0, 0.0, ladder) == 2);
        CHECK(ladder[0] == 60);
        CHECK(ladder[1] == 30);
    }
    SECTION("an unknown refresh rate or configured rate gives no rung") {
        CHECK(BuildLadder(0.0, 144.0, 24.0, ladder) == 0);
        CHECK(BuildLadder(144.0, 0.0, 24.0, ladder) == 0);
    }
}

TEST_CASE("Shown governor: the cadence divisor scales with the shown rate", "[shown_governor]") {
    // 6 at 144 shown holds real frames to 24 a second.
    CHECK(ScaledDivisor(144.0, 6, 144) == 6);
    CHECK(ScaledDivisor(144.0, 6, 72) == 3);
    CHECK(ScaledDivisor(144.0, 6, 48) == 2);
    // 6 at 120 holds them to 20.
    CHECK(ScaledDivisor(120.0, 6, 60) == 3);
    CHECK(ScaledDivisor(120.0, 6, 40) == 2);
    // No governed rate, no base divisor or no base rate: the base divisor, as is.
    CHECK(ScaledDivisor(144.0, 6, 0) == 6);
    CHECK(ScaledDivisor(144.0, 0, 72) == 0);
    CHECK(ScaledDivisor(0.0, 6, 72) == 6);
    // Never under 1 or over 12.
    CHECK(ScaledDivisor(144.0, 2, 30) == 1);
    CHECK(ScaledDivisor(60.0, 12, 240) == 12);
}

TEST_CASE("Shown governor: the governed rate bounds the autopilot's", "[shown_governor]") {
    CHECK(BoundSlotRate(0.0, 0.0) == 0.0);
    CHECK(BoundSlotRate(75.0, 0.0) == 75.0);
    CHECK(BoundSlotRate(0.0, 72.0) == 72.0);
    CHECK(BoundSlotRate(144.0, 72.0) == 72.0);
    CHECK(BoundSlotRate(60.0, 72.0) == 60.0);
}

TEST_CASE("Shown governor: a window is judged by slots, real frames and the GPU", "[shown_governor]") {
    SECTION("the measured 144 shown window is pressure") {
        CHECK(Judge(Pressured()) == Verdict::Pressure);
    }
    SECTION("the measured 72 shown window is neither pressure nor room") {
        CHECK(Judge(Fine()) == Verdict::Fine);
    }
    SECTION("few slots say nothing") {
        WindowStats window = Pressured();
        window.slots = MinSlots - 1;
        window.hits = 0;
        CHECK(Judge(window) == Verdict::NoData);
    }
    SECTION("without real frames the slots missed are not the GPU's doing") {
        // A loading screen: nothing is presented, every slot is missed.
        CHECK(Judge(Window(0.0, 0.0, 0.1)) == Verdict::NoData);
        CHECK(Judge(Window(0.0, 5.0, 1.0)) == Verdict::NoData);
    }
    SECTION("without a cadence target 5 real frames a second count as flowing") {
        WindowStats window = Window(0.4, 4.0, 1.0);
        window.target_fps = 0.0;
        CHECK(Judge(window) == Verdict::NoData);
        window.real_fps = 6.0;
        CHECK(Judge(window) == Verdict::Pressure);
    }
    SECTION("missed slots while the GPU is mostly idle are not pressure") {
        CHECK(Judge(Window(0.50, 24.0, 0.30)) == Verdict::Fine);
    }
    SECTION("missed slots with the GPU not sampled are pressure") {
        CHECK(Judge(Window(0.50, 24.0, -1.0)) == Verdict::Pressure);
    }
    SECTION("real frames short of the target count only while the GPU is saturated") {
        CHECK(Judge(Window(0.95, 21.0, 0.95)) == Verdict::Pressure);
        CHECK(Judge(Window(0.95, 21.0, 0.80)) == Verdict::Fine);
        CHECK(Judge(Window(0.95, 21.0, -1.0)) == Verdict::Fine);
    }
    SECTION("room needs a sampled GPU, slots met and real frames on target") {
        CHECK(Judge(Roomy()) == Verdict::Room);
        CHECK(Judge(Window(0.95, 24.0, -1.0)) == Verdict::Fine);
        CHECK(Judge(Window(0.80, 24.0, 0.5)) == Verdict::Fine);
        CHECK(Judge(Window(0.95, 22.0, 0.5)) == Verdict::Fine);
    }
    SECTION("more hits than slots count as all of them") {
        WindowStats window = Roomy();
        window.hits = window.slots + 5;
        CHECK(Judge(window) == Verdict::Room);
    }
}

TEST_CASE("Shown governor: a step up costs GPU time in proportion", "[shown_governor]") {
    // 33.1 ms at 72 shown against 47.6 ms at 144: +44%.
    CHECK(StepUpCost(72, 144) == Catch::Approx(1.45));
    CHECK(StepUpCost(48, 72) == Catch::Approx(1.225));
    CHECK(StepUpCost(0, 144) == 1.0);
}

TEST_CASE("Shown governor: the first decision takes the top rung", "[shown_governor]") {
    State state;
    const Decision decision = Decide(state, Ladder144, Pressured());
    CHECK(decision.reason == Reason::Start);
    CHECK(state.rate == 144);
}

TEST_CASE("Shown governor: no ladder, no decision", "[shown_governor]") {
    State state = Settled(72);
    const Decision decision = Decide(state, std::span<const u32>{}, Pressured());
    CHECK(decision.reason == Reason::Hold);
    CHECK(state.rate == 72);
}

TEST_CASE("Shown governor: a step down needs two windows of pressure after the dwell",
          "[shown_governor]") {
    State state;
    REQUIRE(Decide(state, Ladder144, Pressured()).reason == Reason::Start);
    // The dwell: DwellWindows - 1 windows after the start only settle.
    for (u32 index = 1; index < DwellWindows; ++index) {
        CHECK(Decide(state, Ladder144, Pressured()).reason == Reason::Hold);
        CHECK(state.rate == 144);
    }
    // The first window judged counts one, the second steps down.
    CHECK(Decide(state, Ladder144, Pressured()).reason == Reason::Hold);
    CHECK(state.rate == 144);
    const Decision decision = Decide(state, Ladder144, Pressured());
    CHECK(decision.reason == Reason::Pressure);
    CHECK(state.rate == 72);
    // The next step waits for the dwell again.
    CHECK(CountChanges(state, Ladder144, Pressured(), DwellWindows + PressuredWindows - 2) == 0);
    CHECK(state.rate == 72);
    CHECK(Decide(state, Ladder144, Pressured()).reason == Reason::Pressure);
    CHECK(state.rate == 48);
}

TEST_CASE("Shown governor: a single window of pressure does not step", "[shown_governor]") {
    State state = Settled(144);
    u32 changes = 0;
    for (u32 index = 0; index < 20; ++index) {
        changes += Decide(state, Ladder144, Pressured()).reason != Reason::Hold ? 1U : 0U;
        changes += Decide(state, Ladder144, Fine()).reason != Reason::Hold ? 1U : 0U;
    }
    CHECK(changes == 0);
    CHECK(state.rate == 144);
}

TEST_CASE("Shown governor: windows without data break a run of pressure", "[shown_governor]") {
    State state = Settled(144);
    const WindowStats nothing = Window(0.0, 0.0, 0.1);
    u32 changes = 0;
    for (u32 index = 0; index < 20; ++index) {
        changes += Decide(state, Ladder144, Pressured()).reason != Reason::Hold ? 1U : 0U;
        changes += Decide(state, Ladder144, nothing).reason != Reason::Hold ? 1U : 0U;
    }
    CHECK(changes == 0);
    CHECK(state.rate == 144);
}

TEST_CASE("Shown governor: the lowest rung holds under pressure", "[shown_governor]") {
    State state = Settled(48);
    CHECK(CountChanges(state, Ladder144, Pressured(), 100) == 0);
    CHECK(state.rate == 48);
}

TEST_CASE("Shown governor: between pressure and room the rate stays", "[shown_governor]") {
    // The measured 72 shown run: 74% of the slots on time, real frames on target.
    State state = Settled(72);
    CHECK(CountChanges(state, Ladder144, Fine(), 500) == 0);
    CHECK(state.rate == 72);
}

TEST_CASE("Shown governor: a step up needs ten windows of room", "[shown_governor]") {
    State state = Settled(72);
    CHECK(CountChanges(state, Ladder144, Roomy(), RoomyWindows - 1) == 0);
    CHECK(state.rate == 72);
    const Decision decision = Decide(state, Ladder144, Roomy());
    CHECK(decision.reason == Reason::Headroom);
    CHECK(state.rate == 144);
}

TEST_CASE("Shown governor: one window short of room starts the count again", "[shown_governor]") {
    State state = Settled(72);
    CHECK(CountChanges(state, Ladder144, Roomy(), RoomyWindows - 1) == 0);
    CHECK(Decide(state, Ladder144, Fine()).reason == Reason::Hold);
    CHECK(CountChanges(state, Ladder144, Roomy(), RoomyWindows - 1) == 0);
    CHECK(state.rate == 72);
    CHECK(Decide(state, Ladder144, Roomy()).reason == Reason::Headroom);
}

TEST_CASE("Shown governor: a step up must leave the GPU room for what the rate costs",
          "[shown_governor]") {
    // 72 to 144 grows GPU time by 45%: 0.65 busy becomes 0.94, 0.66 becomes 0.96.
    State state = Settled(72);
    CHECK(CountChanges(state, Ladder144, Window(0.95, 24.0, 0.66), 100) == 0);
    CHECK(state.rate == 72);
    CHECK(CountChanges(state, Ladder144, Window(0.95, 24.0, 0.65), RoomyWindows) == 1);
    CHECK(state.rate == 144);
}

TEST_CASE("Shown governor: the top rung holds under room", "[shown_governor]") {
    State state = Settled(144);
    CHECK(CountChanges(state, Ladder144, Roomy(), 100) == 0);
    CHECK(state.rate == 144);
}

TEST_CASE("Shown governor: a failed step up blocks the next for 60 s", "[shown_governor]") {
    State state = Settled(72);
    REQUIRE(CountChanges(state, Ladder144, Roomy(), RoomyWindows) == 1);
    REQUIRE(state.rate == 144);
    // The step up fails: pressure again within the probe.
    CHECK(CountChanges(state, Ladder144, Pressured(), DwellWindows + PressuredWindows - 1) == 1);
    REQUIRE(state.rate == 72);
    CHECK(state.block_left == BlockWindows);
    // Room all the while, and still no step up for BlockWindows - 1 more windows.
    CHECK(CountChanges(state, Ladder144, Roomy(), BlockWindows - 1) == 0);
    CHECK(state.rate == 72);
    // The window that ends the block steps up: the room was there the whole time.
    CHECK(Decide(state, Ladder144, Roomy()).reason == Reason::Headroom);
    CHECK(state.rate == 144);
}

TEST_CASE("Shown governor: pressure long after a step up is not a failure", "[shown_governor]") {
    State state = Settled(72);
    REQUIRE(CountChanges(state, Ladder144, Roomy(), RoomyWindows) == 1);
    REQUIRE(state.rate == 144);
    // Longer than the probe at the new rate, with neither pressure nor room.
    REQUIRE(CountChanges(state, Ladder144, Fine(), ProbeWindows + 2) == 0);
    CHECK(CountChanges(state, Ladder144, Pressured(), PressuredWindows) == 1);
    REQUIRE(state.rate == 72);
    CHECK(state.block_left == 0);
    // So room brings the step up back after the dwell and ten windows.
    CHECK(CountChanges(state, Ladder144, Roomy(), DwellWindows + RoomyWindows - 1) == 1);
    CHECK(state.rate == 144);
}

TEST_CASE("Shown governor: a rate off the ladder snaps to the nearest rung", "[shown_governor]") {
    State state = Settled(100);
    const Decision decision = Decide(state, Ladder144, Roomy());
    CHECK(decision.reason == Reason::Ladder);
    CHECK(state.rate == 72);
    CHECK(state.windows_here == 0);

    // Half way between two rungs: the higher one.
    State tie = Settled(108);
    CHECK(Decide(tie, Ladder144, Roomy()).reason == Reason::Ladder);
    CHECK(tie.rate == 144);
}

TEST_CASE("Shown governor: a new ladder keeps the block on step ups", "[shown_governor]") {
    State state = Settled(100);
    state.block_left = 5;
    REQUIRE(Decide(state, Ladder144, Roomy()).reason == Reason::Ladder);
    CHECK(state.block_left == 5);
}

TEST_CASE("Shown governor: the measured laptop settles at 72 and stays", "[shown_governor]") {
    // 144 shown: 50% of the slots on time and 24 real frames cost 47.6 ms of GPU each, so about 21
    // real frames at a saturated GPU. 72 shown: 74% on time, 33.1 ms per real frame, 79% busy.
    State state;
    REQUIRE(Decide(state, Ladder144, Pressured()).reason == Reason::Start);
    CHECK(CountChanges(state, Ladder144, Pressured(), DwellWindows + PressuredWindows - 1) == 1);
    CHECK(state.rate == 72);
    CHECK(CountChanges(state, Ladder144, Fine(), 300) == 0);
    CHECK(state.rate == 72);
}
