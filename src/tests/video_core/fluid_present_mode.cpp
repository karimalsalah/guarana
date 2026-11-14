// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "common/settings_enums.h"
#include "video_core/renderer_vulkan/fluid_present_mode.h"

using Settings::VSyncMode;
using Vulkan::ChooseFluidPresentMode;

TEST_CASE("FLUID presents with FIFO unless EDEN_NX2_FLUID_MAILBOX finds MAILBOX",
          "[video_core]") {
    // Without the switch: FIFO, as before, whatever the surface lists.
    STATIC_REQUIRE(ChooseFluidPresentMode(false, false) == VSyncMode::Fifo);
    STATIC_REQUIRE(ChooseFluidPresentMode(false, true) == VSyncMode::Fifo);
    // With it: MAILBOX only when the surface lists it.
    STATIC_REQUIRE(ChooseFluidPresentMode(true, false) == VSyncMode::Fifo);
    STATIC_REQUIRE(ChooseFluidPresentMode(true, true) == VSyncMode::Mailbox);
}
