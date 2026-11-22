// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <span>
#include <string>
#include <utility>
#include <vector>

#include "common/common_types.h"
#include "video_core/gpu_costs.h"

/// EDEN_NX2_VRS_AUTO=<target fps> (with EDEN_NX2_VRS): feeds every published GpuCostSnapshot to
/// a CostGovernor and puts its table in the automatic layer of the shading-rate registry
/// (Nx2Debug::SetAutoShadingRates). Off, nothing here runs past one atomic load a snapshot.
namespace VideoCore::AutoShadingRate {

/// Any thread: the target in frames per second, 0 when off. Starts from EDEN_NX2_VRS_AUTO.
[[nodiscard]] double TargetFps();
/// Any thread: sets the target; 0 turns the governor off and empties its layer. The caller turns
/// cost sampling on (Scheduler::RequestCostSampling), without which no snapshot arrives.
void SetTargetFps(double fps);

/// Any thread: whether the governor may coarsen, true unless someone says otherwise. The frame
/// budget controller (VideoCore::FrameBudget) allows it only while the game's own resolution has
/// nothing left to give; disallowing empties the layer at once.
void SetAllowed(bool allowed);
/// Any thread: whether any shader is at a coarser rate through the governor.
[[nodiscard]] bool Coarsening();

/// Pipeline creation: this fragment shader writes depth, so it is never coarsened.
void NoteDepthWriter(u64 fragment_hash);
/// Any thread: fragment shaders the user wants left alone (EDEN_NX2_VRS_AUTO_DENY=<hash>,...).
void SetDenied(std::span<const u64> fragment_hashes, bool denied);

/// Scheduler worker thread, about once a second: a new snapshot was sampled.
void OnSnapshot(const GpuCostSnapshot& snapshot);

/// Fragment size of a CostGovernor level: 1x1, 2x1, 2x2, 4x4.
[[nodiscard]] std::pair<u8, u8> LevelSize(u8 level);

/// The fragment hashes of the snapshot the governor must leave alone: every draw of theirs is a
/// direct draw of at most 6 vertices and one instance (first), or goes to another render target
/// than the main scene target (second).
[[nodiscard]] std::pair<std::vector<u64>, std::vector<u64>> SnapshotDenied(
    const GpuCostSnapshot& snapshot);

struct Status {
    double target_fps{};
    double frame_ms{};  ///< Measured at the last snapshot, 0 when unknown
    double target_ms{};
    bool allowed{true}; ///< SetAllowed
    u64 snapshots{};    ///< Snapshots the governor ran on
    u8 max_level{};
    std::vector<std::pair<u64, u8>> levels; ///< Fragment hash to level, sorted by hash
    std::vector<u64> denied_user;
    std::vector<u64> denied_small; ///< Seen drawing only small direct draws
    std::vector<u64> denied_side;  ///< Drew only into side targets in the last snapshot
    u64 depth_writers{};
    std::string last_action; ///< The last raise, lower or drop
    std::string hold_reason; ///< Why the last snapshot was not acted on, empty otherwise
};
[[nodiscard]] Status GetStatus();

} // namespace VideoCore::AutoShadingRate
