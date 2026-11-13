// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace Common {
struct PowerStatus {
    int percentage = -1;
    bool charging = false;
    bool has_battery = true;
    int ac_online = -1; ///< 1 on AC power, 0 on battery, -1 unknown (set on Windows)
};

PowerStatus GetPowerStatus();
} // namespace Common