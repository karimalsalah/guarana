// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <string>

#include "common/common_types.h"

namespace Core {

class System;

/// EDEN_NX2_AUTOPILOT=1: every 2 s measures the real (guest) fps, the intervals of the shown
/// frames, the refresh, the host GPU time and the power source, lets
/// VideoCore::AutopilotController decide and applies the steps: the frame generation target or
/// multiplier, the GPU time scale of the frame budget and the power policy. Only presentation and
/// resolution change; the game, its saves and its state are not touched. What it changed is put
/// back when the game stops.
class Nx2AutopilotSession {
public:
    struct State {
        bool running{};
        u64 windows{};
        u64 changes{};
        double real_fps{};
        double shown_p50_ms{};
        double shown_p95_ms{};
        double shown_even_percent{};
        u32 shown_samples{};
        double refresh_hz{};
        std::string refresh_source{"unknown"}; ///< present thread, display, or unknown
        double gpu_busy{-1.0};
        std::string power{"unknown"}; ///< ac, battery or unknown
        bool frame_gen_on{};
        u32 frame_gen_target{};
        u32 frame_gen_multiplier{};
        double scale{1.0};
        double scale_base{1.0};
        bool resolution_available{};
        bool power_available{};
        bool power_applied{};
        std::string last_change;
    };

    /// Whether EDEN_NX2_AUTOPILOT=1.
    [[nodiscard]] static bool Requested();

    /// Game boot, after the renderer exists. Null unless asked for.
    [[nodiscard]] static std::unique_ptr<Nx2AutopilotSession> MaybeStart(System& system);

    /// Game stop: ends the thread and puts back what it still holds.
    ~Nx2AutopilotSession();

    /// Any thread: the last window, for the debug server.
    [[nodiscard]] static State LastState();

private:
    struct Impl;
    explicit Nx2AutopilotSession(System& system);

    std::unique_ptr<Impl> impl;
};

} // namespace Core
