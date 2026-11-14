// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <limits>
#include <optional>

#include "common/common_types.h"

namespace VideoCommon {

// One extra word supports an unaligned read starting at the last byte, plus one pointer flag
// per word. Oversized buffers must keep the attached environment's answers.
[[nodiscard]] constexpr std::optional<u64> DetachedConstantBufferBytes(u32 size, u32 maximum) {
    if (size > maximum) {
        return std::nullopt;
    }
    if (size == 0) {
        return 0;
    }
    const u64 slots = (u64{size} + sizeof(u32) - 1) / sizeof(u32);
    return (slots + 1) * sizeof(u32) + slots;
}

[[nodiscard]] constexpr std::optional<u64> ConservativeSnapshotCharge(u64 payload) {
    constexpr u64 overhead = 4096;
    if (payload > (std::numeric_limits<u64>::max() - overhead) / 2) {
        return std::nullopt;
    }
    return payload * 2 + overhead;
}

} // namespace VideoCommon
