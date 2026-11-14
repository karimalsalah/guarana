// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <chrono>
#include <cmath>

#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "video_core/auto_shading_rate.h"
#include "video_core/frame_budget.h"
#include "video_core/frame_budget_controller.h"

namespace {

using VideoCore::FrameBudgetController;
using Action = FrameBudgetController::Action;
using State = FrameBudgetController::State;

constexpr double Target = 1000.0 / 30.0;
constexpr double Slow = 40.0;
constexpr double Fast = 25.0;
constexpr double Steady = 33.0;

/// GPU Clocks at Boost: scales above 4 change nothing.
FrameBudgetController::Input In(double frame_ms, bool hold_lower = false) {
    return {.frame_ms = frame_ms, .target_ms = Target, .ceiling = 4.0, .hold_lower = hold_lower};
}

bool Near(double value, double expected) {
    return std::abs(value - expected) < 1e-9;
}

/// Feeds `ticks` ticks of one frame time, applying every decision; returns the last one.
FrameBudgetController::Decision Run(FrameBudgetController& controller, double& scale,
                                    double frame_ms, u32 ticks) {
    FrameBudgetController::Decision decision;
    for (u32 i = 0; i < ticks; ++i) {
        decision = controller.Update(In(frame_ms), scale);
        scale = decision.scale;
    }
    return decision;
}

} // namespace

TEST_CASE("Frame budget raises the scale on every missed tick, up to its limit",
          "[video_core][frame_budget]") {
    FrameBudgetController controller;
    double scale = 1.0;
    auto decision = controller.Update(In(Slow), scale);
    REQUIRE(decision.action == Action::Raise);
    REQUIRE(decision.state == State::Slow);
    REQUIRE(Near(decision.scale, 1.25));
    REQUIRE_FALSE(decision.at_limit);
    scale = decision.scale;
    // 1.25^7 is past the ceiling.
    decision = Run(controller, scale, Slow, 6);
    REQUIRE(decision.action == Action::Raise);
    REQUIRE(Near(scale, 4.0));
    REQUIRE(decision.at_limit);
    // Nothing left to give: saturated, and the scale stays.
    decision = controller.Update(In(Slow), scale);
    REQUIRE(decision.action == Action::Hold);
    REQUIRE(decision.state == State::Saturated);
    REQUIRE(Near(decision.scale, 4.0));
    REQUIRE(decision.at_limit);

    // GPU Clocks at Normal: at 1 the game already sees the real time.
    FrameBudgetController normal;
    auto input = In(Slow);
    input.ceiling = 1.0;
    decision = normal.Update(input, 1.0);
    REQUIRE(decision.action == Action::Hold);
    REQUIRE(decision.state == State::Saturated);
    REQUIRE(Near(decision.scale, 1.0));
    // A scale above the ceiling comes down to it.
    REQUIRE(Near(normal.Update(input, 2.5).scale, 1.0));
}

TEST_CASE("Frame budget holds inside the band and lowers after a streak with headroom",
          "[video_core][frame_budget]") {
    FrameBudgetController controller;
    double scale = 2.0;
    for (u32 i = 0; i < 16; ++i) {
        const auto decision = controller.Update(In(Steady), scale);
        REQUIRE(decision.action == Action::Hold);
        REQUIRE(decision.state == State::OnTarget);
        REQUIRE(Near(decision.scale, 2.0));
    }
    // Three ticks with headroom are not enough, and a tick inside the band starts over.
    for (u32 round = 0; round < 4; ++round) {
        REQUIRE(Run(controller, scale, Fast, 3).action == Action::Hold);
        REQUIRE(controller.FastStreak() == 3);
        REQUIRE(controller.Update(In(Steady), scale).state == State::OnTarget);
        REQUIRE(controller.FastStreak() == 0);
    }
    REQUIRE(Near(scale, 2.0));
    // The fourth in a row lowers by 8%, and the count starts over.
    const auto decision = Run(controller, scale, Fast, 4);
    REQUIRE(decision.action == Action::Lower);
    REQUIRE(decision.state == State::Fast);
    REQUIRE(Near(scale, 2.0 * 0.92));
    REQUIRE(Run(controller, scale, Fast, 3).action == Action::Hold);
    REQUIRE(Run(controller, scale, Fast, 1).action == Action::Lower);
    REQUIRE(Near(scale, 2.0 * 0.92 * 0.92));
}

