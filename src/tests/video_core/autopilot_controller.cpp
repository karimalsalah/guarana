// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "video_core/autopilot_controller.h"

namespace {

using VideoCore::AutopilotController;
using Input = AutopilotController::Input;
using Decision = AutopilotController::Decision;
using ResolutionAction = AutopilotController::ResolutionAction;
using PowerAction = AutopilotController::PowerAction;
using PowerSource = AutopilotController::PowerSource;

/// Frame generation in target mode at 120 Hz with the given real fps and target.
Input FrameGen(double real_fps, u32 target = 60, double refresh_hz = 120.0) {
    Input input;
    input.real_fps = real_fps;
    input.refresh_hz = refresh_hz;
    input.fg_on = true;
    input.fg_target = target;
    input.fg_multiplier = 2;
    input.fg_multiplier_limit = 4;
    input.shown_samples = 100;
    input.shown_even_percent = 95.0;
    return input;
}

/// A GPU bound game without frame generation.
Input Gpu(double real_fps, double busy, double scale = 1.0) {
    Input input;
    input.real_fps = real_fps;
    input.refresh_hz = 60.0;
    input.gpu_busy = busy;
    input.resolution_available = true;
    input.scale = scale;
    input.scale_base = 1.0;
    input.scale_limit = 4.0;
    return input;
}

Decision Repeat(AutopilotController& controller, const Input& input, u32 windows) {
    Decision last;
    for (u32 i = 0; i < windows; ++i) {
        last = controller.Update(input);
    }
    return last;
}

} // Anonymous namespace

TEST_CASE("Autopilot candidates are the integer divisors of the refresh", "[video_core]") {
    const AutopilotController controller;
    SECTION("120 Hz") {
        REQUIRE(controller.Candidates(120.0) == std::vector<u32>{60, 40, 30, 24, 20});
    }
    SECTION("119.88 Hz counts as 120") {
        REQUIRE(controller.Candidates(119.88) == controller.Candidates(120.0));
    }
    SECTION("59.94 Hz counts as 60") {
        REQUIRE(controller.Candidates(59.94) == std::vector<u32>{60, 30, 20});
    }
    SECTION("144 Hz has no 60") {
        REQUIRE(controller.Candidates(144.0) == std::vector<u32>{72, 48, 36, 24});
    }
    SECTION("165 Hz") {
        REQUIRE(controller.Candidates(164.8) == std::vector<u32>{55, 33});
    }
    SECTION("unknown") {
        REQUIRE(controller.Candidates(0.0).empty());
    }
    SECTION("the best target at 120 Hz: 60 from 20 fps, 40 below") {
        REQUIRE(controller.BestTarget(120.0, 20.0, 1.0) == 60);
        REQUIRE(controller.BestTarget(120.0, 19.9, 1.0) == 40);
        REQUIRE(controller.BestTarget(120.0, 13.0, 1.0) == 30);
        REQUIRE(controller.BestTarget(120.0, 1.0, 1.0) == 0);
    }
}

