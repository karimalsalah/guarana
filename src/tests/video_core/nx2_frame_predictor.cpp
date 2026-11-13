// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <random>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/nx2_frame_predictor.h"

namespace {

using namespace VideoCore::Nx2Predict;

struct Result {
    double mae_active{};
    double mae_ewma{};
    double mae_rls{};
    double coverage{}; ///< percent of intervals within the in-use p90
    size_t samples{};
    bool using_rls{};
    bool finite{true};
};

/// Runs a stream of intervals (ms) through a model and scores everything after `skip` samples.
/// `signal` gives the signals the frame ending interval i is recorded with.
template <typename SignalFor>
Result Run(IntervalModel& model, const std::vector<double>& stream, SignalFor&& signal,
           size_t skip = 200) {
    Result result;
    double sum_active = 0.0;
    double sum_rls = 0.0;
    double sum_ewma = 0.0;
    size_t covered = 0;
    for (size_t i = 0; i < stream.size(); ++i) {
        const Scored scored = model.Update(stream[i], signal(i));
        const Prediction now = model.Current();
        if (!std::isfinite(now.mean_ms) || !std::isfinite(now.p90_ms) || now.p90_ms < now.mean_ms) {
            result.finite = false;
        }
        if (scored.valid && i >= skip) {
            sum_active += scored.active_err_ms;
            sum_rls += scored.rls_err_ms;
            sum_ewma += scored.ewma_err_ms;
            covered += scored.covered ? 1 : 0;
            ++result.samples;
        }
    }
    const double n = static_cast<double>((std::max<size_t>)(result.samples, 1));
    result.mae_active = sum_active / n;
    result.mae_rls = sum_rls / n;
    result.mae_ewma = sum_ewma / n;
    result.coverage = 100.0 * static_cast<double>(covered) / n;
    result.using_rls = model.UsingRls();
    return result;
}

constexpr auto NoSignals = [](size_t) {
    return Signals{.gpu_latency_ms = 10.0, .slot_phase = 0.5};
};

std::vector<double> Noisy(size_t count, std::mt19937& rng, double sigma,
                          const std::function<double(size_t)>& mean) {
    std::normal_distribution<double> noise{0.0, sigma};
    std::vector<double> out(count);
    for (size_t i = 0; i < count; ++i) {
        out[i] = mean(i) + noise(rng);
    }
    return out;
}

constexpr size_t COUNT = 3000;

} // Anonymous namespace

TEST_CASE("FramePredictor model: a steady stream is no worse than the baseline", "[video_core]") {
    std::mt19937 rng{1};
    IntervalModel model;
    const auto stream = Noisy(COUNT, rng, 1.5, [](size_t) { return 45.0; });
    const Result result = Run(model, stream, NoSignals);
    REQUIRE(result.finite);
    REQUIRE(result.samples > 2000U);
    // Nothing to learn: the in-use model stays within a few percent of the EWMA, and both are
    // near the noise floor (the mean absolute deviation of 1.5 ms of noise is 1.2 ms).
    REQUIRE(result.mae_active <= result.mae_ewma * 1.1);
    REQUIRE(result.mae_active < 1.8);
    REQUIRE(result.coverage > 82.0);
    REQUIRE(result.coverage < 97.0);
    REQUIRE(model.Resets() == 0U);
}

TEST_CASE("FramePredictor model: alternating 33 and 50 ms beats the baseline", "[video_core]") {
    std::mt19937 rng{2};
    IntervalModel model;
    const auto stream =
        Noisy(COUNT, rng, 0.7, [](size_t i) { return i % 2 == 0 ? 33.0 : 50.0; });
    const Result result = Run(model, stream, NoSignals);
    REQUIRE(result.finite);
    // The EWMA lands near 41 ms and is wrong by about 8 ms every frame; the lags give the
    // next one away.
    REQUIRE(result.mae_ewma > 6.0);
    REQUIRE(result.mae_active < 0.3 * result.mae_ewma);
    REQUIRE(result.mae_active < 2.5);
    REQUIRE(result.using_rls);
    REQUIRE(result.coverage > 82.0);
    REQUIRE(result.coverage < 97.0);
}

