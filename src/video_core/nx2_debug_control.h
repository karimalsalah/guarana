// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/common_types.h"

/// Requests from the debug server (EDEN_NX2_DEBUG_SERVER, docs/nx2/debug-mcp.md) to the thread
/// that owns the rasterizer. The server posts them from its own thread; RasterizerVulkan takes
/// them at the end of each presented frame, so every cache they touch is used from its owner.
/// Without the server nothing is posted and the per-frame cost is one relaxed atomic load.
namespace VideoCore::Nx2Debug {

struct CaptureRequest {
    u64 id;
    std::filesystem::path directory;
    bool sampled;
    /// The uniform and storage buffers of every draw and dispatch, and dispatch group counts.
    bool buffers;
};

struct CaptureResult {
    std::string error;
};

struct GpuReadRequest {
    u64 id;
    u64 gpu_address;
    u64 size;
};

struct GpuReadResult {
    std::vector<u8> bytes;
    /// Tegra::MemoryManager::GetID of the address space that maps the whole range.
    u64 address_space;
    std::string error;
};

struct RejectChange {
    u64 generation;
    /// Whether a hash left the list, so programs rejected before must be lowered again.
    bool unrejected;
};

/// Opens a debug server session (clears requests and rejections) or ends it (wakes waiters).
void BeginSession();
void EndSession();

/// Rasterizer thread: whether a capture or a reject change is waiting.
[[nodiscard]] bool HasRequests() noexcept;

/// Server thread: queues a frame capture; the rasterizer takes it at the next frame end.
void RequestCapture(CaptureRequest request);
/// Server thread: withdraws a capture that was not taken yet (it timed out).
void CancelCapture(u64 id);
/// Server thread: waits for the capture's manifest to be complete. Empty on timeout or when the
/// session ends.
[[nodiscard]] std::optional<CaptureResult> WaitCapture(u64 id, std::chrono::milliseconds timeout);
/// Rasterizer thread.
[[nodiscard]] std::optional<CaptureRequest> TakeCapture();
/// Frame dump writer thread, once the manifest summary is written (or the capture failed).
void FinishCapture(u64 id, CaptureResult result);

/// Server thread: queues a read of GPU virtual memory; the rasterizer takes it at the next frame
/// end, where the GPU's own reads of its address spaces run.
void RequestGpuRead(GpuReadRequest request);
/// Server thread: withdraws a read that was not taken yet (it timed out).
void CancelGpuRead(u64 id);
/// Server thread: empty on timeout or when the session ends.
[[nodiscard]] std::optional<GpuReadResult> WaitGpuRead(u64 id, std::chrono::milliseconds timeout);
/// Rasterizer thread.
[[nodiscard]] std::optional<GpuReadRequest> TakeGpuRead();
/// Rasterizer thread.
void FinishGpuRead(u64 id, GpuReadResult result);

/// Server thread: adds or removes native program keys or shader hashes (as EDEN_NX2_REJECT_HASH
/// takes them). Returns the generation to wait for.
u64 SetRejected(std::span<const u64> hashes, bool rejected);
[[nodiscard]] std::vector<u64> RejectedHashes();
/// Lowering: whether the debug server rejects this program. Cheap while the list is empty.
[[nodiscard]] bool IsRejected(u64 hash);
[[nodiscard]] bool AnyRejected() noexcept;
/// Rasterizer thread: the pending reject change, once.
[[nodiscard]] std::optional<RejectChange> TakeRejectChange();
/// Rasterizer thread: the cached programs of this generation were dropped.
void NoteRejectApplied(u64 generation);
/// Server thread: waits until the rasterizer applied the generation.
[[nodiscard]] bool WaitRejectApplied(u64 generation, std::chrono::milliseconds timeout);

/// Fragment size a fragment shader is shaded at (EDEN_NX2_VRS, VK_KHR_fragment_shading_rate).
struct ShadingRate {
    u8 width{1};
    u8 height{1};

    bool operator==(const ShadingRate&) const = default;
};

struct ShadingRateTable {
    u64 generation{};
    /// The rate of every fragment shader not listed.
    ShadingRate fallback{};
    /// Sorted by fragment shader hash.
    std::vector<std::pair<u64, ShadingRate>> rates;
    /// The automatic governor's layer (EDEN_NX2_VRS_AUTO), sorted by hash. Used only where the
    /// manual table says nothing: no entry in rates and a 1x1 fallback.
    std::vector<std::pair<u64, ShadingRate>> auto_rates;
};

/// "1x1", "2x1", "1x2", "2x2", "4x2", "2x4" or "4x4".
[[nodiscard]] std::optional<ShadingRate> ParseShadingRate(std::string_view text);
/// Any thread: sets the rate of these fragment shader hashes ("1x1" forgets them), or with
/// `all` the rate of every shader not listed. The table starts from EDEN_NX2_VRS_HASHES
/// (<hash>:<rate>,...) and EDEN_NX2_VRS_DEFAULT (<rate>), and outlives debug sessions.
void SetShadingRates(std::span<const u64> hashes, ShadingRate rate, bool all);
/// Any thread: replaces the automatic layer (VideoCore::AutoShadingRate). The manual entries are
/// never changed by it and win over it.
void SetAutoShadingRates(std::vector<std::pair<u64, ShadingRate>> rates);
[[nodiscard]] ShadingRateTable ShadingRates();
/// Any thread, each draw: the rate wanted for this fragment shader. One atomic load and a search
/// in a copy the thread keeps; the lock is taken only after the table changed.
[[nodiscard]] ShadingRate ShadingRateFor(u64 fragment_hash);

/// Alpha-test prepass split (EDEN_NX2_PREPASS_HASHES): the draws of the listed fragment shaders
/// are drawn as a depth pass and a colour pass at equal depth, where the pipeline and the draw's
/// state allow it (PipelineCache, RasterizerVulkan::PrepareDraw).
struct PrepassTable {
    u64 generation{};
    /// Sorted fragment shader hashes.
    std::vector<u64> hashes;
};

struct PrepassCounts {
    u64 pipelines;   ///< graphics pipelines built with the two passes' pipelines
    u64 split_draws; ///< draws recorded as two passes
    u64 plain_draws; ///< draws of those pipelines whose state did not allow it
};

/// Any thread: lists these fragment shader hashes or takes them off the list. The list starts
/// from EDEN_NX2_PREPASS_HASHES (<hash>,...) and outlives debug sessions.
void SetPrepassHashes(std::span<const u64> hashes, bool on);
[[nodiscard]] PrepassTable PrepassHashes();
/// Any thread: changes whenever the list does. One atomic load.
[[nodiscard]] u64 PrepassGeneration() noexcept;
/// Any thread: whether the fragment shader is listed. One atomic load and a search in a copy
/// the thread keeps; the lock is taken only after the list changed.
[[nodiscard]] bool IsPrepassHash(u64 fragment_hash);
/// EDEN_NX2_PREPASS_FORCE=1, read once: the split is also taken where work other than shading
/// then happens twice (a stage before the fragment one that writes storage, the samples-passed
/// counter).
[[nodiscard]] bool PrepassForced();
void CountPrepassPipeline() noexcept;
void CountPrepassDraw(bool split) noexcept;
[[nodiscard]] PrepassCounts PrepassCountsSoFar() noexcept;

} // namespace VideoCore::Nx2Debug
