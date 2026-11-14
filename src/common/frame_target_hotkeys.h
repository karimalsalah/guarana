// SPDX-FileCopyrightText: Copyright 2026 Guarana contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstdlib>
#include <limits>
#include <string_view>

#include "common/common_types.h"

namespace Common::FrameTargetHotkeys {

/// The frame generation targets the "Frame target N" hotkeys set and "Cycle frame target" steps
/// through, lowest first.
inline constexpr std::array<u32, 3> Presets{30, 40, 60};

/// The target after `current` in the cycle: the first preset above it, else the first preset again.
/// A target of 0 (a fixed multiplier) or one above every preset starts the cycle over.
[[nodiscard]] constexpr u32 NextPreset(u32 current) {
    for (const u32 preset : Presets) {
        if (preset > current) {
            return preset;
        }
    }
    return Presets.front();
}

/// EDEN_NX2_FRAME_TARGET_HOTKEYS: unset, empty, 0 and false are off.
[[nodiscard]] inline bool EnabledFromValue(const char* value) {
    return value != nullptr && std::strtoul(value, nullptr, 10) != 0;
}

[[nodiscard]] inline bool Enabled() {
    return EnabledFromValue(std::getenv("EDEN_NX2_FRAME_TARGET_HOTKEYS"));
}

/// Session-only target: hotkeys never mutate configuration storage read by renderer threads.
class TargetOverride {
public:
    void Set(u32 target) {
        value.store(target, std::memory_order_relaxed);
    }

    void Reset() {
        value.store(Unset, std::memory_order_relaxed);
    }

    template <typename ReadConfigured>
    [[nodiscard]] u32 Resolve(bool enabled, ReadConfigured read_configured) const {
        if (enabled) {
            const u32 target = value.load(std::memory_order_relaxed);
            if (target != Unset) {
                return target;
            }
        }
        return read_configured();
    }

private:
    static constexpr u32 Unset = (std::numeric_limits<u32>::max)();
    std::atomic<u32> value{Unset};
};

inline TargetOverride SessionTarget;

/// The hotkeys the switch adds (the names in UISettings::default_hotkeys). With the switch off
/// Configure does not list them and the main window does not connect them.
[[nodiscard]] constexpr bool IsHotkeyName(std::string_view name) {
    return name == "Frame target 30" || name == "Frame target 40" || name == "Frame target 60" ||
           name == "Cycle frame target";
}

} // namespace Common::FrameTargetHotkeys
