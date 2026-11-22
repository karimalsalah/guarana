// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <map>
#include <span>
#include <string>
#include <string_view>

#include "common/common_types.h"

namespace Core::NCE {

/// What native code execution (NCE, docs/NCE.md) would do with one module's code, found with the
/// patcher's own instruction decoders (instructions.h). Plain arithmetic, so it runs on every host,
/// those without NCE included.
struct Readiness {
    u64 instructions{};
    /// Rewritten by the patcher into calls to the emulator: supervisor calls, the thread pointer
    /// (MRS TPIDR_EL0 and TPIDRRO_EL0, MSR TPIDR_EL0) and the counter (MRS CNTPCT_EL0, CNTFRQ_EL0).
    u64 svc{};
    u64 thread_pointer{};
    u64 counter{};
    /// Exclusive loads and stores, which the patcher makes ordered.
    u64 exclusive{};
    /// Every other system register read (MRS) and write (MSR), by its 15-bit encoding: these run
    /// on the host CPU as they are, and read the host's value.
    std::map<u32, u64> native_reads;
    std::map<u32, u64> native_writes;
};

[[nodiscard]] Readiness ScanText(std::span<const u32> words);

/// The register's name when NCE has a reason to care about it (CNTVCT_EL0, FPCR, ...), else its
/// generic form, S<op0>_<op1>_C<CRn>_C<CRm>_<op2>.
[[nodiscard]] std::string SystemRegisterName(u32 encoding);

/// One log line for a module: the counts, then the registers that run natively.
[[nodiscard]] std::string FormatReadiness(const Readiness& readiness, std::string_view module);

/// EDEN_NX2_NCE_REPORT (docs/switches.md): log the report for every module a process loads.
[[nodiscard]] bool ReadinessReportEnabled();

} // namespace Core::NCE
