// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <span>

#include "common/common_types.h"

/// Continuous input-to-photon samples, and the Reflex-style render-queue sleep that can use them.
///
/// The input time of a guest frame is the first npad publish after the previous Switch 2 present
/// returns (a publish during that present is dropped; after a clawback sleep, the first publish
/// after that sleep). On Switch 1 it is the first publish after the guest present is counted.
/// The present time is the real
/// host frame's vkQueuePresent return, paired in order with guest presents. Generated frames are
/// not samples: the caller only reports a real present. Photon time is that present plus half a
/// refresh when the period is known, an estimate of average scan-out. The display's own processing
/// after the present call is not included.
///
/// EDEN_NX2_LATENCY_CLAWBACK holds the guest at the end of a Switch 2 present, after the frame is
/// already queued, while more real frames are unshown than the target. Unset, both switches leave
/// the hooks as a flag check. This is not NVIDIA Reflex, XeLL or Anti-Lag, and it is not the
/// picture-onset probe (the debug server's input_latency).
namespace Core::LatencyClawback {

inline constexpr u32 PendingCap = 8;
inline constexpr u32 Window = 256;
inline constexpr u32 WarmupIntervals = 8;
/// Unmatched guest presents beyond this, once they outnumber matched ones, stop the sleep.
inline constexpr u64 DropRunaway = 30;
inline constexpr s64 MaxSleepNs = 20'000'000;
inline constexpr s64 MinSleepNs = 500'000;
inline constexpr s64 MinIntervalNs = 2'000'000;
inline constexpr s64 MaxIntervalNs = 100'000'000;
inline constexpr s64 MinRefreshNs = 4'000'000;
inline constexpr s64 MaxRefreshNs = 50'000'000;

/// Nonzero decimal: on. Unset, empty, "0", "false" and anything else strtoul reads as 0: off.
[[nodiscard]] inline bool TrackRequested(const char* value) {
    return value != nullptr && std::strtoul(value, nullptr, 10) != 0;
}

/// `1` to `4` real frames allowed in flight. 0 (unset, empty, "0", "false", strtoul 0): off.
/// A larger number counts as 4.
[[nodiscard]] inline u32 ClawbackTarget(const char* value) {
    if (value == nullptr) {
        return 0;
    }
    const unsigned long parsed = std::strtoul(value, nullptr, 10);
    if (parsed == 0) {
        return 0;
    }
    return static_cast<u32>((std::min)(parsed, 4UL));
}

struct Config {
    bool track{};
    u32 target_queue{};
    s64 max_sleep_ns{MaxSleepNs};
};

struct Sample {
    s64 present_ns{}; ///< present return minus the input time
    s64 photon_ns{};  ///< present_ns plus half a refresh, or the same when the period is unknown
    s64 queue_ns{};   ///< present return minus the guest present
    s64 cpu_ns{};     ///< guest present minus the input time
};

struct Pending {
    s64 input_ns{};
    s64 submit_ns{};
};

struct State {
    s64 open_input_ns{};
    /// Set for the duration of a clawback sleep so a pad publish during the hold is not the next
    /// frame's input. The game reads the pad after the present returns.
    bool hold_input{};
    s64 last_submit_ns{};
    /// Clawback sleep inserted since the previous guest present. The next interval subtracts it,
    /// so the hold is not treated as the game taking longer and the next sleep does not grow.
    s64 sleep_since_submit{};
    s64 interval_ewma{};
    u32 intervals{};
    u32 pending_count{};
    u32 pending_head{};
    std::array<Pending, PendingCap> pending{};
    std::array<Sample, Window> window{};
    u32 window_count{};
    u32 window_pos{};
    u64 samples{};
    u64 dropped{};
    u64 sleeps{};
    s64 last_sleep_ns{};
    s64 last_refresh_ns{};
};

struct Series {
    s64 latest{};
    s64 p50{};
    s64 p95{};
    s64 p99{};
};

struct Summary {
    u64 samples{};
    u64 dropped{};
    u32 pending{};
    u32 intervals{};
    s64 interval_ns{};
    u64 sleeps{};
    s64 last_sleep_ns{};
    s64 refresh_ns{};
    Series present{};
    Series photon{};
    Series queue{};
    Series cpu{};
};

struct Readout {
    bool tracking{};
    bool clawback{};
    u32 target_queue{};
    Summary summary{};
};

/// Drops a pad publish that landed while the guest was blocked in the present. The next
/// NoteInput is the following frame's input, taken after the present returns.
inline void RearmAfterPresent(State& state) {
    state.open_input_ns = 0;
}

/// True when this publish opened the frame. A later publish before the guest present does not
/// move it, and a publish during a clawback sleep is ignored.
[[nodiscard]] inline bool NoteInput(State& state, s64 now_ns) {
    if (state.hold_input || now_ns <= 0 || state.open_input_ns != 0) {
        return false;
    }
    state.open_input_ns = now_ns;
    return true;
}

inline void PushPending(State& state, Pending pending) {
    if (state.pending_count == PendingCap) {
        state.pending_head = (state.pending_head + 1) % PendingCap;
        --state.pending_count;
        ++state.dropped;
    }
    const u32 slot = (state.pending_head + state.pending_count) % PendingCap;
    state.pending[slot] = pending;
    ++state.pending_count;
}

/// Closes the open frame and counts it in flight. Does not sleep.
inline void NoteSubmit(State& state, s64 now_ns) {
    if (now_ns <= 0) {
        return;
    }
    if (state.last_submit_ns != 0) {
        const s64 slept = state.sleep_since_submit > 0 ? state.sleep_since_submit : 0;
        const s64 delta = now_ns - state.last_submit_ns - slept;
        if (delta >= MinIntervalNs && delta <= MaxIntervalNs) {
            if (state.intervals == 0) {
                state.interval_ewma = delta;
            } else {
                state.interval_ewma += (delta - state.interval_ewma) / 8;
            }
            ++state.intervals;
        }
    }
    state.sleep_since_submit = 0;
    state.last_submit_ns = now_ns;
    PushPending(state, Pending{state.open_input_ns, now_ns});
    state.open_input_ns = 0;
}

/// Pairs the oldest unmatched guest present with this real host present. False when there is
/// nothing to pair, or the present is earlier than that guest present (it is still consumed, so
/// a host that raced ahead cannot sit on the queue).
[[nodiscard]] inline bool NotePresent(State& state, s64 now_ns, s64 refresh_ns) {
    if (state.pending_count == 0 || now_ns <= 0) {
        return false;
    }
    const Pending frame = state.pending[state.pending_head];
    state.pending_head = (state.pending_head + 1) % PendingCap;
    --state.pending_count;
    if (frame.input_ns <= 0 || frame.submit_ns <= 0 || now_ns < frame.submit_ns ||
        now_ns < frame.input_ns) {
        return false;
    }
    if (refresh_ns >= MinRefreshNs && refresh_ns <= MaxRefreshNs) {
        state.last_refresh_ns = refresh_ns;
    }
    const s64 half = state.last_refresh_ns >= MinRefreshNs && state.last_refresh_ns <= MaxRefreshNs
                         ? state.last_refresh_ns / 2
                         : 0;
    Sample sample;
    sample.cpu_ns = frame.submit_ns - frame.input_ns;
    sample.queue_ns = now_ns - frame.submit_ns;
    sample.present_ns = now_ns - frame.input_ns;
    sample.photon_ns = sample.present_ns + half;
    state.window[state.window_pos] = sample;
    state.window_pos = (state.window_pos + 1) % Window;
    if (state.window_count < Window) {
        ++state.window_count;
    }
    ++state.samples;
    return true;
}

/// One guest-present interval when more real frames are in flight than the target, after the
/// interval has settled and a present has matched. A clawback sleep already inside that gap is
/// not part of the interval. Never more than max_sleep_ns. Zero when the sleep would fight a
/// present hook that is not keeping up.
[[nodiscard]] inline s64 ClawbackSleepNs(const State& state, const Config& config) {
    if (config.target_queue == 0 || config.target_queue > 4) {
        return 0;
    }
    if (state.samples == 0 || state.intervals < WarmupIntervals) {
        return 0;
    }
    if (state.dropped > state.samples && state.dropped > DropRunaway) {
        return 0;
    }
    if (state.pending_count <= config.target_queue) {
        return 0;
    }
    if (state.interval_ewma < MinIntervalNs || state.interval_ewma > MaxIntervalNs) {
        return 0;
    }
    s64 sleep = state.interval_ewma;
    if (config.max_sleep_ns < sleep) {
        sleep = config.max_sleep_ns;
    }
    return sleep >= MinSleepNs ? sleep : 0;
}

[[nodiscard]] inline Series SeriesOf(std::span<s64> values) {
    Series series;
    if (values.empty()) {
        return series;
    }
    series.latest = values.back();
    std::sort(values.begin(), values.end());
    const std::size_t last = values.size() - 1;
    series.p50 = values[last * 50 / 100];
    series.p95 = values[last * 95 / 100];
    series.p99 = values[last * 99 / 100];
    return series;
}

[[nodiscard]] inline Summary Summarize(const State& state) {
    Summary summary;
    summary.samples = state.samples;
    summary.dropped = state.dropped;
    summary.pending = state.pending_count;
    summary.intervals = state.intervals;
    summary.interval_ns = state.interval_ewma;
    summary.sleeps = state.sleeps;
    summary.last_sleep_ns = state.last_sleep_ns;
    summary.refresh_ns = state.last_refresh_ns;
    if (state.window_count == 0) {
        return summary;
    }
    std::array<s64, Window> present{};
    std::array<s64, Window> photon{};
    std::array<s64, Window> queue{};
    std::array<s64, Window> cpu{};
    const u32 count = state.window_count;
    const u32 begin = (state.window_pos + Window - count) % Window;
    for (u32 index = 0; index < count; ++index) {
        const Sample& sample = state.window[(begin + index) % Window];
        present[index] = sample.present_ns;
        photon[index] = sample.photon_ns;
        queue[index] = sample.queue_ns;
        cpu[index] = sample.cpu_ns;
    }
    summary.present = SeriesOf(std::span<s64>{present.data(), count});
    summary.photon = SeriesOf(std::span<s64>{photon.data(), count});
    summary.queue = SeriesOf(std::span<s64>{queue.data(), count});
    summary.cpu = SeriesOf(std::span<s64>{cpu.data(), count});
    return summary;
}

/// The first npad publish of the open frame. No-op unless a switch is on.
void NoteInputSample();

/// One guest present (Switch 1 composite or Switch 2 vi2), at `submit_ns` on the steady clock.
void NoteGuestPresent(s64 submit_ns);

/// One real host frame, when vkQueuePresent returned. `refresh_ns` is the display period, or 0.
void NotePresented(s64 present_ns, s64 refresh_ns);

/// End of a Switch 2 vi2 present, after the frame is queued. Re-arms the input stamp so the next
/// pad publish (after this returns, and after a clawback sleep) is the next frame's. Sleeps when
/// `allow_sleep` and the render queue is over the clawback target. Pass false when a suspended
/// channel needs this present to return before the GPU can finish. Unset: returns without either.
void FinishGuestPresent(bool allow_sleep);

[[nodiscard]] Readout Read();

} // namespace Core::LatencyClawback
