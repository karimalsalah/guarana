// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "core/hle/kernel/nx2_svc_fastpath.h"

namespace {
using Kernel::Nx2SvcFastpath::ContinueAfterSvc;
using Kernel::Nx2SvcFastpath::ParseEnabled;
} // namespace

TEST_CASE("SVC fastpath switch parsing is off unless a nonzero decimal is set",
          "[core][svc_fastpath]") {
    REQUIRE_FALSE(ParseEnabled(nullptr));
    REQUIRE_FALSE(ParseEnabled(""));
    REQUIRE_FALSE(ParseEnabled("0"));
    REQUIRE_FALSE(ParseEnabled("00"));
    REQUIRE_FALSE(ParseEnabled("false"));
    REQUIRE_FALSE(ParseEnabled("abc"));
    REQUIRE(ParseEnabled("1"));
    REQUIRE(ParseEnabled("2"));
    REQUIRE(ParseEnabled(" 1"));
}

TEST_CASE("SVC fastpath continues only when enabled, multicore, uninterrupted, runnable and on the same core",
          "[core][svc_fastpath]") {
    // The whole truth table: a continue is allowed only when every gate holds.
    for (int enabled = 0; enabled < 2; ++enabled) {
        for (int single_core = 0; single_core < 2; ++single_core) {
            for (int interrupted = 0; interrupted < 2; ++interrupted) {
                for (int runnable = 0; runnable < 2; ++runnable) {
                    for (int same_core = 0; same_core < 2; ++same_core) {
                        const bool expected = enabled == 1 && single_core == 0 &&
                                              interrupted == 0 && runnable == 1 && same_core == 1;
                        REQUIRE(ContinueAfterSvc(enabled == 1, single_core == 1,
                                                 interrupted == 1, runnable == 1,
                                                 same_core == 1) == expected);
                    }
                }
            }
        }
    }
}

TEST_CASE("SVC fastpath never continues a halted or migrated thread", "[core][svc_fastpath]") {
    // A SVC that blocked, yielded to another thread or terminated leaves the thread
    // non-runnable or the core interrupted; both must fall back to the scheduler.
    REQUIRE_FALSE(ContinueAfterSvc(true, false, false, false, true)); // not runnable
    REQUIRE_FALSE(ContinueAfterSvc(true, false, true, true, true));   // interrupt pending
    REQUIRE_FALSE(ContinueAfterSvc(true, false, true, false, true));  // both
    // A SVC that migrated the current thread leaves it runnable on another host core: the
    // outer loop would re-enter via CurrentPhysicalCore(), so continuing here would run the
    // thread on the source core's JIT (the OoT boot hang, 2026-11-22).
    REQUIRE_FALSE(ContinueAfterSvc(true, false, false, true, false));
    // Single-core must always return so CoreTiming advances.
    REQUIRE_FALSE(ContinueAfterSvc(true, true, false, true, true));
    // Disabled is byte-for-byte the old behaviour.
    REQUIRE_FALSE(ContinueAfterSvc(false, false, false, true, true));
    // The one allowed case.
    REQUIRE(ContinueAfterSvc(true, false, false, true, true));
}