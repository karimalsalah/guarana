// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <vector>

#include "common/common_types.h"

/// EDEN_NX2_POWER_POLICY=igpu: for the length of a game session, turns the processor performance
/// boost mode off and lowers the minimum processor state to 5% in the active Windows power
/// scheme, so the CPU leaves the package's power budget to the integrated GPU. The values it
/// found are kept in a restore file in the config directory until they are written back.
namespace Common::PowerPolicy {

struct Setting {
    std::string name; ///< "boost_mode", "min_state" or "min_state_1" (the second core class)
    u32 saved_ac{};
    u32 saved_dc{};
    u32 applied_ac{};
    u32 applied_dc{};
};

struct Status {
    bool requested{}; ///< EDEN_NX2_POWER_POLICY=igpu
    bool active{};    ///< The scheme holds the applied values now
    std::string scheme; ///< GUID of the scheme that was changed
    std::vector<Setting> settings;
    std::string message; ///< Why it is not active, or what the last call did
};

/// Whether EDEN_NX2_POWER_POLICY=igpu is set.
[[nodiscard]] bool Requested();

/// Emulator start: writes back what a restore file left by a session that never restored
/// (a crash) says, and deletes it. A file whose session is still running is left alone.
void RecoverStale();

/// Game boot: saves the active scheme's values and applies the policy. False, with nothing
/// changed, when a call fails (a host that wants elevation for it) or off Windows.
bool Apply();

/// Game stop and emulator exit: writes the saved values back. Does nothing when not active.
void Restore();

[[nodiscard]] Status GetStatus();

} // namespace Common::PowerPolicy
