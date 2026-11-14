// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <utility>

#include "common/common_types.h"

namespace Vulkan {

/// EDEN_NX2_CACHE_SAFE: non-zero decimal integer enables serialized driver pipeline cache reads
/// where the driver cannot safely read a VkPipelineCache while pipelines build into it.
[[nodiscard]] inline bool PipelineCacheSafeRequested(const char* value) noexcept {
    return value != nullptr && std::strtoul(value, nullptr, 10) != 0;
}

/// Serializes driver pipeline cache reads only when requested and the driver cannot read its cache
/// concurrently with pipeline creation (VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS, IGCIT #1614).
[[nodiscard]] constexpr bool ShouldSerializeDriverPipelineCacheReads(
    bool can_read_during_builds, bool cache_safe_requested) noexcept {
    return cache_safe_requested && !can_read_during_builds;
}

/// Shared/exclusive gate between vkCreate*Pipelines (shared) and vkGetPipelineCacheData
/// (exclusive), plus a monotonic count of pipeline builds into the driver cache.
class DriverPipelineCacheGate {
public:
    /// Held by each pipeline build into a VkPipelineCache: shared when `serialize_reads` is true,
    /// unlocked otherwise. Must not be acquired recursively on the same thread.
    [[nodiscard]] std::shared_lock<std::shared_timed_mutex> LockForBuild(
        bool serialize_reads) const {
        std::shared_lock<std::shared_timed_mutex> lock;
        if (serialize_reads) {
            lock = std::shared_lock{mutex};
        }
        // Counted once the lock is held: a serialized read sees every build counted before it.
        builds.fetch_add(1, std::memory_order_relaxed);
        return lock;
    }

    /// Held by each read of a VkPipelineCache (vkGetPipelineCacheData): exclusive when
    /// `serialize_reads` is true, unlocked otherwise. With `max_wait` it gives up after that
    /// timeout, returning an unlocked unique_lock.
    [[nodiscard]] std::unique_lock<std::shared_timed_mutex> LockForRead(
        bool serialize_reads,
        std::optional<std::chrono::milliseconds> max_wait = std::nullopt) const {
        if (!serialize_reads) {
            return {};
        }
        if (!max_wait) {
            return std::unique_lock{mutex};
        }
        std::unique_lock lock{mutex, std::defer_lock};
        static_cast<void>(lock.try_lock_for(*max_wait));
        return lock;
    }

    /// Attempts to acquire LockForRead up to `max_tries` times while `serialize_reads` is true,
    /// calling `backoff()` between tries so running builds can finish without starving, and
    /// aborting early if `stop_requested()` becomes true.
    template <typename StopFn, typename BackoffFn>
    [[nodiscard]] std::unique_lock<std::shared_timed_mutex> LockForReadWithRetries(
        bool serialize_reads, std::optional<std::chrono::milliseconds> max_wait, int max_tries,
        StopFn&& stop_requested, BackoffFn&& backoff, int& out_tries) const {
        out_tries = 1;
        auto lock = LockForRead(serialize_reads, max_wait);
        for (; serialize_reads && !lock.owns_lock() && out_tries < max_tries; ++out_tries) {
            backoff();
            if (stop_requested()) {
                break;
            }
            lock = LockForRead(serialize_reads, max_wait);
        }
        return lock;
    }

    /// Pipeline builds into a VkPipelineCache so far. When read under LockForRead(true), counts
    /// exactly the builds that completed before the exclusive read lock was acquired.
    [[nodiscard]] u64 Builds() const noexcept {
        return builds.load(std::memory_order_relaxed);
    }

private:
    mutable std::shared_timed_mutex mutex;
    mutable std::atomic<u64> builds{};
};

enum class DriverCacheSaveOutcome {
    Written,
    SkippedUnchangedBuilds,
    SkippedSameSize,
    BusyBuilding,
    ReadFailed,
    WriteFailed,
};

struct DriverCacheSaveResult {
    using Clock = std::chrono::steady_clock;

