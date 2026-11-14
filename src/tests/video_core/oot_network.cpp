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

TEST_CASE("Ocarina of Time step guard skips the network only for the replaced step",
          "[video_core][oot_network]") {
    constexpr u64 address = 0x5'0000'0000ULL;
    const Area step{0, 0, 1200, 672};

    CHECK(SkipAllowed(true, address, step, address, step));
    CHECK_FALSE(SkipAllowed(false, address, step, address, step));
    CHECK_FALSE(SkipAllowed(true, address, step, address + 0x10000, step));
    CHECK_FALSE(SkipAllowed(true, address, step, address, Area{0, 0, 1201, 672}));
    CHECK_FALSE(SkipAllowed(true, address, step, address, Area{0, 0, 1200, 671}));
    CHECK_FALSE(SkipAllowed(true, address, step, address, Area{1, 0, 1200, 672}));
    CHECK_FALSE(SkipAllowed(true, address, step, address, Area{0, 1, 1200, 672}));
}

TEST_CASE("Ocarina of Time output pass is known by its shape", "[video_core][oot_network]") {
    // Issue #98: the network's 1195x672 image read, the 1920x1080 output stored.
    const OutputShapeInput issue{
        .scene_e5b9g9r9 = true,
        .output_b10g11r11 = true,
        .scene_address = 0x5'0000'0000ULL,
        .output_address = 0x5'1000'0000ULL,
        .scene_w = 1195,
        .scene_h = 672,
        .output_w = 1920,
        .output_h = 1080,
    };
    CHECK(IsOutputShape(issue));

    // No smaller is enough: an output the size of the image it reads has the shape too.
    OutputShapeInput same_size = issue;
    same_size.output_w = issue.scene_w;
    same_size.output_h = issue.scene_h;
    CHECK(IsOutputShape(same_size));

    OutputShapeInput same_address = issue;
    same_address.output_address = issue.scene_address;
    CHECK_FALSE(IsOutputShape(same_address));

    OutputShapeInput no_output = issue;
    no_output.output_address = 0;
    CHECK_FALSE(IsOutputShape(no_output));

    OutputShapeInput narrower = issue;
    narrower.output_w = 1194;
    CHECK_FALSE(IsOutputShape(narrower));

    OutputShapeInput shorter = issue;
    shorter.output_h = 671;
    CHECK_FALSE(IsOutputShape(shorter));

    OutputShapeInput scene_format = issue;
    scene_format.scene_e5b9g9r9 = false;
    CHECK_FALSE(IsOutputShape(scene_format));

    OutputShapeInput output_format = issue;
    output_format.output_b10g11r11 = false;
    CHECK_FALSE(IsOutputShape(output_format));
}

TEST_CASE("Ocarina of Time output pass reads what the tonemap drew", "[video_core][oot_network]") {
    const Area drawn{0, 0, 1200, 672};
    CHECK(MatchesDrawnArea(1195, 672, drawn));
    CHECK(MatchesDrawnArea(1200, 672, drawn));
    CHECK_FALSE(MatchesDrawnArea(960, 540, drawn));
    CHECK_FALSE(MatchesDrawnArea(1195, 540, drawn));
    CHECK_FALSE(MatchesDrawnArea(0, 0, drawn));
    CHECK_FALSE(MatchesDrawnArea(1195, 672, Area{}));
    // 1/32 of 1200 is 37.5 texels: 37 off in either direction matches, 38 does not.
    CHECK(MatchesDrawnArea(1163, 672, drawn));
    CHECK(MatchesDrawnArea(1237, 672, drawn));
    CHECK_FALSE(MatchesDrawnArea(1162, 672, drawn));
    CHECK_FALSE(MatchesDrawnArea(1238, 672, drawn));
    // The drawn area's corner does not matter, only its size.
    CHECK(MatchesDrawnArea(1195, 672, Area{8, 4, 1208, 676}));
}
