// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <limits>
#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/async_draw_policy.h"
#include "video_core/texture_cache/types.h"

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

TEST_CASE("Native draws into a redrawn target may skip while the pipeline builds (Eglantine #874)",
          "[video_core][async_draw][skip_redrawn]") {
    // A depth-tested draw writing a color target first written in an earlier frame
    REQUIRE(Vulkan::MaySkipRedrawnNativeDraw(true, 1, true));
    REQUIRE(Vulkan::MaySkipRedrawnNativeDraw(true, 4, true));
    // A target the game fills once, this frame (a font atlas, a texture for later frames): waits
    REQUIRE_FALSE(Vulkan::MaySkipRedrawnNativeDraw(true, 1, false));
    // No depth test (Star Fox's glyphs are drawn without one): waits
    REQUIRE_FALSE(Vulkan::MaySkipRedrawnNativeDraw(false, 1, true));
    // Depth only, every color write masked: waits
    REQUIRE_FALSE(Vulkan::MaySkipRedrawnNativeDraw(true, 0, true));
    // A redrawn draw is Bounded: it waits for the build while the frame's budget lasts (a fast
    // link of built libraries lands inside it), and is skipped once the budget is spent.
    REQUIRE(Vulkan::DecideNativePending(Vulkan::NativePendingMode::Bounded, true, 8000) ==
            Vulkan::NativeDrawAction::WaitTimed);
    REQUIRE(Vulkan::DecideNativePending(Vulkan::NativePendingMode::Bounded, true, 0) ==
            Vulkan::NativeDrawAction::Skip);
}

// Ported with EDEN_NX2_ASYNC_DISCARD=2 from Fritchii (a962f20533).
/// EDEN_NX2_ASYNC_DISCARD=2: native draws into a target that is not recurring always wait, and
/// the target class never makes a draw skippable that the heuristic keeps.
TEST_CASE("Native pending draws into persistent targets wait", "[video_core][async_draw]") {
    REQUIRE_FALSE(Vulkan::CanDiscardPendingDraw(true, true, 0, 0, true, false));
    REQUIRE_FALSE(Vulkan::CanDiscardPendingDraw(true, false, 7, 7, true, false));
    REQUIRE(Vulkan::CanDiscardPendingDraw(true, true, 0, 0, true, true));
    REQUIRE_FALSE(Vulkan::CanDiscardPendingDraw(true, false, 0, 0, true, true));
    // Non-native draws keep the legacy heuristic.
    REQUIRE(Vulkan::CanDiscardPendingDraw(false, true, 0, 0, true, false));
    REQUIRE_FALSE(Vulkan::CanDiscardPendingDraw(false, false, 6, 6, true, false));
}

TEST_CASE("Render targets are recurring only after two cleared frames",
          "[video_core][async_draw]") {
    using VideoCommon::ClassifyClearedTarget;
    using VideoCommon::DrawTargetClass;
    constexpr u64 Never = ~u64{0};
    // Never cleared, or not in this frame: persistent.
    REQUIRE(ClassifyClearedTarget(Never, Never, 10) == DrawTargetClass::Persistent);
    REQUIRE(ClassifyClearedTarget(9, 8, 10) == DrawTargetClass::Persistent);
    // Cleared now for the first time, or after a long gap: first-time.
    REQUIRE(ClassifyClearedTarget(10, Never, 10) == DrawTargetClass::FirstTime);
    REQUIRE(ClassifyClearedTarget(10, 10 - VideoCommon::RECURRING_CLEAR_WINDOW - 1, 10) ==
            DrawTargetClass::FirstTime);
    // Cleared now and in a recent earlier frame: recurring.
    REQUIRE(ClassifyClearedTarget(10, 9, 10) == DrawTargetClass::Recurring);
    REQUIRE(ClassifyClearedTarget(10, 10 - VideoCommon::RECURRING_CLEAR_WINDOW, 10) ==
            DrawTargetClass::Recurring);
    // A clock that went back never counts as recurring.
    REQUIRE(ClassifyClearedTarget(10, 11, 10) == DrawTargetClass::FirstTime);
    // The draw takes the least safe class of its targets.
    REQUIRE(DrawTargetClass::Recurring < DrawTargetClass::FirstTime);
    REQUIRE(DrawTargetClass::FirstTime < DrawTargetClass::Persistent);
}

TEST_CASE("A native draw needs history on every render target it writes, through the RT map",
          "[video_core][async_draw]") {
    const auto identity = [](u32 i) { return i; };
    const auto to_rt1 = [](u32) { return 1u; };
    const auto writes_all = [](u32) { return true; };
    const auto rt0_only = [](u32 rt) { return rt == 0; };
    const auto none = [](u32) { return false; };
    // Output 0 maps to RT1: RT0's history must not let a first write into RT1 be skipped.
    REQUIRE_FALSE(Vulkan::ColorWritesHaveHistory(1, to_rt1, writes_all, rt0_only));
    REQUIRE(Vulkan::ColorWritesHaveHistory(1, to_rt1, writes_all, writes_all));
    REQUIRE(Vulkan::ColorWritesHaveHistory(1, identity, writes_all, rt0_only));
    // Two outputs, one into a target first written this frame.
    REQUIRE_FALSE(Vulkan::ColorWritesHaveHistory(2, identity, writes_all, rt0_only));
    // A masked output needs no history; a draw that writes no color is not skippable.
    REQUIRE(Vulkan::ColorWritesHaveHistory(2, identity, rt0_only, rt0_only));
    REQUIRE_FALSE(Vulkan::ColorWritesHaveHistory(2, identity, none, writes_all));
    REQUIRE_FALSE(Vulkan::ColorWritesHaveHistory(0, identity, writes_all, writes_all));
}
