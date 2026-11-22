// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "common/logging.h"
#include "video_core/nx2_async_readback.h"
#include "video_core/nx2_frame_predictor.h"

namespace VideoCore::Nx2Predict {

namespace {

/// Features and the residual are in units of this many ms, so that weights stay near 1.
constexpr double SCALE_MS = 20.0;
/// The EWMA baseline's weight on the newest interval.
constexpr double EWMA_ALPHA = 0.25;
/// RLS forgetting factor: the model follows roughly the last hundred frames.
constexpr double FORGET = 0.99;
/// Initial covariance (ridge strength 1/COVARIANCE_START) and its bound per entry.
constexpr double COVARIANCE_START = 10.0;
constexpr double COVARIANCE_MAX = 25.0;
constexpr double WEIGHT_MAX = 4.0;
/// Weight of the newest sample in each model's recent error.
constexpr double ERROR_DECAY = 0.06;
/// The RLS model takes over when its recent error is this much lower than the baseline's, and
/// gives back when it is 5% worse.
constexpr double SWITCH_IN = 0.9;
constexpr double SWITCH_OUT = 1.05;
constexpr double QUANTILE = 0.9;
/// A gap between real frames this long is a pause, not an interval.
constexpr double GAP_LIMIT_MS = 500.0;
/// Frames closer than this are one frame recorded twice.
constexpr double MIN_GAP_MS = 1.0;
/// Predictions reach at most this many slots past the baseline's.
constexpr u64 EXTRA_SLOTS = 2;
constexpr double HOLD_SLACK_MS = 0.3;
constexpr double HOLD_CAP_MIN_MS = 0.3;
constexpr double HOLD_CAP_MAX_MS = 2.0;
constexpr s64 WINDOW_NS = 10'000'000'000;

/// Not a number gives `low`.
constexpr double Clamp(double value, double low, double high) {
    return value > high ? high : (value >= low ? value : low);
}

bool IsFinite(double value) {
    return std::isfinite(value);
}

} // Anonymous namespace

IntervalModel::IntervalModel() {
    for (size_t i = 0; i < FEATURES; ++i) {
        covariance[i][i] = COVARIANCE_START;
    }
}

void IntervalModel::BreakStream() {
    history_count = 0;
    ewma = 0.0;
    predicted = false;
}

void IntervalModel::Reset() {
    const u64 count = resets + 1;
    *this = IntervalModel();
    resets = count;
}

bool IntervalModel::Finite() const {
    if (!IsFinite(ewma) || !IsFinite(base_ms) || !IsFinite(rls_ms) || !IsFinite(rls_err) ||
        !IsFinite(ewma_err) || !IsFinite(rls_scale) || !IsFinite(rls_quantile.offset_ms) ||
        !IsFinite(ewma_quantile.offset_ms) || !IsFinite(rls_quantile.mean_abs_ms) ||
        !IsFinite(ewma_quantile.mean_abs_ms)) {
        return false;
    }
    for (size_t i = 0; i < FEATURES; ++i) {
        if (!IsFinite(weights[i])) {
            return false;
        }
        for (size_t j = 0; j < FEATURES; ++j) {
            if (!IsFinite(covariance[i][j])) {
                return false;
            }
        }
    }
    return true;
}

double IntervalModel::MaxWeight() const {
    double largest = 0.0;
    for (const double weight : weights) {
        largest = (std::max)(largest, std::abs(weight));
    }
    return largest;
}

double IntervalModel::MaxCovariance() const {
    double largest = 0.0;
    for (const auto& row : covariance) {
        for (const double value : row) {
            largest = (std::max)(largest, std::abs(value));
        }
    }
    return largest;
}

void IntervalModel::BuildFeatures(const Signals& signals, Vector& x) const {
    x[0] = 1.0;
    for (size_t k = 0; k < LAGS; ++k) {
        const double value = k < history_count ? history[k] : ewma;
        x[1 + k] = Clamp((value - ewma) / SCALE_MS, -5.0, 5.0);
    }
    const double latency = Clamp(signals.gpu_latency_ms, 0.0, 100.0);
    x[7] = latency / SCALE_MS;
    x[8] = Clamp((latency - last_latency_ms) / SCALE_MS, -2.0, 2.0);
    x[9] = Clamp(signals.read_wait_ms, 0.0, 200.0) / SCALE_MS;
    x[10] = Clamp(signals.read_waits, 0.0, 8.0) / 4.0;
    x[11] = Clamp(signals.in_flight, 0.0, 16.0) / 8.0;
    x[12] = Clamp(signals.slot_phase, 0.0, 1.0) - 0.5;
    x[13] = Clamp((ewma - 40.0) / SCALE_MS, -5.0, 5.0);
}

void IntervalModel::LearnRls(const Vector& x, double target) {
    std::array<double, FEATURES> gain{};
    double quadratic = 0.0;
    for (size_t i = 0; i < FEATURES; ++i) {
        double sum = 0.0;
        for (size_t j = 0; j < FEATURES; ++j) {
            sum += covariance[i][j] * x[j];
        }
        gain[i] = sum;
        quadratic += sum * x[i];
    }
    // Without excitation nothing is learned and nothing is forgotten, so the covariance does not
    // grow along directions the stream never visits.
    const double forget = quadratic < 1.0e-4 ? 1.0 : FORGET;
    const double denominator = forget + quadratic;
    if (!(denominator > 1.0e-9) || !IsFinite(denominator)) {
        return;
    }
    double estimate = 0.0;
    for (size_t i = 0; i < FEATURES; ++i) {
        estimate += weights[i] * x[i];
    }
    double error = target - estimate;
    // Huber: a spike is mostly unpredictable and moves the weights by no more than a few typical
    // errors.
    const double clip = 3.0 * rls_scale + 0.05;
    rls_scale += 0.03 * ((std::min)(std::abs(error), 4.0 * rls_scale + 0.2) - rls_scale);
    error = Clamp(error, -clip, clip);
    for (size_t i = 0; i < FEATURES; ++i) {
        weights[i] = Clamp(weights[i] + gain[i] / denominator * error, -WEIGHT_MAX, WEIGHT_MAX);
    }
    for (size_t i = 0; i < FEATURES; ++i) {
        for (size_t j = i; j < FEATURES; ++j) {
            const double value = (covariance[i][j] - gain[i] * gain[j] / denominator) / forget;
            covariance[i][j] = value;
            covariance[j][i] = value;
        }
    }
    // Bound each entry's growth; scaling row and column i together keeps it positive.
    std::array<double, FEATURES> shrink{};
    bool any = false;
    for (size_t i = 0; i < FEATURES; ++i) {
        shrink[i] = 1.0;
        const double diagonal = covariance[i][i];
        if (diagonal > COVARIANCE_MAX) {
            shrink[i] = std::sqrt(COVARIANCE_MAX / diagonal);
            any = true;
        } else if (diagonal < 0.0) {
            // Rounding broke positivity: start this direction over.
            for (size_t j = 0; j < FEATURES; ++j) {
                covariance[i][j] = covariance[j][i] = 0.0;
            }
            covariance[i][i] = COVARIANCE_START;
        }
    }
    if (any) {
        for (size_t i = 0; i < FEATURES; ++i) {
            for (size_t j = 0; j < FEATURES; ++j) {
                covariance[i][j] *= shrink[i] * shrink[j];
            }
        }
    }
}

void IntervalModel::LearnQuantile(Quantile& quantile, double err_ms) {
    quantile.mean_abs_ms += 0.05 * (std::abs(err_ms) - quantile.mean_abs_ms);
    // Stochastic descent of the pinball loss for the QUANTILE of the error.
    const double step = 0.15 * (quantile.mean_abs_ms + 0.5);
    quantile.offset_ms += step * (QUANTILE - (err_ms < quantile.offset_ms ? 1.0 : 0.0));
    quantile.offset_ms = Clamp(quantile.offset_ms, 0.0, 500.0);
}

Scored IntervalModel::Update(double interval_ms, const Signals& signals) {
    Scored out;
    if (!IsFinite(interval_ms) || interval_ms <= 0.0) {
        BreakStream();
        return out;
    }
    interval_ms = Clamp(interval_ms, 0.5, 1000.0);
    if (predicted) {
        const Prediction rls = Rls();
        const Prediction base = Ewma();
        const double rls_signed = interval_ms - rls_ms;
        const double ewma_signed = interval_ms - base_ms;
        out.valid = true;
        out.interval_ms = interval_ms;
        out.rls_err_ms = std::abs(rls_signed);
        out.ewma_err_ms = std::abs(ewma_signed);
        out.active_err_ms = use_rls ? out.rls_err_ms : out.ewma_err_ms;
        out.covered = interval_ms <= (use_rls ? rls.p90_ms : base.p90_ms);
        rls_err = rls_err < 0.0 ? out.rls_err_ms
                                : rls_err + ERROR_DECAY * (out.rls_err_ms - rls_err);
        ewma_err = ewma_err < 0.0 ? out.ewma_err_ms
                                  : ewma_err + ERROR_DECAY * (out.ewma_err_ms - ewma_err);
        LearnQuantile(rls_quantile, rls_signed);
        LearnQuantile(ewma_quantile, ewma_signed);
        LearnRls(features, ewma_signed / SCALE_MS);
        ++samples;
        if (samples >= WARMUP) {
            if (!use_rls && rls_err < SWITCH_IN * ewma_err) {
                use_rls = true;
                ++switches;
            } else if (use_rls && rls_err > SWITCH_OUT * ewma_err) {
                use_rls = false;
                ++switches;
            }
        }
    }
    for (size_t k = LAGS - 1; k > 0; --k) {
        history[k] = history[k - 1];
    }
    history[0] = interval_ms;
    history_count = (std::min)(history_count + 1, LAGS);
    ewma = ewma > 0.0 ? ewma + EWMA_ALPHA * (interval_ms - ewma) : interval_ms;

    BuildFeatures(signals, features);
    last_latency_ms = Clamp(signals.gpu_latency_ms, 0.0, 100.0);
    double dot = 0.0;
    for (size_t i = 0; i < FEATURES; ++i) {
        dot += weights[i] * features[i];
    }
    base_ms = ewma;
    rls_ms = Clamp(base_ms + SCALE_MS * dot, (std::max)(0.3 * base_ms, 1.0),
                   (std::min)(3.0 * base_ms, 500.0));
    predicted = true;
    if (!Finite()) {
        Reset();
    }
    return out;
}

Prediction IntervalModel::Rls() const {
    if (!predicted) {
        return {};
    }
    return {rls_ms, rls_ms + (std::max)(rls_quantile.offset_ms, 0.0)};
}

Prediction IntervalModel::Ewma() const {
    if (!predicted) {
        return {};
    }
    return {base_ms, base_ms + (std::max)(ewma_quantile.offset_ms, 0.0)};
}

Prediction IntervalModel::Current() const {
    return use_rls ? Rls() : Ewma();
}

// ----------------------------------------------------------------------------------------------

u32 FramePredictor::Mode() {
    static const u32 value = [] {
        const char* text = std::getenv("EDEN_NX2_PREDICT");
        if (text == nullptr || *text == '\0') {
            return u32{0};
        }
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(text, &end, 10);
        if (end == text) {
            return u32{0};
        }
        return parsed > 3 ? u32{1} : static_cast<u32>(parsed);
    }();
    return value;
}

FramePredictor::FramePredictor() : mode{Mode()} {}

FramePredictor::FramePredictor(u32 mode_) : mode{mode_} {}

FramePredictor& FramePredictor::Instance() {
    static FramePredictor instance;
    return instance;
}

void FramePredictor::OnRealFrame(s64 now_ns, s64 latency_ns, u64 in_flight, double slot_phase) {
    const auto& counters = VideoCore::AsyncReadback::g_counters;
    const u64 wait_ns = counters.reader_wait_ns.load(std::memory_order_relaxed) +
                        counters.gpu_side_wait_ns.load(std::memory_order_relaxed);
    const u64 waits = counters.reader_waits.load(std::memory_order_relaxed) +
                      counters.gpu_side_waits.load(std::memory_order_relaxed);
    OnRealFrame(now_ns, latency_ns, in_flight, slot_phase, wait_ns, waits);
}

void FramePredictor::OnRealFrame(s64 now_ns, s64 latency_ns, u64 in_flight, double slot_phase,
                                 u64 read_wait_ns, u64 read_waits) {
    std::scoped_lock lock{mutex};
    const u64 wait_delta = read_wait_ns >= read_wait_ns_seen ? read_wait_ns - read_wait_ns_seen : 0;
    const u64 waits_delta = read_waits >= read_waits_seen ? read_waits - read_waits_seen : 0;
    read_wait_ns_seen = read_wait_ns;
    read_waits_seen = read_waits;
    if (last_record_ns == 0) {
        last_record_ns = now_ns;
        return;
    }
    const double gap_ms = static_cast<double>(now_ns - last_record_ns) * 1.0e-6;
    if (gap_ms < MIN_GAP_MS) {
        return;
    }
    last_record_ns = now_ns;
    if (gap_ms > GAP_LIMIT_MS) {
        model.BreakStream();
        return;
    }
    const Signals signals{
        .gpu_latency_ms = static_cast<double>(latency_ns) * 1.0e-6,
        .read_wait_ms = static_cast<double>(wait_delta) * 1.0e-6,
        .read_waits = static_cast<double>(waits_delta),
        .in_flight = static_cast<double>(in_flight),
        .slot_phase = slot_phase,
    };
    const Scored scored = model.Update(gap_ms, signals);
    if (!scored.valid) {
        return;
    }
    for (Accumulator* acc : {&window, &total}) {
        ++acc->n;
        acc->sum_active += scored.active_err_ms;
        acc->sum_rls += scored.rls_err_ms;
        acc->sum_ewma += scored.ewma_err_ms;
        acc->covered += scored.covered ? 1 : 0;
    }
    if (window.active.size() < 4096) {
        window.active.push_back(static_cast<float>(scored.active_err_ms));
        window.rls.push_back(static_cast<float>(scored.rls_err_ms));
        window.ewma.push_back(static_cast<float>(scored.ewma_err_ms));
    }
}

s64 FramePredictor::Cover(s64 base_ns) const {
    std::scoped_lock lock{mutex};
    if (!model.Ready()) {
        return base_ns;
    }
    const double p90_ns = model.Current().p90_ms * 1.0e6;
    // Within a factor of the interval the schedule already uses: a model gone wrong costs a few
    // predictions, not the display.
    return static_cast<s64>(Clamp(p90_ns, 0.5 * static_cast<double>(base_ns),
                                  3.0 * static_cast<double>(base_ns)));
}

u64 FramePredictor::PlanSlots(u64 batch, u64 first, u64 base_last, u64 wanted_last, u64 divisor,
                              size_t max_predictions) {
    divisor = (std::max<u64>)(divisor, 1);
    const u64 reach = first + (std::max<u64>)(max_predictions, 1) * divisor - divisor;
    const u64 base = (std::min)(base_last, reach);
    u64 predicted_last = (std::min)(wanted_last, reach);
    predicted_last = (std::min)(predicted_last, base + EXTRA_SLOTS * divisor);
    predicted_last = (std::max)(predicted_last, first);
    {
        std::scoped_lock lock{mutex};
        plans[plan_next] = Plan{batch, first, base, predicted_last};
        plan_next = (plan_next + 1) % plans.size();
    }
    return Schedules() ? predicted_last : base_last;
}

const FramePredictor::Plan* FramePredictor::PlanOf(u64 batch) const {
    for (const Plan& plan : plans) {
        if (plan.batch == batch) {
            return &plan;
        }
    }
    return nullptr;
}

const FramePredictor::Plan* FramePredictor::NewestPlanAtOrBefore(u64 slot) const {
    const Plan* newest = nullptr;
    for (const Plan& plan : plans) {
        if (plan.batch != 0 && plan.first <= slot && (newest == nullptr || plan.batch > newest->batch)) {
            newest = &plan;
        }
    }
    return newest;
}

void FramePredictor::NoteSlot(u64 slot, u64 batch, s64 now_ns) {
    std::scoped_lock lock{mutex};
    for (Accumulator* acc : {&window, &total}) {
        ++acc->slots;
    }
    if (batch != 0) {
        const Plan* const plan = PlanOf(batch);
        if (plan != nullptr && slot > plan->base_last && slot <= plan->pred_last) {
            ++window.avoided;
            ++total.avoided;
        }
    } else {
        ++window.misses;
        ++total.misses;
        const Plan* const plan = NewestPlanAtOrBefore(slot);
        if (plan != nullptr && slot > plan->base_last && slot <= plan->pred_last) {
            ++window.avoidable;
            ++total.avoidable;
        }
    }
    if (window_start_ns == 0) {
        window_start_ns = now_ns;
    } else if (now_ns - window_start_ns >= WINDOW_NS) {
        Log(now_ns);
    }
}

s64 FramePredictor::HoldBudget(s64 now_ns, s64 recorded_ns, s64 latency_ns) const {
    if (!Holds()) {
        return 0;
    }
    std::scoped_lock lock{mutex};
    const double cap_ns = hold_cap_ms * 1.0e6;
    const double wait_ns = static_cast<double>(recorded_ns + latency_ns - now_ns);
    if (wait_ns > cap_ns) {
        return 0;
    }
    return static_cast<s64>((std::min)((std::max)(wait_ns, 0.0) + HOLD_SLACK_MS * 1.0e6, cap_ns));
}

void FramePredictor::HoldResult(bool landed, s64 waited_ns) {
    std::scoped_lock lock{mutex};
    const double waited_ms = static_cast<double>(waited_ns) * 1.0e-6;
    for (Accumulator* acc : {&window, &total}) {
        ++acc->holds;
        ++(landed ? acc->landed : acc->timeouts);
        acc->waited_ms += waited_ms;
    }
    // Hold longer while holds land, less when they time out.
    hold_success += 0.1 * ((landed ? 1.0 : 0.0) - hold_success);
    if (hold_success < 0.5) {
        hold_cap_ms = (std::max)(HOLD_CAP_MIN_MS, hold_cap_ms * 0.95);
    } else if (hold_success > 0.8) {
        hold_cap_ms = (std::min)(HOLD_CAP_MAX_MS, hold_cap_ms * 1.03);
    }
}

Window FramePredictor::Summarize(Accumulator& acc, bool percentiles) {
    Window out;
    out.samples = acc.n;
    if (acc.n != 0) {
        const double n = static_cast<double>(acc.n);
        out.mae_active_ms = acc.sum_active / n;
        out.mae_rls_ms = acc.sum_rls / n;
        out.mae_ewma_ms = acc.sum_ewma / n;
        out.coverage_percent = 100.0 * static_cast<double>(acc.covered) / n;
    }
    const auto p90 = [](std::vector<float>& errors) {
        if (errors.empty()) {
            return 0.0;
        }
        const size_t at = (errors.size() - 1) * 9 / 10;
        std::nth_element(errors.begin(), errors.begin() + static_cast<std::ptrdiff_t>(at),
                         errors.end());
        return static_cast<double>(errors[at]);
    };
    if (percentiles) {
        out.p90_active_ms = p90(acc.active);
        out.p90_rls_ms = p90(acc.rls);
        out.p90_ewma_ms = p90(acc.ewma);
    }
    out.slots = acc.slots;
    out.misses = acc.misses;
    out.avoided = acc.avoided;
    out.avoidable = acc.avoidable;
    out.holds = acc.holds;
    out.landed = acc.landed;
    out.timeouts = acc.timeouts;
    out.waited_ms = acc.waited_ms;
    return out;
}

void FramePredictor::Log(s64 now_ns) {
    last_window = Summarize(window, true);
    const Window& w = last_window;
    const Prediction next = model.Current();
    LOG_INFO(Render_Vulkan,
             "Frame predictor (10 s, EDEN_NX2_PREDICT={}): {} in use ({} switches, {} resets), {} "
             "intervals, mean {:.1f} ms, next {:.1f} ms (p90 {:.1f}); interval error, in use / rls "
             "/ ewma: mean {:.2f} / {:.2f} / {:.2f} ms, p90 {:.2f} / {:.2f} / {:.2f} ms; p90 "
             "covers {:.0f}%",
             mode, model.UsingRls() ? "rls" : "ewma", model.Switches(), model.Resets(), w.samples,
             model.MeanIntervalMs(), next.mean_ms, next.p90_ms, w.mae_active_ms, w.mae_rls_ms,
             w.mae_ewma_ms, w.p90_active_ms, w.p90_rls_ms, w.p90_ewma_ms, w.coverage_percent);
    LOG_INFO(Render_Vulkan,
             "Frame predictor (10 s): {} slots, {} with no frame ready; {} slots shown by a "
             "prediction past the baseline's reach, {} misses the predictor's reach would have "
             "avoided; {} holds for a nearly done real frame, {} landed, {} timed out, {:.1f} ms "
             "waited",
             w.slots, w.misses, w.avoided, w.avoidable, w.holds, w.landed, w.timeouts,
             w.waited_ms);
    window = Accumulator{};
    window_start_ns = now_ns;
}

Snapshot FramePredictor::GetSnapshot() const {
    std::scoped_lock lock{mutex};
    Snapshot out;
    out.mode = mode;
    out.using_rls = model.UsingRls();
    out.samples = model.Samples();
    out.switches = model.Switches();
    out.resets = model.Resets();
    out.mean_interval_ms = model.MeanIntervalMs();
    const Prediction next = model.Current();
    out.next_mean_ms = next.mean_ms;
    out.next_p90_ms = next.p90_ms;
    out.hold_cap_ms = hold_cap_ms;
    Accumulator copy = total;
    copy.active.clear();
    copy.rls.clear();
    copy.ewma.clear();
    out.total = Summarize(copy, false);
    out.last = last_window;
    return out;
}

} // namespace VideoCore::Nx2Predict
