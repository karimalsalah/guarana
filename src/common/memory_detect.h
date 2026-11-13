// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <algorithm>
#include <optional>
#include <string_view>

#include "common/common_types.h"

namespace Common {

struct MemoryInfo {
    u64 TotalPhysicalMemory{};
    u64 TotalSwapMemory{};
};

/**
 * Gets the memory info of the host system
 * @return Reference to a MemoryInfo struct with the physical and swap memory sizes in bytes
 */
[[nodiscard]] const MemoryInfo& GetMemInfo();

/// The host's RAM in bytes. installed: the amount fitted in the machine (Windows' firmware table,
/// macOS). Otherwise it is the memory the OS manages, short of what firmware and an integrated GPU
/// reserve.
struct HostRam {
    u64 bytes{};
    bool installed{};
};

[[nodiscard]] HostRam GetHostRam();

/// Whether the host has less than the 16 GiB of RAM Switch 2 titles need. Memory the OS manages
/// passes from 12 GiB, so a 16 GiB machine that reserves up to 4 GiB for its GPU still passes.
[[nodiscard]] bool IsBelowSwitch2RamMinimum(HostRam ram);

/// This process's memory in bytes. Zero where the host does not report a value.
struct ProcessMemoryUsage {
    u64 commit{};              ///< Private commit (Windows PrivateUsage, Linux VmData).
    u64 working_set{};         ///< Resident memory.
    u64 private_working_set{}; ///< Resident memory not shared with other mappings.
    u64 system_commit{};       ///< Commit charge of the whole system (Windows).
    u64 system_commit_limit{}; ///< Commit limit of the whole system (Windows).
    u64 peak_commit{};         ///< Highest private commit so far (Windows).
    u64 peak_working_set{};    ///< Highest working set so far (Windows).
    u64 physical_total{};      ///< RAM of the host (Windows).
    u64 physical_available{};  ///< RAM the host can still hand out (Windows).
};

[[nodiscard]] ProcessMemoryUsage GetProcessMemoryUsage();

/// Commit the system can still hand out, in bytes: its commit limit (RAM plus the page files)
/// minus what is committed. No value where the host reports no such limit (anything but Windows).
[[nodiscard]] std::optional<u64> GetSystemCommitRoom();

/// Hosts with this much RAM or less run in low memory mode unless EDEN_NX2_LOW_MEMORY says no.
inline constexpr u64 LowMemoryModeRam = u64{20} << 30;

/// Low memory mode: caches that hold host memory to avoid rebuilding things keep less of it.
/// EDEN_NX2_LOW_MEMORY=1 or =0 decides; without it, on for hosts with LowMemoryModeRam or less
/// (not on Android, whose limits are already its own).
[[nodiscard]] bool LowMemoryMode();

/// Why LowMemoryMode() is what it is, for the log.
[[nodiscard]] std::string_view LowMemoryModeReason();

/// What the renderer's caches hold, published by their owners for the debug server's `memory`
/// method. Relaxed stores of values the owner has at hand; zero until first published.
struct MemoryCounters {
    std::atomic<u64> device_usage{};       ///< Device memory in use, as the texture cache counts it
    std::atomic<u64> texture_expected{};   ///< Texture cache thresholds
    std::atomic<u64> texture_critical{};
    std::atomic<u64> buffer_cache_bytes{}; ///< The buffer cache's buffers
    std::atomic<u64> staging_bytes{};      ///< Cached staging buffers (upload, download, device)
    std::atomic<u64> mesh_arena_bytes{};   ///< Mesh emulation output arenas
    std::atomic<u64> graphics_pipelines{}; ///< Graphics pipeline cache entries
    std::atomic<u64> built_pipelines{};    ///< Of them built, at the last eviction check
    std::atomic<u64> kept_pipelines_limit{};
    std::atomic<u64> kept_spirv_entries{}; ///< Compressed SPIR-V kept of evicted pipelines
    std::atomic<u64> kept_spirv_bytes{};
    std::atomic<u64> guest_resident{};     ///< Guest memory resident at the last zero-page trim
    std::atomic<u64> guest_zero_trimmed{}; ///< Zero pages trimmed since boot
    std::atomic<u64> guest_lazy_commit{};  ///< Guest memory committed on demand (lazy commit)
};
inline MemoryCounters g_memory_counters;
/// Physical memory the host can hand out now without paging anything out (0 when unknown).
[[nodiscard]] u64 GetAvailablePhysicalMemory();

/// Whether the host is short of RAM: less than a quarter of it, and less than 4 GiB, free.
[[nodiscard]] constexpr bool IsLowOnMemory(u64 available, u64 total) {
    return available != 0 && available < (std::min<u64>)(total / 4, u64{4} << 30);
}

/// IsLowOnMemory for this host, read again at most once a second.
[[nodiscard]] bool IsHostLowOnMemory();

} // namespace Common
