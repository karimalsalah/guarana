// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

#include "common/common_types.h"

namespace Common {

/// One logical processor as the host reports it.
struct LogicalProcessor {
    /// What the host's affinity call takes (a CPU set id on Windows).
    u32 id;
    /// The physical core: equal for SMT siblings.
    u32 core;
    /// Higher is faster. Equal on every processor of a CPU that is not hybrid.
    u8 efficiency_class;
    /// The last level cache the processor sits behind. Low-power efficiency cores on an island of
    /// their own (Meteor Lake, Arrow Lake) report an index no performance core has.
    u32 last_level_cache = 0;
};

/// Physical cores of the fastest class needed before the GPU threads are pinned.
constexpr std::size_t MinCoresForGpuThreadPinning = 6;

/// Logical processor ids for each pinned role. All three are empty when the host has fewer than
/// MinCoresForGpuThreadPinning performance cores.
struct GpuThreadPinning {
    std::size_t performance_cores{};
    std::vector<u32> front;
    std::vector<u32> back;
    std::vector<u32> guest;
};

/// The split GPU thread's front and back each get a whole physical performance core, so no SMT
/// sibling shares it, and the guest cores get the remaining performance cores. Core 0 takes most
/// interrupts, so the GPU threads get the next two.
[[nodiscard]] GpuThreadPinning PlanGpuThreadPinning(std::span<const LogicalProcessor> processors);

/// Physical performance cores needed before EDEN_NX2_HYBRID places threads. With fewer (2P+8E
/// parts such as the i5-1235U) the GPU front, back and Vulkan worker alone would fill the
/// performance cores, so the CPU is left to Windows.
constexpr std::size_t MinHybridPerformanceCores = 4;

/// Physical performance cores from which the guest cores and every latency thread keep to the
/// performance cores (the six-P layout): the six guest cores, the GPU front and back and the
/// helpers crowd fewer. Below it the four-P layout keeps only the GPU front, back and Vulkan
/// worker there.
constexpr std::size_t SixPHybridPerformanceCores = 6;

/// How a hybrid CPU's processors are shared out (EDEN_NX2_HYBRID). Every list holds host
/// processor ids and is empty when `active` is false.
struct HybridPlan {
    /// False on CPUs that are not hybrid and on those with fewer than MinHybridPerformanceCores
    /// physical performance cores.
    bool active = false;
    /// The four-P layout: fewer than SixPHybridPerformanceCores physical performance cores.
    bool four_p = false;
    std::size_t performance_cores = 0;
    /// Every processor of the fastest class.
    std::vector<u32> performance;
    /// Latency-critical threads other than the GPU front, back and Vulkan worker (guest cores,
    /// present, fencing, timing, vsync, snapshot). Six-P: the performance processors, less the two
    /// cores of the GPU threads when `pin_gpu_threads` is set. Four-P: the performance and
    /// efficiency processors.
    std::vector<u32> latency;
    /// The threads each frame waits on in turn: GPU front, GPU back and Vulkan worker. Six-P: as
    /// `latency` (PinCurrentThread then narrows the front and back). Four-P: the performance
    /// processors.
    std::vector<u32> render;
    std::vector<u32> front;
    std::vector<u32> back;
    /// Efficiency cores behind the performance cores' last level cache: background pools. Four-P
    /// with none there (Lunar Lake): the island's cores, the CPU's only efficiency cores.
    std::vector<u32> efficiency;
    /// Low-power efficiency cores: any non-performance core behind a last level cache of its own,
    /// or the slowest class when there are three. No latency-critical thread runs here.
    std::vector<u32> low_power;
    /// Performance and efficiency processors: the default of every thread without a role.
    std::vector<u32> responsive;
};

[[nodiscard]] HybridPlan PlanHybridCores(std::span<const LogicalProcessor> processors,
                                         bool pin_gpu_threads);

/// The CPUs of a Linux sysfs CPU list such as "0-3,8,10-11", in order. Empty for an empty or
/// malformed list.
[[nodiscard]] std::vector<u32> ParseCpuList(std::string_view list);

} // namespace Common
