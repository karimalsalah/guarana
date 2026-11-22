// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>

#include <catch2/catch_test_macros.hpp>

#include "core/arm/nce/readiness.h"

namespace {

// One instruction of each kind the patcher cares about, and two it leaves alone.
constexpr std::array<u32, 9> CODE{
    0xD40000C1, // svc #0x6
    0xD53BD060, // mrs x0, tpidrro_el0
    0xD51BD040, // msr tpidr_el0, x0
    0xD53BE020, // mrs x0, cntpct_el0
    0xD53BE041, // mrs x1, cntvct_el0 (not patched: the host's virtual counter)
    0xD53B4400, // mrs x0, fpcr
    0xD51B4401, // msr fpcr, x1
    0x885F7C20, // ldxr w0, [x1]
    0x8B020020, // add x0, x1, x2
};

} // Anonymous namespace

TEST_CASE("NCE readiness counts what the patcher rewrites and what runs natively",
          "[nce_readiness]") {
    const Core::NCE::Readiness readiness = Core::NCE::ScanText(CODE);
    CHECK(readiness.instructions == CODE.size());
    CHECK(readiness.svc == 1);
    CHECK(readiness.thread_pointer == 2);
    CHECK(readiness.counter == 1);
    CHECK(readiness.exclusive == 1);
    REQUIRE(readiness.native_reads.size() == 2);
    CHECK(readiness.native_reads.at(0x5F02) == 1);
    CHECK(readiness.native_reads.at(0x5A20) == 1);
    REQUIRE(readiness.native_writes.size() == 1);
    CHECK(readiness.native_writes.at(0x5A20) == 1);
}

TEST_CASE("NCE readiness names the registers it reports", "[nce_readiness]") {
    CHECK(Core::NCE::SystemRegisterName(0x5F02) == "CNTVCT_EL0");
    CHECK(Core::NCE::SystemRegisterName(0x5A20) == "FPCR");
    CHECK(Core::NCE::SystemRegisterName(0x5E82) == "TPIDR_EL0");
    // op0 3, op1 0, CRn 0, CRm 0, op2 6: REVIDR_EL1, which the table leaves unnamed.
    CHECK(Core::NCE::SystemRegisterName(0x4006) == "S3_0_C0_C0_6");
    // op0 2: the debug registers.
    CHECK(Core::NCE::SystemRegisterName(0x0012) == "S2_0_C0_C2_2");
}

TEST_CASE("NCE readiness formats one line per module", "[nce_readiness]") {
    const std::string line =
        Core::NCE::FormatReadiness(Core::NCE::ScanText(CODE), "main");
    CHECK(line == "NCE report for main: 9 instructions; patched: 1 SVC, 2 thread pointer, 1 "
                  "counter; 1 exclusive (made ordered); run natively, reads: FPCR x1 CNTVCT_EL0 "
                  "x1; run natively, writes: FPCR x1");
    const std::array<u32, 1> plain{0x8B020020};
    CHECK(Core::NCE::FormatReadiness(Core::NCE::ScanText(plain), "sdk") ==
          "NCE report for sdk: 1 instructions; patched: 0 SVC, 0 thread pointer, 0 counter; 0 "
          "exclusive (made ordered); no other system register access");
}
