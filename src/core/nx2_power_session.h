// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <string>

#include "common/common_types.h"

namespace Core {

class System;

/// EDEN_NX2_POWER_POLICY=igpu on an integrated GPU: holds the power scheme change of the session
/// (Common::PowerPolicy) and, once a second, lets Common::SpinGovernor move the spin scale.
class Nx2PowerSession {
public:
    struct GovernorState {
        bool running{};
        std::string state{"off"}; ///< off, no_data, balanced, gpu_bound or cpu_bound
        u32 spin_percent{};
        u32 ceiling{};
        double target_fps{};
        double frame_ms{};
        double gpu_busy{-1.0}; ///< Busy time over frame time, negative when unknown
        double package_w{};
        double cpu_w{};
        double gpu_w{};
        u64 changes{};
        std::string last_change;
    };

    /// Game boot, after the renderer exists. Null unless the policy is asked for and the
    /// renderer runs on an integrated GPU.
    [[nodiscard]] static std::unique_ptr<Nx2PowerSession> MaybeStart(System& system);

    /// Game stop: ends the governor, puts the spin scale and the power scheme back.
    ~Nx2PowerSession();

    /// Any thread: the last sample of the governor, for the power tool of the debug server.
    [[nodiscard]] static GovernorState LastState();

private:
    struct Impl;
    explicit Nx2PowerSession(System& system);

    std::unique_ptr<Impl> impl;
};

} // namespace Core
