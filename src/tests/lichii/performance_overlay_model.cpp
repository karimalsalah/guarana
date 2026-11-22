// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "lichii/render/performance_overlay_model.h"

namespace {

using namespace Guarana::Overlay;

std::vector<float> Flat(std::size_t n, float ms) {
    return std::vector<float>(n, ms);
}

} // namespace

TEST_CASE("Trace scale keeps a 60 Hz budget and refuses one stall", "[lichii][overlay]") {
    REQUIRE(ScaleMs({}) == Catch::Approx(Budget60Ms * 1.35));
    REQUIRE(ScaleMs(std::array<float, 1>{10.f}) == Catch::Approx(Budget60Ms * 1.35));
    REQUIRE(ScaleMs(std::array<float, 1>{40.f}) == Catch::Approx(40.0 * 1.08));
    REQUIRE(ScaleMs(std::array<float, 1>{200.f}) == Catch::Approx(ScaleCapMs));
}

TEST_CASE("A present is a bar from when it started to when it ended", "[lichii][overlay]") {
    const auto fresh = MarkInterval(0, 16.67, 2, 22.5);
    REQUIRE(fresh.visible);
    REQUIRE(fresh.x1 == Catch::Approx(1));
    REQUIRE(fresh.x0 == Catch::Approx(1.0 - 0.01667 / 2.0));
    REQUIRE(fresh.height == Catch::Approx(16.67 / 22.5));

    const auto mid = MarkInterval(1, 16.67, 2, 22.5);
    REQUIRE(mid.x1 == Catch::Approx(0.5));
    REQUIRE(mid.x0 == Catch::Approx(1.0 - 1.01667 / 2.0));

    REQUIRE_FALSE(MarkInterval(2.0, 16, 2, 22.5).visible);
    REQUIRE_FALSE(MarkInterval(-1, 16, 2, 22.5).visible);

    const auto clipped = MarkInterval(1.99, 50, 2, 80);
    REQUIRE(clipped.visible);
    REQUIRE(clipped.x0 == Catch::Approx(0).margin(1e-9));
    REQUIRE(clipped.x1 == Catch::Approx(1.0 - 1.99 / 2.0));
    REQUIRE(clipped.height == Catch::Approx(50.0 / 80.0));
}

TEST_CASE("1% low averages the slowest slice", "[lichii][overlay]") {
    REQUIRE_FALSE(LowPercent({}));

    const auto locked = LowPercent(Flat(100, 16.666f));
    REQUIRE(locked);
    REQUIRE(locked->samples == 1);
    REQUIRE(locked->ms == Catch::Approx(static_cast<double>(16.666f)));
    REQUIRE(locked->fps == Catch::Approx(1000.0 / static_cast<double>(16.666f)));

    std::vector<float> mixed = Flat(100, 10.f);
    mixed.push_back(50.f);
    const auto low = LowPercent(mixed);
    REQUIRE(low);
    REQUIRE(low->samples == 2);
    REQUIRE(low->ms == Catch::Approx(30));
    REQUIRE(low->fps == Catch::Approx(1000.0 / 30.0));
}

TEST_CASE("Pacing ignores a short window and counts hitches", "[lichii][overlay]") {
    REQUIRE_FALSE(MeasurePacing(Flat(7, 16.f)));

    const auto even = MeasurePacing(Flat(8, 16.7f));
    REQUIRE(even);
    REQUIRE(even->cv == Catch::Approx(0).margin(1e-6));
    REQUIRE(even->hitches == 0);

    std::vector<float> spiked = Flat(7, 16.f);
    spiked.push_back(40.f);
    const auto pacing = MeasurePacing(spiked);
    REQUIRE(pacing);
    REQUIRE(pacing->hitches == 1);
    REQUIRE(pacing->hitch_fraction == Catch::Approx(0.125));
    REQUIRE(NewestIsHitch(40.f, pacing->median_ms, 0.01));
    REQUIRE_FALSE(NewestIsHitch(40.f, pacing->median_ms, 0.4));
    REQUIRE_FALSE(NewestIsHitch(16.f, pacing->median_ms, 0.01));
}

TEST_CASE("The limiter names the thing that is actually in the way", "[lichii][overlay]") {
    Pacing rough{.cv = 0.3, .hitch_fraction = 0.0, .median_ms = 16.f, .hitches = 0};
    Pacing even{.cv = 0.02, .hitch_fraction = 0.0, .median_ms = 16.f, .hitches = 0};
    REQUIRE(Classify(3, rough, 99, 99) == Limiter::Shaders);
    REQUIRE(Classify(0, rough, 40, 40) == Limiter::Pacing);
    REQUIRE(Classify(0, even, 90, 40) == Limiter::Cpu);
    REQUIRE(Classify(0, even, 40, 90) == Limiter::Gpu);
    REQUIRE(Classify(0, even, 80, 75) == Limiter::Steady);
    REQUIRE(Classify(0, even, 50, 48) == Limiter::Steady);
    REQUIRE(Classify(0, std::nullopt, std::nullopt, std::nullopt) == Limiter::Unknown);
    REQUIRE(Classify(0, std::nullopt, 95, std::nullopt) == Limiter::Cpu);
}

