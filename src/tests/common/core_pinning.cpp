// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <iterator>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "common/core_pinning.h"

namespace {

using Common::LogicalProcessor;

// Windows numbers CPU sets from 256 in logical processor order, and every SMT sibling of a core
// reports the logical processor index of the core's first sibling as its CoreIndex.
constexpr u32 FirstCpuSetId = 256;

void AddCores(std::vector<LogicalProcessor>& processors, u32 cores, u32 threads_per_core,
              u8 efficiency_class) {
    for (u32 core = 0; core < cores; ++core) {
        const u32 first = static_cast<u32>(processors.size());
        for (u32 thread = 0; thread < threads_per_core; ++thread) {
            processors.push_back({FirstCpuSetId + first + thread, first, efficiency_class});
        }
    }
}

std::vector<LogicalProcessor> Uniform(u32 cores, u32 threads_per_core) {
    std::vector<LogicalProcessor> processors;
    AddCores(processors, cores, threads_per_core, 0);
    return processors;
}

std::vector<u32> Ids(u32 first, u32 count) {
    std::vector<u32> ids(count);
    for (u32 i = 0; i < count; ++i) {
        ids[i] = FirstCpuSetId + first + i;
    }
    return ids;
}

std::vector<u32> Concat(std::vector<u32> head, const std::vector<u32>& tail) {
    std::ranges::copy(tail, std::back_inserter(head));
    return head;
}

std::vector<u32> Sorted(std::vector<u32> ids) {
    std::ranges::sort(ids);
    return ids;
}

u32 CoreOf(const std::vector<LogicalProcessor>& processors, u32 id) {
    return std::ranges::find(processors, id, &LogicalProcessor::id)->core;
}

void CheckSkipped(const Common::GpuThreadPinning& plan) {
    CHECK(plan.front.empty());
    CHECK(plan.back.empty());
    CHECK(plan.guest.empty());
}

// Front and back each own one whole core, and no guest processor is an SMT sibling of either.
void CheckCoresAreDedicated(const std::vector<LogicalProcessor>& processors,
                            const Common::GpuThreadPinning& plan) {
    REQUIRE(!plan.front.empty());
    REQUIRE(!plan.back.empty());
    const u32 front_core = CoreOf(processors, plan.front.front());
    const u32 back_core = CoreOf(processors, plan.back.front());
    CHECK(front_core != back_core);
    for (const u32 id : plan.front) {
        CHECK(CoreOf(processors, id) == front_core);
    }
    for (const u32 id : plan.back) {
        CHECK(CoreOf(processors, id) == back_core);
    }
    for (const u32 id : plan.guest) {
        CHECK(CoreOf(processors, id) != front_core);
        CHECK(CoreOf(processors, id) != back_core);
    }
}

} // Anonymous namespace

TEST_CASE("Core pinning: 8 cores, 16 threads", "[common][core_pinning]") {
    const auto processors = Uniform(8, 2);
    const auto plan = Common::PlanGpuThreadPinning(processors);
    CHECK(plan.performance_cores == 8);
    CHECK(plan.front == Ids(2, 2));
    CHECK(plan.back == Ids(4, 2));
    CHECK(plan.guest == Concat(Ids(0, 2), Ids(6, 10)));
    CheckCoresAreDedicated(processors, plan);
}

TEST_CASE("Core pinning: 6 cores, 12 threads", "[common][core_pinning]") {
    const auto processors = Uniform(6, 2);
    const auto plan = Common::PlanGpuThreadPinning(processors);
    CHECK(plan.performance_cores == 6);
    CHECK(plan.front == Ids(2, 2));
    CHECK(plan.back == Ids(4, 2));
    CHECK(plan.guest == Concat(Ids(0, 2), Ids(6, 6)));
    CheckCoresAreDedicated(processors, plan);
}

