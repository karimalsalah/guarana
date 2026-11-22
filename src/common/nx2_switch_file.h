// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <utility>

#include "common/switch_origin.h"

// nx2_switches.txt, shared by the frontends: "NAME=VALUE" lines, one per line, '#' comments. The
// Qt frontend reads several copies (src/lichii/nx2_switches.h); the Android app reads the one in
// its user folder.
namespace Common::Nx2SwitchFile {

/// Adds the file's entries to `entries`; a later line wins. A missing file adds nothing.
inline void Parse(const std::filesystem::path& path, std::map<std::string, std::string>& entries) {
    std::ifstream file{path};
    if (!file) {
        return;
    }
    constexpr const char* blank = " \t\r";
    std::string line;
    while (std::getline(file, line)) {
        line.erase(0, line.find_first_not_of(blank));
        line.erase(line.find_last_not_of(blank) + 1);
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        std::string name = line.substr(0, eq);
        std::string value = line.substr(eq + 1);
        name.erase(name.find_last_not_of(blank) + 1);
        value.erase(0, value.find_first_not_of(blank));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        if (!name.empty()) {
            entries.insert_or_assign(std::move(name), std::move(value));
        }
    }
}

/// The names a switch file may set: the EDEN_NX2_* switches and a few diagnostics.
[[nodiscard]] inline bool IsSwitchName(const std::string& name) {
    return name.starts_with("EDEN_NX2_") || name == "EDEN_DKB_FRAME_TRACE" ||
           name == "EDEN_DKB_GPU_SITES" || name == "EDEN_DKB_INDIRECT_READBACK" ||
           name == "EDEN_NATIVE_DRAW_CAPTURE" ||
           name == "EDEN_NATIVE_DRAW_DIAGNOSTICS" || name == "EDEN_NATIVE_FRAME_CAPTURE" ||
           name == "EDEN_NATIVE_SHADER_OVERRIDE_DIR" || name == "EDEN_PUNCH_HOLE" ||
           name == "EDEN_STARFOX_WINDOW_TRACE" || name == "EDEN_VI2_MAX_RESOLUTION_MODE" ||
           name == "EGLANTINE_EXPERIMENTAL_STATIC_VERTEX_REPACK" ||
           name == "EGLANTINE_TEST_VERTEX_REPACK";
}

/// Puts each entry in this process's environment unless the real environment already has the
/// name, and records it as set by the process. Returns how many it set.
inline std::size_t Apply(const std::map<std::string, std::string>& entries) {
    std::size_t set = 0;
    for (const auto& [name, value] : entries) {
        if (std::getenv(name.c_str()) != nullptr) {
            continue;
        }
#ifdef _WIN32
        _putenv_s(name.c_str(), value.c_str());
#else
        setenv(name.c_str(), value.c_str(), 1);
#endif
        Common::SwitchOrigin::RecordSetByProcess(name);
        ++set;
    }
    return set;
}

/// A frontend with one switch file, the player's own (the Android app's user folder): its allowed
/// names go into the environment, the real environment first. Nothing is a shipped value, so a
/// game's switch defaults replace none of them. Returns how many switches it set.
inline std::size_t LoadPlayerFile(const std::filesystem::path& path) {
    std::map<std::string, std::string> entries;
    Parse(path, entries);
    std::erase_if(entries, [](const auto& entry) { return !IsSwitchName(entry.first); });
    return Apply(entries);
}

} // namespace Common::Nx2SwitchFile
