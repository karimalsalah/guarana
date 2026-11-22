// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <thread>

#include "common/logging.h"
#include "common/scope_exit.h"
#include "core/latency_clawback.h"

namespace Core::LatencyClawback {
namespace {

struct Runtime {
    Config config{};
    State state{};
    std::mutex mutex;
    s64 last_log_ns{};
};

[[nodiscard]] s64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

[[nodiscard]] double Ms(s64 ns) {
    return static_cast<double>(ns) / 1'000'000.0;
}

void LogSummary(const Readout& readout) {
    const Summary& summary = readout.summary;
    const char* photon = summary.refresh_ns >= MinRefreshNs && summary.refresh_ns <= MaxRefreshNs
                             ? "half a refresh added"
                             : "present return only";
    LOG_INFO(Service,
             "Latency: samples {} pending {} dropped {} present p50 {:.1f} p95 {:.1f} p99 {:.1f} "
             "ms, photon p50 {:.1f} ms ({}), queue p50 {:.1f} ms, cpu p50 {:.1f} ms, interval "
             "{:.1f} ms, last sleep {:.1f} ms over {} sleeps",
             summary.samples, summary.pending, summary.dropped, Ms(summary.present.p50),
             Ms(summary.present.p95), Ms(summary.present.p99), Ms(summary.photon.p50), photon,
             Ms(summary.queue.p50), Ms(summary.cpu.p50), Ms(summary.interval_ns),
             Ms(summary.last_sleep_ns), summary.sleeps);
}

void Configure(Runtime& runtime) {
    runtime.config.track = TrackRequested(std::getenv("EDEN_NX2_LATENCY_TRACK"));
    runtime.config.target_queue = ClawbackTarget(std::getenv("EDEN_NX2_LATENCY_CLAWBACK"));
    runtime.config.max_sleep_ns = MaxSleepNs;
    runtime.last_log_ns = NowNs();
    if (runtime.config.track) {
        LOG_INFO(Service,
                 "EDEN_NX2_LATENCY_TRACK: recording input-to-present latency; photon adds half "
                 "a refresh when the display period is known");
    }
    if (runtime.config.target_queue != 0) {
        LOG_INFO(Service,
                 "EDEN_NX2_LATENCY_CLAWBACK: holding the guest at the end of a present while "
                 "more than {} real frame(s) are unshown, at most one interval and 20 ms",
                 runtime.config.target_queue);
    }
}

struct Startup {
    Runtime runtime;
    Startup() {
        Configure(runtime);
    }
};

Runtime& Get() {
    // In-place: Runtime holds a mutex, so it cannot be copied out of an initializer.
    static Startup startup;
    return startup.runtime;
}

[[nodiscard]] bool Active(const Runtime& runtime) {
    return runtime.config.track || runtime.config.target_queue != 0;
}

[[nodiscard]] std::optional<Readout> TakeLog(Runtime& runtime, s64 now_ns) {
    if (now_ns - runtime.last_log_ns < 10'000'000'000) {
        return std::nullopt;
    }
    runtime.last_log_ns = now_ns;
    Readout readout;
    readout.tracking = runtime.config.track;
    readout.clawback = runtime.config.target_queue != 0;
    readout.target_queue = runtime.config.target_queue;
    readout.summary = Summarize(runtime.state);
    return readout;
}

void SleepNs(s64 ns) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds{ns};
    const auto coarse = deadline - std::chrono::milliseconds{1};
    if (std::chrono::steady_clock::now() < coarse) {
        std::this_thread::sleep_for(coarse - std::chrono::steady_clock::now());
    }
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
}

} // namespace

void NoteInputSample() {
    Runtime& runtime = Get();
    if (!Active(runtime)) {
        return;
    }
    const s64 now = NowNs();
    std::scoped_lock lock{runtime.mutex};
    (void)NoteInput(runtime.state, now);
}

void NoteGuestPresent(s64 submit_ns) {
    Runtime& runtime = Get();
    if (!Active(runtime)) {
        return;
    }
    std::scoped_lock lock{runtime.mutex};
    NoteSubmit(runtime.state, submit_ns);
}

void NotePresented(s64 present_ns, s64 refresh_ns) {
    Runtime& runtime = Get();
    if (!Active(runtime)) {
        return;
    }
    std::optional<Readout> pending_log;
    {
        std::scoped_lock lock{runtime.mutex};
        (void)NotePresent(runtime.state, present_ns, refresh_ns);
        pending_log = TakeLog(runtime, present_ns);
    }
    if (pending_log) {
        LogSummary(*pending_log);
    }
}

void FinishGuestPresent(bool allow_sleep) {
    Runtime& runtime = Get();
    if (!Active(runtime)) {
        return;
    }
    s64 sleep_ns = 0;
    {
        std::scoped_lock lock{runtime.mutex};
        if (allow_sleep && runtime.config.target_queue != 0) {
            sleep_ns = ClawbackSleepNs(runtime.state, runtime.config);
        }
        // Publishes while this present blocked the game are not the next frame's input.
        RearmAfterPresent(runtime.state);
        if (sleep_ns > 0) {
            runtime.state.hold_input = true;
            runtime.state.last_sleep_ns = sleep_ns;
            runtime.state.sleep_since_submit += sleep_ns;
            ++runtime.state.sleeps;
        }
    }
    if (sleep_ns <= 0) {
        return;
    }
    SCOPE_EXIT {
        std::scoped_lock lock{runtime.mutex};
        runtime.state.hold_input = false;
    };
    SleepNs(sleep_ns);
}

Readout Read() {
    Runtime& runtime = Get();
    std::scoped_lock lock{runtime.mutex};
    Readout readout;
    readout.tracking = runtime.config.track;
    readout.clawback = runtime.config.target_queue != 0;
    readout.target_queue = runtime.config.target_queue;
    readout.summary = Summarize(runtime.state);
    return readout;
}

} // namespace Core::LatencyClawback
