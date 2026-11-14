// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace Common::BootTiming {

/// Milliseconds since the process started.
[[nodiscard]] double Now();

/// One recorded phase boundary: its name, when it happened (ms since the process started) and
/// how long it took (ms since the previous mark, or since `start` for Mark(phase, start) and
/// Span). RecordedPhases returns them in order.
struct Phase {
    std::string name;
    double at_ms{};
    double took_ms{};
};

/// Every Mark and Span since the process started, oldest first (at most the last 256).
[[nodiscard]] std::vector<Phase> RecordedPhases();

/// Logs a boot phase boundary at Info ("Boot timing: ..."): the time since the process started
/// and since the previous mark.
void Mark(std::string_view phase);

/// As Mark, with how long the phase took since `start` (a Now() value).
void Mark(std::string_view phase, double start);

/// Logs a phase that ran before logging started, from `start` to `end` (Now() values). The next
/// Mark still counts from the previous mark.
void Span(std::string_view phase, double start, double end);

} // namespace Common::BootTiming
