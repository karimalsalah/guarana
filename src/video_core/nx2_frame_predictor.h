// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <mutex>
#include <vector>

#include "common/common_types.h"

// EDEN_NX2_PREDICT (experimental, off): a small model that learns, while the game runs, when the
// next real frame will be recorded, and the FLUID presenter (EDEN_NX2_FLUID) uses that to
// schedule its predicted frames.
//
// The model (IntervalModel) predicts the next real-frame interval, in ms, from the last intervals,
// the GPU latency of recent frames, the guest's read-back waits, the GPU ticks in flight and
// where the frame fell in its slot. Two predictors run side by side on every frame and the one
// with the lower recent error is used (with hysteresis):
// - the EWMA baseline;
// - recursive least squares (forgetting factor) on the residual over the EWMA, so that a model
//   that has learned nothing is the baseline. Updates are Huber-clipped, the covariance is
//   bounded, and anything not finite resets it.
// Each also keeps a p90 offset by stochastic descent of the pinball loss.
//
// FramePredictor wraps it for the renderer (RecordFluidFrames: feed a frame, plan how far the
// predictions reach) and the present thread (FluidPresentSlot: count slot outcomes, decide a short
// wait for a real frame that is nearly done, log every 10 s). Times are plain nanoseconds so the
// model and the planning can be tested without a renderer.
//
// EDEN_NX2_PREDICT values: unset or 0 off; 1 schedule and hold; 2 schedule only; 3 learn and log
// only (the schedule is unchanged; the log counts the misses the predictor's schedule would have
// avoided).
namespace VideoCore::Nx2Predict {

inline constexpr size_t LAGS = 6;
inline constexpr size_t FEATURES = 8 + LAGS;

/// What is known when a real frame is recorded, besides its time.
struct Signals {
    double gpu_latency_ms{}; ///< from recording a real frame to its GPU work done (smoothed)
    double read_wait_ms{};   ///< guest read-back waits since the last frame
    double read_waits{};     ///< and their number
    double in_flight{};      ///< GPU ticks submitted and not finished
    double slot_phase{};     ///< 0 to 1: where in its slot the frame was recorded
};

struct Prediction {
    double mean_ms{};
    double p90_ms{}; ///< at least mean_ms
};

/// How the standing prediction did against the interval that followed.
struct Scored {
    bool valid{};
    double interval_ms{};
    double active_err_ms{}; ///< absolute, the model in use
    double rls_err_ms{};
    double ewma_err_ms{};
    bool covered{}; ///< the interval was within the in-use model's p90
};

class IntervalModel {
public:
    static constexpr u64 WARMUP = 24;

    IntervalModel();

    /// Forget the stream (a pause) but keep what the weights learned.
    void BreakStream();
    /// Forget everything.
    void Reset();

    /// An interval ended (ms) and the frame that ended it was recorded with `signals`: score the
    /// standing prediction, learn from it, predict the next interval.
    Scored Update(double interval_ms, const Signals& signals);

    /// The prediction for the next interval, from the model in use; {0, 0} before the second
    /// interval.
    [[nodiscard]] Prediction Current() const;
    [[nodiscard]] Prediction Rls() const;
    [[nodiscard]] Prediction Ewma() const;
    [[nodiscard]] bool Ready() const {
        return predicted && samples >= WARMUP;
    }
    [[nodiscard]] bool UsingRls() const {
        return use_rls;
    }
    [[nodiscard]] u64 Samples() const {
        return samples;
    }
    [[nodiscard]] u64 Switches() const {
        return switches;
    }
    [[nodiscard]] u64 Resets() const {
        return resets;
    }
    [[nodiscard]] double MeanIntervalMs() const {
        return ewma;
    }
    /// For tests: the largest weight and covariance entry.
    [[nodiscard]] double MaxWeight() const;
    [[nodiscard]] double MaxCovariance() const;

private:
    using Vector = std::array<double, FEATURES>;

    struct Quantile {
        double offset_ms{};
        double mean_abs_ms{2.0};
    };

    [[nodiscard]] bool Finite() const;
    void BuildFeatures(const Signals& signals, Vector& x) const;
    void LearnRls(const Vector& x, double target);
    static void LearnQuantile(Quantile& quantile, double err_ms);

    // Interval history, newest first, ms.
    std::array<double, LAGS> history{};
    size_t history_count{};
    double ewma{};
    double last_latency_ms{};

    // The standing prediction, made at the last frame.
    bool predicted{};
    Vector features{};
    double base_ms{};
    double rls_ms{};
    Quantile rls_quantile;
    Quantile ewma_quantile;

    // RLS
    Vector weights{};
    std::array<Vector, FEATURES> covariance{};
    double rls_scale{0.1}; ///< mean absolute normalized error, for the Huber clip

