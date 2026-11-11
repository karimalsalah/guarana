// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "common/settings.h"
#include "video_core/oot_network.h"

using namespace VideoCommon::OotNetwork;

TEST_CASE("Ocarina of Time tonemap shaders are recognized", "[video_core][oot_network]") {
    STATIC_REQUIRE(IsTonemapShader(0xbded5ce917183214ULL));
    STATIC_REQUIRE(IsTonemapShader(0x6cb06edf14be1d79ULL));
    STATIC_REQUIRE(IsTonemapShader(0x3ac011bd2d772994ULL));
    STATIC_REQUIRE_FALSE(IsTonemapShader(0));
    STATIC_REQUIRE_FALSE(IsTonemapShader(PrepassProgramKey));
}

TEST_CASE("Ocarina of Time output passes of the dynamic resolution steps are recognized",
          "[video_core][oot_network]") {
    STATIC_REQUIRE(IsOutputShader(0xe40f5efb5480e692ULL));
    STATIC_REQUIRE(IsOutputShader(0x9302bc0d6d1d9bc1ULL));
    STATIC_REQUIRE(IsOutputShader(0x03bf0ffc7d2d0bedULL));
    STATIC_REQUIRE_FALSE(IsOutputShader(0));
    STATIC_REQUIRE_FALSE(IsOutputShader(NetworkProgramKey));
}

TEST_CASE("Ocarina of Time network bypass follows the setting and the game",
          "[video_core][oot_network]") {
    auto& setting = Settings::values.oot_anti_aliasing;
    const auto previous = setting.GetValue();

    setting.SetValue(Settings::OotAntiAliasing::Game);
    CHECK_FALSE(BypassEnabled(ProgramId));

    setting.SetValue(Settings::OotAntiAliasing::NoSharpening);
    CHECK(BypassEnabled(ProgramId));
    CHECK_FALSE(BypassEnabled(0x0400C3F00006E000ULL));

    setting.SetValue(previous);
}
