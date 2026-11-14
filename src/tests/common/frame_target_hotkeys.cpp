// SPDX-FileCopyrightText: Copyright 2026 Guarana contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <initializer_list>

#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "common/frame_target_hotkeys.h"

using namespace Common::FrameTargetHotkeys;

TEST_CASE("Frame target hotkeys: the presets", "[frame_target_hotkeys]") {
    REQUIRE(Presets.size() == 3U);
    REQUIRE(Presets[0] == 30U);
    REQUIRE(Presets[1] == 40U);
    REQUIRE(Presets[2] == 60U);
}

TEST_CASE("Frame target hotkeys: the cycle", "[frame_target_hotkeys]") {
    REQUIRE(NextPreset(0) == 30U);
    REQUIRE(NextPreset(20) == 30U);
    REQUIRE(NextPreset(30) == 40U);
    REQUIRE(NextPreset(35) == 40U);
    REQUIRE(NextPreset(40) == 60U);
    REQUIRE(NextPreset(59) == 60U);
    REQUIRE(NextPreset(60) == 30U);
    REQUIRE(NextPreset(61) == 30U);
    REQUIRE(NextPreset(240) == 30U);

    u32 target = 0;
    for (const u32 expected : {30U, 40U, 60U, 30U, 40U}) {
        target = NextPreset(target);
        REQUIRE(target == expected);
    }
}

TEST_CASE("Frame target hotkeys: the switch value", "[frame_target_hotkeys]") {
    REQUIRE_FALSE(EnabledFromValue(nullptr));
    REQUIRE_FALSE(EnabledFromValue(""));
    REQUIRE_FALSE(EnabledFromValue("0"));
    REQUIRE_FALSE(EnabledFromValue("false"));
    REQUIRE(EnabledFromValue("1"));
    REQUIRE(EnabledFromValue("2"));
    REQUIRE(EnabledFromValue(" 1"));
    REQUIRE(EnabledFromValue("01"));
}

TEST_CASE("Frame target hotkeys: the hotkey names", "[frame_target_hotkeys]") {
    REQUIRE(IsHotkeyName("Frame target 30"));
    REQUIRE(IsHotkeyName("Frame target 40"));
    REQUIRE(IsHotkeyName("Frame target 60"));
    REQUIRE(IsHotkeyName("Cycle frame target"));
    REQUIRE_FALSE(IsHotkeyName("Toggle Framerate Limit"));
    REQUIRE_FALSE(IsHotkeyName(""));
    REQUIRE_FALSE(IsHotkeyName("frame target 30"));
}
