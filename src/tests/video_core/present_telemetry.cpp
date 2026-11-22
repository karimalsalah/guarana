// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/settings_enums.h"
#include "video_core/renderer_vulkan/present_telemetry.h"

using Settings::VSyncMode;
using Vulkan::CountRefreshes;
using Vulkan::ParsePresentModeOverride;
using Vulkan::RefreshUnits;
using Vulkan::ShownRefreshes;
using Vulkan::SurfaceOffers;

namespace {

ShownRefreshes Count(std::vector<f32> intervals, f64 refresh_hz) {
    std::sort(intervals.begin(), intervals.end());
    return CountRefreshes(intervals, refresh_hz);
}

std::vector<f32> Repeat(f32 interval, std::size_t times) {
    return std::vector<f32>(times, interval);
}

} // namespace

TEST_CASE("EDEN_NX2_PRESENT_MODE names a present mode or nothing", "[video_core]") {
    STATIC_REQUIRE(ParsePresentModeOverride("immediate") == VSyncMode::Immediate);
    STATIC_REQUIRE(ParsePresentModeOverride("IMMEDIATE") == VSyncMode::Immediate);
    STATIC_REQUIRE(ParsePresentModeOverride("fifo") == VSyncMode::Fifo);
    STATIC_REQUIRE(ParsePresentModeOverride("Fifo_Relaxed") == VSyncMode::FifoRelaxed);
    STATIC_REQUIRE(ParsePresentModeOverride("fifo-relaxed") == VSyncMode::FifoRelaxed);
    STATIC_REQUIRE(ParsePresentModeOverride("mailbox") == VSyncMode::Mailbox);
    STATIC_REQUIRE(!ParsePresentModeOverride("").has_value());
    STATIC_REQUIRE(!ParsePresentModeOverride("1").has_value());
    STATIC_REQUIRE(!ParsePresentModeOverride("vsync").has_value());
    STATIC_REQUIRE(!ParsePresentModeOverride("fifo ").has_value());
}

TEST_CASE("A present mode override applies only when the surface lists the mode", "[video_core]") {
    // The reference laptop's surface: IMMEDIATE and FIFO only.
    STATIC_REQUIRE(SurfaceOffers(VSyncMode::Immediate, true, false, false));
    STATIC_REQUIRE(!SurfaceOffers(VSyncMode::Mailbox, true, false, false));
    STATIC_REQUIRE(!SurfaceOffers(VSyncMode::FifoRelaxed, true, false, false));
    // FIFO is always there.
    STATIC_REQUIRE(SurfaceOffers(VSyncMode::Fifo, false, false, false));
    STATIC_REQUIRE(SurfaceOffers(VSyncMode::Mailbox, false, true, false));
    STATIC_REQUIRE(SurfaceOffers(VSyncMode::FifoRelaxed, false, false, true));
}

TEST_CASE("An interval is counted in whole refreshes", "[video_core]") {
    STATIC_REQUIRE(RefreshUnits(16.67, 1000.0 / 60.0) == 1);
    STATIC_REQUIRE(RefreshUnits(33.33, 1000.0 / 60.0) == 2);
    STATIC_REQUIRE(RefreshUnits(24.0, 1000.0 / 60.0) == 1);
    STATIC_REQUIRE(RefreshUnits(26.0, 1000.0 / 60.0) == 2);
    STATIC_REQUIRE(RefreshUnits(4.0, 1000.0 / 60.0) == 0);
    STATIC_REQUIRE(RefreshUnits(16.67, 0.0) == 0);
    STATIC_REQUIRE(RefreshUnits(0.0, 16.67) == 0);
}

TEST_CASE("Shown intervals at 60 Hz: every frame on its refresh, then a missed one",
          "[video_core]") {
    ShownRefreshes even = Count(Repeat(16.67f, 600), 60.0);
    REQUIRE(even.count == std::array<u64, 4>{0, 600, 0, 0});
    REQUIRE(even.median_refreshes == 1);
    REQUIRE(even.missed == 0);

    std::vector<f32> intervals = Repeat(16.67f, 598);
    intervals.push_back(33.3f); // one frame held a second refresh
    intervals.push_back(50.0f); // one held two more
    ShownRefreshes late = Count(intervals, 60.0);
    REQUIRE(late.count == std::array<u64, 4>{0, 598, 1, 1});
    REQUIRE(late.median_refreshes == 1);
    REQUIRE(late.missed == 3);
}

TEST_CASE("Shown intervals at 120 Hz: a 60 fps cadence holds two refreshes each",
          "[video_core]") {
    std::vector<f32> intervals = Repeat(16.67f, 300);
    intervals.push_back(25.0f); // three refreshes against the cadence's two
    intervals.push_back(4.0f);  // a present within half a refresh of the last
    ShownRefreshes counted = Count(intervals, 120.0);
    REQUIRE(counted.count == std::array<u64, 4>{1, 0, 300, 1});
    REQUIRE(counted.median_refreshes == 2);
    REQUIRE(counted.missed == 1);
}

TEST_CASE("Shown intervals at 144 Hz: a 72 fps cadence and a 48 fps step", "[video_core]") {
    ShownRefreshes at72 = Count(Repeat(13.889f, 720), 144.0);
    REQUIRE(at72.count == std::array<u64, 4>{0, 0, 720, 0});
    REQUIRE(at72.median_refreshes == 2);
    REQUIRE(at72.missed == 0);

    std::vector<f32> intervals = Repeat(13.889f, 100);
    std::vector<f32> slower = Repeat(20.833f, 10); // three refreshes each
    intervals.insert(intervals.end(), slower.begin(), slower.end());
    ShownRefreshes mixed = Count(intervals, 144.0);
    REQUIRE(mixed.count == std::array<u64, 4>{0, 0, 100, 10});
    REQUIRE(mixed.median_refreshes == 2);
    REQUIRE(mixed.missed == 10);
}

TEST_CASE("Shown intervals are not counted without a refresh rate", "[video_core]") {
    const ShownRefreshes unknown = Count(Repeat(16.67f, 60), 0.0);
    REQUIRE(unknown.count == std::array<u64, 4>{});
    REQUIRE(unknown.median_refreshes == 0);
    REQUIRE(Count({}, 60.0).count == std::array<u64, 4>{});
}
