// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstddef>
#include <cmath>

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/fluid_compose_math.h"

namespace FCM = Vulkan::FluidComposeMath;

TEST_CASE("EDEN_NX2_FLUID_COMPOSE is read as EDEN_NX2_FLUID is", "[video_core][fluid_compose]") {
    REQUIRE(FCM::ModeOf(nullptr) == 0U);
    REQUIRE(FCM::ModeOf("") == 0U);
    REQUIRE(FCM::ModeOf("0") == 0U);
    REQUIRE(FCM::ModeOf("00") == 0U);
    REQUIRE(FCM::ModeOf("false") == 0U);
    REQUIRE(FCM::ModeOf("x") == 0U);
    REQUIRE(FCM::ModeOf("1") == 1U);
    REQUIRE(FCM::ModeOf("01") == 1U);
    REQUIRE(FCM::ModeOf("2") == 1U);
    REQUIRE(FCM::ModeOf("7") == 1U);
}

TEST_CASE("The internal extent follows the guest's peak and rounds up to even",
          "[video_core][fluid_compose]") {
    constexpr FCM::Extent none{};
    STATIC_REQUIRE(FCM::InternalExtent({1920, 1080}, none).width == 1920);
    STATIC_REQUIRE(FCM::InternalExtent({1920, 1080}, none).height == 1080);
    // Odd sizes round up.
    STATIC_REQUIRE(FCM::InternalExtent({1281, 721}, none).width == 1282);
    STATIC_REQUIRE(FCM::InternalExtent({1281, 721}, none).height == 722);
    // Never smaller than before, on each axis separately.
    constexpr FCM::Extent peak = FCM::InternalExtent({1920, 1080}, none);
    STATIC_REQUIRE(FCM::InternalExtent({1280, 720}, peak).width == 1920);
    STATIC_REQUIRE(FCM::InternalExtent({1280, 720}, peak).height == 1080);
    STATIC_REQUIRE(FCM::InternalExtent({2560, 720}, peak).width == 2560);
    STATIC_REQUIRE(FCM::InternalExtent({2560, 720}, peak).height == 1080);
    // A guest without an area changes nothing.
    STATIC_REQUIRE(FCM::InternalExtent({0, 0}, peak).width == 1920);
    STATIC_REQUIRE(FCM::InternalExtent({1920, 0}, peak).height == 1080);
    STATIC_REQUIRE(FCM::InternalExtent({0, 0}, none).width == 0);
    // A size past the limit is capped.
    STATIC_REQUIRE(FCM::InternalExtent({100000, 100000}, none).width == FCM::MaxExtent);
    STATIC_REQUIRE(FCM::InternalExtent({100000, 100000}, none).height == FCM::MaxExtent);
}

TEST_CASE("The screen rectangle is clamped to the output", "[video_core][fluid_compose]") {
    // A 16:9 screen letterboxed in a 2880x1800 window.
    constexpr FCM::Rect letterbox = FCM::ScreenRect(2880, 1800, 0, 90, 2880, 1710);
    STATIC_REQUIRE(letterbox.x == 0);
    STATIC_REQUIRE(letterbox.y == 90);
    STATIC_REQUIRE(letterbox.width == 2880);
    STATIC_REQUIRE(letterbox.height == 1620);
    // Past the output: clamped.
    constexpr FCM::Rect past = FCM::ScreenRect(100, 100, 50, 50, 500, 500);
    STATIC_REQUIRE(past.x == 50);
    STATIC_REQUIRE(past.width == 50);
    STATIC_REQUIRE(past.height == 50);
    // Reversed edges and an origin outside the output are empty.
    STATIC_REQUIRE(FCM::ScreenRect(100, 100, 60, 10, 40, 90).width == 0);
    STATIC_REQUIRE(FCM::ScreenRect(100, 100, 10, 60, 90, 40).height == 0);
    STATIC_REQUIRE(FCM::ScreenRect(100, 100, 200, 200, 300, 300).width == 0);
    STATIC_REQUIRE(FCM::ScreenRect(100, 100, 200, 200, 300, 300).height == 0);
    // Odd sizes are kept as they are.
    STATIC_REQUIRE(FCM::ScreenRect(1001, 563, 0, 0, 1001, 563).width == 1001);
}

