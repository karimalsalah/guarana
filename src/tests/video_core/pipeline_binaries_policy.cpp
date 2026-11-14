// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/vulkan_common/pipeline_binaries_policy.h"

namespace {
using Vulkan::DecidePipelineBinaries;
using Vulkan::ParsePipelineBinariesOverride;
using Vulkan::PipelineBinariesInputs;
using Vulkan::PipelineBinariesOverride;
using Vulkan::PipelineBinariesReason;

// A driver that offers everything the store needs, with a disk cache of its own and no
// preference for it: NVIDIA's proprietary driver, say.
constexpr PipelineBinariesInputs Offered() {
    return {
        .extension = true,
        .feature = true,
        .maintenance5 = true,
        .prefers_internal_cache = false,
        .driver_caches_pipelines = true,
        .amd_proprietary = false,
        .requested = PipelineBinariesOverride::Unset,
    };
}
} // Anonymous namespace

TEST_CASE("EDEN_NX2_PIPELINE_BINARIES reads numbers as its neighbours do",
          "[video_core][pipeline_binaries]") {
    CHECK(ParsePipelineBinariesOverride("0") == PipelineBinariesOverride::Off);
    CHECK(ParsePipelineBinariesOverride("00") == PipelineBinariesOverride::Off);
    CHECK(ParsePipelineBinariesOverride("false") == PipelineBinariesOverride::Off);
    CHECK(ParsePipelineBinariesOverride("1") == PipelineBinariesOverride::On);
    CHECK(ParsePipelineBinariesOverride("2") == PipelineBinariesOverride::On);
    CHECK(ParsePipelineBinariesOverride(" 1") == PipelineBinariesOverride::On);
}

TEST_CASE("EDEN_NX2_PIPELINE_BINARIES unset, empty or not a number keeps the built-in choice",
          "[video_core][pipeline_binaries]") {
    CHECK(ParsePipelineBinariesOverride(nullptr) == PipelineBinariesOverride::Unset);
    CHECK(ParsePipelineBinariesOverride("") == PipelineBinariesOverride::Unset);
    CHECK(ParsePipelineBinariesOverride("true") == PipelineBinariesOverride::Unset);
    CHECK(ParsePipelineBinariesOverride("yes") == PipelineBinariesOverride::Unset);
}

TEST_CASE("Unset, the store follows main's rule over every combination",
          "[video_core][pipeline_binaries]") {
    // main: pipelineBinaries && maintenance5 && !prefersInternalCache &&
    //       (!DriverCachesPipelines || AMD proprietary), for a driver that offers the extension.
    for (unsigned bits = 0; bits < 16; ++bits) {
        PipelineBinariesInputs in = Offered();
        in.feature = (bits & 1U) != 0;
        in.maintenance5 = (bits & 2U) != 0;
        in.prefers_internal_cache = (bits & 4U) != 0;
        in.driver_caches_pipelines = (bits & 8U) != 0;
        for (const bool amd : {false, true}) {
            in.amd_proprietary = amd;
            const bool main_rule = in.feature && in.maintenance5 && !in.prefers_internal_cache &&
                                   (!in.driver_caches_pipelines || in.amd_proprietary);
            CHECK(DecidePipelineBinaries(in).enabled == main_rule);
        }
    }
}

