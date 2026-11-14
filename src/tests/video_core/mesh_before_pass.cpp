// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <initializer_list>

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/mesh_before_pass.h"

namespace {

using Vulkan::MeshBeforePass::Decide;
using Vulkan::MeshBeforePass::MayHoistDraw;
using Vulkan::MeshBeforePass::ParseSwitch;
using Vulkan::MeshBeforePass::State;
using Vulkan::MeshBeforePass::Verdict;

State OpenPass() {
    return State{
        .pass_active = true,
        .deferred_clear_matches = false,
        .has_prelude = true,
        .pass_has_stores = false,
        .uploads_pending = false,
        .same_framebuffer = true,
        .same_render_area = true,
    };
}

} // namespace

TEST_CASE("EDEN_NX2_MESH_BEFORE_PASS is off unless it holds a nonzero number",
          "[video_core][mesh_before_pass]") {
    REQUIRE_FALSE(ParseSwitch(nullptr));
    REQUIRE_FALSE(ParseSwitch(""));
    REQUIRE_FALSE(ParseSwitch("0"));
    REQUIRE_FALSE(ParseSwitch("false"));
    REQUIRE_FALSE(ParseSwitch("off"));
    REQUIRE(ParseSwitch("1"));
    REQUIRE(ParseSwitch("2"));
    REQUIRE(ParseSwitch(" 1"));
}

TEST_CASE("A mesh draw in a clean open pass runs its dispatches before the pass",
          "[video_core][mesh_before_pass]") {
    REQUIRE(Decide(OpenPass()) == Verdict::Hoist);
}

TEST_CASE("Storage writes in the pass keep the pass cut", "[video_core][mesh_before_pass]") {
    State state{OpenPass()};
    state.pass_has_stores = true;
    REQUIRE(Decide(state) == Verdict::Refuse);
}

TEST_CASE("A pass already handed to the worker keeps the pass cut",
          "[video_core][mesh_before_pass]") {
    State state{OpenPass()};
    state.has_prelude = false;
    REQUIRE(Decide(state) == Verdict::Refuse);
}

TEST_CASE("Pending uploads keep the pass cut", "[video_core][mesh_before_pass]") {
    State state{OpenPass()};
    state.uploads_pending = true;
    REQUIRE(Decide(state) == Verdict::Refuse);
}

TEST_CASE("Another framebuffer or render area keeps the pass cut",
          "[video_core][mesh_before_pass]") {
    State other_framebuffer{OpenPass()};
    other_framebuffer.same_framebuffer = false;
    REQUIRE(Decide(other_framebuffer) == Verdict::Refuse);

    State other_area{OpenPass()};
    other_area.same_render_area = false;
    REQUIRE(Decide(other_area) == Verdict::Refuse);
}

TEST_CASE("Without a pass the dispatches go before the pass the draw begins",
          "[video_core][mesh_before_pass]") {
    State state{};
    REQUIRE(Decide(state) == Verdict::NoPass);

    state.deferred_clear_matches = true;
    REQUIRE(Decide(state) == Verdict::RealizeClear);
}

TEST_CASE("A draw that samples its own render targets keeps its dispatches in place",
          "[video_core][mesh_before_pass]") {
    REQUIRE(MayHoistDraw(true, false, false));
    REQUIRE_FALSE(MayHoistDraw(true, true, false));
    REQUIRE_FALSE(MayHoistDraw(false, false, false));
    REQUIRE_FALSE(MayHoistDraw(false, true, false));
}

TEST_CASE("A draw that writes storage keeps its dispatches in place",
          "[video_core][mesh_before_pass]") {
    REQUIRE_FALSE(MayHoistDraw(true, false, true));
    REQUIRE_FALSE(MayHoistDraw(true, true, true));
}

TEST_CASE("Unset and zero keep mesh dispatches in place for every draw hazard combination",
          "[video_core][mesh_before_pass]") {
    for (const char* value : {static_cast<const char*>(nullptr), "0"}) {
        for (const bool samples_targets : {false, true}) {
            for (const bool writes_storage : {false, true}) {
                REQUIRE_FALSE(MayHoistDraw(ParseSwitch(value), samples_targets, writes_storage));
            }
        }
    }
}

TEST_CASE("Realizing a deferred clear must recheck whether its begin reached the worker",
          "[video_core][mesh_before_pass]") {
    State state{};
    state.deferred_clear_matches = true;
    REQUIRE(Decide(state) == Verdict::RealizeClear);

    // Recording the begin can fill and dispatch a chunk, closing its prelude immediately.
    state = OpenPass();
    state.has_prelude = false;
    REQUIRE(Decide(state) == Verdict::Refuse);
}