TEST_CASE("Frame budget stays above the scale that missed for a cool-down",
          "[video_core][frame_budget]") {
    FrameBudgetController::Config config;
    config.cooldown_ticks = 10;
    config.max_cooldown_ticks = 40;
    FrameBudgetController controller{config};
    double scale = 1.0;
    // A miss at 1.0, mended by one raise.
    REQUIRE(Run(controller, scale, Slow, 1).action == Action::Raise);
    REQUIRE(Near(scale, 1.25));
    REQUIRE(Near(controller.MissScale(), 1.0));
    REQUIRE(controller.CooldownLeft() == 10);
    // Headroom: one lower fits above 1.0 / 0.92, the next does not.
    REQUIRE(Run(controller, scale, Fast, 4).action == Action::Lower);
    REQUIRE(Near(scale, 1.25 * 0.92));
    auto decision = Run(controller, scale, Fast, 4);
    REQUIRE(decision.action == Action::Hold);
    REQUIRE(decision.state == State::Cooldown);
    REQUIRE(Near(scale, 1.25 * 0.92));
    // The cool-down ends ten ticks after the miss; the lower is made then.
    REQUIRE(Run(controller, scale, Fast, 1).state == State::Cooldown);
    REQUIRE(controller.CooldownLeft() == 1);
    decision = Run(controller, scale, Fast, 1);
    REQUIRE(decision.action == Action::Lower);
    REQUIRE(Near(scale, 1.25 * 0.92 * 0.92));
    REQUIRE(controller.CooldownLeft() == 0);

    // That scale misses again: the cool-down doubles and starts from the new miss.
    REQUIRE(Run(controller, scale, Slow, 1).action == Action::Raise);
    REQUIRE(Near(controller.MissScale(), 1.25 * 0.92 * 0.92));
    REQUIRE(controller.CooldownLeft() == 20);
    // A second missed tick is the same run of misses: nothing doubles, the scale goes on up.
    REQUIRE(Run(controller, scale, Slow, 1).action == Action::Raise);
    REQUIRE(Near(controller.MissScale(), 1.25 * 0.92 * 0.92));
    REQUIRE(controller.CooldownLeft() == 20);
    REQUIRE(Near(scale, 1.25 * 0.92 * 0.92 * 1.25 * 1.25));

    // Forgotten after two cool-downs without a miss.
    Run(controller, scale, Steady, 39);
    REQUIRE(controller.MissScale() > 0.0);
    REQUIRE(controller.CooldownLeft() == 0);
    Run(controller, scale, Steady, 1);
    REQUIRE(controller.MissScale() == 0.0);
}

TEST_CASE("Frame budget freezes on loading screens and without presents",
          "[video_core][frame_budget]") {
    FrameBudgetController controller;
    double scale = 1.0;
    REQUIRE(Run(controller, scale, Fast, 3).state == State::Fast);
    REQUIRE(controller.FastStreak() == 3);
    // No presents: nothing moves, and the streak starts over.
    auto decision = controller.Update(In(0.0), scale);
    REQUIRE(decision.action == Action::Hold);
    REQUIRE(decision.state == State::Frozen);
    REQUIRE(controller.FastStreak() == 0);
    // Frames over four targets long.
    for (u32 i = 0; i < 8; ++i) {
        decision = controller.Update(In(Target * 4.5), scale);
        REQUIRE(decision.action == Action::Hold);
        REQUIRE(decision.state == State::Frozen);
        REQUIRE(Near(decision.scale, 1.0));
    }
    // Just under four targets is a slow frame.
    REQUIRE(controller.Update(In(Target * 3.9), scale).action == Action::Raise);
    REQUIRE(Near(controller.MissScale(), 1.0));
    // Without a target the controller is off, forgets, and leaves any scale alone.
    auto off = In(Slow);
    off.target_ms = 0.0;
    decision = controller.Update(off, 9.0);
    REQUIRE(decision.action == Action::Hold);
    REQUIRE(decision.state == State::Off);
    REQUIRE(Near(decision.scale, 9.0));
    REQUIRE(controller.MissScale() == 0.0);
}