TEST_CASE("FramePredictor model: a three frame cycle beats the baseline", "[video_core]") {
    std::mt19937 rng{3};
    IntervalModel model;
    constexpr std::array<double, 3> cycle{33.0, 33.0, 50.0};
    const auto stream = Noisy(COUNT, rng, 0.7, [&cycle](size_t i) { return cycle[i % 3]; });
    const Result result = Run(model, stream, NoSignals);
    REQUIRE(result.finite);
    REQUIRE(result.mae_ewma > 5.0);
    REQUIRE(result.mae_active < 0.4 * result.mae_ewma);
    REQUIRE(result.using_rls);
}

TEST_CASE("FramePredictor model: unpredictable spikes cost no more than the baseline",
          "[video_core]") {
    std::mt19937 rng{4};
    std::bernoulli_distribution spike{0.05};
    IntervalModel model;
    auto stream = Noisy(COUNT, rng, 1.5, [](size_t) { return 40.0; });
    for (double& interval : stream) {
        if (spike(rng)) {
            interval = 110.0;
        }
    }
    const Result result = Run(model, stream, NoSignals);
    REQUIRE(result.finite);
    // Spikes cannot be told ahead, but the baseline is also pulled up after each, which a
    // model that ignores them need not be.
    REQUIRE(result.mae_active <= result.mae_ewma * 1.05);
    REQUIRE(result.coverage > 80.0);
    REQUIRE(result.coverage < 97.0);
}

TEST_CASE("FramePredictor model: a read-back wait that precedes a spike is learned",
          "[video_core]") {
    std::mt19937 rng{5};
    std::bernoulli_distribution wait{0.08};
    std::vector<bool> waits(COUNT);
    for (size_t i = 0; i < COUNT; ++i) {
        waits[i] = wait(rng);
    }
    // The frame recorded after a wait is followed by a long interval.
    const auto stream = Noisy(COUNT, rng, 1.0, [&waits](size_t i) {
        return 40.0 + (i > 0 && waits[i - 1] ? 60.0 : 0.0);
    });
    const auto signals = [&waits](size_t i) {
        return Signals{.gpu_latency_ms = 10.0,
                       .read_wait_ms = waits[i] ? 40.0 : 0.0,
                       .read_waits = waits[i] ? 1.0 : 0.0,
                       .slot_phase = 0.5};
    };
    IntervalModel model;
    const Result result = Run(model, stream, signals);
    REQUIRE(result.finite);
    REQUIRE(result.mae_active < 0.35 * result.mae_ewma);
    REQUIRE(result.using_rls);
}

TEST_CASE("FramePredictor model: a change of level is followed", "[video_core]") {
    std::mt19937 rng{6};
    IntervalModel model;
    auto stream = Noisy(600, rng, 1.0, [](size_t i) { return i < 300 ? 33.0 : 60.0; });
    for (size_t i = 0; i < 330; ++i) {
        model.Update(stream[i], NoSignals(i));
    }
    // 30 frames after the change the prediction is on the new level.
    REQUIRE(std::abs(model.Current().mean_ms - 60.0) < 4.0);
    REQUIRE(model.Current().p90_ms >= model.Current().mean_ms);
}

TEST_CASE("FramePredictor model: garbage never makes it diverge", "[video_core]") {
    std::mt19937 rng{7};
    std::uniform_int_distribution<int> pick{0, 9};
    std::uniform_real_distribution<double> any{-1.0e6, 1.0e6};
    constexpr double nan = std::numeric_limits<double>::quiet_NaN();
    constexpr double inf = std::numeric_limits<double>::infinity();
    IntervalModel model;
    for (size_t i = 0; i < 20000; ++i) {
        double interval = 0.0;
        switch (pick(rng)) {
        case 0:
            interval = 0.6;
            break;
        case 1:
            interval = 1000.0;
            break;
        case 2:
            interval = nan;
            break;
        case 3:
            interval = inf;
            break;
        case 4:
            interval = -5.0;
            break;
        case 5:
            interval = any(rng);
            break;
        default:
            interval = 45.0;
            break;
        }
        const Signals signals{.gpu_latency_ms = pick(rng) == 0 ? any(rng) : 12.0,
                              .read_wait_ms = pick(rng) == 0 ? inf : 0.0,
                              .read_waits = pick(rng) == 0 ? nan : 0.0,
                              .in_flight = pick(rng) == 0 ? any(rng) : 2.0,
                              .slot_phase = pick(rng) == 0 ? any(rng) : 0.3};
        model.Update(interval, signals);
        const Prediction now = model.Current();
        REQUIRE(std::isfinite(now.mean_ms));
        REQUIRE(std::isfinite(now.p90_ms));
        REQUIRE(now.p90_ms >= now.mean_ms);
        REQUIRE(now.mean_ms <= 1500.0);
        REQUIRE(model.MaxWeight() <= 4.0 + 1.0e-9);
        REQUIRE(model.MaxCovariance() <= 25.0 + 1.0e-6);
    }
    // And the model is useful again once the stream is.
    IntervalModel after = std::move(model);
    after.BreakStream();
    const auto stream = Noisy(1000, rng, 1.0, [](size_t) { return 45.0; });
    const Result result = Run(after, stream, NoSignals, 400);
    REQUIRE(result.finite);
    REQUIRE(result.mae_active < 3.0);
}

