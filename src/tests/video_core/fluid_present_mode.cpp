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
    STATIC_REQUIRE(ChooseFluidPresentMode(false, false, false, false) == VSyncMode::Fifo);
    STATIC_REQUIRE(ChooseFluidPresentMode(false, true, false, false) == VSyncMode::Fifo);
    // With it: MAILBOX only when the surface lists it.
    STATIC_REQUIRE(ChooseFluidPresentMode(true, false, false, false) == VSyncMode::Fifo);
    STATIC_REQUIRE(ChooseFluidPresentMode(true, true, false, false) == VSyncMode::Mailbox);
    // A surface that lists IMMEDIATE changes nothing while that switch is off.
    STATIC_REQUIRE(ChooseFluidPresentMode(false, false, false, true) == VSyncMode::Fifo);
    STATIC_REQUIRE(ChooseFluidPresentMode(false, true, false, true) == VSyncMode::Fifo);
    STATIC_REQUIRE(ChooseFluidPresentMode(true, false, false, true) == VSyncMode::Fifo);
    STATIC_REQUIRE(ChooseFluidPresentMode(true, true, false, true) == VSyncMode::Mailbox);
}

TEST_CASE("FLUID presents with IMMEDIATE only when EDEN_NX2_FLUID_IMMEDIATE finds it",
          "[video_core]") {
    // The reference laptop's surface: IMMEDIATE and FIFO, no MAILBOX.
    STATIC_REQUIRE(ChooseFluidPresentMode(false, false, true, true) == VSyncMode::Immediate);
    // The switch asked for and the surface does not list IMMEDIATE: FIFO, as without it.
    STATIC_REQUIRE(ChooseFluidPresentMode(false, false, true, false) == VSyncMode::Fifo);
    STATIC_REQUIRE(ChooseFluidPresentMode(false, true, true, false) == VSyncMode::Fifo);
    // MAILBOX is not asked for: a listed MAILBOX is not used.
    STATIC_REQUIRE(ChooseFluidPresentMode(false, true, true, true) == VSyncMode::Immediate);
}

TEST_CASE("FLUID presents with MAILBOX when both switches are on and it is listed",
          "[video_core]") {
    STATIC_REQUIRE(ChooseFluidPresentMode(true, true, true, true) == VSyncMode::Mailbox);
    STATIC_REQUIRE(ChooseFluidPresentMode(true, true, true, false) == VSyncMode::Mailbox);
    // MAILBOX not listed: IMMEDIATE if listed, else FIFO.
    STATIC_REQUIRE(ChooseFluidPresentMode(true, false, true, true) == VSyncMode::Immediate);
    STATIC_REQUIRE(ChooseFluidPresentMode(true, false, true, false) == VSyncMode::Fifo);
}
