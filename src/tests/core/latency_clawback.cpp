// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "core/latency_clawback.h"

namespace {

using namespace Core::LatencyClawback;

constexpr s64 Interval = 16'666'667;
constexpr s64 Cpu = 2'000'000;
constexpr s64 Queue = 4'000'000;

/// Nine steady frames: eight intervals, each present matched, the queue empty at the end.
State Trained() {
    State state;
    for (int index = 0; index < 9; ++index) {
        const s64 submit = 1'000'000'000 + static_cast<s64>(index) * Interval;
        REQUIRE(NoteInput(state, submit - Cpu));
        NoteSubmit(state, submit);
        REQUIRE(NotePresent(state, submit + Queue, Interval));
    }
    return state;
}

Config Target(u32 frames, s64 max_sleep = MaxSleepNs) {
    return Config{.track = true, .target_queue = frames, .max_sleep_ns = max_sleep};
}

} // namespace

TEST_CASE("Latency switches: zero and false are off", "[latency]") {
    CHECK_FALSE(TrackRequested(nullptr));
    CHECK_FALSE(TrackRequested(""));
    CHECK_FALSE(TrackRequested("0"));
    CHECK_FALSE(TrackRequested("false"));
    CHECK_FALSE(TrackRequested("FALSE"));
    CHECK(TrackRequested("1"));
    CHECK(TrackRequested("2"));

    CHECK(ClawbackTarget(nullptr) == 0);
    CHECK(ClawbackTarget("") == 0);
    CHECK(ClawbackTarget("0") == 0);
    CHECK(ClawbackTarget("false") == 0);
    CHECK(ClawbackTarget("1") == 1);
    CHECK(ClawbackTarget("2") == 2);
    CHECK(ClawbackTarget("4") == 4);
    CHECK(ClawbackTarget("8") == 4);
}

TEST_CASE("Latency tracker: the first pad publish of a frame sticks", "[latency]") {
    State state;
    REQUIRE(NoteInput(state, 1'000'000));
    CHECK_FALSE(NoteInput(state, 2'000'000));
    CHECK(state.open_input_ns == 1'000'000);
    NoteSubmit(state, 5'000'000);
    CHECK(state.open_input_ns == 0);
    REQUIRE(NoteInput(state, 6'000'000));
    CHECK(state.open_input_ns == 6'000'000);
}

TEST_CASE("Latency tracker: the present return drops a publish from inside the present",
          "[latency]") {
    State state;
    REQUIRE(NoteInput(state, 1'000'000));
    RearmAfterPresent(state);
    CHECK(state.open_input_ns == 0);
    REQUIRE(NoteInput(state, 2'000'000));
    CHECK(state.open_input_ns == 2'000'000);
}

TEST_CASE("Latency tracker: a clawback hold is not the next frame's input", "[latency]") {
    State state;
    state.hold_input = true;
    CHECK_FALSE(NoteInput(state, 1'000'000));
    CHECK(state.open_input_ns == 0);
    state.hold_input = false;
    REQUIRE(NoteInput(state, 2'000'000));
    CHECK(state.open_input_ns == 2'000'000);
}

TEST_CASE("Latency tracker: presents pair with guest frames in order", "[latency]") {
    State state;
    REQUIRE(NoteInput(state, 1'000'000));
    NoteSubmit(state, 5'000'000);
    REQUIRE(NoteInput(state, 10'000'000));
    NoteSubmit(state, 15'000'000);
    REQUIRE(NotePresent(state, 20'000'000, Interval));
    CHECK(state.pending_count == 1);
    CHECK(state.samples == 1);
    const Summary first = Summarize(state);
    CHECK(first.present.latest == 19'000'000);
    CHECK(first.queue.latest == 15'000'000);
    CHECK(first.cpu.latest == 4'000'000);
    // Half of 16.666 ms, integer division.
    CHECK(first.photon.latest == 19'000'000 + Interval / 2);
    CHECK(first.refresh_ns == Interval);

    REQUIRE(NotePresent(state, 30'000'000, 0));
    CHECK(state.pending_count == 0);
    const Summary both = Summarize(state);
    CHECK(both.samples == 2);
    CHECK(both.present.latest == 20'000'000);
    CHECK(both.queue.latest == 15'000'000);
    // The period from the first present still estimates scan-out.
    CHECK(both.photon.latest == 20'000'000 + Interval / 2);
}

TEST_CASE("Latency tracker: a present with no guest frame, or an early one, is not a sample",
          "[latency]") {
    State state;
    CHECK_FALSE(NotePresent(state, 10'000'000, Interval));
    REQUIRE(NoteInput(state, 1'000'000));
    NoteSubmit(state, 5'000'000);
    CHECK_FALSE(NotePresent(state, 4'000'000, Interval));
    CHECK(state.pending_count == 0);
    CHECK(state.samples == 0);

    NoteSubmit(state, 0);
    CHECK(state.pending_count == 0);
}

TEST_CASE("Latency tracker: a guest present without a pad publish does not become a sample",
          "[latency]") {
    State state;
    NoteSubmit(state, 5'000'000);
    CHECK_FALSE(NotePresent(state, 10'000'000, Interval));
    CHECK(state.samples == 0);
    CHECK(state.pending_count == 0);
}