TEST_CASE("Core pinning: 6 cores without SMT", "[common][core_pinning]") {
    const auto processors = Uniform(6, 1);
    const auto plan = Common::PlanGpuThreadPinning(processors);
    CHECK(plan.front == Ids(1, 1));
    CHECK(plan.back == Ids(2, 1));
    CHECK(plan.guest == Concat(Ids(0, 1), Ids(3, 3)));
    CheckCoresAreDedicated(processors, plan);
}

TEST_CASE("Core pinning: hybrid 8 P-cores with SMT and 16 E-cores", "[common][core_pinning]") {
    std::vector<LogicalProcessor> processors;
    AddCores(processors, 8, 2, 1);
    AddCores(processors, 16, 1, 0);
    const auto plan = Common::PlanGpuThreadPinning(processors);
    CHECK(plan.performance_cores == 8);
    CHECK(plan.front == Ids(2, 2));
    CHECK(plan.back == Ids(4, 2));
    CHECK(plan.guest == Concat(Ids(0, 2), Ids(6, 10)));
    CheckCoresAreDedicated(processors, plan);
}

TEST_CASE("Core pinning: E-cores enumerated first leave the P-cores to the pinned threads",
          "[common][core_pinning]") {
    std::vector<LogicalProcessor> processors;
    AddCores(processors, 4, 1, 0);
    AddCores(processors, 6, 2, 1);
    const auto plan = Common::PlanGpuThreadPinning(processors);
    CHECK(plan.performance_cores == 6);
    CHECK(plan.front == Ids(6, 2));
    CHECK(plan.back == Ids(8, 2));
    CHECK(plan.guest == Concat(Ids(4, 2), Ids(10, 6)));
    CheckCoresAreDedicated(processors, plan);
}

TEST_CASE("Core pinning: hybrid 4 P-cores and 8 E-cores is skipped", "[common][core_pinning]") {
    std::vector<LogicalProcessor> processors;
    AddCores(processors, 4, 2, 1);
    AddCores(processors, 8, 1, 0);
    const auto plan = Common::PlanGpuThreadPinning(processors);
    CHECK(plan.performance_cores == 4);
    CheckSkipped(plan);
}

TEST_CASE("Core pinning: 4 cores, 4 threads is skipped", "[common][core_pinning]") {
    const auto plan = Common::PlanGpuThreadPinning(Uniform(4, 1));
    CHECK(plan.performance_cores == 4);
    CheckSkipped(plan);
}

TEST_CASE("Core pinning: 2 cores is skipped", "[common][core_pinning]") {
    for (const u32 threads : {1U, 2U}) {
        const auto plan = Common::PlanGpuThreadPinning(Uniform(2, threads));
        CHECK(plan.performance_cores == 2);
        CheckSkipped(plan);
    }
}

TEST_CASE("Core pinning: no processors reported", "[common][core_pinning]") {
    const auto plan = Common::PlanGpuThreadPinning({});
    CHECK(plan.performance_cores == 0);
    CheckSkipped(plan);
}

TEST_CASE("Core pinning: the enumeration order does not change the cores",
          "[common][core_pinning]") {
    const auto ordered = Uniform(8, 2);
    auto reversed = ordered;
    std::ranges::reverse(reversed);
    const auto expected = Common::PlanGpuThreadPinning(ordered);
    const auto plan = Common::PlanGpuThreadPinning(reversed);
    CHECK(Sorted(plan.front) == expected.front);
    CHECK(Sorted(plan.back) == expected.back);
    CHECK(Sorted(plan.guest) == expected.guest);
}

