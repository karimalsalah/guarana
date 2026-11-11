// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "common/logging.h"
#include "common/power_meter.h"
#include "common/power_tuning.h"
#include "common/thread.h"

namespace Common::PowerTuning {

namespace {

u32 EnvironmentSpinPercent() {
    const char* text = std::getenv("EDEN_NX2_SPIN_SCALE");
    return text ? SpinPercent(std::strtod(text, nullptr)) : 100;
}

bool EnvironmentEcoBackground() {
    const char* text = std::getenv("EDEN_NX2_ECO_BACKGROUND");
    return text && std::strtol(text, nullptr, 10) != 0;
}

s64 EnvironmentIdleSpin() {
    const char* text = std::getenv("EDEN_NX2_IDLE_SPIN_US");
    return text ? static_cast<s64>(std::strtoll(text, nullptr, 10)) : s64{-1};
}

} // Anonymous namespace

std::atomic<u32> spin_percent{EnvironmentSpinPercent()};
std::atomic<bool> eco_background{EnvironmentEcoBackground()};
std::atomic<s64> idle_spin_us{EnvironmentIdleSpin()};

u32 SpinPercent(double scale) {
    if (!std::isfinite(scale)) {
        return 100;
    }
    return static_cast<u32>(std::lround((std::clamp)(scale, 0.0, 1.0) * 100.0));
}

void TagBackgroundThread() {
    if (eco_background.load(std::memory_order_relaxed)) {
        SetCurrentThreadEcoQoS(true);
    }
}

void LogState() {
    PowerMeter& meter = PowerMeter::Instance();
    LOG_INFO(Common,
             "Power meter {} (channels: {}); spin scale {}%, eco background {}, idle spin {} us",
             meter.Available() ? "available" : "unavailable", meter.ChannelNames(),
             spin_percent.load(std::memory_order_relaxed),
             eco_background.load(std::memory_order_relaxed),
             idle_spin_us.load(std::memory_order_relaxed));
}

} // namespace Common::PowerTuning
