// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/nx2_shown_frames.h"

namespace {
namespace Shown = VideoCore::Nx2ShownFrames;
using namespace std::chrono_literals;

Shown::Shown Frame(Shown::Kind kind, s64 presented_ns, s32 picture = -1) {
    Shown::Shown frame{};
    frame.kind = kind;
    frame.presented_ns = presented_ns;
    frame.picture = picture;
    return frame;
}
} // Anonymous namespace

TEST_CASE("Shown frames sheet lays out rows of four", "[nx2_shown_frames]") {
    const Shown::SheetLayout twelve = Shown::LayoutSheet(12, 480, 270);
    CHECK(twelve.columns == 4);
    CHECK(twelve.rows == 3);
    CHECK(twelve.width == Shown::SHEET_GAP + 4 * (480 + Shown::SHEET_GAP));
    CHECK(twelve.height == Shown::SHEET_GAP + 3 * (Shown::SHEET_STRIP + 270 + Shown::SHEET_GAP));

    CHECK(Shown::LayoutSheet(1, 480, 270).columns == 1);
    CHECK(Shown::LayoutSheet(3, 480, 270).columns == 3);
    // More than 16 frames: a near-square grid.
    const Shown::SheetLayout seventeen = Shown::LayoutSheet(17, 480, 270);
    CHECK(seventeen.columns == 5);
    CHECK(seventeen.rows == 4);
    const Shown::SheetLayout most = Shown::LayoutSheet(32, 960, 540);
    CHECK(most.columns == 6);
    CHECK(most.rows == 6);
    CHECK(Shown::LayoutSheet(0, 480, 270).width == 0);
}

TEST_CASE("Shown frames sheet tiles go left to right, then down", "[nx2_shown_frames]") {
    const Shown::SheetLayout layout = Shown::LayoutSheet(12, 480, 270);
    const Shown::SheetTile first = Shown::TileAt(layout, 0, 480, 270);
    CHECK(first.x == Shown::SHEET_GAP);
    CHECK(first.y == Shown::SHEET_GAP + Shown::SHEET_STRIP);
    const Shown::SheetTile sixth = Shown::TileAt(layout, 5, 480, 270);
    CHECK(sixth.x == Shown::SHEET_GAP + 480 + Shown::SHEET_GAP);
    CHECK(sixth.y == Shown::SHEET_GAP + (Shown::SHEET_STRIP + 270 + Shown::SHEET_GAP) +
                         Shown::SHEET_STRIP);
    const Shown::SheetTile last = Shown::TileAt(layout, 11, 480, 270);
    CHECK(last.x + 480 + Shown::SHEET_GAP == layout.width);
    CHECK(last.y + 270 + Shown::SHEET_GAP == layout.height);
}

TEST_CASE("Shown frames sheet shows each picture under its kind's strip", "[nx2_shown_frames]") {
    Shown::Result result;
    result.width = 2;
    result.height = 1;
    // Picture 0: two BGRA pixels; frame 1 has no picture.
    result.pixels = {10, 20, 30, 255, 40, 50, 60, 255, 0, 0, 0, 0, 0, 0, 0, 0};
    result.frames = {Frame(Shown::Kind::Predicted, 0, 0), Frame(Shown::Kind::Interpolated, 1)};
    const Shown::SheetLayout layout = Shown::LayoutSheet(2, 2, 1);
    const std::vector<u8> sheet = Shown::ComposeSheet(result, layout);
    REQUIRE(sheet.size() == size_t{layout.width} * layout.height * 3);
    const auto pixel = [&](u32 x, u32 y) {
        const size_t at = (size_t{y} * layout.width + x) * 3;
        return std::vector<u8>{sheet[at], sheet[at + 1], sheet[at + 2]};
    };
    const Shown::SheetTile first = Shown::TileAt(layout, 0, 2, 1);
    CHECK(pixel(first.x, first.y) == std::vector<u8>{30, 20, 10});
    CHECK(pixel(first.x + 1, first.y) == std::vector<u8>{60, 50, 40});
    CHECK(pixel(first.x, first.y - 1) == std::vector<u8>{240, 150, 32});
    const Shown::SheetTile second = Shown::TileAt(layout, 1, 2, 1);
    CHECK(pixel(second.x, second.y) == std::vector<u8>{96, 96, 96});
    CHECK(pixel(second.x, second.y - Shown::SHEET_STRIP) == std::vector<u8>{64, 140, 240});
    // The gaps keep the background.
    CHECK(pixel(0, 0) == std::vector<u8>{32, 32, 32});

    result.height = 0;
    CHECK(Shown::ComposeSheet(result, layout).empty());
}

