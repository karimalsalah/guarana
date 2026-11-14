// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "video_core/vulkan_common/driver_pipeline_cache_gate.h"

namespace {

using namespace std::chrono_literals;

class Signal {
public:
    void Notify() {
        {
            std::scoped_lock lock{mutex};
            ready = true;
        }
        cv.notify_all();
    }

    void Wait() {
        std::unique_lock lock{mutex};
        cv.wait(lock, [this] { return ready; });
    }

    [[nodiscard]] bool WaitFor(std::chrono::milliseconds timeout) {
        std::unique_lock lock{mutex};
        return cv.wait_for(lock, timeout, [this] { return ready; });
    }

private:
    std::mutex mutex;
    std::condition_variable cv;
    bool ready{false};
};

struct FakeDriverCache {
    std::string blob{"initial-driver-cache-blob"};
    size_t query_calls{};
    size_t read_calls{};
    size_t write_calls{};
    bool fail_write{};
    std::string written_blob;

    Vulkan::DriverCacheSaveResult RunSave(
        Vulkan::DriverPipelineCacheSaveTracker& tracker,
        const Vulkan::DriverPipelineCacheGate& gate, bool serialize_reads, bool skip_same_size,
        std::optional<std::chrono::milliseconds> max_lock_wait = std::nullopt,
        int max_lock_tries = 4) {
        return tracker.Save(
            gate, serialize_reads, skip_same_size, max_lock_wait, max_lock_tries,
            [] { return false; }, [] {},
            [this](size_t& out_size) {
                ++query_calls;
                out_size = blob.size();
                return true;
            },
            [this](size_t& in_out_size, char* dst) {
                ++read_calls;
                in_out_size = blob.size();
                std::memcpy(dst, blob.data(), blob.size());
                return true;
            },
            [this](std::unique_ptr<char[]>& data, size_t size) {
                ++write_calls;
                if (fail_write) {
                    return false;
                }
                written_blob.assign(data.get(), size);
                data.reset();
                return true;
            });
    }
};

} // namespace

TEST_CASE("Driver pipeline cache switch and driver policy", "[video_core][pipeline_cache]") {
    CHECK_FALSE(Vulkan::PipelineCacheSafeRequested(nullptr));
    CHECK_FALSE(Vulkan::PipelineCacheSafeRequested(""));
    CHECK_FALSE(Vulkan::PipelineCacheSafeRequested("0"));
    CHECK_FALSE(Vulkan::PipelineCacheSafeRequested("0x"));
    CHECK_FALSE(Vulkan::PipelineCacheSafeRequested("false"));
    CHECK_FALSE(Vulkan::PipelineCacheSafeRequested("off"));
    CHECK(Vulkan::PipelineCacheSafeRequested("1"));
    CHECK(Vulkan::PipelineCacheSafeRequested("2"));
    CHECK(Vulkan::PipelineCacheSafeRequested("1x"));
    CHECK(Vulkan::PipelineCacheSafeRequested("-1"));
    CHECK(Vulkan::PipelineCacheSafeRequested(" -1"));

    // Drivers where CanReadPipelineCacheDuringBuilds() is true never serialize reads.
    CHECK_FALSE(Vulkan::ShouldSerializeDriverPipelineCacheReads(true, false));
    CHECK_FALSE(Vulkan::ShouldSerializeDriverPipelineCacheReads(true, true));

    // Drivers where CanReadPipelineCacheDuringBuilds() is false (Intel Windows) serialize only
    // when EDEN_NX2_CACHE_SAFE is enabled.
    CHECK_FALSE(Vulkan::ShouldSerializeDriverPipelineCacheReads(false, false));
    CHECK(Vulkan::ShouldSerializeDriverPipelineCacheReads(false, true));
}

