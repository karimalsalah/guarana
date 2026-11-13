// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>

#include "common/common_types.h"

namespace VideoCommon::RunningSamplesQuery {

/// Slots in a bank of samples (occlusion) queries.
inline constexpr size_t BankSlots = 256;

/// Banks a running samples query may span before CloseCounter drops its count as a reset would.
/// Gameplay windows in Hyrule Field and Kokiri Forest on an AMD iGPU (Lichii issue 1428) went
/// past 4 banks (1,024 slots) and their occlusion-tested objects blinked; with 32 banks the
/// blinking stopped and the remaining drops, at about 8,250 slots, came during scene loads.
inline constexpr size_t MaxBanks = 32;

inline constexpr size_t MaxSlots = MaxBanks * BankSlots;

/// True when a running query has outgrown the bound and its count is dropped.
[[nodiscard]] constexpr bool IsOverBound(size_t size_slots) {
    return size_slots > MaxSlots;
}

/// True for the 1st, 2nd, 4th, 8th... drop, the ones logged: a window this long during gameplay
/// doesn't go unnoticed and a title screen doesn't flood the log.
[[nodiscard]] constexpr bool IsLoggedDrop(u64 drop) {
    return drop != 0 && (drop & (drop - 1)) == 0;
}

} // namespace VideoCommon::RunningSamplesQuery
