// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdlib>

#include "common/common_types.h"

/// EDEN_NX2_UMA=<bitmask> (experiment, 0 or unset changes nothing): buffer uploads and
/// submissions for integrated GPUs, where device memory is system memory.
namespace VideoCommon::Nx2Uma {

/// Buffer copies share commands: reordered uploads are recorded once per submission, one
/// vkCmdCopyBuffer per source and destination pair, and every in-order buffer copy joins the
/// upload batch (Scheduler::RecordUpload) instead of taking its own barriers.
inline constexpr u32 Coalesce = 1;
/// Integrated GPUs: the buffer cache's buffers are host-mapped, and an upload whose destination
/// no pending GPU work uses is written straight into them (BufferCacheRuntime::DirectUploadTarget).
inline constexpr u32 DirectUploads = 2;
/// Reserved for texture uploads through VK_EXT_host_image_copy: not implemented.
inline constexpr u32 HostImageCopy = 4;
/// The draw budget submits only once the host GPU ran out of submitted work, up to 512 draws
/// (RasterizerVulkan::DeferBudgetFlush).
inline constexpr u32 AdaptiveSubmit = 8;
/// A buffer's used ranges are forgotten a generation at a time instead of only when the GPU is
/// past its last use (Buffer::AgeUsageTracking): more uploads are reordered or written directly.
inline constexpr u32 UsageGenerations = 16;

[[nodiscard]] inline u32 SwitchValue() {
    const char* const value = std::getenv("EDEN_NX2_UMA");
    return value != nullptr ? static_cast<u32>(std::strtoul(value, nullptr, 0)) : 0;
}

inline u32 g_flags = SwitchValue();

/// Called by VideoCore::CreateGPU: static initialization runs before the frontend loads
/// nx2_switches.txt, so a switch set only there takes effect from that point on.
inline void ReadSwitch() {
    g_flags = SwitchValue();
}

[[nodiscard]] inline bool Has(u32 bit) {
    return (g_flags & bit) != 0;
}

} // namespace VideoCommon::Nx2Uma