TEST_CASE("Unset keeps AMD's proprietary driver on and NVIDIA's off",
          "[video_core][pipeline_binaries]") {
    PipelineBinariesInputs in = Offered();
    // NVIDIA: a driver cache of its own, not AMD.
    CHECK_FALSE(DecidePipelineBinaries(in).enabled);
    CHECK(DecidePipelineBinaries(in).reason == PipelineBinariesReason::DriverKeepsCache);

    in.amd_proprietary = true;
    CHECK(DecidePipelineBinaries(in).enabled);
    CHECK(DecidePipelineBinaries(in).reason == PipelineBinariesReason::AmdProprietary);

    // AMD, but the driver asks for its internal cache.
    in.prefers_internal_cache = true;
    CHECK_FALSE(DecidePipelineBinaries(in).enabled);
    CHECK(DecidePipelineBinaries(in).reason == PipelineBinariesReason::PrefersInternalCache);

    // A driver with no cache of its own (Intel, Mesa with its cache off).
    in = Offered();
    in.driver_caches_pipelines = false;
    CHECK(DecidePipelineBinaries(in).enabled);
    CHECK(DecidePipelineBinaries(in).reason == PipelineBinariesReason::NoDriverCache);
}

TEST_CASE("EDEN_NX2_PIPELINE_BINARIES=0 turns the store off wherever it would run",
          "[video_core][pipeline_binaries]") {
    PipelineBinariesInputs in = Offered();
    in.amd_proprietary = true;
    in.requested = PipelineBinariesOverride::Off;
    CHECK_FALSE(DecidePipelineBinaries(in).enabled);
    CHECK(DecidePipelineBinaries(in).reason == PipelineBinariesReason::ForcedOff);

    in.driver_caches_pipelines = false;
    in.amd_proprietary = false;
    CHECK_FALSE(DecidePipelineBinaries(in).enabled);
    CHECK(DecidePipelineBinaries(in).reason == PipelineBinariesReason::ForcedOff);
}

TEST_CASE("EDEN_NX2_PIPELINE_BINARIES=1 turns the store on wherever the driver offers it",
          "[video_core][pipeline_binaries]") {
    PipelineBinariesInputs in = Offered();
    in.requested = PipelineBinariesOverride::On;
    // A driver cache of its own, not AMD (Mesa RADV with its disk cache on, NVIDIA).
    CHECK(DecidePipelineBinaries(in).enabled);
    CHECK(DecidePipelineBinaries(in).reason == PipelineBinariesReason::ForcedOn);

    // The driver preferring its internal cache is overridden too.
    in.prefers_internal_cache = true;
    CHECK(DecidePipelineBinaries(in).enabled);
}

TEST_CASE("EDEN_NX2_PIPELINE_BINARIES=1 never enables what the driver lacks",
          "[video_core][pipeline_binaries]") {
    PipelineBinariesInputs in = Offered();
    in.requested = PipelineBinariesOverride::On;

    in.extension = false;
    CHECK_FALSE(DecidePipelineBinaries(in).enabled);
    CHECK(DecidePipelineBinaries(in).reason == PipelineBinariesReason::NotOffered);

    in = Offered();
    in.requested = PipelineBinariesOverride::On;
    in.feature = false;
    CHECK_FALSE(DecidePipelineBinaries(in).enabled);
    CHECK(DecidePipelineBinaries(in).reason == PipelineBinariesReason::FeatureAbsent);

    in = Offered();
    in.requested = PipelineBinariesOverride::On;
    in.maintenance5 = false;
    CHECK_FALSE(DecidePipelineBinaries(in).enabled);
    CHECK(DecidePipelineBinaries(in).reason == PipelineBinariesReason::NoMaintenance5);
}

TEST_CASE("Every reason has a log text", "[video_core][pipeline_binaries]") {
    for (const PipelineBinariesReason reason :
         {PipelineBinariesReason::NotOffered, PipelineBinariesReason::FeatureAbsent,
          PipelineBinariesReason::NoMaintenance5, PipelineBinariesReason::ForcedOff,
          PipelineBinariesReason::ForcedOn, PipelineBinariesReason::PrefersInternalCache,
          PipelineBinariesReason::NoDriverCache, PipelineBinariesReason::AmdProprietary,
          PipelineBinariesReason::DriverKeepsCache}) {
        const std::string_view text = Vulkan::PipelineBinariesReasonText(reason);
        CHECK_FALSE(text.empty());
        CHECK(text != "unknown");
    }
}