TEST_CASE("FramePredictor model: a pause is not an interval", "[video_core]") {
    IntervalModel model;
    for (int i = 0; i < 100; ++i) {
        model.Update(45.0, NoSignals(0));
    }
    const u64 samples = model.Samples();
    model.BreakStream();
    REQUIRE(model.Current().mean_ms == 0.0);
    // The first interval after a break has no prediction to score.
    REQUIRE_FALSE(model.Update(45.0, NoSignals(0)).valid);
    REQUIRE(model.Update(45.0, NoSignals(0)).valid);
    REQUIRE(model.Samples() == samples + 1);
}

namespace {

/// Frames every `interval_ns` from 1 s on, through the 6 argument OnRealFrame.
void Feed(FramePredictor& predictor, size_t frames, s64 interval_ns, s64& now_ns) {
    for (size_t i = 0; i < frames; ++i) {
        predictor.OnRealFrame(now_ns, 12'000'000, 2, 0.5, 0, 0);
        now_ns += interval_ns;
    }
}

} // Anonymous namespace

TEST_CASE("FramePredictor: Cover is the schedule's own until the model is ready", "[video_core]") {
    FramePredictor predictor{2};
    s64 now = 1'000'000'000;
    Feed(predictor, 5, 45'000'000, now);
    REQUIRE(predictor.Cover(50'000'000) == 50'000'000);
    Feed(predictor, 60, 45'000'000, now);
    const s64 cover = predictor.Cover(50'000'000);
    // The p90 of a steady 45 ms stream, not the 50 ms the schedule had.
    REQUIRE(cover >= 44'000'000);
    REQUIRE(cover <= 60'000'000);
    REQUIRE(predictor.GetSnapshot().samples >= 60U);
    // Never more than three times, never less than half of what the schedule uses.
    REQUIRE(predictor.Cover(1'000'000) <= 3'000'000);
    REQUIRE(predictor.Cover(400'000'000) >= 200'000'000);
}