TEST_CASE("Autopilot frame generation target follows the real fps", "[video_core]") {
    AutopilotController controller;
    SECTION("a target the real fps carries is kept") {
        const Decision decision = Repeat(controller, FrameGen(25.0), 10);
        REQUIRE_FALSE(decision.fg_target);
        REQUIRE_FALSE(decision.fg_multiplier);
    }
    SECTION("below the minimum it steps down by one candidate only") {
        const Decision decision = controller.Update(FrameGen(8.0));
        REQUIRE(decision.fg_target == 40);
    }
    SECTION("each further step needs the dwell") {
        REQUIRE(controller.Update(FrameGen(8.0)).fg_target == 40);
        REQUIRE_FALSE(controller.Update(FrameGen(8.0, 40)).fg_target);
        REQUIRE(controller.Update(FrameGen(8.0, 40)).fg_target == 30);
    }
    SECTION("a higher target needs a margin") {
        // 40 needs 13.3 fps; 60 needs 22 with the margin.
        REQUIRE_FALSE(Repeat(controller, FrameGen(21.0, 40), 10).fg_target);
        REQUIRE(Repeat(controller, FrameGen(22.5, 40), 1).fg_target == 60);
    }
    SECTION("a higher target also needs a long dwell after a change") {
        REQUIRE(controller.Update(FrameGen(12.0)).fg_target == 40);
        for (u32 i = 0; i < 4; ++i) {
            REQUIRE_FALSE(controller.Update(FrameGen(23.0, 40)).fg_target);
        }
        REQUIRE(controller.Update(FrameGen(23.0, 40)).fg_target == 60);
    }
    SECTION("flapping around 20 fps does not change the target") {
        u32 changes = 0;
        u32 target = 60;
        for (u32 i = 0; i < 40; ++i) {
            const double fps = i % 2 == 0 ? 19.0 : 21.0;
            const Decision decision = controller.Update(FrameGen(fps, target));
            if (decision.fg_target) {
                target = *decision.fg_target;
                ++changes;
            }
        }
        REQUIRE(changes == 0);
    }
    SECTION("a target that is no divisor of the refresh snaps in one step") {
        const Decision decision = controller.Update(FrameGen(30.0, 60, 144.0));
        REQUIRE(decision.fg_target == 72);
    }
    SECTION("an unknown refresh holds the target") {
        const Decision decision = controller.Update(FrameGen(8.0, 60, 0.0));
        REQUIRE_FALSE(decision.fg_target);
    }
    SECTION("frame generation off is never touched") {
        Input input = FrameGen(8.0);
        input.fg_on = false;
        const Decision decision = Repeat(controller, input, 10);
        REQUIRE_FALSE(decision.fg_target);
        REQUIRE_FALSE(decision.fg_multiplier);
    }
    SECTION("no data decides nothing") {
        REQUIRE_FALSE(controller.Update(FrameGen(0.0)).fg_target);
        REQUIRE_FALSE(controller.Update(FrameGen(2.0)).fg_target);
    }
}

TEST_CASE("Autopilot steps the target down for uneven shown frames", "[video_core]") {
    AutopilotController controller;
    Input input = FrameGen(30.0);
    input.shown_even_percent = 50.0;
    REQUIRE_FALSE(controller.Update(input).fg_target);
    REQUIRE_FALSE(controller.Update(input).fg_target);
    const Decision decision = controller.Update(input);
    REQUIRE(decision.fg_target == 40);
    // The target left is not tried again for a while, however good the real fps is.
    input.fg_target = 40;
    input.shown_even_percent = 95.0;
    REQUIRE_FALSE(Repeat(controller, input, 20).fg_target);
}

TEST_CASE("Autopilot fixed multiplier follows the real fps within the user's limit", "[video_core]") {
    AutopilotController controller;
    Input input = FrameGen(14.0, 0);
    input.fg_multiplier = 2;
    SECTION("14 fps at 120 Hz aims at 40 and asks for 3") {
        REQUIRE(controller.Update(input).fg_multiplier == 3);
    }
    SECTION("never above the limit") {
        input.fg_multiplier_limit = 2;
        REQUIRE_FALSE(controller.Update(input).fg_multiplier);
    }
    SECTION("a ratio near the multiplier keeps it") {
        input = FrameGen(28.0, 0);
        input.fg_multiplier = 2;
        REQUIRE_FALSE(Repeat(controller, input, 10).fg_multiplier);
    }
    SECTION("never changes the target") {
        REQUIRE_FALSE(controller.Update(input).fg_target);
    }
}

