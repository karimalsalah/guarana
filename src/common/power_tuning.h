// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>

#include "common/common_types.h"

/// Switches for trading host CPU power against latency (the CPU and an integrated GPU share one
/// package power limit). Read once from the environment; power_set of the debug server changes
/// them live. The defaults keep the behaviour without them.
namespace Common::PowerTuning {

/// EDEN_NX2_SPIN_SCALE=<0..1> as a percentage: scales the busy-wait bounds of the GPU threads.
extern std::atomic<u32> spin_percent;
/// EDEN_NX2_ECO_BACKGROUND=1: background threads ask for EcoQoS (Windows) when they start.
extern std::atomic<bool> eco_background;
/// EDEN_NX2_IDLE_SPIN_US=<n>: microseconds an idle guest core polls before blocking, negative
/// for the per-title default.
extern std::atomic<s64> idle_spin_us;

/// A busy-wait bound scaled by spin_percent. Call it once before the loop.
template <typename T>
[[nodiscard]] T ScaleSpin(T bound) {
    const u32 percent = spin_percent.load(std::memory_order_relaxed);
    return percent >= 100 ? bound : static_cast<T>(bound * percent / 100);
}

/// 0..1 to the percentage spin_percent holds; out of range values are clamped.
[[nodiscard]] u32 SpinPercent(double scale);

/// For a pure background thread, at its start: applies eco_background to it.
void TagBackgroundThread();

/// One Info line: the host power meter's channels and the switches.
void LogState();

} // namespace Common::PowerTuning
