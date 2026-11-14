// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <limits>
#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/async_draw_policy.h"

TEST_CASE("Native pending draws are never discarded", "[video_core][async_draw]") {
    constexpr std::array<u32, 6> counts{0, 1, 6, 7, 4096, std::numeric_limits<u32>::max()};
    for (const bool depth : {false, true}) {
        for (const u32 indices : counts) {
            for (const u32 vertices : counts) {
                REQUIRE_FALSE(Vulkan::CanDiscardPendingDraw(true, depth, indices, vertices));
            }
        }
    }
}

TEST_CASE("Native pending draws with depth can be discarded when opted in",
          "[video_core][async_draw]") {
    REQUIRE(Vulkan::CanDiscardPendingDraw(true, true, 0, 0, true));
    REQUIRE_FALSE(Vulkan::CanDiscardPendingDraw(true, false, 7, 7, true));
    REQUIRE_FALSE(Vulkan::CanDiscardPendingDraw(true, false, 6, 100, true));
    REQUIRE_FALSE(Vulkan::CanDiscardPendingDraw(true, false, 0, 0, true));
    // The opt-in never changes non-native draws.
    REQUIRE(Vulkan::CanDiscardPendingDraw(false, true, 0, 0, true) ==
            Vulkan::CanDiscardPendingDraw(false, true, 0, 0));
    REQUIRE_FALSE(Vulkan::CanDiscardPendingDraw(false, false, 0, 0, true));
}

TEST_CASE("Star Fox font atlas draws wait even when the native wait budget is exhausted",
          "[video_core][async_draw][native_pending]") {
    constexpr std::array<u32, 4> glyph_counts{30, 96, 384, 672};
    for (const u32 count : glyph_counts) {
        const bool can_discard = Vulkan::CanDiscardPendingDraw(true, false, count, count, true);
        for (const auto mode : {Vulkan::NativePendingMode::Wait, Vulkan::NativePendingMode::Bounded,
                                Vulkan::NativePendingMode::Skip}) {
            for (const s64 budget : {8000, 0, -500}) {
                REQUIRE(Vulkan::DecideNativePending(mode, can_discard, budget) ==
                        Vulkan::NativeDrawAction::Wait);
            }
        }
    }
}

TEST_CASE("Legacy pending draws retain the async heuristic", "[video_core][async_draw]") {
    REQUIRE(Vulkan::CanDiscardPendingDraw(false, true, 0, 0));
    REQUIRE(Vulkan::CanDiscardPendingDraw(false, false, 7, 7));
    REQUIRE_FALSE(Vulkan::CanDiscardPendingDraw(false, false, 6, 100));
    REQUIRE_FALSE(Vulkan::CanDiscardPendingDraw(false, false, 100, 6));
    REQUIRE_FALSE(Vulkan::CanDiscardPendingDraw(false, false, 0, 0));
}

TEST_CASE("A draw is dropped before any build only when it would be skipped without a wait",
          "[video_core][async_draw]") {
    using Vulkan::MaySkipUnbuiltDraw;
    using Vulkan::NativePendingMode;
    for (const auto mode : {NativePendingMode::Wait, NativePendingMode::Bounded,
                            NativePendingMode::Skip}) {
        for (const s64 budget : {8000, 0, -500}) {
            // A draw that is not native follows the heuristic alone.
            REQUIRE(MaySkipUnbuiltDraw(false, mode, true, budget));
            REQUIRE_FALSE(MaySkipUnbuiltDraw(false, mode, false, budget));
            // The heuristic keeps a native draw whatever the mode is.
            REQUIRE_FALSE(MaySkipUnbuiltDraw(true, mode, false, budget));
        }
    }
    REQUIRE_FALSE(MaySkipUnbuiltDraw(true, NativePendingMode::Wait, true, 0));
    REQUIRE(MaySkipUnbuiltDraw(true, NativePendingMode::Skip, true, 8000));
    // Bounded waits while the frame's budget lasts.
    REQUIRE_FALSE(MaySkipUnbuiltDraw(true, NativePendingMode::Bounded, true, 8000));
    REQUIRE(MaySkipUnbuiltDraw(true, NativePendingMode::Bounded, true, 0));
    REQUIRE(MaySkipUnbuiltDraw(true, NativePendingMode::Bounded, true, -500));
}