TEST_CASE("Driver pipeline cache gate blocks saves during builds and builds during saves",
          "[video_core][pipeline_cache]") {
    SECTION("multiple pipeline builds share the gate concurrently") {
        Vulkan::DriverPipelineCacheGate gate;
        Signal first_holding;
        Signal second_holding;
        std::atomic_bool build_a_owned{false};
        std::atomic_bool build_b_owned{false};

        std::thread build_a([&] {
            const auto lock = gate.LockForBuild(true);
            build_a_owned.store(lock.owns_lock(), std::memory_order_release);
            first_holding.Notify();
            second_holding.Wait();
        });
        std::thread build_b([&] {
            first_holding.Wait();
            const auto lock = gate.LockForBuild(true);
            build_b_owned.store(lock.owns_lock(), std::memory_order_release);
            second_holding.Notify();
        });

        build_a.join();
        build_b.join();
        CHECK(build_a_owned.load(std::memory_order_acquire));
        CHECK(build_b_owned.load(std::memory_order_acquire));
        CHECK(gate.Builds() == 2);
    }

    SECTION("no save starts while a build holds the gate") {
        Vulkan::DriverPipelineCacheGate gate;
        Vulkan::DriverPipelineCacheSaveTracker tracker;
        FakeDriverCache fake;
        tracker.ResetForLoad(gate.Builds());

        Signal build_holding;
        Signal release_build;
        Signal save_entered_thread;
        std::atomic_bool save_read_started{false};

        std::thread builder([&] {
            const auto lock = gate.LockForBuild(true);
            static_cast<void>(lock);
            build_holding.Notify();
            release_build.Wait();
        });
        build_holding.Wait();

        Vulkan::DriverCacheSaveResult result{};
        std::thread saver([&] {
            save_entered_thread.Notify();
            result = tracker.Save(
                gate, true, false, std::nullopt, 1, [] { return false; }, [] {},
                [&](size_t& out_size) {
                    save_read_started.store(true, std::memory_order_release);
                    out_size = fake.blob.size();
                    return true;
                },
                [&](size_t& in_out_size, char* dst) {
                    in_out_size = fake.blob.size();
                    std::memcpy(dst, fake.blob.data(), fake.blob.size());
                    return true;
                },
                [&](std::unique_ptr<char[]>& data, size_t size) {
                    fake.written_blob.assign(data.get(), size);
                    return true;
                });
        });

        save_entered_thread.Wait();
        std::this_thread::sleep_for(30ms);
        CHECK_FALSE(save_read_started.load(std::memory_order_acquire));

        release_build.Notify();
        builder.join();
        saver.join();

        CHECK(save_read_started.load(std::memory_order_acquire));
        CHECK(result.outcome == Vulkan::DriverCacheSaveOutcome::Written);
        CHECK(fake.written_blob == fake.blob);
    }

    SECTION("no build starts while a save reads the cache, and builds resume during disk write") {
        Vulkan::DriverPipelineCacheGate gate;
        Vulkan::DriverPipelineCacheSaveTracker tracker;
        FakeDriverCache fake;
        tracker.ResetForLoad(gate.Builds());
        static_cast<void>(gate.LockForBuild(true));

        Signal reading_cache;
        Signal finish_reading;
        Signal writing_file;
        Signal build_attempting;
        Signal build_acquired;
        std::atomic_bool build_started{false};
        std::atomic_bool build_resumed_during_write{false};

        Vulkan::DriverCacheSaveResult result{};
        std::thread saver([&] {
            result = tracker.Save(
                gate, true, false, std::nullopt, 1, [] { return false; }, [] {},
                [&](size_t& out_size) {
                    out_size = fake.blob.size();
                    return true;
                },
                [&](size_t& in_out_size, char* dst) {
                    reading_cache.Notify();
                    finish_reading.Wait();
                    in_out_size = fake.blob.size();
                    std::memcpy(dst, fake.blob.data(), fake.blob.size());
                    return true;
                },
                [&](std::unique_ptr<char[]>& data, size_t size) {
                    writing_file.Notify();
                    // Because LockForRead is released before write_file runs, the queued build
                    // can proceed without waiting for disk I/O to finish.
                    build_resumed_during_write.store(build_acquired.WaitFor(2000ms),
                                                     std::memory_order_release);
                    fake.written_blob.assign(data.get(), size);
                    return true;
                });
        });

        reading_cache.Wait();

        std::thread builder([&] {
            build_attempting.Notify();
            const auto lock = gate.LockForBuild(true);
            static_cast<void>(lock);
            build_started.store(true, std::memory_order_release);
            build_acquired.Notify();
        });

        build_attempting.Wait();
        std::this_thread::sleep_for(30ms);
        CHECK_FALSE(build_started.load(std::memory_order_acquire));

        finish_reading.Notify();
        writing_file.Wait();
        builder.join();
        saver.join();

        CHECK(build_resumed_during_write.load(std::memory_order_acquire));
        CHECK(build_started.load(std::memory_order_acquire));
        CHECK(result.outcome == Vulkan::DriverCacheSaveOutcome::Written);
        CHECK(gate.Builds() == 2);
    }

    SECTION("timed periodic save yields to long builds, retries on backoff, and honours stop") {
        Vulkan::DriverPipelineCacheGate gate;
        Vulkan::DriverPipelineCacheSaveTracker tracker;
        FakeDriverCache fake;
        tracker.ResetForLoad(gate.Builds());

        Signal build_holding;
        Signal release_build;
        std::thread builder([&] {
            const auto lock = gate.LockForBuild(true);
            static_cast<void>(lock);
            build_holding.Notify();
            release_build.Wait();
        });
        build_holding.Wait();

        // While the build stays active across all tries, the timed save drops cleanly.
        const auto busy = tracker.Save(
            gate, true, true, 5ms, 2, [] { return false; }, [] {},
            [&](size_t& out_size) {
                out_size = fake.blob.size();
                return true;
            },
            [&](size_t&, char*) { return true; },
            [&](std::unique_ptr<char[]>&, size_t) { return true; });
        CHECK(busy.outcome == Vulkan::DriverCacheSaveOutcome::BusyBuilding);
        CHECK(busy.lock_tries == 2);

        // When stop is requested during backoff, no further tries are attempted.
        int stop_tries = 0;
        {
            const auto stopped_lock = gate.LockForReadWithRetries(
                true, 5ms, 4, [] { return true; }, [] {}, stop_tries);
            CHECK_FALSE(stopped_lock.owns_lock());
            CHECK(stop_tries == 1);
        }

        // Releasing the build during backoff lets the retry acquire the gate.
        int retry_tries = 0;
        {
            const auto acquired_lock = gate.LockForReadWithRetries(
                true, 5ms, 4, [] { return false; },
                [&] {
                    release_build.Notify();
                    builder.join();
                },
                retry_tries);
            CHECK(acquired_lock.owns_lock());
            CHECK(retry_tries == 2);
        }
        if (builder.joinable()) {
            release_build.Notify();
            builder.join();
        }
    }
}

