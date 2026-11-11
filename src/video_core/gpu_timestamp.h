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

/// A GPU time scale in the 10 bits an anchor has for it: (64 + mantissa) / 64 * 2^(exponent - 8),
/// so 1/256 to 254 to within 0.8%, and 0 for 1.0 (also for a scale that is not positive).
[[nodiscard]] constexpr u64 AmpereScaleCode(f64 scale) {
    if (!(scale > 0.0)) {
        return 0;
    }
    u64 exponent = 8;
    while (scale >= 2.0 && exponent < 15) {
        scale *= 0.5;
        ++exponent;
    }
    while (scale < 1.0 && exponent > 0) {
        scale *= 2.0;
        --exponent;
    }
    // Out of range: the nearest end.
    u64 mantissa = 0;
    if (scale >= 2.0) {
        mantissa = 63;
    } else if (scale > 1.0) {
        mantissa = static_cast<u64>((scale - 1.0) * 64.0 + 0.5);
    }
    if (mantissa == 64) {
        // Rounded up to the next power of two.
        if (exponent < 15) {
            mantissa = 0;
            ++exponent;
        } else {
            mantissa = 63;
        }
    }
    return ((exponent ^ 8) << 6) | mantissa;
}

[[nodiscard]] constexpr f64 AmpereScaleOfCode(u64 code) {
    const u64 exponent = ((code >> 6) & 15) ^ 8;
    const u64 mantissa = 64 + (code & 63);
    return static_cast<f64>(mantissa << exponent) / 16384.0;
}

/// The scale a Switch 2 title's clock really runs with when `scale` is asked for.
[[nodiscard]] constexpr f64 AmpereLatchedScale(f64 scale) {
    return AmpereScaleOfCode(AmpereScaleCode(scale));
}

/// The last present's CoreTiming time with the GPU clock and the GPU time scale latched with it,
/// packed into one word so all three change together: a GPU Clocks or scale change takes effect at
/// the next present, never mid-frame, where it could move a timestamp back.
struct AmpereAnchor {
    /// The time keeps its low 52 bits; the rest are those of the time it is read at.
    static constexpr u64 TimeMask = (u64{1} << 52) - 1;

    u64 bits{};

    [[nodiscard]] static constexpr AmpereAnchor At(u64 present_ns, Settings::GpuClock clock,
                                                   f64 scale = 1.0) {
        return {((present_ns & TimeMask) << 12) | (AmpereScaleCode(scale) << 2) |
                static_cast<u64>(clock)};
    }
    /// The present's time, read at `now` less than 26 days (2^51 ns) after it. 0 before the first
    /// present; `now` when the present is not before it.
    [[nodiscard]] constexpr u64 PresentNs(u64 now) const {
        const u64 low = bits >> 12;
        if (low == 0) {
            return 0;
        }
        const u64 since = (now - low) & TimeMask;
        return since <= TimeMask / 2 ? now - since : now;
    }
    [[nodiscard]] constexpr Settings::GpuClock Clock() const {
        return static_cast<Settings::GpuClock>(bits & 3);
    }
    [[nodiscard]] constexpr f64 Scale() const {
        return AmpereScaleOfCode((bits >> 2) & 1023);
    }
};

/// The time since the last present a Switch 2 title sees: divided by the clock's divisor, then
/// multiplied by `scale` (VideoCore::FrameBudget::GpuTimeScale, 1 unless something sets it).
/// Never more than the real time, so a timestamp stays behind the CPU clock and ahead of the ones
/// before it.
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
/// ticks by the current clock. The anchor's scale is AmpereTimeSincePresent's.
template <typename Timing>
[[nodiscard]] u64 ReadGpuTimestamp(const Timing& timing, bool ampere, Settings::GpuClock clock,
                                   AmpereAnchor anchor) {
    if (ampere) {
        const u64 now = static_cast<u64>(timing.GetGlobalTimeNs().count());
        const u64 present_ns = anchor.PresentNs(now);
        if (present_ns == 0 || now <= present_ns) {
            return now;
        }
        return present_ns +
               AmpereTimeSincePresent(now - present_ns, anchor.Clock(), anchor.Scale());
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
