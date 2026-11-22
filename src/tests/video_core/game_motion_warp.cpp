// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/present/game_motion_warp.h"

namespace {

using namespace Vulkan::GameMotionWarp;

// A 100 x 4 texel guide over a 100 x 4 pixel scene, sampled at the nearest texel and clamped at
// the edges, as the shader's sampler does. Motion is in UV: 0.1 is ten pixels.
constexpr int WIDTH = 100;
constexpr int HEIGHT = 4;
constexpr Vec2 SCREEN{static_cast<float>(WIDTH), static_cast<float>(HEIGHT)};
constexpr float BACKGROUND = 0.2f;
constexpr float FOREGROUND = 0.8f;
constexpr float TENTH_PIXEL = 0.1f / static_cast<float>(WIDTH);

struct Field {
    std::vector<GuideTexel> texels = std::vector<GuideTexel>(WIDTH * HEIGHT);

    GuideTexel operator()(Vec2 uv) const {
        const int x = std::clamp(static_cast<int>(std::floor(uv.x * WIDTH)), 0, WIDTH - 1);
        const int y = std::clamp(static_cast<int>(std::floor(uv.y * HEIGHT)), 0, HEIGHT - 1);
        return texels[static_cast<size_t>(y * WIDTH + x)];
    }

    /// Every texel whose centre's x lies in [from, to) gets `texel`.
    void Fill(float from, float to, GuideTexel texel) {
        for (int y = 0; y < HEIGHT; ++y) {
            for (int x = 0; x < WIDTH; ++x) {
                const float u = (static_cast<float>(x) + 0.5f) / WIDTH;
                if (u >= from && u < to) {
                    texels[static_cast<size_t>(y * WIDTH + x)] = texel;
                }
            }
        }
    }
};

Vec2 At(float u) {
    return {u, 0.5f};
}

bool Near(Vec2 a, Vec2 b) {
    return std::fabs(a.x - b.x) <= TENTH_PIXEL && std::fabs(a.y - b.y) <= TENTH_PIXEL;
}

// A square of the foreground (nearness FOREGROUND) moving ten pixels to the right per real frame
// over a still background: it covers [0.4, 0.5) in the previous frame and [0.5, 0.6) in the
// newest. Both guides say it came from ten pixels to the left (constant velocity).
struct MovingSquare {
    Field newest;
    Field previous;

    MovingSquare() {
        newest.Fill(0.0f, 1.0f, {{0.0f, 0.0f}, BACKGROUND});
        previous.Fill(0.0f, 1.0f, {{0.0f, 0.0f}, BACKGROUND});
        newest.Fill(0.5f, 0.6f, {{-0.1f, 0.0f}, FOREGROUND});
        previous.Fill(0.4f, 0.5f, {{-0.1f, 0.0f}, FOREGROUND});
    }
};

} // Anonymous namespace

TEST_CASE("A pan moves every pixel along the game's motion", "[video_core][frame_gen]") {
    Field guide;
    guide.Fill(0.0f, 1.0f, {{-0.1f, 0.0f}, 0.5f});
    for (const float t : {0.0f, 0.25f, 0.5f, 2.0f / 3.0f, 1.0f}) {
        for (const float u : {0.3f, 0.5f, 0.7f}) {
            const Pick pick = Interpolate(guide, guide, At(u), t, SCREEN);
            INFO("t " << t << ", u " << u);
            // Ten pixels in one interval: the newest frame's point is (1 - t) of them to the
            // right, and the same point in the previous frame is where q is moved back by t.
            REQUIRE(Near(pick.newest, At(u + 0.1f * (1.0f - t))));
            REQUIRE(Near(pick.previous, At(u - 0.1f * t)));
            REQUIRE(pick.newest_weight == t);
        }
    }
}

