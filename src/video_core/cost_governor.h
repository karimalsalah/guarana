// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/common_types.h"
#include "video_core/gpu_costs.h"

namespace VideoCore {

/// Picks which fragment shaders shade at a coarser rate from the sampled GPU costs. Pure logic:
/// the caller applies the table.
class CostGovernor {
public:
    /// Rate levels: 0 = 1x1, 1 = 2x1, 2 = 2x2, 3 = 4x4.
    static constexpr u8 MaxRateLevel = 3;
    using RateTable = std::unordered_map<u64, u8>; ///< Fragment hash to level; absent is 0

    struct Config {
        double raise_ratio = 1.05; ///< Over budget above target * raise_ratio
        double lower_ratio = 0.85; ///< Headroom below target * lower_ratio
        u32 raise_after = 2;       ///< Consecutive snapshots over budget before a raise
        u32 lower_after = 4;       ///< Consecutive snapshots with headroom before a lower
        double min_total_ms = 0.5; ///< Cheaper shaders are left alone
        u8 max_level = 2;
        std::unordered_set<u64> deny; ///< Fragment hashes never coarsened
    };

    enum class Action : u8 { Hold, Raise, Lower };
    struct Decision {
        Action action = Action::Hold;
        u64 fragment_hash{};
        u8 level{}; ///< The hash's level after the action
    };

    CostGovernor() = default;
    explicit CostGovernor(Config config_) : config{std::move(config_)} {}

    /// Feeds one snapshot with the frame time measured around it; changes at most one entry of
    /// rates by one level.
    Decision Update(const GpuCostSnapshot& snapshot, double frame_ms, double target_ms,
                    RateTable& rates);

    /// Forgets the streaks and the raise history (scene change, table cleared by the caller).
    void Reset();

    [[nodiscard]] const Config& GetConfig() const {
        return config;
    }
    void SetConfig(Config config_) {
        config = std::move(config_);
    }

private:
    Config config;
    u32 over_streak{};
    u32 under_streak{};
    std::vector<u64> raised; ///< One fragment hash per raise, oldest first
};

} // namespace VideoCore
