// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>

#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "video_core/renderer_vulkan/present/fsr3_frame_gen.h"

using Vulkan::Fsr3PingPong;

TEST_CASE("FSR3 ping-pong: the two flow sets read the images in opposite roles",
          "[video_core]") {
    STATIC_REQUIRE(Fsr3PingPong::SetRoles(0).current == 0);
    STATIC_REQUIRE(Fsr3PingPong::SetRoles(0).history == 1);
    STATIC_REQUIRE(Fsr3PingPong::SetRoles(1).current == 1);
    STATIC_REQUIRE(Fsr3PingPong::SetRoles(1).history == 0);
    STATIC_REQUIRE(Fsr3PingPong{}.FlowSet() == 0);
    STATIC_REQUIRE(Fsr3PingPong{}.Now().current == 0);
}

TEST_CASE("FSR3 ping-pong: current alternates A, B, A and the bound set matches it",
          "[video_core]") {
    Fsr3PingPong roles;
    for (u32 advance = 1; advance <= 7; ++advance) {
        roles.Advance();
        const u32 expected_current = advance % 2;
        REQUIRE(roles.Now().current == expected_current);
        REQUIRE(roles.Now().history == 1 - expected_current);
        REQUIRE(roles.FlowSet() == expected_current);
        const Fsr3PingPong::Roles bound = Fsr3PingPong::SetRoles(roles.FlowSet());
        REQUIRE(bound.current == roles.Now().current);
        REQUIRE(bound.history == roles.Now().history);
    }
}

TEST_CASE("FSR3 ping-pong: the flow set reads the newest frame as current, the one before as "
          "history",
          "[video_core]") {
    // What each image holds, as frame numbers. The first frame fills both, as CopyImageToTwo does.
    std::array<u64, 2> images{0, 0};
    Fsr3PingPong roles;
    for (u64 frame = 1; frame <= 9; ++frame) {
        // Process: swap roles, then copy the new frame into the image now current.
        roles.Advance();
        images[roles.Now().current] = frame;

        const Fsr3PingPong::Roles bound = Fsr3PingPong::SetRoles(roles.FlowSet());
        REQUIRE(images[bound.current] == frame);
        REQUIRE(images[bound.history] == frame - 1);
    }
}
