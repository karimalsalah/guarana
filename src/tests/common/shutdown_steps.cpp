// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/shutdown_progress.h"

namespace {

using namespace std::chrono_literals;

std::vector<std::string_view> StepNames() {
    std::vector<std::string_view> names;
    for (const Common::ShutdownStepRecord& record : Common::RecordedShutdownSteps()) {
        names.emplace_back(record.step);
    }
    return names;
}

std::string_view LastStep() {
    const char* const step = Common::LastShutdownStep();
    return step != nullptr ? std::string_view{step} : std::string_view{};
}

/// Members destroyed in reverse order: `second` (its marker ends "second members"), then `first`
struct Members {
    struct Slow {
        ~Slow() {
            std::this_thread::sleep_for(2ms);
        }
    };

    Common::ShutdownStepMarker first_destroyed{"first members"};
    Slow first;
    Common::ShutdownStepMarker second_destroyed{"second members"};
    Slow second;
};

} // Anonymous namespace

TEST_CASE("Shutdown steps are timed in order and each notes progress", "[common][shutdown]") {
    Common::BeginShutdownSteps();
    REQUIRE(LastStep() == "begun");
    const u64 begun = Common::ShutdownProgress();
    std::this_thread::sleep_for(2ms);
    Common::ShutdownStepDone("services");
    REQUIRE(LastStep() == "services");
    Common::ShutdownStepDone("pipelines destroyed", 3);
    Common::EndShutdownSteps();
    REQUIRE(LastStep() == "done");

    const auto records = Common::RecordedShutdownSteps();
    REQUIRE(records.size() == 2);
    REQUIRE(std::string_view{records[0].step} == "services");
    REQUIRE(records[0].ms >= 1.0);
    REQUIRE_FALSE(records[0].count.has_value());
    REQUIRE(records[0].progress == begun + 1);
    REQUIRE(std::string_view{records[1].step} == "pipelines destroyed");
    REQUIRE(records[1].count == std::optional<size_t>{3});
    REQUIRE(records[1].progress == begun + 2);
    REQUIRE(records[1].ms >= 0.0);
}

TEST_CASE("A step outside a teardown only notes progress", "[common][shutdown]") {
    Common::BeginShutdownSteps();
    Common::ShutdownStepDone("kernel");
    Common::EndShutdownSteps();
    const u64 progress = Common::ShutdownProgress();

    Common::ShutdownStepDone("a renderer destroyed at a failed boot");

    REQUIRE(Common::ShutdownProgress() == progress + 1);
    REQUIRE(StepNames() == std::vector<std::string_view>{"kernel"});
    REQUIRE(LastStep() == "done");
}

TEST_CASE("Markers end the steps of the members declared after them", "[common][shutdown]") {
    Common::BeginShutdownSteps();
    {
        Members members;
    }
    Common::EndShutdownSteps();

    REQUIRE(StepNames() == std::vector<std::string_view>{"second members", "first members"});
    for (const Common::ShutdownStepRecord& record : Common::RecordedShutdownSteps()) {
        REQUIRE(record.ms >= 1.0);
    }
}

TEST_CASE("A stop request clears the last step until the teardown begins", "[common][shutdown]") {
    Common::BeginShutdownSteps();
    Common::ShutdownStepDone("audio");
    Common::EndShutdownSteps();
    REQUIRE(LastStep() == "done");

    Common::ClearLastShutdownStep();
    REQUIRE(Common::LastShutdownStep() == nullptr);
    Common::BeginShutdownSteps();
    REQUIRE(LastStep() == "begun");
    Common::ShutdownStepDone("cheats");
    REQUIRE(LastStep() == "cheats");
    Common::EndShutdownSteps();
}

TEST_CASE("A teardown moves while its progress changes or its thread is busy",
          "[common][shutdown]") {
    constexpr u64 Second = 1'000'000'000;
    Common::ShutdownWatch watch{Second};
    watch.Reset(10, 0);

    // Half a second of CPU in a check: a thread that only woke up now and then.
    REQUIRE_FALSE(watch.Moved(10, Second / 2));
    // A second or more since the previous check: busy, though no step was done.
    REQUIRE(watch.Moved(10, Second / 2 + Second));
    REQUIRE_FALSE(watch.Moved(10, Second / 2 + Second + 1));
    // A step done counts without CPU time.
    REQUIRE(watch.Moved(11, Second / 2 + Second + 1));
    REQUIRE_FALSE(watch.Moved(11, Second / 2 + Second + 1));
    // Where the CPU time cannot be read, only progress counts, also on the next check.
    REQUIRE_FALSE(watch.Moved(11, std::nullopt));
    REQUIRE_FALSE(watch.Moved(11, 10 * Second));
    REQUIRE(watch.Moved(11, 12 * Second));
    REQUIRE(watch.Moved(12, std::nullopt));

    Common::ShutdownWatch progress_only;
    progress_only.Reset(5, std::nullopt);
    REQUIRE_FALSE(progress_only.Moved(5, std::nullopt));
    REQUIRE(progress_only.Moved(6, std::nullopt));
}

TEST_CASE("A teardown busy for too many checks without a step done is taken for a hung one",
          "[common][shutdown]") {
    // A thread that spins on a lock is as busy as one that frees memory.
    constexpr u64 Second = 1'000'000'000;
    Common::ShutdownWatch watch{Second, 2};
    watch.Reset(1, 0);
    REQUIRE(watch.Moved(1, 10 * Second));
    REQUIRE(watch.Moved(1, 20 * Second));
    REQUIRE_FALSE(watch.Moved(1, 30 * Second));
    // A step done starts the count again.
    REQUIRE(watch.Moved(2, 40 * Second));
    REQUIRE(watch.Moved(2, 50 * Second));
    REQUIRE(watch.Moved(2, 60 * Second));
    REQUIRE_FALSE(watch.Moved(2, 70 * Second));
    // So does a new wait.
    watch.Reset(2, 70 * Second);
    REQUIRE(watch.Moved(2, 80 * Second));
}