TEST_CASE("The slot pass is worthwhile from three quarters of the frame on each side",
          "[video_core][fluid_compose]") {
    constexpr FCM::Extent internal{1920, 1080};
    // Upscaling, 1:1 and a window a little smaller.
    STATIC_REQUIRE(FCM::Worthwhile({0, 90, 2880, 1620}, internal));
    STATIC_REQUIRE(FCM::Worthwhile({0, 0, 1920, 1080}, internal));
    // The boundary: 1440x810 is exactly three quarters.
    STATIC_REQUIRE(FCM::Worthwhile({0, 0, 1440, 810}, internal));
    STATIC_REQUIRE_FALSE(FCM::Worthwhile({0, 0, 1439, 810}, internal));
    STATIC_REQUIRE_FALSE(FCM::Worthwhile({0, 0, 1440, 809}, internal));
    // Either side alone is enough to refuse.
    STATIC_REQUIRE_FALSE(FCM::Worthwhile({0, 0, 3840, 600}, internal));
    STATIC_REQUIRE_FALSE(FCM::Worthwhile({0, 0, 600, 2160}, internal));
    // Nothing to scale.
    STATIC_REQUIRE_FALSE(FCM::Worthwhile({0, 0, 0, 0}, internal));
    STATIC_REQUIRE_FALSE(FCM::Worthwhile({0, 0, 1920, 1080}, {0, 0}));
    // A large internal frame in a small window is left to the classic path, which filters it.
    STATIC_REQUIRE_FALSE(FCM::Worthwhile({0, 0, 1280, 720}, {3840, 2160}));
}

TEST_CASE("The push constants match the shader's block", "[video_core][fluid_compose]") {
    constexpr FCM::PushConstants constants =
        FCM::MakePushConstants(2880, 1800, FCM::Rect{0, 90, 2880, 1620});
    STATIC_REQUIRE(constants.out_width == 2880);
    STATIC_REQUIRE(constants.out_height == 1800);
    STATIC_REQUIRE(constants.rect_x == 0);
    STATIC_REQUIRE(constants.rect_y == 90);
    STATIC_REQUIRE(constants.rect_width == 2880);
    STATIC_REQUIRE(constants.rect_height == 1620);
    STATIC_REQUIRE(constants.filter_mode == 0);
    // The layout is also pinned by static_asserts in the header: seven 32-bit scalars.
    STATIC_REQUIRE(sizeof(FCM::PushConstants) == 7 * sizeof(s32));
}

TEST_CASE("Work groups cover the output", "[video_core][fluid_compose]") {
    STATIC_REQUIRE(FCM::GroupCount(0) == 0);
    STATIC_REQUIRE(FCM::GroupCount(1) == 1);
    STATIC_REQUIRE(FCM::GroupCount(16) == 1);
    STATIC_REQUIRE(FCM::GroupCount(17) == 2);
    STATIC_REQUIRE(FCM::GroupCount(2880) == 180);
    STATIC_REQUIRE(FCM::GroupCount(1800) == 113);
    for (u32 extent = 1; extent < 200; ++extent) {
        const u32 groups = FCM::GroupCount(extent);
        REQUIRE(groups * FCM::GroupSize >= extent);
        REQUIRE((groups - 1) * FCM::GroupSize < extent);
    }
}

TEST_CASE("The source is sampled at the centre of each rectangle pixel",
          "[video_core][fluid_compose]") {
    constexpr FCM::Rect rect{0, 90, 2880, 1620};
    // The first pixel of the rectangle is half a source texel inside the corner.
    const FCM::Uv first = FCM::SourceUv(0, 90, rect);
    REQUIRE(first.inside);
    REQUIRE(std::abs(first.u - 0.5f / 2880.0f) < 1.0e-7f);
    REQUIRE(std::abs(first.v - 0.5f / 1620.0f) < 1.0e-7f);
    // So is the last, from the opposite corner.
    const FCM::Uv last = FCM::SourceUv(2879, 90 + 1619, rect);
    REQUIRE(last.inside);
    REQUIRE(std::abs(last.u - (1.0f - 0.5f / 2880.0f)) < 1.0e-6f);
    REQUIRE(std::abs(last.v - (1.0f - 0.5f / 1620.0f)) < 1.0e-6f);
    // One pixel outside on each side is a bar.
    REQUIRE_FALSE(FCM::SourceUv(0, 89, rect).inside);
    REQUIRE_FALSE(FCM::SourceUv(0, 1710, rect).inside);
    REQUIRE_FALSE(FCM::SourceUv(-1, 90, rect).inside);
    REQUIRE_FALSE(FCM::SourceUv(2880, 90, rect).inside);
    // 1:1: the pixel's centre is a source texel's centre.
    constexpr FCM::Rect same{0, 0, 1920, 1080};
    const FCM::Uv texel = FCM::SourceUv(960, 540, same);
    REQUIRE(std::abs(texel.u * 1920.0f - 960.5f) < 1.0e-3f);
    REQUIRE(std::abs(texel.v * 1080.0f - 540.5f) < 1.0e-3f);
    // An empty rectangle has no pixel inside.
    REQUIRE_FALSE(FCM::SourceUv(0, 0, FCM::Rect{}).inside);
}

