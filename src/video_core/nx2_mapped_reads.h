// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdlib>

#include "common/common_types.h"

/// EDEN_NX2_MAPPED_READS=1 (experiment, unset or 0 changes nothing).
///
/// Port of zortos293/Lichii#1903. On an integrated GPU other than MoltenVK the buffer cache is
/// allocated host-visible, every submission ends with a host-read barrier, and a download of a
/// mapped buffer waits for the submission that last wrote it. `write_tick` is a different field
/// and stays as it was.
namespace VideoCommon::Nx2MappedReads {

enum class HostReadWait : u8 {
    FinishAll,   ///< Submissions carry no host-read barrier, or this buffer has no GPU write yet.
    WaitForTick, ///< The writing submission's barrier has made its bytes host-visible.
};

[[nodiscard]] inline bool RequestedFrom(const char* value) noexcept {
    return value != nullptr && std::strtoul(value, nullptr, 0) != 0;
}

[[nodiscard]] inline bool ActiveFor(bool requested, bool integrated, bool molten_vk) noexcept {
    return requested && integrated && !molten_vk;
}

/// `host_read_barrier` is Scheduler::HostReadsMappedBuffers: the switch, on this device.
[[nodiscard]] inline HostReadWait ChooseHostRead(bool host_read_barrier,
                                                 u64 gpu_write_tick) noexcept {
    if (!host_read_barrier || gpu_write_tick == 0) {
        return HostReadWait::FinishAll;
    }
    return HostReadWait::WaitForTick;
}

[[nodiscard]] inline bool SwitchOn() {
    return RequestedFrom(std::getenv("EDEN_NX2_MAPPED_READS"));
}

inline bool g_enabled = SwitchOn();

/// Called by VideoCore::CreateGPU. Static initialization runs before the frontend loads
/// nx2_switches.txt, so a switch set only there takes effect from that point on. The scheduler
/// latches Active() in its constructor, which runs after this.
inline void ReadSwitch() {
    g_enabled = SwitchOn();
}

[[nodiscard]] inline bool Enabled() noexcept {
    return g_enabled;
}

[[nodiscard]] inline bool Active(bool integrated, bool molten_vk) noexcept {
    return ActiveFor(g_enabled, integrated, molten_vk);
}

} // namespace VideoCommon::Nx2MappedReads