    DriverCacheSaveOutcome outcome{DriverCacheSaveOutcome::ReadFailed};
    size_t cache_size{};
    u64 builds_since_saved{};
    int lock_tries{1};
    Clock::time_point wait_begin{};
    Clock::time_point read_begin{};
    Clock::time_point read_end{};
};

/// Tracks what the on-disk driver pipeline cache file currently holds (its driver-reported byte
/// size and the build count when it was loaded or last written) and coordinates gated reads and
/// unchanged-save skipping. Caller serializes concurrent saves externally.
class DriverPipelineCacheSaveTracker {
public:
    void ResetForLoad(u64 builds_now) noexcept {
        saved_size = 0;
        saved_builds = builds_now;
    }

    void SetSavedSize(size_t held_size) noexcept {
        saved_size = held_size;
    }

    [[nodiscard]] size_t SavedSize() const noexcept {
        return saved_size;
    }

    [[nodiscard]] u64 SavedBuilds() const noexcept {
        return saved_builds;
    }

    template <typename StopFn, typename BackoffFn, typename QuerySizeFn, typename ReadDataFn,
              typename WriteFileFn>
    [[nodiscard]] DriverCacheSaveResult Save(
        const DriverPipelineCacheGate& gate, bool serialize_reads, bool skip_same_size,
        std::optional<std::chrono::milliseconds> max_lock_wait, int max_lock_tries,
        StopFn&& stop_requested, BackoffFn&& backoff, QuerySizeFn&& query_size,
        ReadDataFn&& read_data, WriteFileFn&& write_file) {
        using Clock = DriverCacheSaveResult::Clock;
        DriverCacheSaveResult result{};
        result.wait_begin = Clock::now();

        size_t cache_size = 0;
        std::unique_ptr<char[]> cache_data;
        u64 builds = 0;
        {
            auto read_lock = gate.LockForReadWithRetries(
                serialize_reads, max_lock_wait, max_lock_tries, std::forward<StopFn>(stop_requested),
                std::forward<BackoffFn>(backoff), result.lock_tries);
            result.read_begin = Clock::now();
            if (serialize_reads && !read_lock.owns_lock()) {
                result.outcome = DriverCacheSaveOutcome::BusyBuilding;
                result.cache_size = saved_size;
                result.read_end = result.read_begin;
                return result;
            }
            builds = gate.Builds();
            result.builds_since_saved = builds - saved_builds;
            if (serialize_reads && builds == saved_builds) {
                result.outcome = DriverCacheSaveOutcome::SkippedUnchangedBuilds;
                result.cache_size = saved_size;
                result.read_end = Clock::now();
                return result;
            }
            bool read_ok = query_size(cache_size);
            if (read_ok && serialize_reads && skip_same_size && cache_size == saved_size) {
                // Warm-boot cache hits built pipelines without growing the driver's cache. Leave
                // saved_builds untouched so a non-size-skipping save on exit or reload still
                // writes if needed.
                result.outcome = DriverCacheSaveOutcome::SkippedSameSize;
                result.cache_size = cache_size;
                result.read_end = Clock::now();
                return result;
            }
            if (read_ok) {
                cache_data = std::make_unique_for_overwrite<char[]>(cache_size);
                if (cache_size != 0) {
                    read_ok = read_data(cache_size, cache_data.get());
                }
            }
            if (!read_ok) {
                result.outcome = DriverCacheSaveOutcome::ReadFailed;
                result.cache_size = saved_size;
                result.read_end = Clock::now();
                return result;
            }
        }
        result.read_end = Clock::now();
        result.cache_size = cache_size;

        if (!write_file(cache_data, cache_size)) {
            result.outcome = DriverCacheSaveOutcome::WriteFailed;
            return result;
        }
        saved_size = cache_size;
        saved_builds = builds;
        result.outcome = DriverCacheSaveOutcome::Written;
        return result;
    }

private:
    size_t saved_size{};
    u64 saved_builds{};
};

} // namespace Vulkan
