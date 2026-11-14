// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "video_core/renderer_vulkan/present/fsr3_frame_gen.h"

using Vulkan::Fsr3MedianOnce;

TEST_CASE("FSR3 median once: the switch is read as a number, like its neighbours",
          "[video_core]") {
    // Unset, empty, `0`, `00`, `false` and anything that is not a number are off.
    REQUIRE_FALSE(Fsr3MedianOnce::Enabled(nullptr));
    REQUIRE_FALSE(Fsr3MedianOnce::Enabled(""));
    REQUIRE_FALSE(Fsr3MedianOnce::Enabled("0"));
    REQUIRE_FALSE(Fsr3MedianOnce::Enabled("00"));
    REQUIRE_FALSE(Fsr3MedianOnce::Enabled("false"));
    REQUIRE_FALSE(Fsr3MedianOnce::Enabled("off"));
    // Any number but 0 is on.
    REQUIRE(Fsr3MedianOnce::Enabled("1"));
    REQUIRE(Fsr3MedianOnce::Enabled("2"));
    REQUIRE(Fsr3MedianOnce::Enabled("10"));
}

TEST_CASE("FSR3 median once: workgroups cover the flow image, no more than one too many",
          "[video_core]") {
    STATIC_REQUIRE(Fsr3MedianOnce::kGroupSize == 16U);
    STATIC_REQUIRE(Fsr3MedianOnce::GroupCount(0U) == 0U);
    STATIC_REQUIRE(Fsr3MedianOnce::GroupCount(1U) == 1U);
    STATIC_REQUIRE(Fsr3MedianOnce::GroupCount(16U) == 1U);
    STATIC_REQUIRE(Fsr3MedianOnce::GroupCount(17U) == 2U);
    // The flow image of a 2880x1800 output, and of a 1920x1080 one.
    STATIC_REQUIRE(Fsr3MedianOnce::GroupCount(1440U) == 90U);
    STATIC_REQUIRE(Fsr3MedianOnce::GroupCount(900U) == 57U);
    STATIC_REQUIRE(Fsr3MedianOnce::GroupCount(960U) == 60U);
    STATIC_REQUIRE(Fsr3MedianOnce::GroupCount(540U) == 34U);

    // The same arithmetic as the optical flow's own `(extent + 15) / 16`.
    for (u32 extent = 1; extent <= 4096U; ++extent) {
        REQUIRE(Fsr3MedianOnce::GroupCount(extent) == (extent + 15U) / 16U);
        REQUIRE(Fsr3MedianOnce::GroupCount(extent) * Fsr3MedianOnce::kGroupSize >= extent);
        REQUIRE((Fsr3MedianOnce::GroupCount(extent) - 1U) * Fsr3MedianOnce::kGroupSize < extent);
    }
}
