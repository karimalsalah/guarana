// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>

#include "video_core/cost_governor.h"

namespace VideoCore {

CostGovernor::Decision CostGovernor::Update(const GpuCostSnapshot& snapshot, double frame_ms,
                                            double target_ms, RateTable& rates) {
    if (target_ms <= 0.0 || frame_ms <= 0.0) {
        return {};
    }
    const u8 max_level = (std::min)(config.max_level, MaxRateLevel);
    if (frame_ms > target_ms * config.raise_ratio) {
        under_streak = 0;
        if (++over_streak < (std::max)(config.raise_after, u32{1})) {
            return {};
        }
        // A fragment shader's cost is spread over the vertex shaders it is drawn with.
        std::unordered_map<u64, double> totals;
        for (const GpuCostSnapshot::Entry& entry : snapshot.work) {
            if (!entry.IsCompute()) {
                totals[entry.fragment_hash] += entry.total_ms;
            }
        }
        bool found{};
        u64 best{};
        double best_ms{};
        for (const auto& [hash, total_ms] : totals) {
            if (total_ms < config.min_total_ms || config.deny.contains(hash)) {
                continue;
            }
            const auto it = rates.find(hash);
            if (it != rates.end() && it->second >= max_level) {
                continue;
            }
            // Ties go to the lower hash, so the choice does not depend on the map's order.
            if (!found || total_ms > best_ms || (total_ms == best_ms && hash < best)) {
                found = true;
                best = hash;
                best_ms = total_ms;
            }
        }
        if (!found) {
            // Nothing left to coarsen: the streak is kept, so a new candidate is raised at once.
            return {};
        }
        over_streak = 0;
        const u8 level = ++rates[best];
        raised.push_back(best);
        return {Action::Raise, best, level};
    }
    if (frame_ms < target_ms * config.lower_ratio) {
        over_streak = 0;
        if (++under_streak < (std::max)(config.lower_after, u32{1})) {
            return {};
        }
        // Entries the caller removed or lowered itself are skipped.
        while (!raised.empty()) {
            const u64 hash = raised.back();
            raised.pop_back();
            const auto it = rates.find(hash);
            if (it == rates.end() || it->second == 0) {
                continue;
            }
            under_streak = 0;
            const u8 level = --it->second;
            if (level == 0) {
                rates.erase(it);
            }
            return {Action::Lower, hash, level};
        }
        return {};
    }
    // Inside the band: both streaks start over.
    over_streak = 0;
    under_streak = 0;
    return {};
}

void CostGovernor::Reset() {
    over_streak = 0;
    under_streak = 0;
    raised.clear();
}

} // namespace VideoCore
