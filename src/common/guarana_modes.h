// SPDX-FileCopyrightText: Copyright 2026 Guarana contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Common::Guarana {

/// The play modes of the Guarana settings page. Custom changes nothing.
enum class Mode { Custom, Smooth, Quality, Battery };

[[nodiscard]] Mode ModeFromString(std::string_view name);
[[nodiscard]] const char* ModeToString(Mode mode);

/// The EDEN_NX2_* switches a mode puts in the user's nx2_switches.txt. Empty for Custom and
/// Quality: nothing steers the GPU, power or presentation.
///   Smooth:  EDEN_NX2_FLUID=2, EDEN_NX2_CADENCE=3, EDEN_NX2_EXTRAPOLATE=1, EDEN_NX2_GPL=1,
///            EDEN_NX2_EARLY_SUBMIT=1, EDEN_NX2_EARLY_PAGES=256, EDEN_NX2_EARLY_AFTER=1 (the pacing
///            measured best)
///   Battery: EDEN_NX2_POWER_POLICY=igpu, EDEN_NX2_ECO_BACKGROUND=1, EDEN_NX2_AUTOPILOT=1
[[nodiscard]] std::vector<std::pair<std::string, std::string>> ModeSwitches(Mode mode);

/// Returns `file_text` with the block the page manages replaced by `switches` (removed when it is
/// empty). The block is placed at the top, so a line the user wrote below it wins. Everything
/// outside the block is kept byte for byte.
[[nodiscard]] std::string ReplaceManagedBlock(
    const std::string& file_text, const std::vector<std::pair<std::string, std::string>>& switches);

} // namespace Common::Guarana
