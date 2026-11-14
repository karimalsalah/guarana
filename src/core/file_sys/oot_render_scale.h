// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdlib>
#include <optional>

#include "common/common_types.h"
#include "common/settings.h"

// EDEN_NX2_OOT_RENDER_SCALE: Ocarina of Time's fixed render scale under the Max Resolution
// add-on, without NVIDIA DLSS. The render-scale records in the patch manager (ProfessorTee's
// #903) apply only with DLSS on a Streamline build; with the switch the 0.75 and 0.5 records
// apply next to Max Resolution, and the applied-scale state that turns FSR on and the network
// bypass off stays unset.
namespace FileSys::OotRenderScale {

enum class Request : u8 {
    Off,       ///< unset, empty, 0 or false: the patch manager behaves as without the switch
    Percent75, ///< 75: 0.75 of the mode's output, the DLSS Performance record
    Percent50, ///< 50: 0.5 of the mode's output, the DLSS Ultra Performance record
    Invalid,   ///< any other number: ignored, with a warning
};

/// The switch's value, parsed as a number as EDEN_NX2_FLUID is (strtoul, base 10), so unset,
/// empty, `0` and `false` are off. Only 75 and 50 name a render scale.
[[nodiscard]] inline Request ParseRequest(const char* value) {
    if (value == nullptr) {
        return Request::Off;
    }
    switch (std::strtoul(value, nullptr, 10)) {
    case 0:
        return Request::Off;
    case 75:
        return Request::Percent75;
    case 50:
        return Request::Percent50;
    default:
        return Request::Invalid;
    }
}

/// The request as a percentage of the output resolution; 100 when nothing is requested.
[[nodiscard]] constexpr u32 Percent(Request request) {
    switch (request) {
    case Request::Percent75:
        return 75;
    case Request::Percent50:
        return 50;
    case Request::Off:
    case Request::Invalid:
        break;
    }
    return 100;
}

/// The built-in render-scale record that holds the request's scale. The records are named after
/// the DLSS quality whose render size they give.
[[nodiscard]] constexpr std::optional<Settings::NvidiaDlssQuality> RecordQuality(Request request) {
    switch (request) {
    case Request::Percent75:
        return Settings::NvidiaDlssQuality::Performance;
    case Request::Percent50:
        return Settings::NvidiaDlssQuality::UltraPerformance;
    case Request::Off:
    case Request::Invalid:
        break;
    }
    return std::nullopt;
}

/// Whether the render-scale record of `record` applies because of the switch: it is the
/// requested one, the Max Resolution patch was applied (its fixed-scale path is what the record
/// scales), and DLSS is not already choosing a render scale.
[[nodiscard]] constexpr bool WantedBySwitch(Request request, Settings::NvidiaDlssQuality record,
                                            bool max_resolution_applied,
                                            bool dlss_scale_in_use) {
    return max_resolution_applied && !dlss_scale_in_use && RecordQuality(request) == record;
}

} // namespace FileSys::OotRenderScale
