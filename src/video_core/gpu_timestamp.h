// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"
#include "common/settings_enums.h"

namespace Tegra {

/// How much faster Boost and Overclock make Switch 2 GPU work look.
[[nodiscard]] constexpr u64 AmpereClockDivisor(Settings::GpuClock clock) {
    switch (clock) {
    case Settings::GpuClock::Boost:
        return 4;
    case Settings::GpuClock::Overclock:
        return 16;
    default:
        return 1;
    }
}

/// The last present's CoreTiming time and the GPU clock latched with it, packed into one word so
/// both change together: a GPU Clocks change takes effect at the next present, never mid-frame.
struct AmpereAnchor {
    u64 bits{};

    [[nodiscard]] static constexpr AmpereAnchor At(u64 present_ns, Settings::GpuClock clock) {
        return {present_ns << 2 | static_cast<u64>(clock)};
    }
    [[nodiscard]] constexpr u64 PresentNs() const {
        return bits >> 2;
    }
    [[nodiscard]] constexpr Settings::GpuClock Clock() const {
        return static_cast<Settings::GpuClock>(bits & 3);
    }
};

/// The time since the last present a Switch 2 title sees: divided by the clock's divisor, then
/// multiplied by `scale` (EDEN_NX2_GPU_TIME_SCALE, 1 without it). Never more than the real time,
/// so a timestamp stays behind the CPU clock and ahead of the ones before it.
[[nodiscard]] constexpr u64 AmpereTimeSincePresent(u64 since_present_ns, Settings::GpuClock clock,
                                                   f64 scale) {
    const u64 clocked = since_present_ns / AmpereClockDivisor(clock);
    if (scale == 1.0) {
        return clocked;
    }
    const f64 scaled = static_cast<f64>(clocked) * scale;
    return scaled >= static_cast<f64>(since_present_ns) ? since_present_ns
                                                        : static_cast<u64>(scaled);
}

/// Switch 2 titles read report timestamps as nanoseconds and calibrate them against CPU time, so
/// Boost and Overclock shrink only the time since the last present: GPU durations within a frame
/// look shorter, while a timestamp never runs ahead of the CPU clock and trails it by less than
/// the time since that present. Before the first present the clock is exact. Maxwell scales its
/// ticks by the current clock. `ampere_scale` is AmpereTimeSincePresent's.
template <typename Timing>
[[nodiscard]] u64 ReadGpuTimestamp(const Timing& timing, bool ampere, Settings::GpuClock clock,
                                   AmpereAnchor anchor, f64 ampere_scale = 1.0) {
    if (ampere) {
        const u64 now = static_cast<u64>(timing.GetGlobalTimeNs().count());
        const u64 present_ns = anchor.PresentNs();
        if (present_ns == 0 || now <= present_ns) {
            return now;
        }
        return present_ns + AmpereTimeSincePresent(now - present_ns, anchor.Clock(), ampere_scale);
    }
    const u64 ticks = timing.GetGPUTicks();
    switch (clock) {
    case Settings::GpuClock::Boost:
        return ticks / 256;
    case Settings::GpuClock::Overclock:
        return ticks / 512;
    default:
        return ticks;
    }
}

} // namespace Tegra