TEST_CASE("Driver pipeline cache skip rule avoids unchanged writes and writes on new builds",
          "[video_core][pipeline_cache]") {
    Vulkan::DriverPipelineCacheGate gate;
    Vulkan::DriverPipelineCacheSaveTracker tracker;
    FakeDriverCache fake;

    // Simulate loading an existing driver cache blob on boot.
    tracker.ResetForLoad(gate.Builds());
    tracker.SetSavedSize(fake.blob.size());

    SECTION("unchanged build count skips without reading or writing; one new pipeline writes") {
        const auto unchanged = fake.RunSave(tracker, gate, true, false);
        CHECK(unchanged.outcome == Vulkan::DriverCacheSaveOutcome::SkippedUnchangedBuilds);
        CHECK(unchanged.cache_size == fake.blob.size());
        CHECK(fake.query_calls == 0);
        CHECK(fake.read_calls == 0);
        CHECK(fake.write_calls == 0);

        // Build one new pipeline into the driver cache.
        {
            const auto build_lock = gate.LockForBuild(true);
            static_cast<void>(build_lock);
            fake.blob += "+pipeline1";
        }

        const auto written = fake.RunSave(tracker, gate, true, false);
        CHECK(written.outcome == Vulkan::DriverCacheSaveOutcome::Written);
        CHECK(written.builds_since_saved == 1);
        CHECK(written.cache_size == fake.blob.size());
        CHECK(fake.query_calls == 1);
        CHECK(fake.read_calls == 1);
        CHECK(fake.write_calls == 1);
        CHECK(fake.written_blob == fake.blob);
        CHECK(tracker.SavedBuilds() == gate.Builds());
        CHECK(tracker.SavedSize() == fake.blob.size());

        // Saving again with zero builds since that write skips again.
        const auto second_unchanged = fake.RunSave(tracker, gate, true, false);
        CHECK(second_unchanged.outcome == Vulkan::DriverCacheSaveOutcome::SkippedUnchangedBuilds);
        CHECK(fake.write_calls == 1);
    }

    SECTION("warm boot cache hits with unchanged size skip boot write, then write on growth or exit") {
        // Three warm-boot vkCreate*Pipelines calls hit the loaded driver cache without growing it.
        for (int i = 0; i < 3; ++i) {
            static_cast<void>(gate.LockForBuild(true));
        }

        const auto warm_boot = fake.RunSave(tracker, gate, true, true);
        CHECK(warm_boot.outcome == Vulkan::DriverCacheSaveOutcome::SkippedSameSize);
        CHECK(warm_boot.builds_since_saved == 3);
        CHECK(fake.query_calls == 1);
        CHECK(fake.read_calls == 0);
        CHECK(fake.write_calls == 0);

        // One genuinely new pipeline grows the driver cache during play: periodic flush writes it.
        {
            const auto build_lock = gate.LockForBuild(true);
            static_cast<void>(build_lock);
            fake.blob += "+new-play-pipeline";
        }
        const auto periodic = fake.RunSave(tracker, gate, true, true);
        CHECK(periodic.outcome == Vulkan::DriverCacheSaveOutcome::Written);
        CHECK(periodic.builds_since_saved == 4);
        CHECK(fake.write_calls == 1);
        CHECK(fake.written_blob == fake.blob);
    }

    SECTION("exit save without skip_same_size still writes after a size-skipped warm boot") {
        static_cast<void>(gate.LockForBuild(true));
        const auto warm_boot = fake.RunSave(tracker, gate, true, true);
        CHECK(warm_boot.outcome == Vulkan::DriverCacheSaveOutcome::SkippedSameSize);
        CHECK(fake.write_calls == 0);

        const auto exit_save = fake.RunSave(tracker, gate, true, false);
        CHECK(exit_save.outcome == Vulkan::DriverCacheSaveOutcome::Written);
        CHECK(fake.write_calls == 1);
    }

    SECTION("failed disk write preserves saved state so the next save retries") {
        {
            const auto build_lock = gate.LockForBuild(true);
            static_cast<void>(build_lock);
            fake.blob += "+retry-me";
        }
        fake.fail_write = true;
        const auto failed = fake.RunSave(tracker, gate, true, false);
        CHECK(failed.outcome == Vulkan::DriverCacheSaveOutcome::WriteFailed);
        CHECK(tracker.SavedBuilds() == 0);

        fake.fail_write = false;
        const auto retried = fake.RunSave(tracker, gate, true, false);
        CHECK(retried.outcome == Vulkan::DriverCacheSaveOutcome::Written);
        CHECK(tracker.SavedBuilds() == 1);
        CHECK(fake.written_blob == fake.blob);
    }
}

TEST_CASE("Unserialized driver pipeline cache path preserves existing behaviour",
          "[video_core][pipeline_cache]") {
    Vulkan::DriverPipelineCacheGate gate;
    Vulkan::DriverPipelineCacheSaveTracker tracker;
    FakeDriverCache fake;

    tracker.ResetForLoad(gate.Builds());
    tracker.SetSavedSize(fake.blob.size());

    // Where CanReadPipelineCacheDuringBuilds() is true (serialize_reads = false), neither lock
    // acquires the mutex and saves write unconditionally even when builds and size are unchanged.
    const auto build_lock = gate.LockForBuild(false);
    CHECK_FALSE(build_lock.owns_lock());
    const auto read_lock = gate.LockForRead(false);
    CHECK_FALSE(read_lock.owns_lock());

    tracker.ResetForLoad(gate.Builds());
    tracker.SetSavedSize(fake.blob.size());

    const auto saved = fake.RunSave(tracker, gate, false, true);
    CHECK(saved.outcome == Vulkan::DriverCacheSaveOutcome::Written);
    CHECK(fake.query_calls == 1);
    CHECK(fake.read_calls == 1);
    CHECK(fake.write_calls == 1);
}
