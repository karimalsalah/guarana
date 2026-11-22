// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstdlib>
#include "common/common_types.h"
#include "core/hardware_properties.h"

// EDEN_NX2_SVC_FASTPATH=1: after a supervisor call that leaves its thread runnable with no
// interrupt pending, PhysicalCore::RunThread continues the JIT loop in place instead of
// returning to the scheduler round trip (off by default). The outer guest-thread loop
// (CpuManager::MultiCoreRunGuestThread) re-enters RunThread with the same thread under these
// conditions, so continuing skips only the round trip. A SVC that blocks or yields to another
// thread has already switched fibers inside Svc::Call; one that terminates or suspends leaves
// the thread non-runnable and still returns to the scheduler. Single-core is excluded: its loop
// must advance CoreTiming after every return.
//
// Migration gate: the scheduler can move the CURRENT thread to another core inside a SVC
// (load balancing in UpdateHighestPriorityThreadsImpl, YieldWithCoreMigration,
// SetThreadCoreMask). The thread stays runnable, but its fiber resumes on the destination
// core's host thread while this RunThread frame still belongs to the source core. The outer
// loop handles that by re-fetching kernel.CurrentPhysicalCore() after every return; continuing
// in place would run the thread on the source core's ARM interface, racing the source core's
// next thread on the same JIT. So a continue also requires that the current host core is still
// this core (same_core). Verified 2026-11-22: without this gate EDEN_NX2_SVC_FASTPATH=1 hangs
// Ocarina of Time Switch 2 at boot (guest ~31 s, zero presents, no core ever schedules again);
// the same build with the switch off boots normally.
namespace Kernel::Nx2SvcFastpath {

[[nodiscard]] inline bool ParseEnabled(const char* value) {
    return value != nullptr && std::strtoul(value, nullptr, 10) != 0;
}

[[nodiscard]] inline bool Enabled() {
    static const bool enabled = ParseEnabled(std::getenv("EDEN_NX2_SVC_FASTPATH"));
    return enabled;
}

/// The continue decision, as a pure function so the unit tests can hold the whole truth table.
[[nodiscard]] inline bool ContinueAfterSvc(bool enabled, bool single_core, bool interrupted,
                                           bool runnable, bool same_core) {
    return enabled && !single_core && !interrupted && runnable && same_core;
}

/// In-place continues per core, so a GUEST_THREAD_STATS window can prove the switch fired.
inline std::array<std::atomic<u64>, Core::Hardware::NUM_CPU_CORES> g_continues{};

} // namespace Kernel::Nx2SvcFastpath