TEST_CASE("Frame budget waits for the shading rates and stops lowers that show nothing",
          "[video_core][frame_budget]") {
    FrameBudgetController::Config config;
    config.lower_after = 1;
    config.max_blind_lowers = 3;
    FrameBudgetController controller{config};
    double scale = 4.0;
    // Something else still gives quality back: no lower.
    for (u32 i = 0; i < 4; ++i) {
        const auto waiting = controller.Update(In(Fast, true), scale);
        REQUIRE(waiting.action == Action::Hold);
        REQUIRE(waiting.state == State::Waiting);
        REQUIRE(waiting.at_limit);
        REQUIRE(Near(waiting.scale, 4.0));
    }
    // Three lowers leave the frame time where it was: the game does not follow, so they stop.
    auto decision = Run(controller, scale, Fast, 3);
    REQUIRE(decision.action == Action::Lower);
    REQUIRE_FALSE(decision.at_limit);
    REQUIRE(controller.BlindLowers() == 3);
    decision = Run(controller, scale, Fast, 5);
    REQUIRE(decision.action == Action::Hold);
    REQUIRE(decision.state == State::NoEffect);
    REQUIRE(Near(scale, 4.0 * 0.92 * 0.92 * 0.92));
    // Frames a tenth slower, still with headroom: the lowers show, so three more are made.
    REQUIRE(Run(controller, scale, Fast * 1.1, 3).action == Action::Lower);
    REQUIRE(Run(controller, scale, Fast * 1.1, 1).state == State::NoEffect);
    // A miss starts the count over as well.
    REQUIRE(Run(controller, scale, Slow, 1).action == Action::Raise);
    REQUIRE(controller.BlindLowers() == 0);
}

TEST_CASE("Frame budget keeps the scale inside its limits", "[video_core][frame_budget]") {
    FrameBudgetController::Config config;
    config.lower_after = 1;
    config.max_blind_lowers = 0;
    FrameBudgetController controller{config};
    double scale = 0.06;
    // 0.0552, 0.0508, then the lowest scale.
    REQUIRE(Run(controller, scale, Fast, 3).action == Action::Lower);
    REQUIRE(Near(scale, 0.05));
    for (u32 i = 0; i < 8; ++i) {
        const auto decision = controller.Update(In(Fast), scale);
        REQUIRE(decision.action == Action::Hold);
        REQUIRE(decision.state == State::Fast);
        REQUIRE(Near(decision.scale, 0.05));
    }
    // A scale set by hand outside the limits is brought inside.
    REQUIRE(Near(controller.Update(In(Steady), 0.001).scale, 0.05));
    REQUIRE(Near(controller.Update(In(Steady), 100.0).scale, 4.0));
}

