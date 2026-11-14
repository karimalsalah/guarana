// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/boot_timing.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

#include <fmt/format.h>

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
        const auto ticks = [](const FILETIME& filetime) {
            return (static_cast<u64>(filetime.dwHighDateTime) << 32) | filetime.dwLowDateTime;
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

#ifdef _WIN32
/// A copy of the standard output handle the process started with, taken before main: the console
/// toggle (src/lichii/debugger/console.cpp) reopens stdout, which closes the original handle.
/// Null when there is none (a GUI start without redirection). Not inherited by child processes.
HANDLE StartupOutput() {
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out == nullptr || out == INVALID_HANDLE_VALUE) {
        return nullptr;
    }
    HANDLE copy = nullptr;
    if (DuplicateHandle(GetCurrentProcess(), out, GetCurrentProcess(), &copy, 0, FALSE,
                        DUPLICATE_SAME_ACCESS) == 0) {
        return nullptr;
    }
    // Nor the original: a child started with inherited handles would keep the launcher's pipe open.
    SetHandleInformation(out, HANDLE_FLAG_INHERIT, 0);
    return copy;
}

const HANDLE startup_output = StartupOutput();
#endif

std::atomic<bool> session_output{false};
std::mutex session_mutex;
// The last progress line written (SessionProgress): stage, then whole percent. Under session_mutex.
int session_progress_stage = -1;
int session_progress_percent = -1;

} // Anonymous namespace

void SetSessionOutput(bool enabled) {
    std::scoped_lock lock{session_mutex};
    session_output.store(enabled, std::memory_order_relaxed);
    session_progress_stage = -1;
    session_progress_percent = -1;
}

void SessionProgress(int stage, std::size_t value, std::size_t total) {
    static constexpr const char* names[] = {"prepare", "build", "complete", "warm"};
    if (stage < 0 || stage >= 4 || !session_output.load(std::memory_order_relaxed)) {
        return;
    }
    const int percent = total > 0 ? static_cast<int>((std::min)(value, total) * 100 / total) : 0;
    {
        std::scoped_lock lock{session_mutex};
        if (stage == session_progress_stage && percent == session_progress_percent) {
            return;
        }
        session_progress_stage = stage;
        session_progress_percent = percent;
    }
    SessionLine(fmt::format("Boot progress: {} {} {}", names[stage], value, total));
}

void SessionLine(std::string_view line) {
    if (!session_output.load(std::memory_order_relaxed)) {
        return;
    }
    std::string text{line};
    text += '\n';
    std::scoped_lock lock{session_mutex};
    if (!session_output.load(std::memory_order_relaxed)) {
        return;
    }
#ifdef _WIN32
    DWORD written = 0;
    if (startup_output == nullptr ||
        WriteFile(startup_output, text.data(), static_cast<DWORD>(text.size()), &written,
                  nullptr) == 0) {
        // No output, or the reader is gone (a broken pipe): stop trying.
        session_output.store(false, std::memory_order_relaxed);
    }
#else
    if (std::fwrite(text.data(), 1, text.size(), stdout) != text.size() ||
        std::fflush(stdout) != 0) {
        session_output.store(false, std::memory_order_relaxed);
    }
#endif
}

double Now() {
    return std::chrono::duration<double, std::milli>(Clock::now() - process_start).count();
}

void Mark(std::string_view phase) {
    const double now = Now();
    const double since_last = now - last_mark.exchange(now, std::memory_order_relaxed);
    LOG_INFO(Common, "Boot timing: {} at {:.1f} ms (+{:.1f} ms)", phase, now, since_last);
    if (session_output.load(std::memory_order_relaxed)) {
        SessionLine(fmt::format("Boot timing: {} at {:.1f} ms (+{:.1f} ms)", phase, now, since_last));
    }
}

void Mark(std::string_view phase, double start) {
    const double now = Now();
    const double since_last = now - last_mark.exchange(now, std::memory_order_relaxed);
    LOG_INFO(Common, "Boot timing: {} at {:.1f} ms (+{:.1f} ms), took {:.1f} ms", phase, now,
             since_last, now - start);
    if (session_output.load(std::memory_order_relaxed)) {
        SessionLine(fmt::format("Boot timing: {} at {:.1f} ms (+{:.1f} ms), took {:.1f} ms", phase,
                                now, since_last, now - start));
    }
}

void Span(std::string_view phase, double start, double end) {
    LOG_INFO(Common, "Boot timing: {} at {:.1f} ms, took {:.1f} ms", phase, end, end - start);
    if (session_output.load(std::memory_order_relaxed)) {
        SessionLine(fmt::format("Boot timing: {} at {:.1f} ms, took {:.1f} ms", phase, end,
                                end - start));
    }
}

} // namespace Common::BootTiming