TEST_CASE("Core pinning: sysfs CPU lists", "[common][core_pinning]") {
    using Common::ParseCpuList;
    CHECK(ParseCpuList("0-3,8,10-11") == std::vector<u32>{0, 1, 2, 3, 8, 10, 11});
    CHECK(ParseCpuList("16-23\n") == std::vector<u32>{16, 17, 18, 19, 20, 21, 22, 23});
    CHECK(ParseCpuList("5") == std::vector<u32>{5});
    CHECK(ParseCpuList("").empty());
    CHECK(ParseCpuList("3-1").empty());
    CHECK(ParseCpuList("1,x").empty());
    CHECK(ParseCpuList("0-").empty());
    CHECK(ParseCpuList("0-4294967295").empty());
    CHECK(ParseCpuList("4294967295") == std::vector<u32>{4294967295U});
    CHECK(ParseCpuList("4294967294-4294967295") == std::vector<u32>{4294967294U, 4294967295U});
    CHECK(ParseCpuList("4294967296").empty());
}

TEST_CASE("Core pinning: a 4P+4E CPU whose E-cores rank lower is skipped",
          "[common][core_pinning]") {
    // As LinuxProcessors reports an Intel hybrid CPU without cpu_capacity: the cpu_atom cores at
    // half capacity.
    std::vector<LogicalProcessor> processors;
    AddCores(processors, 4, 2, 128);
    AddCores(processors, 4, 1, 64);
    const auto plan = Common::PlanGpuThreadPinning(processors);
    CHECK(plan.performance_cores == 4);
    CheckSkipped(plan);
}

namespace {

// A Core Ultra 9 285H as Windows reports it: 6 P cores (class 2), 8 E cores (class 1) behind the
// same cache, 2 low-power E cores (class 0) behind a cache of their own.
std::vector<LogicalProcessor> ArrowLakeH() {
    std::vector<LogicalProcessor> processors;
    AddCores(processors, 6, 1, 2);
    AddCores(processors, 8, 1, 1);
    AddCores(processors, 2, 1, 0);
    for (size_t i = 14; i < processors.size(); ++i) {
        processors[i].last_level_cache = 1;
    }
    return processors;
}

} // namespace

TEST_CASE("Hybrid plan: Arrow Lake-H puts the guest on the P cores and the pools on the E cores",
          "[common][core_pinning]") {
    const auto processors = ArrowLakeH();
    const auto plan = Common::PlanHybridCores(processors, true);
    REQUIRE(plan.active);
    CHECK(plan.performance_cores == 6);
    CHECK(plan.performance == Ids(0, 6));
    CHECK(plan.efficiency == Ids(6, 8));
    CHECK(plan.low_power == Ids(14, 2));
    CHECK(plan.responsive == Ids(0, 14));
    CHECK(plan.front == Ids(1, 1));
    CHECK(plan.back == Ids(2, 1));
    CHECK(Sorted(plan.latency) == Concat(Ids(0, 1), Ids(3, 3)));
}

TEST_CASE("Hybrid plan: without pinning every latency thread may use every P core",
          "[common][core_pinning]") {
    const auto plan = Common::PlanHybridCores(ArrowLakeH(), false);
    REQUIRE(plan.active);
    CHECK(plan.latency == Ids(0, 6));
    CHECK(plan.front == Ids(0, 6));
    CHECK(plan.back == Ids(0, 6));
}

TEST_CASE("Hybrid plan: low-power cores are the lowest of three classes even behind one cache",
          "[common][core_pinning]") {
    auto processors = ArrowLakeH();
    for (auto& processor : processors) {
        processor.last_level_cache = 0;
    }
    const auto plan = Common::PlanHybridCores(processors, true);
    REQUIRE(plan.active);
    CHECK(plan.efficiency == Ids(6, 8));
    CHECK(plan.low_power == Ids(14, 2));
}

TEST_CASE("Hybrid plan: two classes behind one cache have no low-power cores",
          "[common][core_pinning]") {
    std::vector<LogicalProcessor> processors;
    AddCores(processors, 8, 2, 2);
    AddCores(processors, 16, 1, 1);
    const auto plan = Common::PlanHybridCores(processors, true);
    REQUIRE(plan.active);
    CHECK(plan.performance.size() == 16);
    CHECK(plan.efficiency.size() == 16);
    CHECK(plan.low_power.empty());
}