TEST_CASE("Latency tracker: the window keeps the last 256 samples", "[latency]") {
    State state;
    for (int index = 0; index < 266; ++index) {
        const s64 input = static_cast<s64>(index + 1) * 1'000'000;
        REQUIRE(NoteInput(state, input));
        // Space submits so the interval filter does not matter to the window.
        NoteSubmit(state, input + 1'000'000);
        REQUIRE(NotePresent(state, input + 2'000'000, 0));
    }
    const Summary summary = Summarize(state);
    CHECK(summary.samples == 266);
    CHECK(state.window_count == Window);
    // Present latency is 2 ms for every frame. The latest is the 266th.
    CHECK(summary.present.latest == 2'000'000);
    CHECK(summary.present.p50 == 2'000'000);
    CHECK(summary.present.p99 == 2'000'000);
}

TEST_CASE("Latency tracker: percentiles of a rising present latency", "[latency]") {
    State state;
    for (int index = 0; index < 100; ++index) {
        const s64 input = 1'000'000'000 + static_cast<s64>(index) * Interval;
        REQUIRE(NoteInput(state, input));
        NoteSubmit(state, input + 1'000'000);
        const s64 present_latency = static_cast<s64>(index + 1) * 1'000'000;
        REQUIRE(NotePresent(state, input + present_latency, 0));
    }
    const Summary summary = Summarize(state);
    CHECK(summary.samples == 100);
    CHECK(summary.present.latest == 100'000'000);
    // Sorted 1..100 ms. Index (99 * percent) / 100.
    CHECK(summary.present.p50 == 50'000'000);
    CHECK(summary.present.p95 == 95'000'000);
    CHECK(summary.present.p99 == 99'000'000);
}

TEST_CASE("Latency clawback: one interval once the queue is over target", "[latency]") {
    State state = Trained();
    CHECK(state.intervals == WarmupIntervals);
    CHECK(state.samples == 9);
    CHECK(state.pending_count == 0);
    CHECK(state.interval_ewma == Interval);

    const s64 next = 1'000'000'000 + 9 * Interval;
    REQUIRE(NoteInput(state, next - Cpu));
    NoteSubmit(state, next);
    CHECK(ClawbackSleepNs(state, Target(1)) == 0);

    const s64 after = next + Interval;
    REQUIRE(NoteInput(state, after - Cpu));
    NoteSubmit(state, after);
    CHECK(state.pending_count == 2);
    CHECK(ClawbackSleepNs(state, Target(1)) == Interval);
    CHECK(ClawbackSleepNs(state, Target(2)) == 0);
    CHECK(ClawbackSleepNs(state, Target(1, 3'000'000)) == 3'000'000);
    CHECK(ClawbackSleepNs(state, Target(1, 100'000)) == 0);
    CHECK(ClawbackSleepNs(state, Config{}) == 0);
}

TEST_CASE("Latency clawback: a sleep does not lengthen the next interval", "[latency]") {
    State state = Trained();
    CHECK(state.interval_ewma == Interval);
    state.sleep_since_submit = 5'000'000;
    const s64 next = 1'000'000'000 + 9 * Interval + state.sleep_since_submit;
    REQUIRE(NoteInput(state, next - Cpu));
    NoteSubmit(state, next);
    CHECK(state.sleep_since_submit == 0);
    CHECK(state.interval_ewma == Interval);
    CHECK(state.intervals == WarmupIntervals + 1);
}

TEST_CASE("Latency clawback: no sleep before a matched present or a settled interval", "[latency]") {
    State state;
    state.intervals = WarmupIntervals;
    state.interval_ewma = Interval;
    state.pending_count = 3;
    state.samples = 0;
    CHECK(ClawbackSleepNs(state, Target(1)) == 0);

    state.samples = 1;
    state.intervals = WarmupIntervals - 1;
    CHECK(ClawbackSleepNs(state, Target(1)) == 0);
}

TEST_CASE("Latency clawback: unmatched presents stop the sleep", "[latency]") {
    State state;
    state.intervals = WarmupIntervals;
    state.interval_ewma = Interval;
    state.pending_count = 3;
    state.samples = 1;
    state.dropped = DropRunaway + 1;
    CHECK(ClawbackSleepNs(state, Target(1)) == 0);

    state.samples = DropRunaway + 2;
    CHECK(ClawbackSleepNs(state, Target(1)) == Interval);
}

TEST_CASE("Latency tracker: the pending queue drops the oldest past eight", "[latency]") {
    State state;
    for (u32 index = 0; index < PendingCap + 3; ++index) {
        const s64 submit = 1'000'000'000 + static_cast<s64>(index) * Interval;
        REQUIRE(NoteInput(state, submit - 1'000'000));
        NoteSubmit(state, submit);
    }
    CHECK(state.pending_count == PendingCap);
    CHECK(state.dropped == 3);
    REQUIRE(NotePresent(state, 1'000'000'000 + 3 * Interval + Queue, 0));
    // The three oldest were dropped, so the match is the fourth submit.
    const s64 fourth_input = 1'000'000'000 + 3 * Interval - 1'000'000;
    CHECK(Summarize(state).present.latest ==
          (1'000'000'000 + 3 * Interval + Queue) - fourth_input);
}
