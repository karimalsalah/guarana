// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>

#include "common/common_types.h"

namespace Common {

/// Runs a slow query for a period on a helper thread, once every interval, and publishes the
/// result in one atomic word, so that a time-critical thread can read it without a lock and
/// without waiting for the query. Built for VblankClock's display-mode recheck: the query is
/// QueryDisplayConfig, which can take milliseconds.
///
/// The first query runs one interval after construction: the owner is expected to hold a value
/// from its own first query already, and Latest() is 0 until the first result lands.
///
/// A result belongs to the target it was queried for. Retarget() says the target changed (the
/// window moved to another monitor): a result queried before the call is never returned by
/// Latest(), also not when its query was still running at the call, until a query that began
/// after the call has been published.
class PeriodWorker {
public:
    using Query = std::function<std::chrono::nanoseconds()>;

    struct Stats {
        u64 queries{};
        std::chrono::nanoseconds total{};
        std::chrono::nanoseconds longest{};
    };

    /// query runs on the helper thread only. A result that is not positive, or that does not fit
    /// in 32 bits of nanoseconds (about 4.29 s), is dropped and the last result stays.
    PeriodWorker(Query query_, std::chrono::nanoseconds interval_)
        : query{std::move(query_)}, interval{interval_}, worker{[this] { Run(); }} {}

    ~PeriodWorker() {
        Stop();
    }

    PeriodWorker(const PeriodWorker&) = delete;
    PeriodWorker& operator=(const PeriodWorker&) = delete;

    /// Ends the helper thread, after a query that is running has returned. Safe to call twice.
    void Stop() {
        {
            std::scoped_lock lock{mutex};
            stopping = true;
        }
        wake.notify_all();
        if (worker.joinable()) {
            worker.join();
        }
    }

    /// The target of the query changed. Call it after the change is visible to the query.
    void Retarget() {
        generation.fetch_add(1, std::memory_order_acq_rel);
    }

    /// The latest result for the current target, 0 when there is none yet.
    [[nodiscard]] std::chrono::nanoseconds Latest() const {
        const u64 value = cell.load(std::memory_order_acquire);
        if (static_cast<u32>(value >> 32) != generation.load(std::memory_order_acquire)) {
            return {};
        }
        return std::chrono::nanoseconds{static_cast<s64>(value & u64{0xffff'ffff})};
    }

    /// What the queries cost the helper thread, which is what they cost the thread that used to
    /// run them. Exact once Stop() has returned.
    [[nodiscard]] Stats GetStats() const {
        return Stats{
            .queries = queries.load(std::memory_order_relaxed),
            .total = std::chrono::nanoseconds{total_ns.load(std::memory_order_relaxed)},
            .longest = std::chrono::nanoseconds{longest_ns.load(std::memory_order_relaxed)},
        };
    }

private:
    void Run() {
        std::unique_lock lock{mutex};
        // wait_for returns whether stopping is set: false is the end of an interval.
        while (!wake.wait_for(lock, interval, [this] { return stopping; })) {
            lock.unlock();
            Sample();
            lock.lock();
        }
    }

    void Sample() {
        // The target is read by the query after this, so a Retarget between the two makes the
        // result carry an older generation than the current one, and Latest() drops it.
        const u32 queried_for = generation.load(std::memory_order_acquire);
        const auto begun = std::chrono::steady_clock::now();
        const std::chrono::nanoseconds reported = query();
        const auto took = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - begun);
        queries.fetch_add(1, std::memory_order_relaxed);
        total_ns.fetch_add(took.count(), std::memory_order_relaxed);
        if (took.count() > longest_ns.load(std::memory_order_relaxed)) {
            longest_ns.store(took.count(), std::memory_order_relaxed);
        }
        const s64 ns = reported.count();
        if (ns > 0 && ns <= static_cast<s64>((std::numeric_limits<u32>::max)())) {
            cell.store((u64{queried_for} << 32) | static_cast<u64>(ns), std::memory_order_release);
        }
    }

    Query query;
    std::chrono::nanoseconds interval;
    std::mutex mutex;
    std::condition_variable wake;
    bool stopping{};
    /// The generation in the upper 32 bits, the period in nanoseconds in the lower 32.
    std::atomic<u64> cell{0};
    std::atomic<u32> generation{1};
    std::atomic<u64> queries{0};
    std::atomic<s64> total_ns{0};
    std::atomic<s64> longest_ns{0};
    // Last: the thread starts in the constructor and reads everything above.
    std::thread worker;
};

} // namespace Common