TEST_CASE("Frame budget ticks on the guest presents of half a second",
          "[video_core][frame_budget]") {
    namespace Budget = VideoCore::FrameBudget;
    namespace Auto = VideoCore::AutoShadingRate;
    using namespace std::chrono_literals;
    const Settings::GpuClock previous_clock = Settings::values.gpu_clock.GetValue();
    SCOPE_EXIT {
        Budget::SetTargetFps(0.0);
        Budget::SetGpuTimeScale(1.0);
        Settings::values.gpu_clock.SetValue(previous_clock);
    };
    Settings::values.gpu_clock.SetValue(Settings::GpuClock::Boost);
    Budget::SetGpuTimeScale(1.0);
    Budget::SetTargetFps(30.0);
    REQUIRE(Budget::TargetFps() == 30.0);
    REQUIRE(Budget::PresentInterval() == 33'333'333ns);
    REQUIRE(Budget::RecentWorkMs() == 0.0);
    REQUIRE(Budget::GetStatus().state == "off");
    const Budget::Status start = Budget::GetStatus();

    // 40 ms frames, which the pacer has nothing to hold: 13 of them are one tick, and a miss.
    Budget::TimePoint now{10s};
    Budget::OnGuestPresent(now, now);
    const auto slow_tick = [&now] {
        for (int i = 0; i < 13; ++i) {
            now += 40ms;
            Budget::OnGuestPresent(now, now);
        }
    };
    slow_tick();
    Budget::Status status = Budget::GetStatus();
    REQUIRE(status.state == "slow");
    REQUIRE(status.samples == 13);
    REQUIRE(status.frame_ms == 40.0);
    REQUIRE(status.work_ms == 40.0);
    REQUIRE(Near(status.scale, 1.25));
    REQUIRE(status.max_scale == 4.0);
    REQUIRE(status.ticks == start.ticks + 1);
    REQUIRE(status.raises == start.raises + 1);
    REQUIRE(Near(Budget::GpuTimeScale(), 1.25));
    REQUIRE(Budget::RecentWorkMs() == 40.0);
    // The game still has resolution to give: the shading-rate governor may not coarsen.
    REQUIRE_FALSE(status.shading_allowed);
    REQUIRE_FALSE(Auto::GetStatus().allowed);

    // Six more misses reach the limit, one more there is saturation: the governor may act.
    for (int i = 0; i < 6; ++i) {
        slow_tick();
    }
    REQUIRE(Near(Budget::GpuTimeScale(), 4.0));
    REQUIRE(Budget::GetStatus().state == "slow");
    REQUIRE_FALSE(Auto::GetStatus().allowed);
    slow_tick();
    status = Budget::GetStatus();
    REQUIRE(status.state == "saturated");
    REQUIRE(Near(status.scale, 4.0));
    REQUIRE(status.shading_allowed);
    REQUIRE(Auto::GetStatus().allowed);

    // Frames done in 20 ms and held to the interval: headroom, and after four ticks of it one
    // lower, which takes the scale off its limit and the governor's permission with it.
    const auto interval = Budget::PresentInterval();
    for (int i = 0; i < 4 * 16; ++i) {
        Budget::OnGuestPresent(now + 20ms, now + interval);
        now += interval;
    }
    status = Budget::GetStatus();
    REQUIRE(status.state == "fast");
    REQUIRE(status.samples == 16);
    REQUIRE(status.work_ms == 20.0);
    REQUIRE(status.frame_ms > 33.3);
    REQUIRE(status.frame_ms < 33.4);
    REQUIRE(status.ticks == start.ticks + 12);
    REQUIRE(status.lowers == start.lowers + 1);
    REQUIRE(Near(status.scale, 4.0 * 0.92));
    REQUIRE(Near(status.miss_scale, 1.0));
    REQUIRE(status.cooldown_ticks == 26);
    REQUIRE_FALSE(status.shading_allowed);
    REQUIRE_FALSE(Auto::GetStatus().allowed);

    // A scale set by hand is what the controller goes on from and what stays without a target.
    Budget::SetGpuTimeScale(2.0);
    REQUIRE(Budget::GpuTimeScale() == 2.0);
    Budget::SetTargetFps(0.0);
    REQUIRE(Budget::TargetFps() == 0.0);
    REQUIRE(Budget::PresentInterval() == 0ns);
    REQUIRE(Budget::GpuTimeScale() == 2.0);
    REQUIRE(Budget::RecentWorkMs() == 0.0);
    REQUIRE(Budget::GetStatus().state == "off");
    REQUIRE(Auto::GetStatus().allowed);
    // Off, presents are not looked at.
    Budget::OnGuestPresent(now, now);
    Budget::OnGuestPresent(now + 40ms, now + 40ms);
    REQUIRE(Budget::GetStatus().samples == 0);
}

namespace {

/// Ocarina of Time with the Max Resolution add-on on a Panther Lake integrated GPU: a 50 ms
/// target, and frames of 55 to 97 ms without the pacing wait whatever the scale.
constexpr std::array<double, 12> FixedResolutionTrace{97.0, 62.0, 88.0, 55.0, 79.0, 93.0,
                                                      68.0, 84.0, 71.0, 90.0, 58.0, 96.0};

FrameBudgetController::Input In50(double frame_ms, bool hold_lower = false) {
    return {.frame_ms = frame_ms, .target_ms = 50.0, .ceiling = 4.0, .hold_lower = hold_lower};
}

/// EDEN_NX2_BUDGET_UNANSWERED=1.
FrameBudgetController::Config Watching() {
    FrameBudgetController::Config config;
    config.max_unanswered = 8;
    return config;
}

} // namespace

