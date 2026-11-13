// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string_view>

namespace Common::BootTiming {

/// Milliseconds since the process started.
[[nodiscard]] double Now();

/// Logs a boot phase boundary at Info ("Boot timing: ..."): the time since the process started
/// and since the previous mark.
void Mark(std::string_view phase);

/// As Mark, with how long the phase took since `start` (a Now() value).
void Mark(std::string_view phase, double start);

} // namespace Common::BootTiming
