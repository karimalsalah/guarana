// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdlib>

#include "common/common_types.h"

/// EDEN_NX2_GPUCOST_QUERY, EDEN_NX2_GPUCOST_PRESENT (default off, 0 or unset changes nothing):
/// emulator-issued GPU commands that give the same image with fewer barriers or passes.
namespace VideoCommon::Nx2GpuCost {

[[nodiscard]] inline u32 ReadValue(const char* name) {
    const char* const value = std::getenv(name);
    return value != nullptr ? static_cast<u32>(std::strtoul(value, nullptr, 0)) : 0;
}

/// Query syncs (QueryCacheBase::NotifyWFI).
/// 1: a sample query copy takes no barrier of its own: the barrier before the sync's buffer
///    copies (QueryCacheRuntime::Barriers(true)) already orders the copies' writes before them.
/// 2: WaitForIdle records no barrier of its own after a sync that recorded one: the barrier
///    after the sync's copies already waits for every stage of all earlier work.
inline constexpr u32 QueryNoCopyBarrier = 1;
inline constexpr u32 QuerySkipWfiBarrier = 2;

inline u32 g_query = ReadValue("EDEN_NX2_GPUCOST_QUERY");
/// Present: a single opaque layer that covers the whole frame is drawn without the clear before it.
inline u32 g_present = ReadValue("EDEN_NX2_GPUCOST_PRESENT");

/// Called by VideoCore::CreateGPU: static initialization runs before the frontend loads
/// nx2_switches.txt, so a switch set only there takes effect from that point on.
inline void ReadSwitch() {
    g_query = ReadValue("EDEN_NX2_GPUCOST_QUERY");
    g_present = ReadValue("EDEN_NX2_GPUCOST_PRESENT");
}

[[nodiscard]] inline bool Query(u32 bit) {
    return (g_query & bit) != 0;
}

[[nodiscard]] inline bool PresentSkipsClear() {
    return (g_present & 1) != 0;
}

} // namespace VideoCommon::Nx2GpuCost