TEST_CASE("Frame budget holds a scale whose raises the frame time does not answer",
          "[video_core][frame_budget]") {
    FrameBudgetController controller{Watching()};
    double scale = 0.256;
    // Eight missed ticks, eight raises.
    for (size_t i = 0; i < 8; ++i) {
        const auto decision = controller.Update(In50(FixedResolutionTrace[i]), scale);
        REQUIRE(decision.action == Action::Raise);
        REQUIRE(decision.state == State::Slow);
        REQUIRE_FALSE(decision.scale_spent);
        scale = decision.scale;
    }
    REQUIRE(Near(scale, 0.256 * std::pow(1.25, 8)));
    REQUIRE_FALSE(controller.Unanswered());
    // The ninth finds the frame time no faster than the best of the first three: the scale is
    // held below its limit, and the next lever may act.
    for (size_t i = 8; i < FixedResolutionTrace.size(); ++i) {
        const auto decision = controller.Update(In50(FixedResolutionTrace[i]), scale);
        REQUIRE(decision.action == Action::Hold);
        REQUIRE(decision.state == State::Unanswered);
        REQUIRE(decision.scale_spent);
        REQUIRE_FALSE(decision.at_limit);
        REQUIRE(Near(decision.scale, scale));
    }
    REQUIRE(controller.Unanswered());
    REQUIRE(Near(controller.UnansweredFrom(), 0.256));
    REQUIRE(controller.UnansweredTicks() == 9);

    // On target while another lever holds coarse rates: the scale stays out of its way, and a
    // new miss is not raised against either.
    auto decision = controller.Update(In50(48.0, true), scale);
    REQUIRE(decision.state == State::OnTarget);
    REQUIRE(decision.scale_spent);
    decision = controller.Update(In50(80.0, true), scale);
    REQUIRE(decision.action == Action::Hold);
    REQUIRE(decision.state == State::Unanswered);
    REQUIRE(decision.scale_spent);
    // On target with nothing else holding quality back: undone, the scale is tried again.
    decision = controller.Update(In50(48.0), scale);
    REQUIRE(decision.state == State::OnTarget);
    REQUIRE_FALSE(decision.scale_spent);
    REQUIRE_FALSE(controller.Unanswered());
    REQUIRE(controller.UnansweredFrom() == 0.0);
    decision = controller.Update(In50(80.0), scale);
    REQUIRE(decision.action == Action::Raise);
    REQUIRE(decision.state == State::Slow);

    // A new target forgets it all.
    REQUIRE(Run(controller, scale, Slow, 1).action == Action::Raise);
    controller.Reset();
    REQUIRE_FALSE(controller.Unanswered());
    REQUIRE(controller.UnansweredTicks() == 0);
}

TEST_CASE("Frame budget keeps raising while the frame time answers",
          "[video_core][frame_budget]") {
    // A game whose dynamic resolution follows the scale a tick late: 90 ms at 0.256, 50.6 ms
    // at 0.977.
    const auto frame_at = [](double at_scale) {
        return 30.0 + 60.0 * std::pow(0.256 / at_scale, 0.8);
    };
    FrameBudgetController controller{Watching()};
    double scale = 0.256;
    double rendered = scale;
    u32 raises = 0;
    FrameBudgetController::Decision decision;
    for (u32 i = 0; i < 20; ++i) {
        decision = controller.Update(In50(frame_at(rendered)), scale);
        REQUIRE(decision.state != State::Unanswered);
        REQUIRE_FALSE(decision.scale_spent);
        rendered = scale;
        scale = decision.scale;
        raises += decision.action == Action::Raise ? 1 : 0;
    }
    REQUIRE(raises == 7);
    REQUIRE(decision.state == State::OnTarget);
    REQUIRE_FALSE(controller.Unanswered());
}

