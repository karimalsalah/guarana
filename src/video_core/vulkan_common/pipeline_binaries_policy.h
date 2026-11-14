// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdlib>
#include <string_view>

#include "common/common_types.h"

namespace Vulkan {

/// What EDEN_NX2_PIPELINE_BINARIES asks of the VK_KHR_pipeline_binary store.
enum class PipelineBinariesOverride : u8 {
    Unset, ///< Unset, empty or not a number: the built-in choice
    Off,   ///< 0 or false: the store is off
    On,    ///< Any other number: the store is on wherever the driver offers the extension
};

/// Reads EDEN_NX2_PIPELINE_BINARIES: a number is parsed as its neighbours' switches are
/// (strtoul, base 10), so "0" is off and "1" is on. Unlike them, unset and empty are not "off":
/// they keep the built-in choice, and so does anything that is not a number ("true", "yes").
inline PipelineBinariesOverride ParsePipelineBinariesOverride(const char* value) {
    if (value == nullptr || *value == '\0') {
        return PipelineBinariesOverride::Unset;
    }
    char* end = nullptr;
    const unsigned long number = std::strtoul(value, &end, 10);
    if (end != value) {
        return number != 0 ? PipelineBinariesOverride::On : PipelineBinariesOverride::Off;
    }
    return std::string_view{value} == "false" ? PipelineBinariesOverride::Off
                                              : PipelineBinariesOverride::Unset;
}

/// What the device knows when it decides. The order of the members is the order the initializers
/// in vulkan_device.cpp and the tests use.
struct PipelineBinariesInputs {
    bool extension{};               ///< VK_KHR_pipeline_binary is supported and loaded
    bool feature{};                 ///< pipelineBinaries
    bool maintenance5{};            ///< Enabled: the store captures with its create flag
    bool prefers_internal_cache{};  ///< pipelineBinaryPrefersInternalCache
    bool driver_caches_pipelines{}; ///< Device::DriverCachesPipelines()
    bool amd_proprietary{};         ///< VK_DRIVER_ID_AMD_PROPRIETARY
    PipelineBinariesOverride requested{};
};

enum class PipelineBinariesReason : u8 {
    NotOffered,           ///< The driver does not offer the extension
    FeatureAbsent,        ///< The extension is there, the pipelineBinaries feature is not
    NoMaintenance5,       ///< The store needs VK_KHR_maintenance5's capture flag
    ForcedOff,            ///< EDEN_NX2_PIPELINE_BINARIES=0
    ForcedOn,             ///< EDEN_NX2_PIPELINE_BINARIES=1
    PrefersInternalCache, ///< The driver asks for its own cache to be used
    NoDriverCache,        ///< The driver keeps no disk cache of its own
    AmdProprietary,       ///< Its disk cache loses pipelines with dynamic vertex input (#770)
    DriverKeepsCache,     ///< The driver's disk cache would only be duplicated
};

struct PipelineBinariesDecision {
    bool enabled{};
    PipelineBinariesReason reason{PipelineBinariesReason::NotOffered};
};

/// Whether the VK_KHR_pipeline_binary store runs. The override never enables what the driver does
/// not offer: no extension, no pipelineBinaries feature and no maintenance5 stay off. Beyond that,
/// 1 also overrides the driver preferring its internal cache and a driver cache of its own (so it
/// can turn the store on for NVIDIA, where a session stored 2.3 GB), and 0 turns it off. Unset,
/// the answer is what main always gave: the feature, maintenance5, a driver that does not prefer
/// its internal cache, and either no cache of its own or AMD's proprietary driver.
constexpr PipelineBinariesDecision DecidePipelineBinaries(const PipelineBinariesInputs& in) {
    if (!in.extension) {
        return {false, PipelineBinariesReason::NotOffered};
    }
    if (!in.feature) {
        return {false, PipelineBinariesReason::FeatureAbsent};
    }
    if (!in.maintenance5) {
        return {false, PipelineBinariesReason::NoMaintenance5};
    }
    switch (in.requested) {
    case PipelineBinariesOverride::Off:
        return {false, PipelineBinariesReason::ForcedOff};
    case PipelineBinariesOverride::On:
        return {true, PipelineBinariesReason::ForcedOn};
    case PipelineBinariesOverride::Unset:
        break;
    }
    if (in.prefers_internal_cache) {
        return {false, PipelineBinariesReason::PrefersInternalCache};
    }
    if (!in.driver_caches_pipelines) {
        return {true, PipelineBinariesReason::NoDriverCache};
    }
    if (in.amd_proprietary) {
        return {true, PipelineBinariesReason::AmdProprietary};
    }
    return {false, PipelineBinariesReason::DriverKeepsCache};
}

/// Log a decision when the extension is offered or the player supplied any value, including an
/// unusable one. An absent extension must not hide the diagnostic for an ignored override.
constexpr bool ShouldLogPipelineBinariesDecision(PipelineBinariesReason reason, const char* value) {
    return reason != PipelineBinariesReason::NotOffered || value != nullptr;
}

/// The reason as the log line says it.
constexpr const char* PipelineBinariesReasonText(PipelineBinariesReason reason) {
    switch (reason) {
    case PipelineBinariesReason::NotOffered:
        return "the driver does not offer the extension";
    case PipelineBinariesReason::FeatureAbsent:
        return "the driver reports no pipelineBinaries feature";
    case PipelineBinariesReason::NoMaintenance5:
        return "the store needs VK_KHR_maintenance5";
    case PipelineBinariesReason::ForcedOff:
        return "EDEN_NX2_PIPELINE_BINARIES=0";
    case PipelineBinariesReason::ForcedOn:
        return "EDEN_NX2_PIPELINE_BINARIES=1";
    case PipelineBinariesReason::PrefersInternalCache:
        return "the driver prefers its internal cache";
    case PipelineBinariesReason::NoDriverCache:
        return "the driver keeps no pipeline cache of its own";
    case PipelineBinariesReason::AmdProprietary:
        return "AMD's proprietary driver keeps no graphics pipeline with dynamic vertex input";
    case PipelineBinariesReason::DriverKeepsCache:
        return "the driver keeps a pipeline cache of its own";
    }
    return "unknown";
}

} // namespace Vulkan
