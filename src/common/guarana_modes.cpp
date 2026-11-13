// SPDX-FileCopyrightText: Copyright 2026 Guarana contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/guarana_modes.h"

namespace Common::Guarana {

namespace {
constexpr std::string_view BlockBegin = "# >>> Guarana mode (written by the Guarana settings page)";
constexpr std::string_view BlockEnd = "# <<< Guarana mode";
} // namespace

Mode ModeFromString(std::string_view name) {
    if (name == "smooth") {
        return Mode::Smooth;
    }
    if (name == "quality") {
        return Mode::Quality;
    }
    if (name == "battery") {
        return Mode::Battery;
    }
    return Mode::Custom;
}

const char* ModeToString(Mode mode) {
    switch (mode) {
    case Mode::Smooth:
        return "smooth";
    case Mode::Quality:
        return "quality";
    case Mode::Battery:
        return "battery";
    case Mode::Custom:
        break;
    }
    return "custom";
}

std::vector<std::pair<std::string, std::string>> ModeSwitches(Mode mode) {
    switch (mode) {
    case Mode::Smooth:
        // The configuration measured smoothest (Ocarina of Time, Arc 140T): the vblank-locked
        // presenter, extrapolated frames, pipeline library links and early read-backs.
        return {{"EDEN_NX2_FLUID", "1"},         {"EDEN_NX2_EXTRAPOLATE", "1"},
                {"EDEN_NX2_GPL", "1"},           {"EDEN_NX2_EARLY_SUBMIT", "1"},
                {"EDEN_NX2_EARLY_PAGES", "256"}, {"EDEN_NX2_EARLY_AFTER", "1"}};
    case Mode::Battery:
        return {{"EDEN_NX2_POWER_POLICY", "igpu"},
                {"EDEN_NX2_ECO_BACKGROUND", "1"},
                {"EDEN_NX2_AUTOPILOT", "1"}};
    case Mode::Custom:
    case Mode::Quality:
        break;
    }
    return {};
}

std::string ReplaceManagedBlock(const std::string& file_text,
                                const std::vector<std::pair<std::string, std::string>>& switches) {
    std::string rest = file_text;
    const auto begin = rest.find(BlockBegin);
    if (begin != std::string::npos) {
        const auto end = rest.find(BlockEnd, begin);
        if (end != std::string::npos) {
            auto stop = rest.find('\n', end);
            stop = stop == std::string::npos ? rest.size() : stop + 1;
            if (stop < rest.size() && rest[stop] == '\n') {
                ++stop;
            }
            rest.erase(begin, stop - begin);
        }
    }
    if (switches.empty()) {
        return rest;
    }
    std::string block;
    block += BlockBegin;
    block += "\n";
    for (const auto& [name, value] : switches) {
        block += name + "=" + value + "\n";
    }
    block += BlockEnd;
    block += "\n";
    if (!rest.empty()) {
        block += "\n";
    }
    return block + rest;
}

} // namespace Common::Guarana