TEST_CASE("Frame budget counts holds at the limit once raises were tried",
          "[video_core][frame_budget]") {
    // From 2.0 four raises reach the limit, and the holds there go on counting.
    FrameBudgetController controller{Watching()};
    double scale = 2.0;
    for (u32 i = 0; i < 8; ++i) {
        const auto decision = controller.Update(In(Slow), scale);
        REQUIRE(decision.state == (i < 4 ? State::Slow : State::Saturated));
        REQUIRE_FALSE(controller.Unanswered());
        scale = decision.scale;
    }
    REQUIRE(Near(scale, 4.0));
    const auto decision = controller.Update(In(Slow), scale);
    REQUIRE(decision.action == Action::Hold);
    REQUIRE(decision.state == State::Unanswered);
    REQUIRE(decision.at_limit);
    REQUIRE(decision.scale_spent);
    REQUIRE(Near(controller.UnansweredFrom(), 2.0));

    // GPU Clocks at Normal: a run that begins at the limit never tried a raise, so it stays
    // saturated.
    FrameBudgetController normal{Watching()};
    auto input = In(Slow);
    input.ceiling = 1.0;
    for (u32 i = 0; i < 20; ++i) {
        REQUIRE(normal.Update(input, 1.0).state == State::Saturated);
        REQUIRE_FALSE(normal.Unanswered());
    }

    // Without EDEN_NX2_BUDGET_UNANSWERED the scale walks to its limit and stays saturated.
    FrameBudgetController never;
    double never_scale = 1.0;
    const auto last = Run(never, never_scale, Slow, 30);
    REQUIRE(last.state == State::Saturated);
    REQUIRE(Near(never_scale, 4.0));
    REQUIRE_FALSE(never.Unanswered());
}

TEST_CASE("Frame budget finds an unanswered scale when the slowest ticks come first",
          "[video_core][frame_budget]") {
    // The worst order for the check: the reference is the slowest frames of the trace, so
    // noise under it can pass for an answer.
    constexpr std::array<double, 4> slowest_first{97.0, 96.0, 93.0, 90.0};
    FrameBudgetController controller{Watching()};
    double scale = 0.256;
    size_t found_at = 0;
    for (size_t i = 0; i < slowest_first.size() + FixedResolutionTrace.size() && found_at == 0;
         ++i) {
        const double frame_ms = i < slowest_first.size()
                                    ? slowest_first[i]
                                    : FixedResolutionTrace[i - slowest_first.size()];
        scale = controller.Update(In50(frame_ms), scale).scale;
        if (controller.Unanswered()) {
            found_at = i + 1;
        }
    }
    REQUIRE(found_at != 0);
    REQUIRE(found_at <= 13);
    REQUIRE(scale < 4.0);
}

TEST_CASE("Frame budget without EDEN_NX2_BUDGET_UNANSWERED raises past an unanswered run",
          "[video_core][frame_budget]") {
    namespace Budget = VideoCore::FrameBudget;
    namespace Auto = VideoCore::AutoShadingRate;
    using namespace std::chrono_literals;
    const Settings::GpuClock previous_clock = Settings::values.gpu_clock.GetValue();
    SCOPE_EXIT {
        Budget::SetTargetFps(0.0);
        Budget::SetGpuTimeScale(1.0);
        Settings::values.gpu_clock.SetValue(previous_clock);
    };
    Settings::values.gpu_clock.SetValue(Settings::GpuClock::Boost);
    Budget::SetGpuTimeScale(0.256);
    Budget::SetTargetFps(20.0);
    const Budget::Status start = Budget::GetStatus();

    Budget::TimePoint now{100s};
    Budget::OnGuestPresent(now, now);
    // Presents of one frame time, unpaced since they are later than the interval, up to a tick.
    const auto tick = [&now](double frame_ms) {
        const u64 ticks = Budget::GetStatus().ticks;
        while (Budget::GetStatus().ticks == ticks) {
            now += std::chrono::duration_cast<Budget::TimePoint::duration>(
                std::chrono::duration<double, std::milli>{frame_ms});
            Budget::OnGuestPresent(now, now);
        }
    };
    for (size_t i = 0; i < 8; ++i) {
        tick(FixedResolutionTrace[i]);
    }
    Budget::Status status = Budget::GetStatus();
    REQUIRE(status.state == "slow");
    REQUIRE(status.raises == start.raises + 8);
    REQUIRE(status.unanswered_from == 0.0);
    REQUIRE_FALSE(status.shading_allowed);
    REQUIRE_FALSE(Auto::GetStatus().allowed);

    // The switch is unset in the tests: the ninth missed tick raises as before.
    tick(FixedResolutionTrace[8]);
    status = Budget::GetStatus();
    REQUIRE(status.state == "slow");
    REQUIRE(status.raises == start.raises + 9);
    REQUIRE(Near(status.scale, 0.256 * std::pow(1.25, 9)));
    REQUIRE(status.unanswered_from == 0.0);
    REQUIRE_FALSE(status.shading_allowed);
    REQUIRE_FALSE(Auto::GetStatus().allowed);
}
