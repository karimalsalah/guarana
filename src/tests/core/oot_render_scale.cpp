// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <optional>

#include <catch2/catch_test_macros.hpp>

#include "common/settings.h"
#include "core/file_sys/oot_render_scale.h"

using namespace FileSys::OotRenderScale;
using Quality = Settings::NvidiaDlssQuality;

TEST_CASE("EDEN_NX2_OOT_RENDER_SCALE is parsed as a number", "[patch_manager][oot_render_scale]") {
    CHECK(ParseRequest(nullptr) == Request::Off);
    CHECK(ParseRequest("") == Request::Off);
    CHECK(ParseRequest("0") == Request::Off);
    CHECK(ParseRequest("false") == Request::Off);
    CHECK(ParseRequest("75") == Request::Percent75);
    CHECK(ParseRequest("50") == Request::Percent50);
    CHECK(ParseRequest("1") == Request::Invalid);
    CHECK(ParseRequest("74") == Request::Invalid);
    CHECK(ParseRequest("100") == Request::Invalid);
    CHECK(ParseRequest("true") == Request::Off);
}

TEST_CASE("EDEN_NX2_OOT_RENDER_SCALE names the record of its scale",
          "[patch_manager][oot_render_scale]") {
    STATIC_REQUIRE(Percent(Request::Percent75) == 75);
    STATIC_REQUIRE(Percent(Request::Percent50) == 50);
    STATIC_REQUIRE(Percent(Request::Off) == 100);
    STATIC_REQUIRE(Percent(Request::Invalid) == 100);
    STATIC_REQUIRE(RecordQuality(Request::Percent75) == Quality::Performance);
    STATIC_REQUIRE(RecordQuality(Request::Percent50) == Quality::UltraPerformance);
    STATIC_REQUIRE_FALSE(RecordQuality(Request::Off).has_value());
    STATIC_REQUIRE_FALSE(RecordQuality(Request::Invalid).has_value());
}

TEST_CASE("EDEN_NX2_OOT_RENDER_SCALE applies one record, and only next to Max Resolution",
          "[patch_manager][oot_render_scale]") {
    STATIC_REQUIRE(WantedBySwitch(Request::Percent75, Quality::Performance, true, false));
    STATIC_REQUIRE(WantedBySwitch(Request::Percent50, Quality::UltraPerformance, true, false));
    // The other records stay off.
    STATIC_REQUIRE_FALSE(WantedBySwitch(Request::Percent75, Quality::Balanced, true, false));
    STATIC_REQUIRE_FALSE(WantedBySwitch(Request::Percent75, Quality::UltraPerformance, true, false));
    STATIC_REQUIRE_FALSE(WantedBySwitch(Request::Percent50, Quality::Performance, true, false));
    // No Max Resolution patch applied, or DLSS already choosing the scale.
    STATIC_REQUIRE_FALSE(WantedBySwitch(Request::Percent75, Quality::Performance, false, false));
    STATIC_REQUIRE_FALSE(WantedBySwitch(Request::Percent75, Quality::Performance, true, true));
    // Off and invalid requests apply nothing.
    for (const auto quality : {Quality::Balanced, Quality::Performance, Quality::UltraPerformance}) {
        CHECK_FALSE(WantedBySwitch(Request::Off, quality, true, false));
        CHECK_FALSE(WantedBySwitch(Request::Invalid, quality, true, false));
    }
}