TEST_CASE("Shown frames times are relative to the first present", "[nx2_shown_frames]") {
    std::vector<Shown::Shown> frames{
        Frame(Shown::Kind::Real, 5'000'000'000),
        Frame(Shown::Kind::Predicted, 5'008'334'499),
        Frame(Shown::Kind::Predicted, 5'016'667'500),
    };
    frames[0].scheduled_ns = 5'000'200'000; // presented 200 us before its vblank
    frames[0].recorded_ns = 4'960'000'000;
    frames[2].scheduled_ns = 5'016'666'000;
    frames[2].recorded_ns = 4'960'000'000;
    const std::vector<Shown::Times> times = Shown::RelativeTimes(frames);
    REQUIRE(times.size() == 3);
    CHECK(times[0].presented_us == 0);
    CHECK(times[0].scheduled_us == 200);
    CHECK(times[0].late_us == -200);
    CHECK_FALSE(times[0].interval_us.has_value());
    CHECK(times[0].age_us == 40'000);
    // Rounded to nearest: 8334.499 us down, 16667.5 us up.
    CHECK(times[1].presented_us == 8'334);
    CHECK(times[1].interval_us == 8'334);
    CHECK_FALSE(times[1].scheduled_us.has_value());
    CHECK_FALSE(times[1].late_us.has_value());
    CHECK_FALSE(times[1].age_us.has_value());
    CHECK(times[2].presented_us == 16'668);
    CHECK(times[2].scheduled_us == 16'666);
    CHECK(times[2].late_us == 2);
    CHECK(times[2].interval_us == 8'333);
    CHECK(times[2].age_us == 56'668);
    CHECK(Shown::RelativeTimes({}).empty());

    // Copy-ahead: the time is the copy's submission, so no lateness or age.
    frames[2].deferred = true;
    const std::vector<Shown::Times> deferred = Shown::RelativeTimes(frames);
    CHECK(deferred[2].scheduled_us == 16'666);
    CHECK_FALSE(deferred[2].late_us.has_value());
    CHECK_FALSE(deferred[2].age_us.has_value());
    CHECK(deferred[2].interval_us == 8'333);
}

TEST_CASE("Shown frames capture is armed from request to finish", "[nx2_shown_frames]") {
    REQUIRE_FALSE(Shown::Armed());
    REQUIRE(Shown::RequestCapture({.id = 1, .count = 2, .height = 270}));
    CHECK(Shown::Armed());
    CHECK(Shown::PicturesArmed());
    CHECK_FALSE(Shown::RequestCapture({.id = 2, .count = 2, .height = 270}));
    const auto taken = Shown::TakeCapture();
    REQUIRE(taken.has_value());
    CHECK(taken->id == 1);
    CHECK_FALSE(Shown::TakeCapture().has_value());
    CHECK(Shown::Armed());
    Shown::Result result;
    result.frames = {Frame(Shown::Kind::Real, 1), Frame(Shown::Kind::Predicted, 2)};
    Shown::FinishCapture(1, std::move(result));
    CHECK_FALSE(Shown::Armed());
    CHECK_FALSE(Shown::PicturesArmed());
    const auto answer = Shown::WaitCapture(1, 0ms);
    REQUIRE(answer.has_value());
    CHECK(answer->frames.size() == 2);
    CHECK_FALSE(Shown::WaitCapture(1, 0ms).has_value());
}

TEST_CASE("Shown frames capture that timed out is dropped", "[nx2_shown_frames]") {
    REQUIRE(Shown::RequestCapture({.id = 3, .count = 1, .height = 0}));
    CHECK(Shown::Armed());
    CHECK_FALSE(Shown::PicturesArmed());
    REQUIRE(Shown::TakeCapture().has_value());
    Shown::CancelCapture(3);
    // The presenter still finishes it; a new capture may queue meanwhile.
    CHECK(Shown::Armed());
    REQUIRE(Shown::RequestCapture({.id = 4, .count = 1, .height = 0}));
    Shown::FinishCapture(3, {});
    CHECK_FALSE(Shown::WaitCapture(3, 0ms).has_value());
    CHECK(Shown::Armed());
    REQUIRE(Shown::TakeCapture().has_value());
    Shown::FinishCapture(4, {});
    CHECK(Shown::WaitCapture(4, 0ms).has_value());
    CHECK_FALSE(Shown::Armed());

    // Withdrawn before the presenter took it.
    REQUIRE(Shown::RequestCapture({.id = 5, .count = 1, .height = 0}));
    Shown::CancelCapture(5);
    CHECK_FALSE(Shown::Armed());
    CHECK_FALSE(Shown::TakeCapture().has_value());
}

TEST_CASE("Shown frames capture cut short by the presenter disarms", "[nx2_shown_frames]") {
    // The server timed out and withdrew it, then the presenter was destroyed mid-capture and
    // answered with the frames it had.
    REQUIRE(Shown::RequestCapture({.id = 6, .count = 4, .height = 270}));
    REQUIRE(Shown::TakeCapture().has_value());
    Shown::CancelCapture(6);
    CHECK(Shown::PicturesArmed());
    Shown::Result partial;
    partial.frames = {Frame(Shown::Kind::Real, 1)};
    partial.picture_note = "the presenter stopped before all frames were shown";
    Shown::FinishCapture(6, std::move(partial));
    CHECK_FALSE(Shown::Armed());
    CHECK_FALSE(Shown::PicturesArmed());
    CHECK_FALSE(Shown::WaitCapture(6, 0ms).has_value());
    // The next capture is taken again.
    REQUIRE(Shown::RequestCapture({.id = 7, .count = 1, .height = 0}));
    REQUIRE(Shown::TakeCapture().has_value());
    Shown::FinishCapture(7, {});
    CHECK(Shown::WaitCapture(7, 0ms).has_value());
    CHECK_FALSE(Shown::Armed());
}
