// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "common/event_ring.h"

TEST_CASE("Event ring retains newest events in time order", "[common][event_ring]") {
    Common::EventRing<3> ring;
    REQUIRE(ring.Snapshot().size == 0);
    for (std::uint64_t value = 0; value < 5; ++value) {
        ring.Record(7, {value, value + 1, value + 2, value + 3}, "test");
    }
    const auto history = ring.Snapshot();
    REQUIRE(history.size == 3);
    REQUIRE(history.overwritten == 2);
    for (std::size_t index = 0; index < history.size; ++index) {
        const auto& event = history.events[index];
        REQUIRE(event.kind == 7);
        REQUIRE(event.fields == (std::array<std::uint64_t, 4>{index + 2, index + 3,
                                                          index + 4, index + 5}));
        REQUIRE(event.tag == std::string_view{"test"});
        if (index != 0) {
            REQUIRE(event.timestamp >= history.events[index - 1].timestamp);
        }
    }
    REQUIRE(ring.Snapshot().size == 3);
    Common::EventRing<1> single;
    single.Record(1, {}, "first");
    single.Record(2, {}, "last");
    REQUIRE(single.Snapshot().events[0].kind == 2);
    REQUIRE(single.Snapshot().overwritten == 1);
}

TEST_CASE("Event ring serializes writers and concurrent snapshots", "[common][event_ring]") {
    Common::EventRing<17> ring;
    std::atomic<unsigned int> finished{};
    std::array<std::thread, 4> writers;
    for (std::size_t writer = 0; writer < writers.size(); ++writer) {
        writers[writer] = std::thread([&, writer] {
            for (std::uint64_t index = 0; index < 1000; ++index) {
                ring.Record(9, {writer, index, writer, index}, "concurrent");
            }
            finished.fetch_add(1);
        });
    }
    bool consistent = true;
    do {
        const auto history = ring.Snapshot();
        consistent = consistent && history.size <= 17;
        for (std::size_t index = 0; index < history.size; ++index) {
            const auto& event = history.events[index];
            consistent = consistent && event.kind == 9 && event.fields[0] == event.fields[2] &&
                         event.fields[1] == event.fields[3];
            if (index != 0) {
                consistent = consistent && event.timestamp >= history.events[index - 1].timestamp;
            }
        }
    } while (finished.load() != writers.size());
    for (auto& writer : writers) {
        writer.join();
    }
    REQUIRE(consistent);
    const auto history = ring.Snapshot();
    REQUIRE(history.size == 17);
    REQUIRE(history.overwritten == 4000 - 17);
}
