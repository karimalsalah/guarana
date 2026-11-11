// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <vector>

#include <fmt/format.h>

#include "common/logging.h"
#include "common/settings.h"
#include "video_core/auto_shading_rate.h"
#include "video_core/frame_budget.h"
#include "video_core/frame_budget_controller.h"
#include "video_core/gpu_timestamp.h"

namespace VideoCore::FrameBudget {
namespace {

using Action = FrameBudgetController::Action;
using State = FrameBudgetController::State;

/// The presents of this long are one tick of the controller.
constexpr auto TickInterval = std::chrono::milliseconds{500};
/// Presents a tick needs for its frame times, and the most it keeps.
constexpr size_t MinSamples = 3;
constexpr size_t MaxSamples = 512;
/// The frame time without the pacing wait that this share of a tick's frames stays under is the
/// controller's input.
constexpr size_t WorkPercent = 80;

double SanitizeFps(double fps) {
    return std::isfinite(fps) && fps >= 1.0 ? (std::min)(fps, 1000.0) : 0.0;
}

struct Session {
    Session() {
        if (const char* const text = std::getenv("EDEN_NX2_GPU_TIME_SCALE");
            text != nullptr && text[0] != '\0') {
            char* end = nullptr;
            const f64 value = std::strtod(text, &end);
            if (end == text || !std::isfinite(value) || value <= 0.0) {
                LOG_WARNING(HW_GPU, "EDEN_NX2_GPU_TIME_SCALE={} is not a positive number; ignored",
                            text);
            } else {
                manual_scale = value;
                logged_scale = value;
                scale.store(value, std::memory_order_relaxed);
                LOG_INFO(HW_GPU,
                         "EDEN_NX2_GPU_TIME_SCALE: Switch 2 titles see {} x the GPU time since "
                         "each present that GPU Clocks gives them, at most the real time",
                         value);
            }
        }
        if (const char* const text = std::getenv("EDEN_NX2_TARGET_FPS");
            text != nullptr && text[0] != '\0') {
            const double fps = SanitizeFps(std::strtod(text, nullptr));
            if (fps > 0.0) {
                target_fps.store(fps, std::memory_order_relaxed);
                LOG_INFO(HW_GPU,
                         "EDEN_NX2_TARGET_FPS: guest presents are held to {:.2f} ms and the GPU "
                         "time scale follows the frame time from {}",
                         1000.0 / fps, manual_scale);
            } else {
                LOG_WARNING(HW_GPU, "EDEN_NX2_TARGET_FPS: bad target \"{}\" (want fps >= 1)", text);
            }
        }
    }

    std::atomic<f64> scale{1.0};
    std::atomic<double> target_fps{0.0};
    std::atomic<double> recent_work_ms{0.0};

