// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/boot_timing.h"

#include <atomic>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#endif

#include "common/common_types.h"
#include "common/logging.h"

namespace Common::BootTiming {
namespace {

using Clock = std::chrono::steady_clock;

/// The steady clock's reading when the process was created (Windows), or at static
/// initialization, just before main, elsewhere.
Clock::time_point ProcessStart() {
    const Clock::time_point now = Clock::now();
#ifdef _WIN32
    FILETIME creation{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetProcessTimes(GetCurrentProcess(), &creation, &exited, &kernel, &user) != 0) {
        FILETIME current{};
        GetSystemTimePreciseAsFileTime(&current);
        const auto ticks = [](const FILETIME& time) {
            return (static_cast<u64>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
        };
        if (ticks(current) > ticks(creation)) {
            // FILETIME counts 100 ns intervals.
            const std::chrono::nanoseconds age{
                static_cast<s64>((ticks(current) - ticks(creation)) * 100)};
            return now - std::chrono::duration_cast<Clock::duration>(age);
        }
    }
#endif
    return now;
}

const Clock::time_point process_start = ProcessStart();
std::atomic<double> last_mark{0.0};

} // Anonymous namespace

double Now() {
    return std::chrono::duration<double, std::milli>(Clock::now() - process_start).count();
}

void Mark(std::string_view phase) {
    const double now = Now();
    const double since_last = now - last_mark.exchange(now, std::memory_order_relaxed);
    LOG_INFO(Common, "Boot timing: {} at {:.1f} ms (+{:.1f} ms)", phase, now, since_last);
}

void Mark(std::string_view phase, double start) {
    const double now = Now();
    const double since_last = now - last_mark.exchange(now, std::memory_order_relaxed);
    LOG_INFO(Common, "Boot timing: {} at {:.1f} ms (+{:.1f} ms), took {:.1f} ms", phase, now,
             since_last, now - start);
}

} // namespace Common::BootTiming