TEST_CASE("Hybrid plan: CPUs that are not hybrid or have too few P cores are left alone",
          "[common][core_pinning]") {
    CHECK(!Common::PlanHybridCores(Uniform(8, 2), true).active);
    CHECK(!Common::PlanHybridCores({}, true).active);
    std::vector<LogicalProcessor> small;
    AddCores(small, 2, 2, 2);
    AddCores(small, 8, 1, 1);
    CHECK(!Common::PlanHybridCores(small, true).active);
}

namespace {

// Moves the processors from `first` on behind a last level cache of their own.
void OnIsland(std::vector<LogicalProcessor>& processors, size_t first) {
    for (size_t i = first; i < processors.size(); ++i) {
        processors[i].last_level_cache = 1;
    }
}

// A Core Ultra 9 386H (Panther Lake): 4 P cores, 8 E cores behind the same cache and 4 low-power
// E cores on an island, no SMT. `three_classes` false is the same CPU with the low-power cores
// reported in the E cores' class, told apart only by their cache.
std::vector<LogicalProcessor> PantherLakeH(bool three_classes) {
    std::vector<LogicalProcessor> processors;
    AddCores(processors, 4, 1, 2);
    AddCores(processors, 8, 1, 1);
    AddCores(processors, 4, 1, three_classes ? 0 : 1);
    OnIsland(processors, 12);
    return processors;
}

// A Lunar Lake CPU: 4 P cores and 4 E cores, the E cores on an island of their own.
std::vector<LogicalProcessor> LunarLake() {
    std::vector<LogicalProcessor> processors;
    AddCores(processors, 4, 1, 1);
    AddCores(processors, 4, 1, 0);
    OnIsland(processors, 4);
    return processors;
}

void CheckInactive(const Common::HybridPlan& plan) {
    CHECK(!plan.active);
    CHECK(!plan.four_p);
    CHECK(plan.performance.empty());
    CHECK(plan.latency.empty());
    CHECK(plan.render.empty());
    CHECK(plan.front.empty());
    CHECK(plan.back.empty());
    CHECK(plan.efficiency.empty());
    CHECK(plan.low_power.empty());
    CHECK(plan.responsive.empty());
}

} // namespace

TEST_CASE("Hybrid plan: Arrow Lake-H keeps the six-P layout", "[common][core_pinning]") {
    for (const bool pin : {true, false}) {
        const auto plan = Common::PlanHybridCores(ArrowLakeH(), pin);
        REQUIRE(plan.active);
        CHECK(!plan.four_p);
        CHECK(plan.performance == Ids(0, 6));
        CHECK(plan.efficiency == Ids(6, 8));
        CHECK(plan.low_power == Ids(14, 2));
        CHECK(plan.responsive == Ids(0, 14));
        // The Vulkan worker, GPU front and back start where the guest cores do, as before.
        CHECK(plan.render == plan.latency);
        CHECK(Sorted(plan.latency) == (pin ? Concat(Ids(0, 1), Ids(3, 3)) : Ids(0, 6)));
        CHECK(plan.front == (pin ? Ids(1, 1) : Ids(0, 6)));
        CHECK(plan.back == (pin ? Ids(2, 1) : Ids(0, 6)));
    }
}

TEST_CASE("Hybrid plan: Panther Lake-H keeps the GPU threads on the P cores and the guest on P "
          "and E cores",
          "[common][core_pinning]") {
    for (const bool three_classes : {true, false}) {
        for (const bool pin : {true, false}) {
            const auto plan = Common::PlanHybridCores(PantherLakeH(three_classes), pin);
            REQUIRE(plan.active);
            CHECK(plan.four_p);
            CHECK(plan.performance_cores == 4);
            CHECK(plan.performance == Ids(0, 4));
            CHECK(plan.efficiency == Ids(4, 8));
            CHECK(plan.low_power == Ids(12, 4));
            CHECK(plan.responsive == Ids(0, 12));
            CHECK(plan.render == Ids(0, 4));
            CHECK(plan.front == Ids(0, 4));
            CHECK(plan.back == Ids(0, 4));
            CHECK(plan.latency == Ids(0, 12));
        }
    }
}

