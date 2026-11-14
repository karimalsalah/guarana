// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/query_cache/bank_base.h"
#include "video_core/query_cache/query_stream.h"
#include "video_core/query_cache/running_query_bound.h"

namespace {

class TestStreamer : public VideoCommon::SimpleStreamer<VideoCommon::HostQueryBase> {
public:
    TestStreamer() : SimpleStreamer{0} {}

    using SimpleStreamer::Free;
    using SimpleStreamer::FreeMany;

    size_t WriteCounter(VAddr, bool, u32, std::optional<u32>) override {
        return BuildQuery();
    }

    size_t Build() {
        return BuildQuery();
    }

    size_t Slots() const {
        return slot_queries.size();
    }
};

} // Anonymous namespace

TEST_CASE("Query banks count references taken and dropped", "[video_core][query_cache]") {
    VideoCommon::BankBase bank{4};
    bank.AddReference(3);
    bank.CloseReference(2);
    CHECK(bank.References() == 1);
    bank.Close();
    CHECK_FALSE(bank.IsDead());
    bank.CloseReference();
    CHECK(bank.IsDead());
}

TEST_CASE("Freed query ids are reused oldest first, a batch at a time",
          "[video_core][query_cache]") {
    TestStreamer streamer;
    for (size_t id = 0; id < 4; ++id) {
        REQUIRE(streamer.Build() == id);
    }
    const std::vector<size_t> freed{2, 1};
    streamer.FreeMany(freed);
    CHECK(streamer.Build() == 2);
    streamer.Free(0);
    CHECK(streamer.Build() == 1);
    CHECK(streamer.Build() == 0);
    CHECK(streamer.Build() == 4);
    CHECK(streamer.Slots() == 5);
}

TEST_CASE("A running samples query of 2,000 slots keeps its count", "[video_core][query_cache]") {
    namespace Bound = VideoCommon::RunningSamplesQuery;
    STATIC_REQUIRE(Bound::BankSlots == 256);
    STATIC_REQUIRE(Bound::MaxSlots == 32 * Bound::BankSlots);
    // CloseCounter checks the window at each submission, which grows it by a slot at a time.
    size_t first_drop = 0;
    for (size_t size_slots = 1; size_slots <= 2000; ++size_slots) {
        if (Bound::IsOverBound(size_slots)) {
            first_drop = size_slots;
            break;
        }
    }
    CHECK(first_drop == 0);
    // The old bound of 4 banks dropped it from slot 1,025 on.
    CHECK_FALSE(Bound::IsOverBound(4 * Bound::BankSlots + 1));
    CHECK_FALSE(Bound::IsOverBound(Bound::MaxSlots));
}

TEST_CASE("A running samples query past the bound drops its count, with a warning",
          "[video_core][query_cache]") {
    namespace Bound = VideoCommon::RunningSamplesQuery;
    CHECK(Bound::IsOverBound(Bound::MaxSlots + 1));
    // Ocarina of Time's title screen reaches 16,384 slots: still dropped.
    CHECK(Bound::IsOverBound(16384));
    // The warning fires on the 1st, 2nd, 4th, 8th... drop.
    std::vector<u64> logged;
    for (u64 drop = 1; drop <= 20; ++drop) {
        if (Bound::IsLoggedDrop(drop)) {
            logged.push_back(drop);
        }
    }
    CHECK(logged == std::vector<u64>{1, 2, 4, 8, 16});
    CHECK_FALSE(Bound::IsLoggedDrop(0));
}

TEST_CASE("Query ids stay unique when the building thread changes", "[video_core][query_cache]") {
    TestStreamer streamer;
    std::mutex handoff;
    std::deque<size_t> built;
    std::vector<char> live;
    std::atomic<bool> reused_live{};
    std::atomic<bool> building{true};
    std::thread fencing([&] {
        std::vector<size_t> batch;
        while (true) {
            const bool last = !building.load();
            {
                std::scoped_lock lock{handoff};
                batch.assign(built.begin(), built.end());
                built.clear();
                for (const size_t id : batch) {
                    live[id] = 0;
                }
            }
            streamer.FreeMany(batch);
            if (last) {
                return;
            }
            std::this_thread::yield();
        }
    });
    const auto build = [&] {
        for (size_t i = 0; i < 20000; ++i) {
            const size_t id = streamer.Build();
            std::scoped_lock lock{handoff};
            if (id >= live.size()) {
                live.resize(id + 1);
            }
            if (live[id]) {
                reused_live = true;
            }
            live[id] = 1;
            built.push_back(id);
        }
    };
    // The split's start and stop: the builder moves to a new thread, which takes over the list
    // the previous one left, ordered by the thread's start and the join.
    std::thread{build}.join();
    build();
    std::thread{build}.join();
    building = false;
    fencing.join();
    CHECK_FALSE(reused_live);
}