TEST_CASE("Frame times fall into refresh budgets", "[lichii][overlay]") {
    REQUIRE(BinIndex(Budget120Ms) == 0);
    REQUIRE(BinIndex(8.5) == 1);
    REQUIRE(BinIndex(Budget60Ms) == 2);
    REQUIRE(BinIndex(Budget60Ms + 0.2) == 3);
    REQUIRE(BinIndex(Budget30Ms) == 4);
    REQUIRE(BinIndex(33.5) == 5);
    REQUIRE(BinIndex(100) == 6);
    REQUIRE(BinIndex(100.2) == 7);

    const auto histogram = BuildHistogram(std::array<float, 3>{16.6f, 16.6f, 40.f});
    REQUIRE(histogram.total == 3);
    REQUIRE(histogram.counts[2] == 2);
    REQUIRE(histogram.counts[5] == 1);
    REQUIRE(histogram.Max() == 2);
}

TEST_CASE("Thread load is busy time over the wall", "[lichii][overlay]") {
    REQUIRE_FALSE(PercentOfWall(std::nullopt, 100).known);
    REQUIRE_FALSE(PercentOfWall(50, 0).known);
    const auto half = PercentOfWall(50, 100);
    REQUIRE(half.known);
    REQUIRE(half.percent == Catch::Approx(50));
    REQUIRE(PercentOfWall(500, 100).percent == Catch::Approx(100));
}

TEST_CASE("RAM shows this process and the host's room", "[lichii][overlay]") {
    constexpr std::uint64_t GiB = std::uint64_t{1} << 30;
    REQUIRE_FALSE(RamFigure(0, 8 * GiB));

    const auto roomy = RamFigure(10 * GiB, 3 * GiB);
    REQUIRE(roomy);
    REQUIRE(roomy->used_gib == Catch::Approx(10));
    REQUIRE(roomy->free_gib);
    REQUIRE(*roomy->free_gib == Catch::Approx(3));
    REQUIRE_FALSE(roomy->low);

    REQUIRE(RamFigure(17 * GiB, GiB + GiB / 2)->low);
    REQUIRE_FALSE(RamFigure(17 * GiB, RamLowBytes)->low);

    const auto unknown_room = RamFigure(4 * GiB, 0);
    REQUIRE(unknown_room);
    REQUIRE_FALSE(unknown_room->free_gib);
    REQUIRE_FALSE(unknown_room->low);
}

TEST_CASE("Video memory room is the budget, and RAM too when it is shared", "[lichii][overlay]") {
    constexpr std::uint64_t GiB = std::uint64_t{1} << 30;
    const auto card = VramFigure(3 * GiB, 8 * GiB, false, GiB);
    REQUIRE(card.used_gib == Catch::Approx(3));
    REQUIRE(card.free_gib);
    REQUIRE(*card.free_gib == Catch::Approx(5));
    REQUIRE_FALSE(card.low);

    const auto full = VramFigure(9 * GiB, 8 * GiB, false, 0);
    REQUIRE(*full.free_gib == Catch::Approx(0));
    REQUIRE(full.low);
    REQUIRE(VramFigure(7 * GiB + GiB / 2, 8 * GiB, false, 0).low);

    const auto no_budget = VramFigure(2 * GiB, 0, false, 6 * GiB);
    REQUIRE_FALSE(no_budget.free_gib);
    REQUIRE_FALSE(no_budget.low);

    // An integrated GPU's budget can be most of the RAM; the RAM still free bounds it.
    const auto shared = VramFigure(3 * GiB, 16 * GiB, true, 2 * GiB);
    REQUIRE(*shared.free_gib == Catch::Approx(2));
    REQUIRE_FALSE(shared.low);
    REQUIRE(VramFigure(3 * GiB, 16 * GiB, true, GiB / 2).low);
    REQUIRE(*VramFigure(3 * GiB, 4 * GiB, true, 8 * GiB).free_gib == Catch::Approx(1));
    REQUIRE(*VramFigure(3 * GiB, 0, true, 5 * GiB).free_gib == Catch::Approx(5));
}

TEST_CASE("Springs snap once and do not overshoot", "[lichii][overlay]") {
    Spring spring;
    spring.Step(10, 0);
    REQUIRE(spring.y == Catch::Approx(10));
    spring.Step(10, 0);
    REQUIRE(spring.y == Catch::Approx(10));

    spring.Snap(0);
    double peak = 0;
    for (int i = 0; i < 90; ++i) {
        spring.Step(100, 1.0 / 60.0);
        peak = std::max(peak, spring.y);
    }
    REQUIRE(spring.y == Catch::Approx(100).margin(0.05));
    REQUIRE(peak <= 100.05);
    spring.Disarm();
    REQUIRE_FALSE(spring.armed);
}
