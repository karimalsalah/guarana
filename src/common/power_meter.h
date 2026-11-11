// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <string>

namespace Common {

/// Host package power through the Windows Energy Meter Interface (RAPL on Intel and AMD).
/// Unavailable on other hosts and where no meter has a package, core or GPU channel.
class PowerMeter {
public:
    struct Sample {
        double package_w{};
        double cpu_w{};
        double gpu_w{};
        bool valid{};
    };

    static PowerMeter& Instance();

    ~PowerMeter();
    PowerMeter(const PowerMeter&) = delete;
    PowerMeter& operator=(const PowerMeter&) = delete;

    [[nodiscard]] bool Available() const;

    /// Average watts since the previous Read (invalid on the first). Channels are matched by
    /// name: PKG is the package, PP0 the CPU cores, PP1 the integrated GPU. Thread-safe; meant
    /// for a few calls per second at most.
    Sample Read();

    /// Every channel found, comma separated ("none" without any).
    [[nodiscard]] std::string ChannelNames() const;

private:
    PowerMeter();

    struct Impl;
    std::unique_ptr<Impl> impl;
};

/// User and kernel CPU time this process has used, in seconds.
[[nodiscard]] double ProcessCpuSeconds();

} // namespace Common
