// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdlib>

#include "common/common_types.h"

/// EDEN_NX2_HITCH_*: frame-time hitch experiments of the Guarana line (docs/hitches.md). Every
/// switch is off unless set; each is read once, on first use (after nx2_switches.txt is applied).
namespace VideoCommon::Hitch {

[[nodiscard]] inline bool EnvOn(const char* name) {
    const char* const value = std::getenv(name);
    return value != nullptr && value[0] != '\0' && !(value[0] == '0' && value[1] == '\0');
}

/// EDEN_NX2_HITCH_FINISH_TRACE=1 (diagnostic): every 10 s, the callers of Scheduler::Finish by
/// file and line, with their waits.
[[nodiscard]] inline bool FinishTrace() {
    static const bool on = EnvOn("EDEN_NX2_HITCH_FINISH_TRACE");
    return on;
}

/// EDEN_NX2_HITCH_GC_BATCH=1 (experiment): the texture collector downloads the images it evicts
/// in batches of up to 64 MiB and waits for the host GPU once per batch, not once per image.
[[nodiscard]] inline bool GcBatch() {
    static const bool on = EnvOn("EDEN_NX2_HITCH_GC_BATCH");
    return on;
}

/// EDEN_NX2_HITCH_GC_CLEAN_FIRST=1 (experiment): among the oldest images the texture collector may
/// delete, those it can drop at once go before those it must download first.
[[nodiscard]] inline bool GcCleanFirst() {
    static const bool on = EnvOn("EDEN_NX2_HITCH_GC_CLEAN_FIRST");
    return on;
}

/// EDEN_NX2_HITCH_UMA_BUDGET_MIB=<n> (experiment): the share of system memory an integrated GPU's
/// device budget may reach, in MiB, instead of 4 GiB (6 GiB with aggressive VRAM use). 0: unset.
/// At most 16 GiB: a budget of 20 GiB or more would also keep ASTC textures uncompressed
/// (UseBc3ForSwitch2OnSmallCards).
[[nodiscard]] inline u64 UmaBudgetMiB() {
    static const u64 value = [] {
        const char* const text = std::getenv("EDEN_NX2_HITCH_UMA_BUDGET_MIB");
        const u64 mib = text != nullptr ? static_cast<u64>(std::strtoull(text, nullptr, 10)) : u64{0};
        return mib < u64{16384} ? mib : u64{16384};
    }();
    return value;
}

} // namespace VideoCommon::Hitch
