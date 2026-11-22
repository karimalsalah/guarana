// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "common/common_types.h"

namespace Settings {

inline constexpr u64 MarioKartWorldProgramId = 0x0400C3F00006E000ULL;
inline constexpr u64 HyruleWarriorsProgramId = 0x04003F40232DE000ULL;

// Settings that only affect one game. Per-game configuration shows them in that game's Tweaks tab.
inline const std::map<u64, std::vector<std::string>> game_tweaks{
    {MarioKartWorldProgramId,
     {"mkw_30fps", "mkw_60fps", "mkw_60fps_fallback", "mkw_dynamic_fps"}},
    {HyruleWarriorsProgramId, {"hyrule_dynamic_fps", "hyrule_30fps"}},
};

inline const std::vector<std::string>* FindGameTweaks(u64 program_id) {
    const auto it = game_tweaks.find(program_id);
    return it == game_tweaks.end() ? nullptr : &it->second;
}

inline bool IsGameTweak(const std::string& key) {
    for (const auto& [program_id, keys] : game_tweaks) {
        for (const auto& tweak : keys) {
            if (tweak == key) {
                return true;
            }
        }
    }
    return false;
}

// Guarana: whether `key` is a tweak of that game. Eglantine's Tweaks tab is not ported, so the
// game's own Adv. Graphics page lists its tweaks until it is.
inline bool HasGameTweak(u64 program_id, const std::string& key) {
    const auto* tweaks = FindGameTweaks(program_id);
    return tweaks != nullptr && std::find(tweaks->begin(), tweaks->end(), key) != tweaks->end();
}

} // namespace Settings
