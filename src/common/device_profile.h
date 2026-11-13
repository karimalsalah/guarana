// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <span>
#include <string>
#include <string_view>

#include "common/common_types.h"

/// What the user's machine is: the GPU, CPU, RAM, power source, display and NPU, and the device
/// class a future per-class default would key on. The renderer logs it once at boot as one
/// `Device profile:` line (docs/DEVICES.md). Nothing reads the class to change a default yet.
namespace Common {

enum class DeviceClass : u8 {
    Unknown,
    IntegratedLowPower,
    Integrated,
    DiscreteEntry,
    DiscreteHigh,
};

enum class GpuKind : u8 {
    Unknown,
    Integrated,
    Discrete,
    Virtual,
    Cpu, ///< A software renderer
};

enum class PowerSource : u8 {
    Unknown,
    Ac,
    Battery,
};

enum class NpuPresence : u8 {
    Unknown,
    Absent,
    Present,
};

struct GpuProfile {
    u32 vendor_id{};    ///< PCI vendor ID; 0 when unknown.
    std::string model;  ///< The device name the driver reports.
    std::string driver; ///< The driver version as the renderer logs it; empty when unknown.
    GpuKind kind{GpuKind::Unknown};
    u64 local_memory{}; ///< Bytes in the largest device-local heap.
    bool uma{};         ///< Device memory is the host's RAM (IsUnifiedMemory).
};

struct CpuProfile {
    u32 logical{};  ///< Logical processors; 0 when unknown.
    u32 physical{}; ///< Cores; 0 when unknown.
    /// Cores of each kind on a hybrid CPU, all 0 on others: performance (P), efficiency (E) and
    /// low-power efficiency (LPE) cores.
    u32 performance{};
    u32 efficiency{};
    u32 low_power{};

    [[nodiscard]] bool Hybrid() const {
        return performance != 0 && efficiency + low_power != 0;
    }
};

struct DeviceProfile {
    GpuProfile gpu;
    CpuProfile cpu;
    u64 ram{};            ///< Bytes of RAM; 0 when unknown.
    bool ram_installed{}; ///< ram is the amount fitted (else the memory the OS manages).
    PowerSource power{PowerSource::Unknown};
    double refresh_hz{}; ///< The refresh rate of the window's display; 0 when unknown.
    NpuPresence npu{NpuPresence::Unknown};
};

/// An integrated GPU with less RAM than this is IntegratedLowPower.
inline constexpr u64 LowPowerRamLimit = u64{10} << 30;

/// A discrete GPU with at least this much device-local memory is DiscreteHigh: 8 GB and more.
/// Drivers keep some of a card's memory back, so an 8 GB card can list a little under 8 GiB
/// (an RX 6600 XT lists 8176 MiB): the limit is 7.5 GiB.
inline constexpr u64 DiscreteHighLocalMemory = u64{7680} << 20;

/// One physical core as the host reports it.
struct CpuCore {
    u32 logical{1}; ///< Its logical processors (SMT threads).
    /// Higher is faster; the same on every core of a CPU that is not hybrid.
    u8 efficiency_class{};
};

/// Counts the cores. A CPU whose cores report more than one efficiency class is hybrid: the
/// fastest class are its performance cores. With two classes the others are efficiency cores;
/// with three or more the slowest class are the low-power efficiency cores and the classes
/// between are efficiency cores (a Core Ultra 9 285H reports 6 cores of class 2, 8 of class 1 and
/// 2 of class 0 on Windows: 6P+8E+2LPE).
[[nodiscard]] CpuProfile CountCores(std::span<const CpuCore> cores);

/// Whether the GPU's device memory is the host's RAM: an integrated GPU or a software renderer,
/// or a driver that lists every heap as device-local (a UMA driver must list its one shared heap
/// as device-local).
[[nodiscard]] bool IsUnifiedMemory(GpuKind kind, u32 heaps, u32 device_local_heaps);

struct DeviceClassification {
    DeviceClass device_class{DeviceClass::Unknown};
    /// A few words for people, such as "Intel Arc 140T, integrated, 32 GB, hybrid 6P+8E+2LPE,
    /// NPU": the GPU, its kind (with its memory when discrete), the RAM, the cores, "on battery"
    /// on battery and "NPU" when there is one.
    std::string label;
};

/// The device class of a profile, with its label:
/// - IntegratedLowPower: an integrated GPU with under LowPowerRamLimit of RAM, or on battery.
/// - Integrated: any other integrated GPU.
/// - DiscreteHigh: a discrete GPU with DiscreteHighLocalMemory or more, on AC or on battery.
/// - DiscreteEntry: a discrete GPU with less.
/// - Unknown: a virtual GPU, a software renderer, a GPU of unknown kind, or a discrete GPU that
///   lists no device-local memory.
/// Unknown RAM or an unknown power source never makes a class low power.
[[nodiscard]] DeviceClassification Classify(const DeviceProfile& profile);

[[nodiscard]] std::string_view DeviceClassName(DeviceClass device_class);

/// The device name without trademark signs, parenthesized suffixes and a trailing " GPU" or
/// " Graphics": "Intel(R) Arc(TM) 140T GPU (16GB)" is "Intel Arc 140T".
[[nodiscard]] std::string ShortGpuName(std::string_view model);

/// "Intel", "AMD", "NVIDIA" and the other GPU vendors by PCI vendor ID; the ID in hex otherwise.
[[nodiscard]] std::string GpuVendorName(u32 vendor_id);

/// The text of the `Device profile:` log line: `class=... label="..."` and one `key=value` per
/// field (docs/DEVICES.md).
[[nodiscard]] std::string DescribeDeviceProfile(const DeviceProfile& profile,
                                                const DeviceClassification& classification);

/// The host's part of the profile: CPU, RAM, power source and NPU. The GPU and the display are
/// left for the renderer to fill in. The CPU, RAM and NPU are read once per process, the power
/// source on every call.
[[nodiscard]] DeviceProfile DetectHostProfile();

} // namespace Common