TEST_CASE("Autopilot lowers the resolution only when GPU bound", "[video_core]") {
    AutopilotController controller;
    SECTION("GPU bound below the goal: one step") {
        const Decision decision = controller.Update(Gpu(20.0, 0.97));
        REQUIRE(decision.resolution == ResolutionAction::Lower);
        REQUIRE(decision.scale == 1.25);
    }
    SECTION("CPU bound: not touched") {
        REQUIRE(Repeat(controller, Gpu(20.0, 0.5), 10).resolution == ResolutionAction::Hold);
    }
    SECTION("at the goal: not touched") {
        REQUIRE(Repeat(controller, Gpu(30.0, 0.98), 10).resolution == ResolutionAction::Hold);
    }
    SECTION("GPU time unknown: not touched") {
        REQUIRE(Repeat(controller, Gpu(20.0, -1.0), 10).resolution == ResolutionAction::Hold);
    }
    SECTION("the frame budget owns the resolution: not touched") {
        Input input = Gpu(20.0, 0.97);
        input.resolution_available = false;
        REQUIRE(Repeat(controller, input, 10).resolution == ResolutionAction::Hold);
    }
    SECTION("steps wait for the game to follow") {
        REQUIRE(controller.Update(Gpu(20.0, 0.97)).resolution == ResolutionAction::Lower);
        REQUIRE(controller.Update(Gpu(22.0, 0.97, 1.25)).resolution == ResolutionAction::Hold);
        REQUIRE(controller.Update(Gpu(22.0, 0.97, 1.25)).resolution == ResolutionAction::Hold);
        REQUIRE(controller.Update(Gpu(22.0, 0.97, 1.25)).resolution == ResolutionAction::Lower);
    }
    SECTION("bounded by twice the starting scale") {
        double scale = 1.0;
        for (u32 i = 0; i < 40; ++i) {
            const Decision decision = controller.Update(Gpu(10.0 * scale, 0.97, scale));
            if (decision.resolution == ResolutionAction::Lower) {
                REQUIRE(decision.scale <= 2.0);
                scale = decision.scale;
            }
        }
        REQUIRE(scale == 2.0);
    }
    SECTION("the frame budget's own limit bounds it too") {
        Input input = Gpu(10.0, 0.97, 1.5);
        input.scale_limit = 1.5;
        REQUIRE(Repeat(controller, input, 10).resolution == ResolutionAction::Hold);
    }
    SECTION("steps that do not raise the real fps are undone, then held") {
        double scale = 1.0;
        u32 lowers = 0;
        Decision last;
        for (u32 i = 0; i < 12; ++i) {
            last = controller.Update(Gpu(10.0, 0.97, scale));
            if (last.resolution == ResolutionAction::Lower) {
                scale = last.scale;
                ++lowers;
            } else if (last.resolution == ResolutionAction::Raise) {
                REQUIRE(last.scale == 1.0);
                scale = last.scale;
                break;
            }
        }
        REQUIRE(lowers == 2);
        REQUIRE(scale == 1.0);
        // Held for a minute: no new run.
        REQUIRE(Repeat(controller, Gpu(10.0, 0.97, 1.0), 20).resolution == ResolutionAction::Hold);
    }
}

TEST_CASE("Autopilot gives resolution back after 10 s of headroom", "[video_core]") {
    AutopilotController controller;
    const Input headroom = Gpu(30.0, 0.5, 1.5625);
    for (u32 i = 0; i < 4; ++i) {
        REQUIRE(controller.Update(headroom).resolution == ResolutionAction::Hold);
    }
    const Decision decision = controller.Update(headroom);
    REQUIRE(decision.resolution == ResolutionAction::Raise);
    REQUIRE(decision.scale == 1.25);
    // One step at a time, each after another 10 s.
    Input next = headroom;
    next.scale = 1.25;
    for (u32 i = 0; i < 4; ++i) {
        REQUIRE(controller.Update(next).resolution == ResolutionAction::Hold);
    }
    REQUIRE(controller.Update(next).scale == 1.0);
}