TEST_CASE("The ends of the interval are the two real frames", "[video_core][frame_gen]") {
    const MovingSquare scene;
    for (const float u : {0.1f, 0.45f, 0.55f, 0.65f}) {
        const Pick last = Interpolate(scene.newest, scene.previous, At(u), 1.0f, SCREEN);
        INFO("u " << u);
        REQUIRE(Near(last.newest, At(u)));
        REQUIRE(last.newest_weight == 1.0f);
    }
    // At t = 0 the background the square left in the newest frame is the previous frame's square.
    const Pick first = Interpolate(scene.newest, scene.previous, At(0.45f), 0.0f, SCREEN);
    REQUIRE(first.newest_weight == 0.0f);
    REQUIRE(Near(first.previous, At(0.45f)));
}

TEST_CASE("A foreground over a moving background fills what each frame saw",
          "[video_core][frame_gen]") {
    const MovingSquare scene;
    const auto pick = [&](float u) {
        return Interpolate(scene.newest, scene.previous, At(u), 0.5f, SCREEN);
    };
    // Halfway, the square covers [0.45, 0.55).
    SECTION("inside the square, both frames saw it: mixed along its trajectory") {
        for (const float u : {0.5f, 0.54f}) {
            const Pick inside = pick(u);
            INFO("u " << u);
            REQUIRE(Near(inside.newest, At(u + 0.05f)));
            REQUIRE(Near(inside.previous, At(u - 0.05f)));
            REQUIRE(inside.newest_weight == 0.5f);
        }
    }
    SECTION("the square's leading edge comes from the previous frame, where it was") {
        const Pick leading = pick(0.46f);
        REQUIRE(leading.newest_weight == 0.0f);
        REQUIRE(Near(leading.previous, At(0.41f)));
        REQUIRE(scene.previous(leading.previous).nearness == FOREGROUND);
    }
    SECTION("background the square has left: the newest frame alone, never the square") {
        const Pick left = pick(0.42f);
        REQUIRE(left.newest_weight == 1.0f);
        REQUIRE(Near(left.newest, At(0.42f)));
        REQUIRE(scene.newest(left.newest).nearness == BACKGROUND);
    }
    SECTION("background the square covers in the newest frame: the previous frame saw it") {
        for (const float u : {0.56f, 0.58f}) {
            const Pick revealed = pick(u);
            INFO("u " << u);
            REQUIRE(revealed.newest_weight == 0.0f);
            REQUIRE(Near(revealed.previous, At(u)));
            REQUIRE(scene.previous(revealed.previous).nearness == BACKGROUND);
        }
    }
    SECTION("far from the square nothing moves") {
        const Pick still = pick(0.2f);
        REQUIRE(Near(still.newest, At(0.2f)));
        REQUIRE(Near(still.previous, At(0.2f)));
        REQUIRE(still.newest_weight == 0.5f);
    }
}

TEST_CASE("Every in-between pixel gets a source inside the scene", "[video_core][frame_gen]") {
    // Seeded noise: motion up to a fifth of the screen either way and any nearness, different in
    // the two frames. Whatever wins, the pick is finite and clamped sampling stays defined.
    std::uint64_t state = 0x5eed;
    const auto next = [&state] {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<float>(state >> 40) / static_cast<float>(1U << 24);
    };
    Field newest;
    Field previous;
    for (Field* const field : {&newest, &previous}) {
        for (GuideTexel& texel : field->texels) {
            texel = {{(next() - 0.5f) * 0.4f, (next() - 0.5f) * 0.4f}, next()};
        }
    }
    for (int x = 0; x < WIDTH; ++x) {
        for (const float t : {0.25f, 0.5f, 0.75f}) {
            const Pick pick =
                Interpolate(newest, previous, At((static_cast<float>(x) + 0.5f) / WIDTH), t, SCREEN);
            REQUIRE(std::isfinite(pick.newest.x));
            REQUIRE(std::isfinite(pick.newest.y));
            REQUIRE(std::isfinite(pick.previous.x));
            REQUIRE(std::isfinite(pick.previous.y));
            REQUIRE(pick.newest_weight >= 0.0f);
            REQUIRE(pick.newest_weight <= 1.0f);
        }
    }
}