    /// Guards everything below.
    std::mutex mutex;
    f64 manual_scale{1.0};
    f64 logged_scale{1.0}; ///< The scale of the last log line
    TimePoint last_log{};
    FrameBudgetController controller;
    std::vector<double> interval_samples;
    std::vector<double> work_samples;
    TimePoint last_return{};  ///< When the last present returned to the game
    TimePoint window_start{}; ///< When the current tick's presents began
    /// What AutoShadingRate was last told, nothing while it has its default.
    std::optional<bool> shading_allowed;
    double frame_ms{};
    double work_ms{};
    u32 samples{};
    State state{State::Off};
    u64 ticks{};
    u64 raises{};
    u64 lowers{};
    std::string last_change;
};

Session& Instance() {
    static Session session;
    return session;
}

/// The value `percent` of the way up the sorted values (50: the median), 0 with fewer than
/// MinSamples of them.
double Percentile(std::vector<double>& values, size_t percent) {
    if (values.size() < MinSamples) {
        return 0.0;
    }
    const size_t index = (std::min)(values.size() - 1, values.size() * percent / 100);
    const auto place = values.begin() + index;
    std::ranges::nth_element(values, place);
    return *place;
}

double Ceiling() {
    return static_cast<double>(Tegra::AmpereClockDivisor(Settings::values.gpu_clock.GetValue()));
}

/// Caller holds session.mutex. A new target starts from nothing but the scale.
void ResetLocked(Session& session) {
    session.controller.Reset();
    session.interval_samples.clear();
    session.work_samples.clear();
    session.last_return = TimePoint{};
    session.window_start = TimePoint{};
    session.frame_ms = 0.0;
    session.work_ms = 0.0;
    session.samples = 0;
    session.state = State::Off;
    session.recent_work_ms.store(0.0, std::memory_order_relaxed);
    session.logged_scale = session.scale.load(std::memory_order_relaxed);
}

/// Caller holds session.mutex. One tick over the presents gathered up to `now`.
void TickLocked(Session& session, double target_fps, TimePoint now) {
    const double target_ms = 1000.0 / target_fps;
    session.samples = static_cast<u32>(session.interval_samples.size());
    session.frame_ms = Percentile(session.interval_samples, 50);
    // Four frames in five have to fit: a median would call a tick with two late frames in five
    // on target, and a few hitches (shader builds) still do not count.
    session.work_ms = Percentile(session.work_samples, WorkPercent);
    session.interval_samples.clear();
    session.work_samples.clear();
    session.window_start = now;
    session.recent_work_ms.store(session.work_ms, std::memory_order_relaxed);
    ++session.ticks;

    // The shading-rate governor, when it is on, takes the same target.
    const double shading_fps = AutoShadingRate::TargetFps();
    if (shading_fps > 0.0 && shading_fps != target_fps) {
        AutoShadingRate::SetTargetFps(target_fps);
    }
    const bool was_allowed = session.shading_allowed.value_or(false);

    const f64 before = session.scale.load(std::memory_order_relaxed);
    const FrameBudgetController::Input input{
        .frame_ms = session.work_ms,
        .target_ms = target_ms,
        .ceiling = Ceiling(),
        // The governor gives its coarse rates back before the resolution goes up again.
        .hold_lower = was_allowed && shading_fps > 0.0 && AutoShadingRate::Coarsening(),
    };
    const FrameBudgetController::Decision decision = session.controller.Update(input, before);
    session.state = decision.state;
    if (decision.scale != before) {
        session.scale.store(decision.scale, std::memory_order_relaxed);
    }
    if (decision.action != Action::Hold) {
        const bool raise = decision.action == Action::Raise;
        if (raise) {
            ++session.raises;
        } else {
            ++session.lowers;
        }
        session.last_change = fmt::format("{} {:.3f} -> {:.3f} at {:.2f} ms",
                                          raise ? "raise" : "lower", before, decision.scale,
                                          session.work_ms);
    }

    // The governor may coarsen from a miss at the upper limit until the scale leaves the limit.
    bool allowed = was_allowed;
    if (decision.state == State::Saturated) {
        allowed = true;
    } else if (decision.state != State::Frozen && !decision.at_limit) {
        allowed = false;
    }
    if (!session.shading_allowed || *session.shading_allowed != allowed) {
        session.shading_allowed = allowed;
        AutoShadingRate::SetAllowed(allowed);
    }

    const f64 logged = session.logged_scale;
    if (logged > 0.0 && (decision.scale > logged * 1.1 || decision.scale < logged / 1.1) &&
        now - session.last_log >= std::chrono::seconds{1}) {
        LOG_INFO(HW_GPU,
                 "Frame budget: GPU time scale {:.3f} -> {:.3f} ({}), frame {:.2f} ms without "
                 "the pacing wait, {:.2f} ms presented, target {:.2f} ms",
                 logged, decision.scale, FrameBudgetStateName(decision.state), session.work_ms,
                 session.frame_ms, target_ms);
        session.logged_scale = decision.scale;
        session.last_log = now;
    }
}

} // Anonymous namespace

f64 GpuTimeScale() {
    return Instance().scale.load(std::memory_order_relaxed);
}

void SetGpuTimeScale(f64 scale) {
    if (!std::isfinite(scale) || scale <= 0.0) {
        return;
    }
    Session& session = Instance();
    std::scoped_lock lock{session.mutex};
    session.manual_scale = scale;
    session.logged_scale = scale;
    session.scale.store(scale, std::memory_order_relaxed);
    LOG_INFO(HW_GPU, "Frame budget: GPU time scale set to {:.3f}", scale);
}

double TargetFps() {
    return Instance().target_fps.load(std::memory_order_relaxed);
}

void SetTargetFps(double fps) {
    fps = SanitizeFps(fps);
    Session& session = Instance();
    std::scoped_lock lock{session.mutex};
    const double before = session.target_fps.load(std::memory_order_relaxed);
    if (fps == before) {
        return;
    }
    session.target_fps.store(fps, std::memory_order_relaxed);
    LOG_INFO(HW_GPU, "Frame budget: target {:.1f} fps (was {:.1f})", fps, before);
    if (fps <= 0.0) {
        session.scale.store(session.manual_scale, std::memory_order_relaxed);
        if (session.shading_allowed) {
            session.shading_allowed.reset();
            AutoShadingRate::SetAllowed(true);
        }
    }
    ResetLocked(session);
}

std::chrono::nanoseconds PresentInterval() {
    const double fps = TargetFps();
    return std::chrono::nanoseconds{fps > 0.0 ? static_cast<s64>(1'000'000'000.0 / fps) : s64{0}};
}

void OnGuestPresent(TimePoint pace_begin, TimePoint pace_end) {
    Session& session = Instance();
    if (session.target_fps.load(std::memory_order_relaxed) <= 0.0) {
        return;
    }
    std::scoped_lock lock{session.mutex};
    const double target_fps = session.target_fps.load(std::memory_order_relaxed);
    if (target_fps <= 0.0) {
        return;
    }
    const auto ms = [](auto duration) {
        return std::chrono::duration<double, std::milli>(duration).count();
    };
    if (session.last_return != TimePoint{} && pace_begin >= session.last_return &&
        session.interval_samples.size() < MaxSamples) {
        session.interval_samples.push_back(ms(pace_end - session.last_return));
        session.work_samples.push_back(ms(pace_begin - session.last_return));
    }
    session.last_return = pace_end;
    if (session.window_start == TimePoint{}) {
        session.window_start = pace_end;
        return;
    }
    if (pace_end - session.window_start >= TickInterval) {
        TickLocked(session, target_fps, pace_end);
    }
}

double RecentWorkMs() {
    return Instance().recent_work_ms.load(std::memory_order_relaxed);
}

Status GetStatus() {
    Session& session = Instance();
    std::scoped_lock lock{session.mutex};
    const double target_fps = session.target_fps.load(std::memory_order_relaxed);
    const f64 scale = session.scale.load(std::memory_order_relaxed);
    const FrameBudgetController::Config& config = session.controller.GetConfig();
    return {
        .target_fps = target_fps,
        .target_ms = target_fps > 0.0 ? 1000.0 / target_fps : 0.0,
        .frame_ms = session.frame_ms,
        .work_ms = session.work_ms,
        .samples = session.samples,
        .scale = scale,
        .latched_scale = Tegra::AmpereLatchedScale(scale),
        .manual_scale = session.manual_scale,
        .min_scale = config.min_scale,
        .max_scale = (std::min)(config.max_scale, Ceiling()),
        .state = FrameBudgetStateName(session.state),
        .ticks = session.ticks,
        .raises = session.raises,
        .lowers = session.lowers,
        .miss_scale = session.controller.MissScale(),
        .cooldown_ticks = session.controller.CooldownLeft(),
        .fast_streak = session.controller.FastStreak(),
        .blind_lowers = session.controller.BlindLowers(),
        .shading_allowed = session.shading_allowed.value_or(true),
        .last_change = session.last_change,
    };
}

} // namespace VideoCore::FrameBudget