TEST_CASE("Frame generation fits the slot pass when its frames take the slots",
          "[video_core][fluid_compose]") {
    STATIC_REQUIRE(FCM::FrameGenerationFits(true, true));
    STATIC_REQUIRE(FCM::FrameGenerationFits(false, false));
    STATIC_REQUIRE(FCM::FrameGenerationFits(true, false));
    STATIC_REQUIRE_FALSE(FCM::FrameGenerationFits(false, true));
}

TEST_CASE("Each fallback returns the composite to the classic path", "[video_core][fluid_compose]") {
    constexpr FCM::Gate ready{
        .available = true,
        .hdr = false,
        .extrapolation = false,
        .streamline = false,
        .frame_generation_fits = true,
        .pictures_armed = false,
        .worthwhile = true,
    };
    STATIC_REQUIRE(FCM::Active(ready));
    STATIC_REQUIRE_FALSE(FCM::Active(FCM::Gate{}));

    FCM::Gate gate = ready;
    gate.available = false; // switch off, no FLUID, no storage swapchain, no compute queue
    REQUIRE_FALSE(FCM::Active(gate));
    gate = ready;
    gate.hdr = true;
    REQUIRE_FALSE(FCM::Active(gate));
    gate = ready;
    gate.extrapolation = true;
    REQUIRE_FALSE(FCM::Active(gate));
    gate = ready;
    gate.streamline = true;
    REQUIRE_FALSE(FCM::Active(gate));
    gate = ready;
    gate.frame_generation_fits = false;
    REQUIRE_FALSE(FCM::Active(gate));
    gate = ready;
    gate.pictures_armed = true;
    REQUIRE_FALSE(FCM::Active(gate));
    gate = ready;
    gate.worthwhile = false;
    REQUIRE_FALSE(FCM::Active(gate));
}

TEST_CASE("Compose filter selection defaults to bilinear", "[video_core][fluid_compose]") {
    REQUIRE(FCM::FilterModeOf(nullptr) == 0U);
    REQUIRE(FCM::FilterModeOf("") == 0U);
    REQUIRE(FCM::FilterModeOf("false") == 0U);
    REQUIRE(FCM::FilterModeOf("0") == 0U);
    REQUIRE(FCM::FilterModeOf("1") == 1U);
    REQUIRE(FCM::FilterModeOf("2") == 2U);
    REQUIRE(FCM::FilterModeOf("7") == 2U);
}

