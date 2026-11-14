// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>

// Which switches a frontend put in the environment from the nx2_switches.txt shipped next to the
// executable alone, so that a game's switch defaults (Core::GamePresets::ApplySwitchDefaults) can
// replace those and keep every other value: one from the real environment or from the player's own
// nx2_switches.txt. A record of this process only, never an environment variable, so nothing a
// child process inherits changes. The Qt frontend writes it once at start (LoadNx2Switches); a
// frontend that reads no switch file leaves it empty, and then every switch counts as the player's.
namespace Common::SwitchOrigin {

using SwitchMap = std::map<std::string, std::string>;

/// The entries of `merged` (every file's, as the loader puts them in the environment) that the
/// shipped file (`shipped`) set, that none of the player's files named (`named_by_player`, to any
/// value, the shipped one included) and that the real environment does not set (`in_environment`).
/// Pure.
[[nodiscard]] SwitchMap ShippedOnly(const SwitchMap& shipped, const SwitchMap& merged,
                                    const std::set<std::string>& named_by_player,
                                    const std::function<bool(const std::string&)>& in_environment);

/// Replaces the record with `switches` (ShippedOnly's result).
void RecordShippedOnly(SwitchMap switches);

/// The value the shipped file alone gave `name`, if the record holds it.
[[nodiscard]] std::optional<std::string> ShippedOnlyValue(std::string_view name);

/// Records that this process put `name` in its own environment: LoadNx2Switches from a switch
/// file, ApplySwitchDefaults from a game's switch default. Never a name of the real environment.
void RecordSetByProcess(const std::string& name);

/// The names RecordSetByProcess recorded. A process started in this one's place (the in-app
/// update's restart and installer) gets an environment without them, so it reads the switch
/// files again and no value of this process counts there as the player's.
[[nodiscard]] std::set<std::string> SetByProcess();

} // namespace Common::SwitchOrigin
