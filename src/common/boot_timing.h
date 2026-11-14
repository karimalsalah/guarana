// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <string_view>

namespace Common::BootTiming {

/// Milliseconds since the process started.
[[nodiscard]] double Now();

/// Logs a boot phase boundary at Info ("Boot timing: ..."): the time since the process started
/// and since the previous mark.
void Mark(std::string_view phase);

/// As Mark, with how long the phase took since `start` (a Now() value).
void Mark(std::string_view phase, double start);

/// Logs a phase that ran before logging started, from `start` to `end` (Now() values). The next
/// Mark still counts from the previous mark.
void Span(std::string_view phase, double start, double end);

/// A launcher session (--launcher-session) turns this on: every Mark and Span line from then on,
/// and every SessionLine, is also written to the standard output the process started with, one
/// line at a time and at once (the log file is buffered), for the launcher that waits on it.
/// Off by default; turning it off ends the output.
void SetSessionOutput(bool enabled);

/// Writes `line` and a newline to the session output while it is on; nothing otherwise.
void SessionLine(std::string_view line);

/// Writes the loading screen's progress to the session output ("Boot progress: build 120 1346")
/// when the stage changes or the whole percent does. `stage` is a VideoCore::LoadCallbackStage
/// value: 0 prepare, 1 build, 2 complete, 3 warm; others are ignored.
void SessionProgress(int stage, std::size_t value, std::size_t total);

} // namespace Common::BootTiming
