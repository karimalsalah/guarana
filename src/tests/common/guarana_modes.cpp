// SPDX-FileCopyrightText: Copyright 2026 Guarana contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "common/guarana_modes.h"

namespace {
using namespace Common::Guarana;
}

TEST_CASE("Guarana modes round trip through their names", "[guarana_modes]") {
    for (const Mode mode : {Mode::Custom, Mode::Smooth, Mode::Quality, Mode::Battery}) {
        REQUIRE(ModeFromString(ModeToString(mode)) == mode);
    }
    REQUIRE(ModeFromString("") == Mode::Custom);
    REQUIRE(ModeFromString("turbo") == Mode::Custom);
}

TEST_CASE("Guarana modes only switch on opt-in steering", "[guarana_modes]") {
    REQUIRE(ModeSwitches(Mode::Custom).empty());
    REQUIRE(ModeSwitches(Mode::Quality).empty());
    REQUIRE(ModeSwitches(Mode::Smooth).size() == 6);
    REQUIRE(ModeSwitches(Mode::Smooth).front().first == "EDEN_NX2_FLUID");
    const auto battery = ModeSwitches(Mode::Battery);
    REQUIRE(battery.front().first == "EDEN_NX2_POWER_POLICY");
    REQUIRE(battery.front().second == "igpu");
}

TEST_CASE("Guarana block is added at the top and replaced in place", "[guarana_modes]") {
    const std::string user = "# mine\nEDEN_NX2_SPLIT=2\n";
    const auto smooth = ReplaceManagedBlock(user, ModeSwitches(Mode::Smooth));
    REQUIRE(smooth.starts_with("# >>> Guarana mode"));
    REQUIRE(smooth.ends_with(user));
    REQUIRE(smooth.find("EDEN_NX2_FLUID=1") != std::string::npos);

    const auto battery = ReplaceManagedBlock(smooth, ModeSwitches(Mode::Battery));
    REQUIRE(battery.find("EDEN_NX2_FLUID") == std::string::npos);
    REQUIRE(battery.find("EDEN_NX2_POWER_POLICY=igpu") != std::string::npos);
    REQUIRE(battery.ends_with(user));
}

TEST_CASE("Guarana block is removed without touching the rest", "[guarana_modes]") {
    const std::string user = "# mine\nEDEN_NX2_SPLIT=2\n";
    const auto with_block = ReplaceManagedBlock(user, ModeSwitches(Mode::Battery));
    const auto removed = ReplaceManagedBlock(with_block, {});
    REQUIRE(removed == user);
    REQUIRE(ReplaceManagedBlock("", {}).empty());
}
