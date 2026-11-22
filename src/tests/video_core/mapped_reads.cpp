// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/buffer_cache/buffer_base.h"
#include "video_core/nx2_mapped_reads.h"

namespace {

using VideoCommon::Nx2MappedReads::ActiveFor;
using VideoCommon::Nx2MappedReads::ChooseHostRead;
using VideoCommon::Nx2MappedReads::HostReadWait;
using VideoCommon::Nx2MappedReads::RequestedFrom;

} // namespace

TEST_CASE("Mapped reads stay off unless the switch is a non-zero number",
          "[video_core][mapped_reads]") {
    CHECK_FALSE(RequestedFrom(nullptr));
    CHECK_FALSE(RequestedFrom(""));
    CHECK_FALSE(RequestedFrom("0"));
    CHECK_FALSE(RequestedFrom("00"));
    CHECK_FALSE(RequestedFrom("false"));
    CHECK(RequestedFrom("1"));
    CHECK(RequestedFrom("2"));
    CHECK(RequestedFrom("0x1"));
}

TEST_CASE("Mapped reads run only on an integrated GPU that is not MoltenVK",
          "[video_core][mapped_reads]") {
    CHECK(ActiveFor(true, true, false));
    CHECK_FALSE(ActiveFor(false, true, false));
    CHECK_FALSE(ActiveFor(true, false, false));
    CHECK_FALSE(ActiveFor(true, true, true));
    CHECK_FALSE(ActiveFor(true, false, true));
}

TEST_CASE("A mapped read waits for the writing tick only when submissions barrier for the host",
          "[video_core][mapped_reads]") {
    CHECK(ChooseHostRead(false, 0) == HostReadWait::FinishAll);
    CHECK(ChooseHostRead(false, 5) == HostReadWait::FinishAll);
    CHECK(ChooseHostRead(true, 0) == HostReadWait::FinishAll);
    CHECK(ChooseHostRead(true, 1) == HostReadWait::WaitForTick);
    CHECK(ChooseHostRead(true, 5) == HostReadWait::WaitForTick);
}

TEST_CASE("The GPU write tick is not the accurate-mode write tick", "[video_core][mapped_reads]") {
    VideoCommon::BufferBase buffer{VideoCommon::NullBufferParams{}};
    CHECK(buffer.getWriteTick() == 0);
    CHECK(buffer.GpuWriteTick() == 0);

    buffer.setWriteTick(4);
    CHECK(buffer.getWriteTick() == 4);
    CHECK(buffer.GpuWriteTick() == 0);

    buffer.SetGpuWriteTick(9);
    CHECK(buffer.getWriteTick() == 4);
    CHECK(buffer.GpuWriteTick() == 9);

    buffer.setWriteTick(6);
    CHECK(buffer.GpuWriteTick() == 9);
}
