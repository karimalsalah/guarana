// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstdlib>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "core/arm/nce/instructions.h"
#include "core/arm/nce/readiness.h"

namespace Core::NCE {
namespace {

struct NamedRegister {
    u32 encoding;
    std::string_view name;
};

// Registers EL0 code reads or writes, and the identification registers it may ask the kernel for.
// The encoding is o0:op1:CRn:CRm:op2, op0 being 2 + o0, as MRS and MSR hold it.
constexpr std::array NAMED_REGISTERS{
    NamedRegister{TpidrEl0, "TPIDR_EL0"},
    NamedRegister{TpidrroEl0, "TPIDRRO_EL0"},
    NamedRegister{CntfrqEl0, "CNTFRQ_EL0"},
    NamedRegister{CntpctEl0, "CNTPCT_EL0"},
    NamedRegister{0x5F02, "CNTVCT_EL0"},
    NamedRegister{0x5F05, "CNTPCTSS_EL0"},
    NamedRegister{0x5F06, "CNTVCTSS_EL0"},
    NamedRegister{0x5A10, "NZCV"},
    NamedRegister{0x5A11, "DAIF"},
    NamedRegister{0x5A20, "FPCR"},
    NamedRegister{0x5A21, "FPSR"},
    NamedRegister{0x5801, "CTR_EL0"},
    NamedRegister{0x5807, "DCZID_EL0"},
    NamedRegister{0x5920, "RNDR"},
    NamedRegister{0x5921, "RNDRRS"},
    NamedRegister{0x5E85, "TPIDR2_EL0"},
    NamedRegister{0x4000, "MIDR_EL1"},
    NamedRegister{0x4005, "MPIDR_EL1"},
    NamedRegister{0x4020, "ID_AA64PFR0_EL1"},
    NamedRegister{0x4021, "ID_AA64PFR1_EL1"},
    NamedRegister{0x4028, "ID_AA64DFR0_EL1"},
    NamedRegister{0x4030, "ID_AA64ISAR0_EL1"},
    NamedRegister{0x4031, "ID_AA64ISAR1_EL1"},
    NamedRegister{0x4038, "ID_AA64MMFR0_EL1"},
};

void AppendRegisters(std::string& out, std::string_view kind, const std::map<u32, u64>& registers) {
    if (registers.empty()) {
        return;
    }
    std::vector<std::pair<u32, u64>> sorted(registers.begin(), registers.end());
    std::ranges::stable_sort(sorted, [](const auto& a, const auto& b) { return a.second > b.second; });
    out += fmt::format("; {}:", kind);
    for (const auto& [encoding, count] : sorted) {
        out += fmt::format(" {} x{}", SystemRegisterName(encoding), count);
    }
}

} // Anonymous namespace

Readiness ScanText(std::span<const u32> words) {
    Readiness readiness;
    readiness.instructions = words.size();
    for (const u32 word : words) {
        if (SVC{word}.Verify()) {
            ++readiness.svc;
        } else if (auto mrs = MRS{word}; mrs.Verify()) {
            const u32 reg = mrs.GetSystemReg();
            if (reg == TpidrEl0 || reg == TpidrroEl0) {
                ++readiness.thread_pointer;
            } else if (reg == CntpctEl0 || reg == CntfrqEl0) {
                ++readiness.counter;
            } else {
                ++readiness.native_reads[reg];
            }
        } else if (auto msr = MSR{word}; msr.Verify()) {
            const u32 reg = msr.GetSystemReg();
            if (reg == TpidrEl0) {
                ++readiness.thread_pointer;
            } else {
                ++readiness.native_writes[reg];
            }
        } else if (Exclusive{word}.Verify()) {
            ++readiness.exclusive;
        }
    }
    return readiness;
}

std::string SystemRegisterName(u32 encoding) {
    const auto named = std::ranges::find(NAMED_REGISTERS, encoding, &NamedRegister::encoding);
    if (named != NAMED_REGISTERS.end()) {
        return std::string{named->name};
    }
    const u32 op0 = 2 + ((encoding >> 14) & 1);
    const u32 op1 = (encoding >> 11) & 7;
    const u32 crn = (encoding >> 7) & 15;
    const u32 crm = (encoding >> 3) & 15;
    const u32 op2 = encoding & 7;
    return fmt::format("S{}_{}_C{}_C{}_{}", op0, op1, crn, crm, op2);
}

std::string FormatReadiness(const Readiness& readiness, std::string_view module) {
    std::string out = fmt::format(
        "NCE report for {}: {} instructions; patched: {} SVC, {} thread pointer, {} counter; {} "
        "exclusive (made ordered)",
        module, readiness.instructions, readiness.svc, readiness.thread_pointer, readiness.counter,
        readiness.exclusive);
    if (readiness.native_reads.empty() && readiness.native_writes.empty()) {
        out += "; no other system register access";
    }
    AppendRegisters(out, "run natively, reads", readiness.native_reads);
    AppendRegisters(out, "run natively, writes", readiness.native_writes);
    return out;
}

bool ReadinessReportEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_NCE_REPORT");
        return value != nullptr && *value != '\0' && std::string_view{value} != "0";
    }();
    return enabled;
}

} // namespace Core::NCE
