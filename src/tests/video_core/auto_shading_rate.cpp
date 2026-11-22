// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <chrono>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "core/core.h"
#include "video_core/auto_shading_rate.h"
#include "video_core/nx2_debug_control.h"

namespace {

namespace Auto = VideoCore::AutoShadingRate;
using VideoCore::GpuCostSnapshot;
using VideoCore::Nx2Debug::ShadingRate;
using VideoCore::Nx2Debug::ShadingRateFor;

constexpr u64 Scene = 0x5CE0E00000000001;
constexpr u64 Quad = 0x5CE0E00000000002;
constexpr u64 Side = 0x5CE0E00000000003;
constexpr u64 Depth = 0x5CE0E00000000004;
constexpr u64 Manual = 0x5CE0E00000000005;

constexpr ShadingRate Rate1x1{};
constexpr ShadingRate Rate2x1{.width = 2, .height = 1};
constexpr ShadingRate Rate2x2{.width = 2, .height = 2};
constexpr ShadingRate Rate4x4{.width = 4, .height = 4};

GpuCostSnapshot::Entry Work(u64 fragment, double total_ms, u32 draws, u32 small_draws,
                            u32 side_draws) {
    GpuCostSnapshot::Entry entry;
    entry.vertex_hash = fragment + 0x100;
    entry.fragment_hash = fragment;
    entry.draws = draws;
    entry.small_draws = small_draws;
    entry.side_draws = side_draws;
    entry.total_ms = total_ms;
    entry.max_ms = total_ms;
    return entry;
}

GpuCostSnapshot Snapshot() {
    GpuCostSnapshot snapshot;
    snapshot.frame = 1;
    snapshot.has_main_target = true;
    snapshot.work.push_back(Work(0, 12.0, 1, 0, 0)); // A dispatch
    snapshot.work.push_back(Work(Quad, 9.0, 2, 2, 0));
    snapshot.work.push_back(Work(Side, 7.0, 4, 0, 4));
    snapshot.work.push_back(Work(Depth, 6.0, 3, 0, 0));
    snapshot.work.push_back(Work(Manual, 5.5, 3, 0, 0));
    snapshot.work.push_back(Work(Scene, 5.0, 3, 1, 1));
    return snapshot;
}

/// Guest presents every interval_ms up to now.
void FillPresents(double interval_ms) {
    const u64 now = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count());
    const u64 interval = static_cast<u64>(interval_ms * 1'000'000.0);
    const u64 count = 20;
    for (u64 present = 1; present <= count; ++present) {
        Core::g_nx2_present_times_ns[present].store(now - (count - present) * interval);
    }
    Core::g_nx2_present_count.store(count);
}

void ClearPresents() {
    for (auto& timestamp : Core::g_nx2_present_times_ns) {
        timestamp.store(0);
    }
    Core::g_nx2_present_count.store(0);
}

} // namespace

TEST_CASE("Snapshot deny list: small draws and side targets", "[video_core][auto_shading_rate]") {
    GpuCostSnapshot snapshot = Snapshot();
    auto denied = Auto::SnapshotDenied(snapshot);
    REQUIRE(denied.first == std::vector<u64>{Quad});
    REQUIRE(denied.second == std::vector<u64>{Side});
    // A shader also drawn with real geometry is not a quad shader.
    snapshot.work.push_back(Work(Quad, 1.0, 1, 0, 0));
    // Without a main scene target nothing is a side target.
    snapshot.has_main_target = false;
    denied = Auto::SnapshotDenied(snapshot);
    REQUIRE(denied.first.empty());
    REQUIRE(denied.second.empty());
}

TEST_CASE("Recent frame time is the median guest present interval",
          "[video_core][auto_shading_rate]") {
    ClearPresents();
    REQUIRE(Core::Nx2RecentFrameMs() == 0.0);
    FillPresents(40.0);
    REQUIRE(Core::Nx2RecentFrameMs() == 40.0);
    // One long frame does not move the median.
    Core::g_nx2_present_times_ns[10].store(Core::g_nx2_present_times_ns[10].load() + 30'000'000);
    REQUIRE(Core::Nx2RecentFrameMs() == 40.0);
    ClearPresents();
}

TEST_CASE("Automatic shading rates fill a layer under the manual one",
          "[video_core][auto_shading_rate]") {
    namespace Nx2Debug = VideoCore::Nx2Debug;
    const std::array manual{Manual};
    const std::array scene{Scene};
    Nx2Debug::SetShadingRates(manual, Rate2x2, false);
    Auto::NoteDepthWriter(Depth);
    FillPresents(40.0);
    Auto::SetTargetFps(30.0);
    REQUIRE(Auto::TargetFps() == 30.0);

    const GpuCostSnapshot snapshot = Snapshot();
    Auto::OnSnapshot(snapshot);
    REQUIRE(ShadingRateFor(Scene) == Rate1x1);
    Auto::OnSnapshot(snapshot);
    // The costliest shader left: not the quad, the side target, the depth writer or the manual.
    REQUIRE(ShadingRateFor(Scene) == Rate2x1);
    REQUIRE(ShadingRateFor(Quad) == Rate1x1);
    REQUIRE(ShadingRateFor(Side) == Rate1x1);
    REQUIRE(ShadingRateFor(Depth) == Rate1x1);
    REQUIRE(ShadingRateFor(Manual) == Rate2x2);
    Auto::Status status = Auto::GetStatus();
    REQUIRE(status.levels.size() == 1);
    REQUIRE(status.levels[0].first == Scene);
    REQUIRE(status.levels[0].second == 1);
    REQUIRE(status.frame_ms == 40.0);
    REQUIRE(status.denied_side == std::vector<u64>{Side});

    // A manual rate wins over the layer, and taking it away shows the layer again.
    Nx2Debug::SetShadingRates(scene, Rate4x4, false);
    REQUIRE(ShadingRateFor(Scene) == Rate4x4);
    Nx2Debug::SetShadingRates(scene, ShadingRate{}, false);
    REQUIRE(ShadingRateFor(Scene) == Rate2x1);

    // Headroom lowers it again.
    FillPresents(20.0);
    for (int i = 0; i < 4; ++i) {
        Auto::OnSnapshot(snapshot);
    }
    REQUIRE(ShadingRateFor(Scene) == Rate1x1);

    // A shader the user denies leaves the layer at once.
    FillPresents(40.0);
    Auto::OnSnapshot(snapshot);
    Auto::OnSnapshot(snapshot);
    REQUIRE(ShadingRateFor(Scene) == Rate2x1);
    Auto::SetDenied(scene, true);
    REQUIRE(ShadingRateFor(Scene) == Rate1x1);
    Auto::SetDenied(scene, false);

    // Turning the governor off empties the layer.
    Auto::OnSnapshot(snapshot);
    Auto::OnSnapshot(snapshot);
    REQUIRE(ShadingRateFor(Scene) == Rate2x1);
    Auto::SetTargetFps(0.0);
    REQUIRE(ShadingRateFor(Scene) == Rate1x1);
    REQUIRE(Auto::GetStatus().levels.empty());

    Nx2Debug::SetShadingRates(manual, ShadingRate{}, false);
    ClearPresents();
}
