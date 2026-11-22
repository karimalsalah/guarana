// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <string>
#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/unoptimized_first.h"
#include "video_core/renderer_vulkan/vk_pipeline_library.h"

namespace {
// Stands in for vk::Pipeline: movable, explicit bool, and its lifetime can be watched.
using Handle = std::shared_ptr<int>;
} // Anonymous namespace

TEST_CASE("Unoptimized first applies only to Intel's Windows driver with libraries",
          "[video_core][vulkan][unoptimized_first]") {
    static_assert(Vulkan::UnoptimizedFirstApplies(true, VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS,
                                                  true));
    CHECK_FALSE(
        Vulkan::UnoptimizedFirstApplies(false, VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS, true));
    // The monolithic route is not changed: nothing without graphics pipeline libraries.
    CHECK_FALSE(
        Vulkan::UnoptimizedFirstApplies(true, VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS, false));
    for (const auto driver : {VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA, VK_DRIVER_ID_AMD_PROPRIETARY,
                              VK_DRIVER_ID_AMD_OPEN_SOURCE, VK_DRIVER_ID_MESA_RADV,
                              VK_DRIVER_ID_NVIDIA_PROPRIETARY, VK_DRIVER_ID_QUALCOMM_PROPRIETARY,
                              VK_DRIVER_ID_MESA_TURNIP}) {
        CHECK_FALSE(Vulkan::UnoptimizedFirstApplies(true, driver, true));
    }
}

TEST_CASE("A library goes unoptimized first only when new and replaceable",
          "[video_core][vulkan][unoptimized_first]") {
    static_assert(Vulkan::BuildsLibraryUnoptimizedFirst(true, false));
    // Another pipeline built the optimized library already: it is used as it is.
    CHECK_FALSE(Vulkan::BuildsLibraryUnoptimizedFirst(true, true));
    // No job scheduler to build the optimized one on: no swap could follow.
    CHECK_FALSE(Vulkan::BuildsLibraryUnoptimizedFirst(false, false));
    CHECK_FALSE(Vulkan::BuildsLibraryUnoptimizedFirst(false, true));
}

TEST_CASE("The optimized pipeline is swapped in once and the first one is kept",
          "[video_core][vulkan][unoptimized_first]") {
    Handle current = std::make_shared<int>(1);
    Handle replacement = std::make_shared<int>(2);
    Handle retired;
    const std::weak_ptr<int> first = current;

    REQUIRE(Vulkan::SwapInReplacement(current, replacement, retired));
    CHECK(*current == 2);
    CHECK_FALSE(replacement);
    REQUIRE(retired);
    CHECK(*retired == 1);
    CHECK_FALSE(first.expired());

    // A second replacement is refused: one swap per pipeline, and the retired one stays.
    replacement = std::make_shared<int>(3);
    CHECK_FALSE(Vulkan::SwapInReplacement(current, replacement, retired));
    CHECK(*current == 2);
    REQUIRE(replacement);
    CHECK(*replacement == 3);
    CHECK(*retired == 1);
    CHECK_FALSE(first.expired());
}

TEST_CASE("Without a replacement the swap moves nothing",
          "[video_core][vulkan][unoptimized_first]") {
    Handle current = std::make_shared<int>(1);
    Handle replacement;
    Handle retired;
    CHECK_FALSE(Vulkan::SwapInReplacement(current, replacement, retired));
    REQUIRE(current);
    CHECK(*current == 1);
    CHECK_FALSE(retired);
}

TEST_CASE("An unoptimized library has its own create flags, so its own key",
          "[video_core][vulkan][unoptimized_first]") {
    // LibraryFlags' two outcomes: with and without link time optimization info.
    for (const VkPipelineCreateFlags base :
         {static_cast<VkPipelineCreateFlags>(VK_PIPELINE_CREATE_LIBRARY_BIT_KHR),
          static_cast<VkPipelineCreateFlags>(
              VK_PIPELINE_CREATE_LIBRARY_BIT_KHR |
              VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT)}) {
        // Switch off: the flags, and so the keys, are the ones built before it.
        CHECK(Vulkan::ShaderLibraryCreateFlags(base, false) == base);
        const VkPipelineCreateFlags unoptimized = Vulkan::ShaderLibraryCreateFlags(base, true);
        CHECK(unoptimized != base);
        CHECK((unoptimized ^ base) ==
              static_cast<VkPipelineCreateFlags>(VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT));
    }
}

TEST_CASE("A library counts as built only once its build gave one",
          "[video_core][vulkan][unoptimized_first]") {
    Vulkan::GraphicsPipelineLibraryCache cache;
    const std::string key{"optimized fragment library"};
    CHECK_FALSE(cache.HasBuilt(key));

    // A build that gives no pipeline (needs no device): the failed key is not retried, and a
    // later pipeline must not take it for an optimized library to use as it is.
    size_t builds = 0;
    const auto fail = [&builds] {
        ++builds;
        return Vulkan::GplLibrary{};
    };
    CHECK(cache.GetOrCreate(Vulkan::GplPart::Fragment, key, fail, 0) == nullptr);
    CHECK_FALSE(cache.HasBuilt(key));
    CHECK(cache.GetOrCreate(Vulkan::GplPart::Fragment, key, fail, 0, false, true) == nullptr);
    CHECK(builds == size_t{1});
    CHECK_FALSE(cache.HasBuilt(key));
    CHECK_FALSE(cache.HasBuilt("another library"));
}

TEST_CASE("Swap delays give nearest-rank p50 and p99 and start over",
          "[video_core][vulkan][unoptimized_first]") {
    Vulkan::SwapDelays delays;
    CHECK(delays.Take().count == size_t{0});

    for (int ms = 100; ms >= 1; --ms) {
        delays.Add(static_cast<double>(ms));
    }
    const Vulkan::SwapDelays::Summary hundred = delays.Take();
    CHECK(hundred.count == size_t{100});
    CHECK(hundred.p50_ms == 50.0);
    CHECK(hundred.p99_ms == 99.0);
    CHECK(hundred.max_ms == 100.0);
    CHECK(delays.Take().count == size_t{0});

    delays.Add(7.0);
    const Vulkan::SwapDelays::Summary one = delays.Take();
    CHECK(one.count == size_t{1});
    CHECK(one.p50_ms == 7.0);
    CHECK(one.p99_ms == 7.0);
    CHECK(one.max_ms == 7.0);

    for (const double ms : {30.0, 10.0, 20.0}) {
        delays.Add(ms);
    }
    const Vulkan::SwapDelays::Summary three = delays.Take();
    CHECK(three.count == size_t{3});
    CHECK(three.p50_ms == 20.0);
    CHECK(three.p99_ms == 30.0);
    CHECK(three.max_ms == 30.0);
}