TEST_CASE("Catmull-Rom five taps preserve constants and integer texels",
          "[video_core][fluid_compose]") {
    for (int x = 0; x <= 100; ++x) {
        for (int y = 0; y <= 100; ++y) {
            const f32 fx = static_cast<f32>(x) / 100.0f;
            const f32 fy = static_cast<f32>(y) / 100.0f;
            const auto cubic = FCM::CatmullRomWeights(fx);
            REQUIRE(std::abs(cubic[0] + cubic[1] + cubic[2] + cubic[3] - 1.0f) < 1.0e-6f);
            const auto axis = FCM::CatmullRomAxis(fx);
            REQUIRE(axis.middle_offset >= 0.0f);
            REQUIRE(axis.middle_offset <= 1.0f);
            const auto weights = FCM::CatmullRomFiveWeights(fx, fy);
            f32 sum = 0.0f;
            for (const f32 weight : weights) {
                sum += weight;
            }
            REQUIRE(std::abs(sum - 1.0f) < 1.0e-6f);
            REQUIRE(std::abs(FCM::CatmullRomFiveSample({0.3f, 0.3f, 0.3f, 0.3f, 0.3f},
                                                     fx, fy) - 0.3f) < 1.0e-6f);
            const f32 clamped = FCM::CatmullRomFiveSample({0.1f, 0.9f, 0.2f, 0.8f, 0.5f}, fx, fy);
            REQUIRE(clamped >= 0.1f);
            REQUIRE(clamped <= 0.9f);
        }
    }
    const auto integer = FCM::CatmullRomFiveWeights(0.0f, 0.0f);
    constexpr std::array<f32, 5> expected{0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    REQUIRE(integer == expected);
    REQUIRE(FCM::CatmullRomAxis(0.0f).middle_offset == 0.0f);
    REQUIRE(FCM::CatmullRomFiveSample({0.0f, 1.0f, 0.4f, 0.0f, 1.0f}, 0.0f, 0.0f) == 0.4f);
    // Negative outer weights would overshoot an isolated bright or dark centre.
    REQUIRE(FCM::CatmullRomFiveSample({0.0f, 0.0f, 1.0f, 0.0f, 0.0f}, 0.5f, 0.5f) == 1.0f);
    REQUIRE(FCM::CatmullRomFiveSample({1.0f, 1.0f, 0.0f, 1.0f, 1.0f}, 0.5f, 0.5f) == 0.0f);
}

TEST_CASE("Catmull-Rom tap positions reconstruct a clamped synthetic image",
          "[video_core][fluid_compose]") {
    std::array<std::array<f32, 8>, 8> image{};
    for (std::size_t y = 0; y < image.size(); ++y) {
        for (std::size_t x = 0; x < image[y].size(); ++x) {
            image[y][x] = static_cast<f32>(x) + 0.25f * static_cast<f32>(y);
        }
    }
    const auto bilinear = [&image](FCM::CubicTap tap) {
        const f32 x = tap.x - 0.5f;
        const f32 y = tap.y - 0.5f;
        const f32 base_x = std::floor(x);
        const f32 base_y = std::floor(y);
        const f32 fx = x - base_x;
        const f32 fy = y - base_y;
        const auto texel = [&image](s32 px, s32 py) {
            return image[static_cast<std::size_t>(std::clamp(py, 0, 7))]
                        [static_cast<std::size_t>(std::clamp(px, 0, 7))];
        };
        const s32 ix = static_cast<s32>(base_x);
        const s32 iy = static_cast<s32>(base_y);
        return (1.0f - fy) * ((1.0f - fx) * texel(ix, iy) + fx * texel(ix + 1, iy)) +
               fy * ((1.0f - fx) * texel(ix, iy + 1) + fx * texel(ix + 1, iy + 1));
    };
    const auto reconstruct = [&bilinear](f32 x, f32 y) {
        const auto positions = FCM::CatmullRomFivePositions(x, y);
        std::array<f32, 5> taps{};
        for (std::size_t i = 0; i < taps.size(); ++i) {
            taps[i] = bilinear(positions[i]);
        }
        return FCM::CatmullRomFiveSample(taps, x - std::floor(x), y - std::floor(y));
    };
    for (std::size_t y = 0; y < image.size(); ++y) {
        for (std::size_t x = 0; x < image[y].size(); ++x) {
            REQUIRE(reconstruct(static_cast<f32>(x), static_cast<f32>(y)) == image[y][x]);
        }
    }
    // Interior ramps tolerate the small bias from dropping the four corner taps.
    for (int x = 0; x <= 100; ++x) {
        for (int y = 0; y <= 100; ++y) {
            const f32 px = 2.0f + static_cast<f32>(x) / 100.0f;
            const f32 py = 3.0f + static_cast<f32>(y) / 100.0f;
            REQUIRE(std::abs(reconstruct(px, py) - (px + 0.25f * py)) < 0.01f);
        }
    }
    // Negative positions use floor, and the sampler clamps beyond all four edges.
    constexpr auto edge = FCM::CatmullRomFivePositions(-0.5f, -0.5f);
    STATIC_REQUIRE(edge[0].x == -1.5f);
    REQUIRE(bilinear({-1.5f, -1.5f}) == image[0][0]);
    REQUIRE(bilinear({10.5f, 10.5f}) == image[7][7]);
}

TEST_CASE("Contrast-adaptive sharpen strengthens detail without clipping",
          "[video_core][fluid_compose]") {
    REQUIRE(FCM::ContrastAdaptiveSharpen(0.6f, {0.4f, 0.4f, 0.4f, 0.4f}) > 0.6f);
    REQUIRE(FCM::ContrastAdaptiveSharpen(0.4f, {0.6f, 0.6f, 0.6f, 0.6f}) < 0.4f);
    for (int c = 0; c <= 100; ++c) {
        const f32 centre = static_cast<f32>(c) / 100.0f;
        REQUIRE(std::abs(FCM::ContrastAdaptiveSharpen(centre, {centre, centre, centre, centre}) -
                         centre) < 1.0e-6f);
        for (int n = 0; n <= 100; ++n) {
            const f32 neighbour = static_cast<f32>(n) / 100.0f;
            const f32 result = FCM::ContrastAdaptiveSharpen(
                centre, {neighbour, neighbour, neighbour, neighbour});
            REQUIRE(result >= 0.0f);
            REQUIRE(result <= 1.0f);
        }
    }
}
