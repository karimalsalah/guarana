// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <string>

#include <catch2/catch_test_macros.hpp>

#include "common/boot_split.h"

namespace BootSplit = Common::BootSplit;

TEST_CASE("Boot split counts only with a debug server address", "[common][boot_split]") {
    REQUIRE_FALSE(BootSplit::ParseEnabled(nullptr));
    REQUIRE_FALSE(BootSplit::ParseEnabled(""));
    REQUIRE(BootSplit::ParseEnabled("127.0.0.1:6545"));
    REQUIRE(BootSplit::ParseEnabled("6545"));
}

TEST_CASE("Boot split totals add up held draws, builds and file reads", "[common][boot_split]") {
    BootSplit::Recorder recorder;
    REQUIRE(recorder.Current().held_draws == 0);

    recorder.AddHeldDraw(2'000'000);
    recorder.AddHeldDraw(3'000'000);
    recorder.AddHeldDispatch(500);
    recorder.AddBuild(BootSplit::BuildKind::GraphicsMonolithic, 10);
    recorder.AddBuild(BootSplit::BuildKind::GraphicsMonolithic, 20);
    recorder.AddBuild(BootSplit::BuildKind::Compute, 7);
    recorder.AddBuild(BootSplit::BuildKind::GraphicsLibraryFragment, 100);
    recorder.AddFileRead(4096, 1000);
    recorder.AddFileRead(1, 5);

    const BootSplit::Totals totals = recorder.Current();
    REQUIRE(totals.held_draws == 2);
    REQUIRE(totals.held_draw_ns == 5'000'000);
    REQUIRE(totals.held_dispatches == 1);
    REQUIRE(totals.held_dispatch_ns == 500);
    const auto at = [](BootSplit::BuildKind kind) { return static_cast<std::size_t>(kind); };
    REQUIRE(totals.builds[at(BootSplit::BuildKind::GraphicsMonolithic)] == 2);
    REQUIRE(totals.build_ns[at(BootSplit::BuildKind::GraphicsMonolithic)] == 30);
    REQUIRE(totals.builds[at(BootSplit::BuildKind::Compute)] == 1);
    REQUIRE(totals.build_ns[at(BootSplit::BuildKind::Compute)] == 7);
    REQUIRE(totals.builds[at(BootSplit::BuildKind::GraphicsLibraryFragment)] == 1);
    REQUIRE(totals.builds[at(BootSplit::BuildKind::GraphicsLibraryPreRasterization)] == 0);
    REQUIRE(totals.builds[at(BootSplit::BuildKind::GraphicsFastLink)] == 0);
    REQUIRE(totals.builds[at(BootSplit::BuildKind::GraphicsOptimizedLink)] == 0);
    REQUIRE(totals.file_reads == 2);
    REQUIRE(totals.file_read_bytes == 4097);
    REQUIRE(totals.file_read_ns == 1005);
}

TEST_CASE("Boot split names every build kind", "[common][boot_split]") {
    REQUIRE(BootSplit::BuildKindNames.size() == BootSplit::BuildKindCount);
    REQUIRE(BootSplit::BuildKindNames[static_cast<std::size_t>(BootSplit::BuildKind::Compute)] ==
            "compute");
    for (const auto name : BootSplit::BuildKindNames) {
        REQUIRE_FALSE(name.empty());
    }
}

TEST_CASE("Boot split difference is the change between two samples", "[common][boot_split]") {
    BootSplit::Recorder recorder;
    recorder.AddHeldDraw(1'000);
    recorder.AddBuild(BootSplit::BuildKind::GraphicsFastLink, 50);
    recorder.AddFileRead(100, 10);
    recorder.RecordFirstFrame(9000.0, 1);

    recorder.AddHeldDraw(4'000);
    recorder.AddBuild(BootSplit::BuildKind::GraphicsFastLink, 70);
    recorder.AddBuild(BootSplit::BuildKind::Compute, 3);
    recorder.AddFileRead(300, 30);
    const BootSplit::Sample mark = recorder.RecordMark("gameplay", 60000.0, 1440);

    const std::optional<BootSplit::Sample> first = recorder.FirstFrame();
    REQUIRE(first.has_value());
    REQUIRE(first->label == "first_frame");
    REQUIRE(first->at_ms == 9000.0);
    REQUIRE(first->presents == 1);
    REQUIRE(first->totals.held_draw_ns == 1'000);

    REQUIRE(mark.label == "gameplay");
    REQUIRE(mark.presents == 1440);
    REQUIRE(mark.totals.held_draw_ns == 5'000);

    const BootSplit::Totals between = BootSplit::Difference(mark.totals, first->totals);
    REQUIRE(between.held_draws == 1);
    REQUIRE(between.held_draw_ns == 4'000);
    REQUIRE(between.builds[static_cast<std::size_t>(BootSplit::BuildKind::GraphicsFastLink)] == 1);
    REQUIRE(between.build_ns[static_cast<std::size_t>(BootSplit::BuildKind::GraphicsFastLink)] ==
            70);
    REQUIRE(between.builds[static_cast<std::size_t>(BootSplit::BuildKind::Compute)] == 1);
    REQUIRE(between.file_reads == 1);
    REQUIRE(between.file_read_bytes == 300);
    REQUIRE(between.file_read_ns == 30);

    // A field that went backwards (a counter from another session) gives 0, not a wrapped value.
    const BootSplit::Totals backwards = BootSplit::Difference(first->totals, mark.totals);
    REQUIRE(backwards.held_draw_ns == 0);
    REQUIRE(backwards.file_read_bytes == 0);
}

TEST_CASE("Boot split first frame sample is replaced by a new session's", "[common][boot_split]") {
    BootSplit::Recorder recorder;
    REQUIRE_FALSE(recorder.FirstFrame().has_value());
    recorder.RecordFirstFrame(100.0, 1);
    recorder.AddHeldDraw(9);
    recorder.RecordFirstFrame(200.0, 1);
    const std::optional<BootSplit::Sample> first = recorder.FirstFrame();
    REQUIRE(first.has_value());
    REQUIRE(first->at_ms == 200.0);
    REQUIRE(first->totals.held_draw_ns == 9);
}

TEST_CASE("Boot split keeps the newest marks", "[common][boot_split]") {
    BootSplit::Recorder recorder;
    REQUIRE(recorder.Marks().empty());
    const std::size_t total = BootSplit::Recorder::MaxMarks + 3;
    for (std::size_t index = 0; index < total; ++index) {
        recorder.RecordMark("mark " + std::to_string(index), static_cast<double>(index), index);
    }
    const std::vector<BootSplit::Sample> marks = recorder.Marks();
    REQUIRE(marks.size() == BootSplit::Recorder::MaxMarks);
    REQUIRE(marks.front().label == "mark 3");
    REQUIRE(marks.back().label == "mark " + std::to_string(total - 1));
}
