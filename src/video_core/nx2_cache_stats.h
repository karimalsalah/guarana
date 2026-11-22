// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>

#include "common/common_types.h"

/// Hit, miss and eviction counters of the texture and buffer caches, published for the debug
/// server's cache_stats tool (docs/nx2/debug-mcp.md). Relaxed atomics: the caches count from
/// their own threads (the GPU front and back), the server reads them. Always counted: a few
/// atomic adds per lookup beside the locks the lookups already take.
namespace VideoCommon::Nx2CacheStats {

struct Counters {
    /// Texture lookups (FindOrInsertImage) that found a live image / made a new one.
    std::atomic<u64> texture_hits{};
    std::atomic<u64> texture_misses{};
    std::atomic<u64> texture_created{};
    std::atomic<u64> texture_deleted{};
    /// Images the garbage collector evicted, and those that downloaded their contents first.
    std::atomic<u64> texture_evicted{};
    std::atomic<u64> texture_evicted_downloaded{};
    /// Buffer lookups (FindBuffer) that found a usable buffer / made a new one.
    std::atomic<u64> buffer_hits{};
    std::atomic<u64> buffer_misses{};
    std::atomic<u64> buffer_created{};
    std::atomic<u64> buffer_deleted{};
    /// Buffers the garbage collector evicted.
    std::atomic<u64> buffer_evicted{};
};

inline Counters g_counters;

inline void Add(std::atomic<u64>& counter, u64 amount = 1) {
    counter.fetch_add(amount, std::memory_order_relaxed);
}

} // namespace VideoCommon::Nx2CacheStats
