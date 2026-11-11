// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string_view>

namespace VideoCommon {

enum class OcarinaUpscaleBackend { Native, Fsr, Dlss };

constexpr OcarinaUpscaleBackend SelectOcarinaUpscaler(bool fsr_enabled, bool dlss_enabled,
                                                    bool dlss_available,
                                                    std::string_view override) {
    if (override == "native") {
        return OcarinaUpscaleBackend::Native;
    }
    if (override == "fsr") {
        return OcarinaUpscaleBackend::Fsr;
    }
    if ((dlss_enabled || override == "dlss") && dlss_available) {
        return OcarinaUpscaleBackend::Dlss;
    }
    return fsr_enabled ? OcarinaUpscaleBackend::Fsr : OcarinaUpscaleBackend::Native;
}

/// Texture LOD bias added to Ocarina's mipmapped samplers while FSR upscales. AMD recommends
/// log2(render / output) - 1: -1.58 for docked 1440p -> 4K.
constexpr float OcarinaFsrLodBias(OcarinaUpscaleBackend backend) {
    return backend == OcarinaUpscaleBackend::Fsr ? -1.58f : 0.0f;
}

/// Whether a sampler with that bias keeps it for an image: only docked (handheld isn't measured),
/// not for images the GPU wrote (render targets would read a mip too sharp), and not once the
/// upscaler failed this session (the game's own pass runs again).
constexpr bool KeepOcarinaFsrLodBias(bool docked, bool upscaler_failed, bool gpu_written) {
    return docked && !upscaler_failed && !gpu_written;
}

/// EDEN_NX2_OOT_FSR_ANY: whether the temporal upscaler takes a scene drawn at
/// render_width x render_height to an output of output_width x output_height: not smaller, at
/// most 4x, and the same shape to within a render pixel (the game draws 854x480 for 1920x1080).
constexpr bool OcarinaUpscaleFits(unsigned render_width, unsigned render_height,
                                  unsigned output_width, unsigned output_height) {
    if (render_width == 0 || render_height == 0 || output_width < render_width ||
        output_height < render_height || output_width > render_width * 4ULL) {
        return false;
    }
    const unsigned long long across = 1ULL * output_width * render_height;
    const unsigned long long down = 1ULL * output_height * render_width;
    const unsigned long long difference = across > down ? across - down : down - across;
    return difference <= (output_width > output_height ? output_width : output_height);
}

/// EDEN_NX2_OOT_FSR_ANY: AMD's bias log2(render / output) - 1 for the upscale in use, in quarter
/// LODs below zero as a sampler variant names it (6 is -1.5): 4 at 1:1, 6 for 3/4 scale, 9 for
/// 854 -> 1920. 0 without sizes.
constexpr unsigned OcarinaFsrLodBiasQuarters(unsigned render_width, unsigned output_width) {
    if (render_width == 0 || output_width < render_width) {
        return 0;
    }
    // log2(output / render) in quarters, rounded, without <cmath>: the largest q whose
    // 2^((2q - 1) / 8) the ratio still reaches.
    constexpr double eighth_root = 1.0905077326652577; // 2^(1/8)
    const double ratio = static_cast<double>(output_width) / static_cast<double>(render_width);
    unsigned quarters = 0;
    for (double bound = eighth_root; quarters < 8 && ratio >= bound;
         bound *= eighth_root * eighth_root) {
        ++quarters;
    }
    return 4 + quarters;
}

} // namespace VideoCommon
