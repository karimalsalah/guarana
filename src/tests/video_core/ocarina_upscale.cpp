// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include "video_core/ocarina_upscale.h"
#include "video_core/fsr.h"

TEST_CASE("OoT temporal backend selection is opt-in and vendor neutral", "[video_core][ocarina]") {
    using enum VideoCommon::OcarinaUpscaleBackend;
    using VideoCommon::SelectOcarinaUpscaler;
    CHECK(SelectOcarinaUpscaler(false, false, false, "") == Native);
    CHECK(SelectOcarinaUpscaler(false, false, true, "") == Native);
    CHECK(SelectOcarinaUpscaler(true, false, false, "") == Fsr);
    CHECK(SelectOcarinaUpscaler(true, false, true, "") == Fsr);
    CHECK(SelectOcarinaUpscaler(false, true, false, "") == Native);
    CHECK(SelectOcarinaUpscaler(false, true, true, "") == Dlss);
    CHECK(SelectOcarinaUpscaler(true, true, false, "") == Fsr);
    CHECK(SelectOcarinaUpscaler(true, true, true, "") == Dlss);
    CHECK(SelectOcarinaUpscaler(true, true, true, "native") == Native);
    CHECK(SelectOcarinaUpscaler(false, true, true, "fsr") == Fsr);
    CHECK(SelectOcarinaUpscaler(false, false, false, "fsr") == Fsr);
    CHECK(SelectOcarinaUpscaler(false, false, false, "dlss") == Native);
    CHECK(SelectOcarinaUpscaler(false, false, true, "dlss") == Dlss);
    CHECK(SelectOcarinaUpscaler(false, true, true, "dlss") == Dlss);
    CHECK(SelectOcarinaUpscaler(false, false, false, "garbage") == Native);
}

TEST_CASE("OoT FSR texture LOD bias follows the upscaler, the console mode and failure",
          "[video_core][ocarina]") {
    using enum VideoCommon::OcarinaUpscaleBackend;
    using VideoCommon::KeepOcarinaFsrLodBias;
    using VideoCommon::OcarinaFsrLodBias;
    CHECK(OcarinaFsrLodBias(Fsr) == -1.58f);
    CHECK(OcarinaFsrLodBias(Dlss) == 0.0f);
    CHECK(OcarinaFsrLodBias(Native) == 0.0f);
    CHECK(KeepOcarinaFsrLodBias(true, false, false));
    CHECK_FALSE(KeepOcarinaFsrLodBias(false, false, false));
    CHECK_FALSE(KeepOcarinaFsrLodBias(true, false, true));
    CHECK_FALSE(KeepOcarinaFsrLodBias(true, true, false));
    CHECK_FALSE(KeepOcarinaFsrLodBias(true, true, true));
}

TEST_CASE("OoT FSR at any resolution step takes scales of the output's shape",
          "[video_core][ocarina]") {
    using VideoCommon::OcarinaUpscaleFits;
    STATIC_REQUIRE(OcarinaUpscaleFits(1440, 810, 1920, 1080));
    STATIC_REQUIRE(OcarinaUpscaleFits(1280, 720, 1920, 1080));
    STATIC_REQUIRE(OcarinaUpscaleFits(1024, 576, 1920, 1080));
    STATIC_REQUIRE(OcarinaUpscaleFits(854, 480, 1920, 1080));
    STATIC_REQUIRE(OcarinaUpscaleFits(1920, 1080, 1920, 1080));
    STATIC_REQUIRE(OcarinaUpscaleFits(1920, 1080, 2560, 1440));
    // The 480p step's images are 864 wide; the game draws 854 of them.
    STATIC_REQUIRE_FALSE(OcarinaUpscaleFits(864, 480, 1920, 1080));
    STATIC_REQUIRE_FALSE(OcarinaUpscaleFits(1920, 1080, 1440, 810));
    STATIC_REQUIRE_FALSE(OcarinaUpscaleFits(256, 144, 1920, 1080));
    STATIC_REQUIRE_FALSE(OcarinaUpscaleFits(1024, 1024, 1920, 1080));
    STATIC_REQUIRE_FALSE(OcarinaUpscaleFits(0, 0, 1920, 1080));
}

TEST_CASE("OoT FSR texture LOD bias follows the scale in use", "[video_core][ocarina]") {
    using VideoCommon::OcarinaFsrLodBiasQuarters;
    STATIC_REQUIRE(OcarinaFsrLodBiasQuarters(1920, 1920) == 4);
    STATIC_REQUIRE(OcarinaFsrLodBiasQuarters(1440, 1920) == 6);
    STATIC_REQUIRE(OcarinaFsrLodBiasQuarters(2560, 3840) == 6);
    STATIC_REQUIRE(OcarinaFsrLodBiasQuarters(1280, 1920) == 6);
    STATIC_REQUIRE(OcarinaFsrLodBiasQuarters(1024, 1920) == 8);
    STATIC_REQUIRE(OcarinaFsrLodBiasQuarters(854, 1920) == 9);
    STATIC_REQUIRE(OcarinaFsrLodBiasQuarters(480, 1920) == 12);
    STATIC_REQUIRE(OcarinaFsrLodBiasQuarters(0, 1920) == 0);
    STATIC_REQUIRE(OcarinaFsrLodBiasQuarters(1920, 1440) == 0);
}

TEST_CASE("FSR temporal sharpness uses the existing stops slider", "[video_core][fsr][ocarina]") {
    CHECK(FSR::RcasSharpnessFromStops(0.0f) == 1.0f);
    CHECK(FSR::RcasSharpnessFromStops(0.81f) == 0.595f);
    CHECK(FSR::RcasSharpnessFromStops(1.0f) == 0.5f);
    CHECK(FSR::RcasSharpnessFromStops(2.0f) == 0.0f);
    CHECK(FSR::RcasSharpnessFromStops(3.0f) == 0.0f);
    CHECK(FSR::RcasSharpnessFromStops(-1.0f) == 1.0f);
}