TEST_CASE("Scene pixels do not drag the HUD along", "[video_core][frame_gen]") {
    // A pan of ten pixels a real frame under a still HUD block over [0.6, 0.7). Halfway, the
    // newest frame's source lies five pixels right of each pixel and the previous frame's five left,
    // so five pixels on each side of the block would take the HUD from one frame.
    Field guide;
    guide.Fill(0.0f, 1.0f, {{-0.1f, 0.0f}, 0.5f});
    const auto is_hud = [](Vec2 uv) { return uv.x >= 0.6f && uv.x < 0.7f; };
    for (const bool fill : {false, true}) {
        int gave_way = 0;
        for (int x = 0; x < WIDTH; ++x) {
            const Vec2 target = At((static_cast<float>(x) + 0.5f) / WIDTH);
            if (is_hud(target)) {
                continue; // the HUD itself: the newest frame, unmoved, before any warp
            }
            Pick pick = Interpolate(guide, guide, target, 0.5f, SCREEN);
            const HudGuard guard = GuardHud(is_hud, pick, target, fill);
            INFO("x " << x << ", fill " << fill);
            REQUIRE((guard == HudGuard::None || guard == HudGuard::OtherFrame));
            gave_way += guard == HudGuard::OtherFrame ? 1 : 0;
            if (pick.newest_weight > 0.0f) {
                REQUIRE_FALSE(is_hud(pick.newest));
            }
            if (pick.newest_weight < 1.0f) {
                REQUIRE_FALSE(is_hud(pick.previous));
            }
        }
        REQUIRE(gave_way == 10);
    }
}

TEST_CASE("A source on the HUD gives way to the other frame's, or the pixel falls back",
          "[video_core][frame_gen]") {
    const Vec2 target = At(0.5f);
    const Vec2 clear = At(0.3f);
    const Vec2 on_hud = At(0.7f);
    const auto is_hud = [](Vec2 uv) { return uv.x >= 0.6f; };
    SECTION("neither on the HUD: unchanged") {
        Pick pick{clear, clear, 0.5f};
        REQUIRE(GuardHud(is_hud, pick, target, false) == HudGuard::None);
        REQUIRE(pick.newest_weight == 0.5f);
    }
    SECTION("the newest frame's source on the HUD: the previous frame's alone") {
        Pick pick{on_hud, clear, 0.5f};
        REQUIRE(GuardHud(is_hud, pick, target, false) == HudGuard::OtherFrame);
        REQUIRE(pick.newest_weight == 0.0f);
        REQUIRE(Near(pick.previous, clear));
    }
    SECTION("the previous frame's source on the HUD: the newest frame's alone") {
        Pick pick{clear, on_hud, 0.5f};
        REQUIRE(GuardHud(is_hud, pick, target, false) == HudGuard::OtherFrame);
        REQUIRE(pick.newest_weight == 1.0f);
        REQUIRE(Near(pick.newest, clear));
    }
    SECTION("a source the pick does not use does not count") {
        Pick newest_alone{clear, on_hud, 1.0f};
        REQUIRE(GuardHud(is_hud, newest_alone, target, false) == HudGuard::None);
        Pick previous_alone{on_hud, clear, 0.0f};
        REQUIRE(GuardHud(is_hud, previous_alone, target, false) == HudGuard::None);
    }
    SECTION("every source used on the HUD: the scene under it with fill, else unmoved") {
        for (const Pick start : {Pick{on_hud, on_hud, 0.5f}, Pick{on_hud, clear, 1.0f}}) {
            Pick filled = start;
            REQUIRE(GuardHud(is_hud, filled, target, true) == HudGuard::SceneUnderHud);
            REQUIRE(Near(filled.newest, on_hud));
            REQUIRE(filled.newest_weight == 1.0f);
            Pick unmoved = start;
            REQUIRE(GuardHud(is_hud, unmoved, target, false) == HudGuard::Unmoved);
            REQUIRE(Near(unmoved.newest, target));
            REQUIRE(unmoved.newest_weight == 1.0f);
        }
        // The previous frame alone (a disocclusion) on the HUD: its position, read in the newest frame.
        Pick previous_alone{clear, on_hud, 0.0f};
        REQUIRE(GuardHud(is_hud, previous_alone, target, true) == HudGuard::SceneUnderHud);
        REQUIRE(Near(previous_alone.newest, on_hud));
    }
}