TEST_CASE("FramePredictor: a gap over half a second restarts the stream", "[video_core]") {
    FramePredictor predictor{2};
    s64 now = 1'000'000'000;
    Feed(predictor, 80, 45'000'000, now);
    const u64 samples = predictor.GetSnapshot().samples;
    now += 3'000'000'000;
    Feed(predictor, 1, 45'000'000, now);
    REQUIRE(predictor.GetSnapshot().samples == samples);
    REQUIRE(predictor.GetSnapshot().next_mean_ms == 0.0);
    // Frames recorded twice within a millisecond count once.
    s64 twice = now;
    predictor.OnRealFrame(twice, 12'000'000, 2, 0.5, 0, 0);
    predictor.OnRealFrame(twice + 100'000, 12'000'000, 2, 0.5, 0, 0);
    REQUIRE(predictor.GetSnapshot().samples == samples);
}

TEST_CASE("FramePredictor: the plan reaches as far as the model asks, within bounds",
          "[video_core]") {
    FramePredictor schedule{2};
    FramePredictor observe{3};
    FramePredictor off{0};
    REQUIRE(schedule.Schedules());
    REQUIRE_FALSE(observe.Schedules());
    REQUIRE_FALSE(off.Enabled());
    // Slots 10, 12, ..: the schedule reaches 14 without the model, the model asks for 20, and
    // two slots more than the schedule's is the most that is recorded.
    REQUIRE(schedule.PlanSlots(1, 10, 14, 20, 2, 6) == 18U);
    // The model asks for less: fewer predictions.
    REQUIRE(schedule.PlanSlots(2, 10, 14, 12, 2, 6) == 12U);
    // Never before the first slot, never past the cap on predictions.
    REQUIRE(schedule.PlanSlots(3, 10, 14, 4, 2, 6) == 10U);
    REQUIRE(schedule.PlanSlots(4, 10, 24, 40, 2, 3) == 14U);
    // Observing leaves the schedule as it is.
    REQUIRE(observe.PlanSlots(1, 10, 14, 20, 2, 6) == 14U);
}

TEST_CASE("FramePredictor: slot outcomes count the misses the plan avoids", "[video_core]") {
    FramePredictor schedule{2};
    FramePredictor observe{3};
    for (FramePredictor* predictor : {&schedule, &observe}) {
        // Batch 1: baseline through slot 14, model through 18.
        REQUIRE(predictor->PlanSlots(1, 10, 14, 20, 2, 6) ==
                (predictor == &schedule ? u64{18} : u64{14}));
        predictor->NoteSlot(10, 1, 1'000'000'000);  // on the plan
        predictor->NoteSlot(14, 1, 1'010'000'000);  // the baseline's last slot
        predictor->NoteSlot(16, 1, 1'020'000'000);  // past the baseline's: avoided if shown
        predictor->NoteSlot(18, 0, 1'030'000'000);  // a miss inside the model's reach
        predictor->NoteSlot(12, 0, 1'040'000'000);  // a miss the baseline would also have had
        predictor->NoteSlot(20, 0, 1'050'000'000);  // a miss past both
    }
    const Snapshot scheduled = schedule.GetSnapshot();
    REQUIRE(scheduled.total.slots == 6U);
    REQUIRE(scheduled.total.misses == 3U);
    REQUIRE(scheduled.total.avoided == 1U);
    REQUIRE(scheduled.total.avoidable == 1U);
    // Observing counts the same slots; what the plan would have avoided is what it records.
    REQUIRE(observe.GetSnapshot().total.avoidable == 1U);
}

TEST_CASE("FramePredictor: the log window closes after ten seconds", "[video_core]") {
    FramePredictor predictor{3};
    s64 now = 1'000'000'000;
    Feed(predictor, 100, 45'000'000, now);
    predictor.NoteSlot(2, 0, now);
    REQUIRE(predictor.GetSnapshot().last.samples == 0U);
    predictor.NoteSlot(4, 0, now + 10'500'000'000);
    const Snapshot snapshot = predictor.GetSnapshot();
    REQUIRE(snapshot.last.samples > 90U);
    REQUIRE(snapshot.last.slots == 2U);
    REQUIRE(snapshot.last.misses == 2U);
    // A steady stream: both models are close, and the p90 of the error is small.
    REQUIRE(snapshot.last.mae_ewma_ms < 1.0);
    REQUIRE(snapshot.last.p90_ewma_ms < 2.0);
    // The window starts over; the totals do not.
    predictor.NoteSlot(6, 0, now + 10'600'000'000);
    REQUIRE(predictor.GetSnapshot().total.slots == 3U);
}

TEST_CASE("FramePredictor: holding for a nearly done frame", "[video_core]") {
    FramePredictor hold{1};
    FramePredictor schedule_only{2};
    // Recorded at 100 ms, expected done 12 ms later: at 111 ms it is 1 ms away.
    constexpr s64 recorded = 100'000'000;
    constexpr s64 latency = 12'000'000;
    REQUIRE(schedule_only.HoldBudget(111'000'000, recorded, latency) == 0);
    const s64 budget = hold.HoldBudget(111'000'000, recorded, latency);
    REQUIRE(budget >= 500'000);
    REQUIRE(budget <= 2'000'000);
    // Overdue: a short wait, not none.
    REQUIRE(hold.HoldBudget(115'000'000, recorded, latency) > 0);
    // Too far to wait for.
    REQUIRE(hold.HoldBudget(100'000'000, recorded, latency) == 0);
    // Timeouts make it shorter, successes longer.
    const double cap = hold.GetSnapshot().hold_cap_ms;
    for (int i = 0; i < 30; ++i) {
        hold.HoldResult(false, 1'000'000);
    }
    const double shrunk = hold.GetSnapshot().hold_cap_ms;
    REQUIRE(shrunk < cap);
    REQUIRE(shrunk >= 0.3);
    for (int i = 0; i < 200; ++i) {
        hold.HoldResult(true, 500'000);
    }
    REQUIRE(hold.GetSnapshot().hold_cap_ms > shrunk);
    REQUIRE(hold.GetSnapshot().hold_cap_ms <= 2.0);
    const Snapshot snapshot = hold.GetSnapshot();
    REQUIRE(snapshot.total.holds == 230U);
    REQUIRE(snapshot.total.landed == 200U);
    REQUIRE(snapshot.total.timeouts == 30U);
}
