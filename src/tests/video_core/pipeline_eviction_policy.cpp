// SPDX-License-Identifier: GPL-3.0-or-later

#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/pipeline_eviction_policy.h"

using Vulkan::DefaultKeptPipelines;
using Vulkan::KeptPipelinesForMemory;
using Vulkan::MaxUnusedPrebuiltPipelines;
using Vulkan::PipelinesToEvict;
using Vulkan::PrebuildAfterLoadSave;
using Vulkan::PrebuildLimit;
using Vulkan::PrebuildStartsWithBootBuilds;
using Vulkan::SaveDriverCacheAtLoadEnd;

TEST_CASE("Nothing is evicted at or under the kept limit", "[video_core][pipeline_eviction]") {
    std::vector<u64> last_uses(1900);
    for (size_t i = 0; i < last_uses.size(); ++i) {
        last_uses[i] = i + 1;
    }
    REQUIRE(PipelinesToEvict(last_uses, 1900, DefaultKeptPipelines).empty());
    REQUIRE(PipelinesToEvict(last_uses, DefaultKeptPipelines, DefaultKeptPipelines).empty());
    REQUIRE(PipelinesToEvict({}, 0, DefaultKeptPipelines).empty());
}

TEST_CASE("Past the limit only the oldest go, back down to the limit",
          "[video_core][pipeline_eviction]") {
    // 5,000 built, all evictable, listed newest first: the 904 oldest go.
    std::vector<u64> last_uses(5000);
    for (size_t i = 0; i < last_uses.size(); ++i) {
        last_uses[i] = 10'000 - i;
    }
    const auto evicted = PipelinesToEvict(last_uses, 5000, DefaultKeptPipelines);
    REQUIRE(evicted.size() == 5000 - DefaultKeptPipelines);
    for (size_t i = 0; i < evicted.size(); ++i) {
        REQUIRE(evicted[i] == last_uses.size() - 1 - i);
    }
}

TEST_CASE("Never-used prebuilt pipelines (frame 0) go first, ties in order",
          "[video_core][pipeline_eviction]") {
    const std::vector<u64> last_uses{500, 0, 200, 0, 900};
    REQUIRE(PipelinesToEvict(last_uses, 7, 4) == std::vector<size_t>{1, 3, 2});
}

TEST_CASE("Recently used pipelines count toward the limit but are never picked",
          "[video_core][pipeline_eviction]") {
    // 4,100 built but only 3 are old enough to evict: all 3 go, the limit is left exceeded.
    const std::vector<u64> last_uses{1, 2, 3};
    REQUIRE(PipelinesToEvict(last_uses, 4100, DefaultKeptPipelines) ==
            std::vector<size_t>{0, 1, 2});
}

TEST_CASE("The kept limit is a quarter of the RAM", "[video_core][pipeline_eviction]") {
    constexpr u64 GiB = u64{1} << 30;
    REQUIRE(KeptPipelinesForMemory(0) == DefaultKeptPipelines);
    REQUIRE(KeptPipelinesForMemory(4 * GiB) == 3495);
    REQUIRE(KeptPipelinesForMemory(8 * GiB) == 6990);
    REQUIRE(KeptPipelinesForMemory(16 * GiB) == 13981);
    REQUIRE(KeptPipelinesForMemory(32 * GiB) == 27962);
}

TEST_CASE("The prebuild holds what the kept limit leaves past the boot builds",
          "[video_core][pipeline_eviction]") {
    REQUIRE(PrebuildLimit(KeptPipelinesForMemory(u64{32} << 30), 5000) == 27962 - 5000);
    REQUIRE(PrebuildLimit(DefaultKeptPipelines, DefaultKeptPipelines) == 0);
    REQUIRE(PrebuildLimit(DefaultKeptPipelines, 9000) == 0);
    REQUIRE(PrebuildLimit(DefaultKeptPipelines, 0) == DefaultKeptPipelines);
    // Without a kept limit (Android), as many as eviction keeps unused
    REQUIRE(PrebuildLimit(0, 1500) == MaxUnusedPrebuiltPipelines);
}

TEST_CASE("The load's driver cache save and when the prebuild starts",
          "[video_core][pipeline_eviction]") {
    // Arguments: async boot, can read during builds, serializes reads, EDEN_NX2_LOAD_SAVE
    // A driver that reads its cache during builds: saved, the prebuild runs beside the boot builds
    REQUIRE(SaveDriverCacheAtLoadEnd(false, true, false, false));
    REQUIRE_FALSE(PrebuildAfterLoadSave(false, false, true, false, false));
    REQUIRE_FALSE(PrebuildAfterLoadSave(false, false, true, false, true));
    // With a precache the prebuild waits for it, on every driver
    REQUIRE(PrebuildAfterLoadSave(false, true, true, false, false));
    REQUIRE(PrebuildAfterLoadSave(false, true, false, false, false));
    // Intel's Windows driver as before: no save, the prebuild beside the boot builds
    REQUIRE_FALSE(SaveDriverCacheAtLoadEnd(false, false, false, false));
    REQUIRE_FALSE(PrebuildAfterLoadSave(false, false, false, false, false));
    // ... with EDEN_NX2_LOAD_SAVE: the save, and the prebuild after it
    REQUIRE(SaveDriverCacheAtLoadEnd(false, false, false, true));
    REQUIRE(PrebuildAfterLoadSave(false, false, false, false, true));
    // ... with EDEN_NX2_CACHE_SAFE: reads wait for builds, as before
    REQUIRE(SaveDriverCacheAtLoadEnd(false, false, true, false));
    REQUIRE(PrebuildAfterLoadSave(false, false, false, true, false));
    // Async boot builds still run when the load ends: no save there, the prebuild at once
    REQUIRE_FALSE(SaveDriverCacheAtLoadEnd(true, true, false, false));
    REQUIRE_FALSE(SaveDriverCacheAtLoadEnd(true, false, false, true));
    REQUIRE_FALSE(PrebuildAfterLoadSave(true, true, false, true, true));
}

TEST_CASE("The prebuild waits for the load's driver cache saves where reads race builds",
          "[video_core][pipeline_eviction]") {
    REQUIRE(PrebuildStartsWithBootBuilds(false, false));
    // After the precache, as its jobs would queue ahead of the precache's
    REQUIRE_FALSE(PrebuildStartsWithBootBuilds(true, false));
    // Intel's Windows driver: a prebuild build running into the cache while the end-of-load save
    // reads it crashes the driver, so the save would have to be skipped
    REQUIRE_FALSE(PrebuildStartsWithBootBuilds(false, true));
    REQUIRE_FALSE(PrebuildStartsWithBootBuilds(true, true));
}