TEST_CASE("Hybrid plan: Lunar Lake's E-core island is its efficiency cores",
          "[common][core_pinning]") {
    for (const bool island : {true, false}) {
        auto processors = LunarLake();
        if (!island) {
            for (auto& processor : processors) {
                processor.last_level_cache = 0;
            }
        }
        const auto plan = Common::PlanHybridCores(processors, true);
        REQUIRE(plan.active);
        CHECK(plan.four_p);
        CHECK(plan.performance == Ids(0, 4));
        CHECK(plan.efficiency == Ids(4, 4));
        CHECK(plan.low_power.empty());
        CHECK(plan.responsive == Ids(0, 8));
        CHECK(plan.render == Ids(0, 4));
        CHECK(plan.latency == Ids(0, 8));
    }
}

TEST_CASE("Hybrid plan: 4 P cores with SMT and 8 E cores use the four-P layout",
          "[common][core_pinning]") {
    std::vector<LogicalProcessor> processors;
    AddCores(processors, 4, 2, 1);
    AddCores(processors, 8, 1, 0);
    const auto plan = Common::PlanHybridCores(processors, true);
    REQUIRE(plan.active);
    CHECK(plan.four_p);
    CHECK(plan.performance_cores == 4);
    CHECK(plan.render == Ids(0, 8));
    CHECK(plan.front == Ids(0, 8));
    CHECK(plan.back == Ids(0, 8));
    CHECK(plan.efficiency == Ids(8, 8));
    CHECK(plan.low_power.empty());
    CHECK(plan.latency == Ids(0, 16));
}

TEST_CASE("Hybrid plan: the four-P layout starts at 4 P cores and ends below 6",
          "[common][core_pinning]") {
    std::vector<LogicalProcessor> three;
    AddCores(three, 3, 1, 1);
    AddCores(three, 8, 1, 0);
    const auto three_plan = Common::PlanHybridCores(three, true);
    CHECK(three_plan.performance_cores == 3);
    CheckInactive(three_plan);
    std::vector<LogicalProcessor> five;
    AddCores(five, 5, 1, 1);
    AddCores(five, 8, 1, 0);
    const auto five_plan = Common::PlanHybridCores(five, true);
    REQUIRE(five_plan.active);
    CHECK(five_plan.four_p);
    CHECK(five_plan.performance_cores == 5);
    CHECK(five_plan.render == Ids(0, 5));
    CHECK(five_plan.latency == Ids(0, 13));
}

TEST_CASE("Hybrid plan: 2 P cores are left to Windows", "[common][core_pinning]") {
    // An i5-1235U (2 P cores with SMT, 8 E cores) and a Meteor Lake-U (2 P, 8 E, 2 low-power E).
    std::vector<LogicalProcessor> alder;
    AddCores(alder, 2, 2, 1);
    AddCores(alder, 8, 1, 0);
    const auto alder_plan = Common::PlanHybridCores(alder, true);
    CHECK(alder_plan.performance_cores == 2);
    CheckInactive(alder_plan);
    std::vector<LogicalProcessor> meteor;
    AddCores(meteor, 2, 2, 1);
    AddCores(meteor, 8, 1, 0);
    AddCores(meteor, 2, 1, 0);
    OnIsland(meteor, 12);
    const auto meteor_plan = Common::PlanHybridCores(meteor, false);
    CHECK(meteor_plan.performance_cores == 2);
    CheckInactive(meteor_plan);
}

TEST_CASE("Hybrid plan: a CPU that is not hybrid gets no plan", "[common][core_pinning]") {
    for (const u32 cores : {4U, 8U}) {
        const auto plan = Common::PlanHybridCores(Uniform(cores, 2), true);
        CHECK(plan.performance_cores == 0);
        CheckInactive(plan);
    }
}
