// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "common/common_types.h"

namespace FrontendCommon {

enum ModInstallResult {
    Cancelled,
    Failed,
    Success,
};

/// The file InstallMod writes into a mod's folder. The game never sees it: it reads only the
/// mod's exefs, romfs, romfslite, romfs_ext and cheats folders.
inline constexpr std::string_view MOD_INFO_FILE = "mod.json";

/// What `mod.json` records about an installed mod.
struct ModInfo {
    std::string name;   ///< The name it is installed under, which is its folder's
    std::string source; ///< The file or folder it was imported from, without the path to it
    std::string date;   ///< When it was installed, in UTC: 2026-11-16T21:04:05Z

    bool operator==(const ModInfo&) const = default;
};

struct ModInstallOptions {
    /// Leave the source where it is. False removes it once the mod is installed.
    bool copy = true;
    /// Replace a mod already installed under the same name. False leaves that mod as it is, and
    /// InstallMod returns Cancelled: ask the player first, then call again with true.
    bool replace_existing = false;
    /// The name to install under. Empty takes the source folder's name.
    std::string name;
    /// What the player picked, for mod.json: a zip's or a folder's name. Empty takes the source
    /// folder's name.
    std::string source;
};

/// A file of the game that more than one mod replaces.
struct ModFileConflict {
    std::string path; ///< Its path in the game's RomFS, with forward slashes
    /// The mods that have it, in load order: the game gets the first one's file.
    std::vector<std::string> mods;

    bool operator==(const ModFileConflict&) const = default;
};

std::vector<std::filesystem::path> GetModFolder(const std::string& root);

/// The folder that holds a game's mods, one folder per mod.
[[nodiscard]] std::filesystem::path GetModsDir(u64 program_id);

/// Whether `name` names one folder inside the folder it is joined to: not empty, "." or "..", and
/// without "/" or "\". On Windows also without ":", since a folder joined with "D:" is drive D's
/// current folder, and not ending in "." or " ", which Windows drops.
[[nodiscard]] bool IsModName(std::string_view name);

/// The name InstallMod installs `path` under with `options`.
[[nodiscard]] std::string InstalledModName(const std::filesystem::path& path,
                                           const ModInstallOptions& options = {});

/// Whether a mod of that name is installed for the game.
[[nodiscard]] bool IsModInstalled(u64 program_id, std::string_view name);

/// Installs the mod folder `path` for the game. The copy is made next to the mods first and
/// swapped in when it is complete, so a copy that fails leaves an installed mod of the same name
/// untouched.
ModInstallResult InstallMod(const std::filesystem::path& path, u64 program_id,
                            const ModInstallOptions& options = {});

/// The time as mod.json stores it: UTC, 2026-11-16T21:04:05Z.
[[nodiscard]] std::string FormatModDate(std::chrono::system_clock::time_point when);

/// Writes mod.json into the mod's folder. A mod.json that is not one of ours (it holds anything
/// but name, source and date) belongs to the mod and is left alone: false.
bool WriteModInfo(const std::filesystem::path& mod_dir, const ModInfo& info);

/// The mod.json of a mod's folder, with the fields it has. nullopt when there is none or it is
/// not a JSON object.
[[nodiscard]] std::optional<ModInfo> ReadModInfo(const std::filesystem::path& mod_dir);

/// The game files that more than one mod in `mods_dir` replaces through romfs or romfslite, by
/// path. Mods named in `disabled` are left out. The order within a conflict is the load order:
/// mods are layered by name and the first one that has a file wins (PatchManager's LayeredFS).
/// Paths are compared as written, so two files that differ only in case are two files here.
[[nodiscard]] std::vector<ModFileConflict> FindModFileConflicts(
    const std::filesystem::path& mods_dir, std::span<const std::string> disabled = {});

} // namespace FrontendCommon
