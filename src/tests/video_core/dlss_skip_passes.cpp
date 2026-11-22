// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/dlss_skip_passes.h"

namespace {
using namespace VideoCommon::DlssSkipPasses;
} // Anonymous namespace

TEST_CASE("EDEN_NX2_DLSS_SKIP_PASSES names its rules", "[video_core][dlss]") {
    REQUIRE(ParseRules(nullptr) == 0);
    REQUIRE(ParseRules("") == 0);
    REQUIRE(ParseRules("0") == 0);
    REQUIRE(ParseRules("drew") == RULE_DREW);
    REQUIRE(ParseRules("blocked") == RULE_BLOCKED);
    REQUIRE(ParseRules("1") == (RULE_DREW | RULE_BLOCKED));
    REQUIRE(ParseRules("both") == (RULE_DREW | RULE_BLOCKED));
    REQUIRE(ParseRules("Drew") == 0);
    REQUIRE(ParseRules("2") == 0);
}

TEST_CASE("Without the switch only a network dropped for good skips its passes",
          "[video_core][dlss]") {
    for (const bool blocked : {false, true}) {
        for (const bool drew : {false, true}) {
            REQUIRE(Skip(0, true, blocked, drew));
            REQUIRE_FALSE(Skip(0, false, blocked, drew));
        }
    }
}

TEST_CASE("Each rule skips only on its own condition", "[video_core][dlss]") {
    // drew (Lichii #864): the replacement drew and will draw again.
    REQUIRE(Skip(RULE_DREW, false, false, true));
    REQUIRE_FALSE(Skip(RULE_DREW, false, true, false));
    // blocked (Lichii #1985): the network cannot be shown, whether or not the replacement drew.
    REQUIRE(Skip(RULE_BLOCKED, false, true, false));
    REQUIRE_FALSE(Skip(RULE_BLOCKED, false, false, true));
    // Both: either condition.
    REQUIRE(Skip(RULE_DREW | RULE_BLOCKED, false, true, false));
    REQUIRE(Skip(RULE_DREW | RULE_BLOCKED, false, false, true));
    REQUIRE_FALSE(Skip(RULE_DREW | RULE_BLOCKED, false, false, false));
}