TEST_CASE("Autopilot headroom run is broken by a busy window", "[video_core]") {
    AutopilotController controller;
    const Input headroom = Gpu(30.0, 0.5, 1.25);
    for (u32 i = 0; i < 4; ++i) {
        controller.Update(headroom);
    }
    controller.Update(Gpu(30.0, 0.8, 1.25));
    for (u32 i = 0; i < 4; ++i) {
        REQUIRE(controller.Update(headroom).resolution == ResolutionAction::Hold);
    }
    REQUIRE(controller.Update(headroom).resolution == ResolutionAction::Raise);
}

TEST_CASE("Autopilot never goes below the starting scale", "[video_core]") {
    AutopilotController controller;
    REQUIRE(Repeat(controller, Gpu(30.0, 0.3, 1.0), 30).resolution == ResolutionAction::Hold);
}

TEST_CASE("Autopilot power policy follows the source with hysteresis", "[video_core]") {
    AutopilotController controller;
    Input input;
    input.power_available = true;
    input.power = PowerSource::Battery;
    SECTION("applies after two windows on battery") {
        REQUIRE(controller.Update(input).power == PowerAction::None);
        REQUIRE(controller.Update(input).power == PowerAction::Apply);
    }
    SECTION("a flip resets the count") {
        REQUIRE(controller.Update(input).power == PowerAction::None);
        input.power = PowerSource::Ac;
        REQUIRE(controller.Update(input).power == PowerAction::None);
        input.power = PowerSource::Battery;
        REQUIRE(controller.Update(input).power == PowerAction::None);
        REQUIRE(controller.Update(input).power == PowerAction::Apply);
    }
    SECTION("restores on AC only what it applied") {
        input.power = PowerSource::Ac;
        REQUIRE(Repeat(controller, input, 5).power == PowerAction::None);
        input.power_applied = true;
        REQUIRE(controller.Update(input).power == PowerAction::Restore);
    }
    SECTION("does not apply twice") {
        input.power_applied = true;
        REQUIRE(Repeat(controller, input, 5).power == PowerAction::None);
    }
    SECTION("a failed apply is not retried") {
        Repeat(controller, input, 2);
        controller.NotePowerFailed();
        REQUIRE(Repeat(controller, input, 10).power == PowerAction::None);
    }
    SECTION("unavailable or unknown does nothing") {
        input.power_available = false;
        REQUIRE(Repeat(controller, input, 5).power == PowerAction::None);
        input.power_available = true;
        input.power = PowerSource::Unknown;
        REQUIRE(Repeat(controller, input, 5).power == PowerAction::None);
    }
    SECTION("power follows even without frame data") {
        input.real_fps = 0.0;
        REQUIRE(Repeat(controller, input, 2).power == PowerAction::Apply);
    }
}

TEST_CASE("Autopilot changes at most one step of each knob a window", "[video_core]") {
    AutopilotController controller;
    Input input = FrameGen(8.0);
    input.gpu_busy = 0.97;
    input.resolution_available = true;
    input.scale_limit = 4.0;
    const Decision decision = controller.Update(input);
    REQUIRE(decision.fg_target == 40);
    REQUIRE(decision.resolution == ResolutionAction::Lower);
    REQUIRE(decision.scale == 1.25);
}

TEST_CASE("Autopilot summarizes shown frame intervals", "[video_core]") {
    SECTION("nothing") {
        REQUIRE(AutopilotController::Summarize({}).samples == 0);
    }
    SECTION("an even cadence") {
        const auto summary = AutopilotController::Summarize(std::vector<double>(50, 16.7));
        REQUIRE(summary.samples == 50);
        REQUIRE(summary.p50_ms == 16.7);
        REQUIRE(summary.even_percent == 100.0);
    }
    SECTION("alternating 8 and 25 ms") {
        std::vector<double> intervals;
        for (u32 i = 0; i < 100; ++i) {
            intervals.push_back(i % 2 == 0 ? 8.0 : 25.0);
        }
        const auto summary = AutopilotController::Summarize(intervals);
        REQUIRE(summary.p95_ms == 25.0);
        REQUIRE(summary.even_percent == 50.0);
    }
}