    // Selection: recent mean absolute error in ms of each, negative before a sample
    double rls_err{-1.0};
    double ewma_err{-1.0};
    bool use_rls{};
    u64 samples{};
    u64 switches{};
    u64 resets{};
};

/// Errors and slot outcomes over a stretch of time, for the log and the debug server.
struct Window {
    u64 samples{};
    double mae_active_ms{};
    double mae_rls_ms{};
    double mae_ewma_ms{};
    double p90_active_ms{}; ///< 90th percentile of the absolute error
    double p90_rls_ms{};
    double p90_ewma_ms{};
    double coverage_percent{}; ///< intervals within the in-use p90 prediction
    u64 slots{};
    u64 misses{};    ///< slots with no frame ready
    u64 avoided{};   ///< slots shown by a prediction past the baseline's reach
    u64 avoidable{}; ///< misses inside the predictor's reach and past the baseline's
    u64 holds{};
    u64 landed{};
    u64 timeouts{};
    double waited_ms{};
};

struct Snapshot {
    u32 mode{};
    bool using_rls{};
    u64 samples{};
    u64 switches{};
    u64 resets{};
    double mean_interval_ms{};
    double next_mean_ms{};
    double next_p90_ms{};
    double hold_cap_ms{};
    Window total; ///< since the game started (the p90s are zero)
    Window last;  ///< the last complete 10 s window
};

class FramePredictor {
public:
    static FramePredictor& Instance();

    /// EDEN_NX2_PREDICT, read at the first call.
    [[nodiscard]] static u32 Mode();

    /// For tests: a predictor in `mode`, not the process's.
    explicit FramePredictor(u32 mode_);

    [[nodiscard]] bool Enabled() const {
        return mode != 0;
    }
    /// Predictions reach as far as the model says.
    [[nodiscard]] bool Schedules() const {
        return mode == 1 || mode == 2;
    }
    [[nodiscard]] bool Holds() const {
        return mode == 1;
    }

    /// Renderer thread, at the start of every real frame's FLUID recording. `latency_ns` is the
    /// presenter's smoothed GPU latency, `in_flight` the GPU ticks submitted and not finished.
    /// `read_wait_ns`, `read_waits` and `gpu_wait_ns` are the running totals of the guest's
    /// read-back waits (nx2_async_readback.h).
    void OnRealFrame(s64 now_ns, s64 latency_ns, u64 in_flight, double slot_phase);
    /// The same with the read-back totals given.
    void OnRealFrame(s64 now_ns, s64 latency_ns, u64 in_flight, double slot_phase,
                     u64 read_wait_ns, u64 read_waits);

    /// Renderer thread: how far past recording + latency the predictions should reach, in ns.
    /// `base_ns` is what the schedule uses without the predictor; it is returned until the model
    /// is ready.
    [[nodiscard]] s64 Cover(s64 base_ns) const;

    /// Renderer thread: the last slot to predict for. `base_last` is what the schedule reaches
    /// without the predictor, `wanted_last` what Cover asks for; slots step by `divisor` from
    /// `first` and at most `max_predictions` are recorded. Registers the plan for the slot
    /// counts, and returns the slot to use.
    [[nodiscard]] u64 PlanSlots(u64 batch, u64 first, u64 base_last, u64 wanted_last, u64 divisor,
                                size_t max_predictions);

    /// Present thread, once per counted slot: `batch` is the batch of the frame the slot shows,
    /// 0 for none. Logs every 10 s.
    void NoteSlot(u64 slot, u64 batch, s64 now_ns);

    /// Present thread: how long to wait (ns, 0 for none) for a real frame recorded at
    /// `recorded_ns` whose GPU work is expected done `latency_ns` later.
    [[nodiscard]] s64 HoldBudget(s64 now_ns, s64 recorded_ns, s64 latency_ns) const;
    void HoldResult(bool landed, s64 waited_ns);

    [[nodiscard]] Snapshot GetSnapshot() const;

private:
    FramePredictor();

    struct Plan {
        u64 batch{};
        u64 first{};
        u64 base_last{};
        u64 pred_last{};
    };
    struct Accumulator {
        u64 n{};
        double sum_active{};
        double sum_rls{};
        double sum_ewma{};
        u64 covered{};
        std::vector<float> active, rls, ewma;
        u64 slots{}, misses{}, avoided{}, avoidable{}, holds{}, landed{}, timeouts{};
        double waited_ms{};
    };

    static Window Summarize(Accumulator& acc, bool percentiles);
    void Log(s64 now_ns);
    [[nodiscard]] const Plan* PlanOf(u64 batch) const;
    [[nodiscard]] const Plan* NewestPlanAtOrBefore(u64 slot) const;

    const u32 mode;
    mutable std::mutex mutex;
    IntervalModel model;
    s64 last_record_ns{};
    u64 read_wait_ns_seen{};
    u64 read_waits_seen{};
    std::array<Plan, 16> plans{};
    size_t plan_next{};
    Accumulator window;
    Accumulator total;
    Window last_window;
    s64 window_start_ns{};
    double hold_cap_ms{1.0};
    double hold_success{0.7};
};

} // namespace VideoCore::Nx2Predict
