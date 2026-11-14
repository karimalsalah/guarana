// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/gpl_parallel_parts.h"
#include "video_core/shader_jobs.h"

namespace {
using namespace std::chrono_literals;

constexpr auto WAIT_LIMIT = 10s;

/// Waits until `flag` is set. False when it was not within WAIT_LIMIT.
bool WaitFor(const std::atomic_bool& flag) {
    const auto deadline = std::chrono::steady_clock::now() + WAIT_LIMIT;
    while (!flag.load()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

/// Occupies every driver worker of a scheduler until released or destroyed. Declared after the
/// scheduler, so it releases the workers, and waits until they left their jobs (which read its
/// members), before the scheduler joins them.
class WorkerBlocker {
public:
    WorkerBlocker(VideoCommon::ShaderJobScheduler& jobs, size_t workers) {
        for (size_t i = 0; i < workers; ++i) {
            handles.push_back(jobs.Submit(VideoCommon::ShaderJob{
                .priority = VideoCommon::ShaderJobPriority::NeedNow,
                .driver =
                    [this] {
                        running.fetch_add(1);
                        WaitFor(release);
                    },
            }));
        }
    }

    ~WorkerBlocker() {
        release.store(true);
        for (const VideoCommon::ShaderJobHandle& handle : handles) {
            handle.Wait();
        }
    }

    /// Whether every worker is inside its blocking job.
    bool AllRunning(size_t workers) const {
        const auto deadline = std::chrono::steady_clock::now() + WAIT_LIMIT;
        while (running.load() < workers) {
            if (std::chrono::steady_clock::now() > deadline) {
                return false;
            }
            std::this_thread::sleep_for(1ms);
        }
        return true;
    }

    void Release() {
        release.store(true);
    }

private:
    std::atomic_bool release{};
    std::atomic<size_t> running{};
    std::vector<VideoCommon::ShaderJobHandle> handles;
};
} // Anonymous namespace

TEST_CASE("Parallel library parts: the switch parses like its neighbours",
          "[video_core][vulkan][gpl_parallel_parts]") {
    REQUIRE_FALSE(Vulkan::GplParallelPartsRequested(nullptr));
    REQUIRE_FALSE(Vulkan::GplParallelPartsRequested(""));
    REQUIRE_FALSE(Vulkan::GplParallelPartsRequested("0"));
    REQUIRE_FALSE(Vulkan::GplParallelPartsRequested("false"));
    REQUIRE_FALSE(Vulkan::GplParallelPartsRequested("off"));
    REQUIRE(Vulkan::GplParallelPartsRequested("1"));
    REQUIRE(Vulkan::GplParallelPartsRequested("2"));
}

TEST_CASE("Parallel library parts: a free worker runs the second part beside the first",
          "[video_core][vulkan][gpl_parallel_parts]") {
    VideoCommon::ShaderJobScheduler jobs{{.cpu_threads = 1, .driver_threads = 2}};
    std::atomic_bool second_started{};
    std::atomic_bool first_saw_second{};
    std::thread::id second_thread;
    // The first part waits for the second to have started: it only finishes in time when the two
    // run at the same time.
    const bool ran_here = Vulkan::RunPartsInParallel(
        jobs, VideoCommon::ShaderJobPriority::Soon,
        [&] { first_saw_second = WaitFor(second_started); },
        [&] {
            second_thread = std::this_thread::get_id();
            second_started = true;
        });
    REQUIRE(first_saw_second.load());
    REQUIRE_FALSE(ran_here);
    REQUIRE(second_thread != std::this_thread::get_id());
}

TEST_CASE("Parallel library parts: a saturated pool cannot deadlock",
          "[video_core][vulkan][gpl_parallel_parts]") {
    constexpr size_t WORKERS = 2;
    VideoCommon::ShaderJobScheduler jobs{{.cpu_threads = 1, .driver_threads = WORKERS}};
    WorkerBlocker blocker{jobs, WORKERS};
    REQUIRE(blocker.AllRunning(WORKERS));
    bool first_ran = false;
    bool second_ran = false;
    std::thread::id second_thread;
    // No worker is free: the calling thread runs the second part itself, after the first.
    const bool ran_here = Vulkan::RunPartsInParallel(
        jobs, VideoCommon::ShaderJobPriority::Background, [&] { first_ran = true; },
        [&] {
            second_ran = true;
            second_thread = std::this_thread::get_id();
        });
    REQUIRE(first_ran);
    REQUIRE(second_ran);
    REQUIRE(ran_here);
    REQUIRE(second_thread == std::this_thread::get_id());
    blocker.Release();
}

TEST_CASE("Parallel library parts: a part the scheduler cancelled runs on the calling thread",
          "[video_core][vulkan][gpl_parallel_parts]") {
    VideoCommon::ShaderJobScheduler jobs{{.cpu_threads = 1, .driver_threads = 1}};
    jobs.Shutdown();
    int calls = 0;
    const bool ran_here = Vulkan::RunPartsInParallel(
        jobs, VideoCommon::ShaderJobPriority::Soon, [&] { ++calls; }, [&] { ++calls; });
    REQUIRE(calls == 2);
    REQUIRE(ran_here);
}

TEST_CASE("Parallel library parts: both parts end before an exception of either is thrown",
          "[video_core][vulkan][gpl_parallel_parts]") {
    VideoCommon::ShaderJobScheduler jobs{{.cpu_threads = 1, .driver_threads = 2}};
    SECTION("the second part throws") {
        bool first_ran = false;
        REQUIRE_THROWS_AS(
            Vulkan::RunPartsInParallel(
                jobs, VideoCommon::ShaderJobPriority::Soon, [&] { first_ran = true; },
                [] { throw std::runtime_error{"second"}; }),
            std::runtime_error);
        REQUIRE(first_ran);
    }
    SECTION("the first part throws and the second still runs") {
        std::atomic_bool second_ran{};
        REQUIRE_THROWS_AS(
            Vulkan::RunPartsInParallel(
                jobs, VideoCommon::ShaderJobPriority::Soon,
                [] { throw std::logic_error{"first"}; }, [&] { second_ran = true; }),
            std::logic_error);
        REQUIRE(second_ran.load());
    }
}
