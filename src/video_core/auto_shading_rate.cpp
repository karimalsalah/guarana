// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include <fmt/format.h>

#include "common/logging.h"
#include "core/core.h"
#include "video_core/auto_shading_rate.h"
#include "video_core/cost_governor.h"
#include "video_core/frame_budget.h"
#include "video_core/nx2_debug_control.h"

namespace VideoCore::AutoShadingRate {
namespace {

/// Bounds the set of shaders seen drawing only small draws.
constexpr size_t MaxSmall = 4096;

struct State {
    std::mutex mutex;
    bool env_read{};
    bool allowed{true};
    double target_fps{};
    CostGovernor governor;
    CostGovernor::RateTable rates;
    std::unordered_set<u64> depth_writers;
    std::unordered_set<u64> user_deny;
    std::unordered_set<u64> small_seen;
    std::vector<u64> side;
    double frame_ms{};
    u64 snapshots{};
    std::string last_action;
    std::string hold_reason;
};

State state;

/// Caller holds state.mutex. Present times are recorded only while someone asks for them.
void SetTargetLocked(double fps) {
    fps = std::isfinite(fps) && fps >= 1.0 ? (std::min)(fps, 1000.0) : 0.0;
    if ((fps > 0.0) != (state.target_fps > 0.0)) {
        if (fps > 0.0) {
            Core::g_nx2_present_time_services.fetch_add(1, std::memory_order_relaxed);
        } else {
            Core::g_nx2_present_time_services.fetch_sub(1, std::memory_order_relaxed);
        }
    }
    state.target_fps = fps;
}

/// Caller holds state.mutex.
void ReadEnvLocked() {
    if (state.env_read) {
        return;
    }
    state.env_read = true;
    CostGovernor::Config config;
    if (const char* value = std::getenv("EDEN_NX2_VRS_AUTO_MAX")) {
        config.max_level = static_cast<u8>(
            (std::clamp)(std::strtol(value, nullptr, 10), 1L, long{CostGovernor::MaxRateLevel}));
    }
    state.governor.SetConfig(std::move(config));
    if (const char* value = std::getenv("EDEN_NX2_VRS_AUTO_DENY")) {
        std::string_view rest{value};
        while (!rest.empty()) {
            const size_t comma = rest.find(',');
            const std::string_view item = rest.substr(0, comma);
            rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
            u64 hash = 0;
            const auto [end, error] =
                std::from_chars(item.data(), item.data() + item.size(), hash, 16);
            if (item.size() == 16 && error == std::errc{} && end == item.data() + item.size()) {
                state.user_deny.insert(hash);
            } else if (!item.empty()) {
                LOG_WARNING(Render_Vulkan, "EDEN_NX2_VRS_AUTO_DENY: bad hash \"{}\"", item);
            }
        }
    }
    const char* const value = std::getenv("EDEN_NX2_VRS_AUTO");
    if (!value) {
        return;
    }
    if (!std::getenv("EDEN_NX2_VRS")) {
        LOG_WARNING(Render_Vulkan, "EDEN_NX2_VRS_AUTO needs EDEN_NX2_VRS: ignored");
        return;
    }
    SetTargetLocked(std::strtod(value, nullptr));
    if (state.target_fps > 0.0) {
        LOG_INFO(Render_Vulkan, "VRS auto: target {:.1f} fps, max level {}, {} denied by the user",
                 state.target_fps, state.governor.GetConfig().max_level, state.user_deny.size());
    } else {
        LOG_WARNING(Render_Vulkan, "EDEN_NX2_VRS_AUTO: bad target \"{}\" (want fps >= 1)", value);
    }
}

/// Caller holds state.mutex.
void PublishLocked() {
    std::vector<std::pair<u64, Nx2Debug::ShadingRate>> table;
    table.reserve(state.rates.size());
    for (const auto& [hash, level] : state.rates) {
        const auto [width, height] = LevelSize(level);
        table.emplace_back(hash, Nx2Debug::ShadingRate{.width = width, .height = height});
    }
    Nx2Debug::SetAutoShadingRates(std::move(table));
}

std::vector<u64> Sorted(const std::unordered_set<u64>& set) {
    std::vector<u64> list(set.begin(), set.end());
    std::ranges::sort(list);
    return list;
}

} // Anonymous namespace

double TargetFps() {
    std::scoped_lock lock{state.mutex};
    ReadEnvLocked();
    return state.target_fps;
}

void SetTargetFps(double fps) {
    std::scoped_lock lock{state.mutex};
    ReadEnvLocked();
    const double before = state.target_fps;
    SetTargetLocked(fps);
    if (state.target_fps == before) {
        return;
    }
    LOG_INFO(Render_Vulkan, "VRS auto: target {:.1f} fps (was {:.1f})", state.target_fps, before);
    if (state.target_fps > 0.0) {
        return;
    }
    state.rates.clear();
    state.side.clear();
    state.governor.Reset();
    state.frame_ms = 0.0;
    state.hold_reason.clear();
    PublishLocked();
}

void SetAllowed(bool allowed) {
    std::scoped_lock lock{state.mutex};
    if (state.allowed == allowed) {
        return;
    }
    state.allowed = allowed;
    if (allowed || state.rates.empty()) {
        return;
    }
    LOG_INFO(Render_Vulkan, "VRS auto: drop {} shaders (the frame budget has resolution left)",
             state.rates.size());
    state.last_action = "drop all (the frame budget has resolution left)";
    state.rates.clear();
    state.governor.Reset();
    PublishLocked();
}

bool Coarsening() {
    std::scoped_lock lock{state.mutex};
    return !state.rates.empty();
}

void NoteDepthWriter(u64 fragment_hash) {
    std::scoped_lock lock{state.mutex};
    state.depth_writers.insert(fragment_hash);
}

void SetDenied(std::span<const u64> fragment_hashes, bool denied) {
    std::scoped_lock lock{state.mutex};
    ReadEnvLocked();
    bool dropped = false;
    for (const u64 hash : fragment_hashes) {
        if (denied) {
            state.user_deny.insert(hash);
            dropped |= state.rates.erase(hash) != 0;
        } else {
            state.user_deny.erase(hash);
        }
    }
    if (dropped) {
        PublishLocked();
    }
}

std::pair<u8, u8> LevelSize(u8 level) {
    switch (level) {
    case 0:
        return {u8{1}, u8{1}};
    case 1:
        return {u8{2}, u8{1}};
    case 2:
        return {u8{2}, u8{2}};
    default:
        return {u8{4}, u8{4}};
    }
}

std::pair<std::vector<u64>, std::vector<u64>> SnapshotDenied(const GpuCostSnapshot& snapshot) {
    struct Draws {
        u32 all{};
        u32 small_draws{};
        u32 side_draws{};
    };
    std::unordered_map<u64, Draws> by_fragment;
    for (const GpuCostSnapshot::Entry& entry : snapshot.work) {
        if (entry.IsCompute()) {
            continue;
        }
        Draws& draws = by_fragment[entry.fragment_hash];
        draws.all += entry.draws;
        draws.small_draws += entry.small_draws;
        draws.side_draws += entry.side_draws;
    }
    std::pair<std::vector<u64>, std::vector<u64>> denied;
    for (const auto& [hash, draws] : by_fragment) {
        if (draws.all == 0) {
            continue;
        }
        if (draws.small_draws == draws.all) {
            denied.first.push_back(hash);
        }
        if (snapshot.has_main_target && draws.side_draws == draws.all) {
            denied.second.push_back(hash);
        }
    }
    std::ranges::sort(denied.first);
    std::ranges::sort(denied.second);
    return denied;
}

void OnSnapshot(const GpuCostSnapshot& snapshot) {
    std::scoped_lock lock{state.mutex};
    ReadEnvLocked();
    if (state.target_fps <= 0.0 || snapshot.frame == 0) {
        return;
    }
    ++state.snapshots;
    const double target_ms = 1000.0 / state.target_fps;
    // Guest presents only: frames the host generates between them never reach this ring. With a
    // frame budget the presents are held to the target, so the headroom only shows in the frame
    // time without that wait.
    const double budget_ms = FrameBudget::RecentWorkMs();
    const double frame_ms = budget_ms > 0.0 ? budget_ms : Core::Nx2RecentFrameMs();
    state.frame_ms = frame_ms;
    state.hold_reason.clear();

    auto [small_now, side_now] = SnapshotDenied(snapshot);
    for (const u64 hash : small_now) {
        if (state.small_seen.size() < MaxSmall) {
            state.small_seen.insert(hash);
        }
    }
    state.side = std::move(side_now);

    const Nx2Debug::ShadingRateTable manual = Nx2Debug::ShadingRates();
    CostGovernor::Config config = state.governor.GetConfig();
    config.deny = state.user_deny;
    config.deny.insert(state.depth_writers.begin(), state.depth_writers.end());
    config.deny.insert(state.small_seen.begin(), state.small_seen.end());
    config.deny.insert(state.side.begin(), state.side.end());
    for (const auto& [hash, rate] : manual.rates) {
        config.deny.insert(hash);
    }

    bool changed = false;
    for (auto it = state.rates.begin(); it != state.rates.end();) {
        if (!config.deny.contains(it->first)) {
            ++it;
            continue;
        }
        state.last_action = fmt::format("drop {:016x} (now denied)", it->first);
        LOG_INFO(Render_Vulkan, "VRS auto: drop {:016x} from level {} (now denied), frame {:.2f} "
                                "ms, target {:.2f} ms",
                 it->first, it->second, frame_ms, target_ms);
        it = state.rates.erase(it);
        changed = true;
    }
    state.governor.SetConfig(std::move(config));

    if (!state.allowed) {
        state.hold_reason = "the frame budget has resolution left";
    } else if (manual.fallback != Nx2Debug::ShadingRate{}) {
        state.hold_reason = "a manual default rate is set";
    } else if (frame_ms <= 0.0) {
        state.hold_reason = "no guest present times in the last second";
    } else {
        const CostGovernor::Decision decision =
            state.governor.Update(snapshot, frame_ms, target_ms, state.rates);
        if (decision.action != CostGovernor::Action::Hold) {
            const char* const verb =
                decision.action == CostGovernor::Action::Raise ? "raise" : "lower";
            const auto [width, height] = LevelSize(decision.level);
            state.last_action =
                fmt::format("{} {:016x} to level {} ({}x{}) at {:.2f} ms", verb,
                            decision.fragment_hash, decision.level, width, height, frame_ms);
            LOG_INFO(Render_Vulkan, "VRS auto: {} {:016x} to level {} ({}x{}), frame {:.2f} ms, "
                                    "target {:.2f} ms",
                     verb, decision.fragment_hash, decision.level, width, height, frame_ms,
                     target_ms);
            changed = true;
        }
    }
    if (changed) {
        PublishLocked();
    }
}

Status GetStatus() {
    std::scoped_lock lock{state.mutex};
    ReadEnvLocked();
    Status status{
        .target_fps = state.target_fps,
        .frame_ms = state.frame_ms,
        .target_ms = state.target_fps > 0.0 ? 1000.0 / state.target_fps : 0.0,
        .allowed = state.allowed,
        .snapshots = state.snapshots,
        .max_level = state.governor.GetConfig().max_level,
        .levels = {state.rates.begin(), state.rates.end()},
        .denied_user = Sorted(state.user_deny),
        .denied_small = Sorted(state.small_seen),
        .denied_side = state.side,
        .depth_writers = state.depth_writers.size(),
        .last_action = state.last_action,
        .hold_reason = state.hold_reason,
    };
    std::ranges::sort(status.levels);
    return status;
}

} // namespace VideoCore::AutoShadingRate
