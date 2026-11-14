// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/vulkan_common/graphics_pipeline_library_policy.h"

TEST_CASE("Graphics libraries require five descriptor sets and fast linking",
          "[video_core][gpl]") {
    using Vulkan::CanUseGraphicsPipelineLibraries;
    CHECK_FALSE(CanUseGraphicsPipelineLibraries(true, true, 4, true));
    CHECK(CanUseGraphicsPipelineLibraries(true, true, 5, true));
    CHECK(CanUseGraphicsPipelineLibraries(true, true, 32, true));
    CHECK_FALSE(CanUseGraphicsPipelineLibraries(false, true, 32, true));
    CHECK_FALSE(CanUseGraphicsPipelineLibraries(true, false, 32, true));
}

TEST_CASE("Graphics libraries require independent interpolation decoration",
          "[video_core][gpl]") {
    using Vulkan::CanUseGraphicsPipelineLibraries;
    CHECK_FALSE(CanUseGraphicsPipelineLibraries(true, true, 32, false));
}

namespace {
// A pipeline loaded from the shader cache at its first draw: built, fast-linked, no optimized
// link queued at its build.
constexpr Vulkan::OptimizedLinkState CachedPipeline() {
    return {
        .built = true,
        .linked = true,
        .fast_link = true,
        .optimize = true,
        .has_optimized_build = false,
        .optimize_cached = true,
        .has_pool = true,
    };
}
} // Anonymous namespace

TEST_CASE("A cached fast-linked pipeline queues its optimized link at a draw with the switch",
          "[video_core][gpl]") {
    using Vulkan::DecideOptimizedLinkAtDraw;
    using Vulkan::OptimizedLinkAtDraw;
    CHECK(DecideOptimizedLinkAtDraw(CachedPipeline()) == OptimizedLinkAtDraw::Queue);

    // Without EDEN_NX2_GPL_OPTIMIZE_CACHED it stays fast-linked, as before, and is counted.
    auto state = CachedPipeline();
    state.optimize_cached = false;
    CHECK(DecideOptimizedLinkAtDraw(state) == OptimizedLinkAtDraw::StaysFast);

    // EDEN_NX2_GPL_OPTIMIZE_THREADS=0: the optimized links run as jobs of the build's shader job
    // scheduler, which a cached pipeline has none of. Nothing is queued on the GPU thread.
    state = CachedPipeline();
    state.has_pool = false;
    CHECK(DecideOptimizedLinkAtDraw(state) == OptimizedLinkAtDraw::StaysFast);
}

TEST_CASE("A pipeline with nothing to optimize queues no link at a draw", "[video_core][gpl]") {
    using Vulkan::DecideOptimizedLinkAtDraw;
    using Vulkan::OptimizedLinkAtDraw;
    // Its build queued the optimized link already (built during play), or a draw did.
    auto state = CachedPipeline();
    state.has_optimized_build = true;
    CHECK(DecideOptimizedLinkAtDraw(state) == OptimizedLinkAtDraw::None);

    // Still building: what the build wrote is not read yet.
    state = CachedPipeline();
    state.built = false;
    CHECK(DecideOptimizedLinkAtDraw(state) == OptimizedLinkAtDraw::None);

    // The monolithic fallback, or a pipeline that does not use libraries.
    state = CachedPipeline();
    state.linked = false;
    CHECK(DecideOptimizedLinkAtDraw(state) == OptimizedLinkAtDraw::None);

    // No fast linking (AMD's Windows driver): the only link was already optimized.
    state = CachedPipeline();
    state.fast_link = false;
    CHECK(DecideOptimizedLinkAtDraw(state) == OptimizedLinkAtDraw::None);

    // EDEN_NX2_GPL_OPTIMIZE=0 keeps every fast link, with or without the switch.
    state = CachedPipeline();
    state.optimize = false;
    CHECK(DecideOptimizedLinkAtDraw(state) == OptimizedLinkAtDraw::None);
}
