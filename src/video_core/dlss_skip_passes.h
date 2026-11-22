// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdlib>
#include <string_view>

#include "common/common_types.h"

// EDEN_NX2_DLSS_SKIP_PASSES: skip a game's DLSS network passes (all but the prepass, once one full
// evaluation recorded them) while their output can only be overwritten. Two rules, ported from
// open zortos293/Lichii pull requests:
// - `drew` (Lichii #864): while the replacement upscale drew the previous evaluation's output and
//   this frame's jitter was captured. Measured upstream: Duskbloods on a Steam Deck, 56.6 to 40.4 ms
//   of GPU time a frame; the Radeon 890M spent 7.6 of 24 ms in these passes.
// - `blocked` (Lichii #1985): while something keeps the network from being shown (the setting off,
//   a replacement forced, resolution scaling, NVIDIA DLSS, no support). It also keeps rejected
//   passes (EDEN_NX2_NO_TENSOR on RADV) from wiping the replacement's motion, depth and jitter, but
//   unlike `drew` it skips even when the replacement then fails to draw.
// Unset, empty or `0`: neither, and only a network dropped for good skips its passes, as before.
namespace VideoCommon::DlssSkipPasses {

inline constexpr u32 RULE_DREW = 1;
inline constexpr u32 RULE_BLOCKED = 2;

/// `drew`, `blocked`, `1` or `both` (both rules); anything else, unset or empty: none.
[[nodiscard]] inline u32 ParseRules(const char* value) {
    if (value == nullptr) {
        return 0;
    }
    const std::string_view text{value};
    if (text == "drew") {
        return RULE_DREW;
    }
    if (text == "blocked") {
        return RULE_BLOCKED;
    }
    if (text == "1" || text == "both") {
        return RULE_DREW | RULE_BLOCKED;
    }
    return 0;
}

/// The switch, read once.
[[nodiscard]] inline u32 Rules() {
    static const u32 rules = ParseRules(std::getenv("EDEN_NX2_DLSS_SKIP_PASSES"));
    return rules;
}

/// Whether a known network pass other than the prepass is skipped. `dropped_for_good`: the network
/// failed for good (always skipped, as before); `blocked`: a blocker keeps the network from being
/// shown; `replacement_will_draw`: the replacement drew last time and has this frame's jitter, and
/// no output-mismatch recheck runs.
[[nodiscard]] constexpr bool Skip(u32 rules, bool dropped_for_good, bool blocked,
                                  bool replacement_will_draw) {
    return dropped_for_good || ((rules & RULE_BLOCKED) != 0 && blocked) ||
           ((rules & RULE_DREW) != 0 && replacement_will_draw);
}

} // namespace VideoCommon::DlssSkipPasses
