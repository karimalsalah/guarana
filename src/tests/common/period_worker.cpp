// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "common/period_worker.h"

using namespace std::chrono_literals;

namespace {

constexpr auto Patience = 10s;
constexpr std::chrono::nanoseconds Fast{1'000'000};
constexpr std::chrono::nanoseconds Hz120{8'333'333};
constexpr std::chrono::nanoseconds Hz144{6'944'444};

template <typename Predicate>
bool WaitUntil(Predicate&& done) {
    const auto deadline = std::chrono::steady_clock::now() + Patience;
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

// A latch a query waits on; it gives up after Patience so that a failing test cannot hang.
class Gate {
public:
    void Open() {
        {
            std::scoped_lock lock{mutex};
            opened = true;
        }
        cv.notify_all();
    }

    void Wait() {
        std::unique_lock lock{mutex};
        cv.wait_for(lock, Patience, [this] { return opened; });
    }

private:
    std::mutex mutex;
    std::condition_variable cv;
    bool opened{};
};

} // Anonymous namespace

TEST_CASE("A period worker has no result until its first query lands", "[common][period_worker]") {
    std::atomic<int> calls{0};
    Common::PeriodWorker worker{[&calls] {
                                    ++calls;
                                    return Hz120;
                                },
                                std::chrono::nanoseconds{1h}};
    CHECK(worker.Latest() == 0ns);
    CHECK(calls.load() == 0);
    worker.Stop();
    CHECK(worker.Latest() == 0ns);
    CHECK(calls.load() == 0);
    CHECK(worker.GetStats().queries == 0);
}

TEST_CASE("A period worker queries on its own thread and publishes the result",
          "[common][period_worker]") {
    const std::thread::id test_thread = std::this_thread::get_id();
    std::atomic<bool> ran_elsewhere{false};
    Common::PeriodWorker worker{[&ran_elsewhere, test_thread] {
                                    ran_elsewhere = std::this_thread::get_id() != test_thread;
                                    return Hz144;
                                },
                                Fast};
    REQUIRE(WaitUntil([&worker] { return worker.Latest() == Hz144; }));
    worker.Stop();
    CHECK(ran_elsewhere.load());
    CHECK(worker.GetStats().queries >= 1U);
    // Stopped for good: the last result stays readable.
    CHECK(worker.Latest() == Hz144);
    worker.Stop();
}

TEST_CASE("A period worker keeps its last result when a query fails or is out of range",
          "[common][period_worker]") {
    std::atomic<int> calls{0};
    Common::PeriodWorker worker{[&calls] {
                                    switch (++calls) {
                                    case 1:
                                        return Hz120;
                                    case 2:
                                        return std::chrono::nanoseconds{0};
                                    case 3:
                                        return std::chrono::nanoseconds{-5};
                                    case 4:
                                        // Does not fit in 32 bits of nanoseconds.
                                        return std::chrono::nanoseconds{5'000'000'000};
                                    default:
                                        return Hz120;
                                    }
                                },
                                Fast};
    REQUIRE(WaitUntil([&calls] { return calls >= 6; }));
    worker.Stop();
    CHECK(worker.Latest() == Hz120);
    CHECK(worker.GetStats().queries >= 6U);
}

TEST_CASE("A period worker never returns a result queried before a Retarget",
          "[common][period_worker]") {
    Gate first_entered;
    Gate first_may_return;
    Gate second_entered;
    Gate second_may_return;
    std::atomic<int> calls{0};
    {
        Common::PeriodWorker worker{[&] {
                                        switch (++calls) {
                                        case 1:
                                            first_entered.Open();
                                            first_may_return.Wait();
                                            return Hz120;
                                        case 2:
                                            second_entered.Open();
                                            second_may_return.Wait();
                                            return Hz144;
                                        default:
                                            return Hz144;
                                        }
                                    },
                                    Fast};
        // The target changes while the first query, which was made for the old one, is running.
        first_entered.Wait();
        REQUIRE(calls.load() >= 1);
        worker.Retarget();
        first_may_return.Open();
        // The old result is published under the old generation, and the second query, made for
        // the new target, has not returned: nothing is returned.
        second_entered.Wait();
        REQUIRE(calls.load() >= 2);
        CHECK(worker.Latest() == 0ns);
        second_may_return.Open();
        CHECK(WaitUntil([&worker] { return worker.Latest() == Hz144; }));
    }
    // Release anything still waiting; the worker is gone.
    first_may_return.Open();
    second_may_return.Open();
}

TEST_CASE("A period worker stops promptly between queries and counts what they cost",
          "[common][period_worker]") {
    const auto begun = std::chrono::steady_clock::now();
    {
        Common::PeriodWorker worker{[] {
                                        std::this_thread::sleep_for(3ms);
                                        return Hz120;
                                    },
                                    Fast};
        REQUIRE(WaitUntil([&worker] { return worker.GetStats().queries >= 2; }));
        worker.Stop();
        const auto stats = worker.GetStats();
        CHECK(stats.queries >= 2U);
        CHECK(stats.longest >= 3ms);
        CHECK(stats.total >= stats.longest);
    }
    // A worker with a long interval does not hold its owner for the interval at destruction.
    {
        Common::PeriodWorker idle{[] { return Hz120; }, std::chrono::nanoseconds{1h}};
    }
    CHECK(std::chrono::steady_clock::now() - begun < Patience);
}
