// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <catch2/catch_test_macros.hpp>
#include "video_core/renderer_vulkan/present_error_policy.h"

TEST_CASE("Acquire results never imply a ready image on failure", "[present_policy]") {
    using namespace Vulkan::PresentPolicy;
    CHECK(ClassifyAcquire(VK_SUCCESS) == Acquire::Ready);
    CHECK(ClassifyAcquire(VK_SUBOPTIMAL_KHR) == Acquire::Ready);
    CHECK(ClassifyAcquire(VK_ERROR_OUT_OF_DATE_KHR) == Acquire::Recreate);
    CHECK(ClassifyAcquire(VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT) == Acquire::Recreate);
    CHECK(ClassifyAcquire(VK_TIMEOUT) == Acquire::Drop);
    CHECK(ClassifyAcquire(VK_NOT_READY) == Acquire::Drop);
    CHECK(ClassifyAcquire(VK_ERROR_DEVICE_LOST) == Acquire::DeviceLost);
    for (const auto result : std::array{VK_ERROR_SURFACE_LOST_KHR, VK_ERROR_OUT_OF_HOST_MEMORY,
                                      VK_ERROR_OUT_OF_DEVICE_MEMORY, VK_ERROR_INITIALIZATION_FAILED,
                                      VK_INCOMPLETE}) {
        CHECK(ClassifyAcquire(result) == Acquire::Error);
    }
}

TEST_CASE("Both queried maximum and chosen swapchain extent must be nonzero", "[present_policy]") {
    using Vulkan::PresentPolicy::HasExtent;
    CHECK(HasExtent({1920, 1080}, {1280, 720}));
    CHECK_FALSE(HasExtent({0, 1080}, {1280, 720}));
    CHECK_FALSE(HasExtent({1920, 0}, {1280, 720}));
    CHECK_FALSE(HasExtent({1920, 1080}, {0, 720}));
    CHECK_FALSE(HasExtent({1920, 1080}, {1280, 0}));
    CHECK_FALSE(HasExtent({0, 0}, {0, 0}));
}
