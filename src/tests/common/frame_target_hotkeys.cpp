// SPDX-FileCopyrightText: Copyright 2026 Guarana contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <initializer_list>
#include <atomic>
#include <thread>

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

TEST_CASE("Frame target hotkeys: session override and disabled fallback", "[frame_target_hotkeys]") {
    TargetOverride target;
    u32 configured = 120;
    unsigned reads = 0;
    const auto read_configured = [&] {
        ++reads;
        return configured;
    };

    REQUIRE(target.Resolve(true, read_configured) == 120U);
    target.Set(40);
    reads = 0;
    REQUIRE(target.Resolve(true, read_configured) == 40U);
    REQUIRE(reads == 0U);
    REQUIRE(configured == 120U);
    REQUIRE(target.Resolve(false, read_configured) == 120U);
    REQUIRE(reads == 1U);

    configured = 0;
    REQUIRE(target.Resolve(false, read_configured) == 0U);
    target.Reset();
    REQUIRE(target.Resolve(true, read_configured) == 0U);
    target.Set(NextPreset(target.Resolve(true, read_configured)));
    REQUIRE(target.Resolve(true, read_configured) == 30U);
    target.Reset();
    configured = 90;
    REQUIRE(target.Resolve(true, read_configured) == 90U);
}

TEST_CASE("Frame target hotkeys: concurrent publication", "[frame_target_hotkeys]") {
    TargetOverride target;
    std::atomic<bool> start{false};
    std::atomic<bool> invalid{false};
    const auto read = [&] {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        for (unsigned i = 0; i < 10000; ++i) {
            const u32 value = target.Resolve(true, [] { return 120U; });
            if (value != 30 && value != 40 && value != 60 && value != 120) {
                invalid.store(true, std::memory_order_relaxed);
            }
        }
    };
    std::jthread first(read);
    std::jthread second(read);
    start.store(true, std::memory_order_release);
    for (unsigned i = 0; i < 10000; ++i) {
        target.Set(Presets[i % Presets.size()]);
        if (i % 4 == 0) {
            target.Reset();
        }
    }
    first.join();
    second.join();
    REQUIRE_FALSE(invalid.load(std::memory_order_relaxed));
}
