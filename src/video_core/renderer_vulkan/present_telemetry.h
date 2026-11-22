// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

#include "common/common_types.h"
#include "common/settings_enums.h"

namespace Vulkan {

namespace PresentTelemetryDetail {

[[nodiscard]] constexpr char Lower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] constexpr bool EqualsLower(std::string_view value, std::string_view lower) {
    if (value.size() != lower.size()) {
        return false;
    }
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (Lower(value[i]) != lower[i]) {
            return false;
        }
    }
    return true;
}

} // namespace PresentTelemetryDetail

/// EDEN_NX2_PRESENT_MODE (diagnostic): the present mode to ask for, whatever the vsync setting,
/// frame generation and EDEN_NX2_FLUID would choose. `immediate`, `fifo`, `fifo_relaxed` (or
/// `fifo-relaxed`) and `mailbox`, in any case. Anything else, empty included, is no override.
[[nodiscard]] constexpr std::optional<Settings::VSyncMode> ParsePresentModeOverride(
    std::string_view value) {
    using PresentTelemetryDetail::EqualsLower;
    if (EqualsLower(value, "immediate")) {
        return Settings::VSyncMode::Immediate;
    }
    if (EqualsLower(value, "fifo")) {
        return Settings::VSyncMode::Fifo;
    }
    if (EqualsLower(value, "fifo_relaxed") || EqualsLower(value, "fifo-relaxed")) {
        return Settings::VSyncMode::FifoRelaxed;
    }
    if (EqualsLower(value, "mailbox")) {
        return Settings::VSyncMode::Mailbox;
    }
    return std::nullopt;
}

/// Whether the surface lists `mode`. Every surface has FIFO: Vulkan requires it.
[[nodiscard]] constexpr bool SurfaceOffers(Settings::VSyncMode mode, bool has_immediate,
                                           bool has_mailbox, bool has_fifo_relaxed) {
    switch (mode) {
    case Settings::VSyncMode::Immediate:
        return has_immediate;
    case Settings::VSyncMode::Mailbox:
        return has_mailbox;
    case Settings::VSyncMode::FifoRelaxed:
        return has_fifo_relaxed;
    default:
        return true;
    }
}

/// EDEN_NX2_SHOWN_STATS: the intervals between presents, counted in display refreshes.
struct ShownRefreshes {
    /// Intervals under half a refresh (a frame replaced or torn before it stayed a whole
    /// refresh), then those of 1, 2, and 3 or more refreshes.
    std::array<u64, 4> count{};
    /// The median interval in refreshes, at least 1: the cadence the frames came at.
    u64 median_refreshes{};
    /// Refreshes the longer intervals took beyond the median: a frame that should have been
    /// replaced stayed on screen that much longer.
    u64 missed{};
};

/// The refreshes one interval spans, to the nearest whole one: 0 for an interval under half a
/// refresh. 0 when either value is not positive.
[[nodiscard]] constexpr u64 RefreshUnits(f64 interval_ms, f64 period_ms) {
    if (!(interval_ms > 0.0) || !(period_ms > 0.0)) {
        return 0;
    }
    return static_cast<u64>(interval_ms / period_ms + 0.5);
}

/// Counts sorted intervals (milliseconds, ascending) in refreshes of a `refresh_hz` display. An
/// empty count when the rate is not known (not positive) or there are no intervals.
[[nodiscard]] constexpr ShownRefreshes CountRefreshes(std::span<const f32> sorted_intervals_ms,
                                                      f64 refresh_hz) {
    ShownRefreshes result{};
    if (!(refresh_hz > 0.0) || sorted_intervals_ms.empty()) {
        return result;
    }
    const f64 period_ms = 1000.0 / refresh_hz;
    const f64 median = static_cast<f64>(sorted_intervals_ms[(sorted_intervals_ms.size() - 1) / 2]);
    result.median_refreshes = std::max<u64>(1, RefreshUnits(median, period_ms));
    for (const f32 interval : sorted_intervals_ms) {
        const u64 units = RefreshUnits(static_cast<f64>(interval), period_ms);
        ++result.count[static_cast<std::size_t>(std::min<u64>(units, 3))];
        if (units > result.median_refreshes) {
            result.missed += units - result.median_refreshes;
        }
    }
    return result;
}

} // namespace Vulkan
