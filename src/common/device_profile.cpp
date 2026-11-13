// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <bit>

#include <windows.h>
#include <cfgmgr32.h>
#elif defined(__linux__)
#include <fstream>
#include <map>
#include <optional>

#include "common/core_pinning.h"
#endif

#include <fmt/format.h>

#include "common/device_power_state.h"
#include "common/device_profile.h"
#include "common/memory_detect.h"

namespace Common {
namespace {

/// Rounded to whole GiB, the unit RAM and video memory are sold in ("32 GB").
u64 RoundedGiB(u64 bytes) {
    return (bytes + (u64{1} << 29)) >> 30;
}

std::string HybridCores(const CpuProfile& cpu) {
    std::string text = fmt::format("{}P", cpu.performance);
    if (cpu.efficiency != 0) {
        text += fmt::format("+{}E", cpu.efficiency);
    }
    if (cpu.low_power != 0) {
        text += fmt::format("+{}LPE", cpu.low_power);
    }
    return text;
}

std::string_view GpuKindName(GpuKind kind) {
    switch (kind) {
    case GpuKind::Integrated:
        return "integrated";
    case GpuKind::Discrete:
        return "discrete";
    case GpuKind::Virtual:
        return "virtual";
    case GpuKind::Cpu:
        return "cpu";
    case GpuKind::Unknown:
        break;
    }
    return "unknown";
}

std::string_view PowerSourceName(PowerSource power) {
    switch (power) {
    case PowerSource::Ac:
        return "ac";
    case PowerSource::Battery:
        return "battery";
    case PowerSource::Unknown:
        break;
    }
    return "unknown";
}

std::string_view NpuName(NpuPresence npu) {
    switch (npu) {
    case NpuPresence::Present:
        return "yes";
    case NpuPresence::Absent:
        return "no";
    case NpuPresence::Unknown:
        break;
    }
    return "unknown";
}

DeviceClass ClassOf(const DeviceProfile& profile) {
    switch (profile.gpu.kind) {
    case GpuKind::Integrated: {
        const bool little_ram = profile.ram != 0 && profile.ram < LowPowerRamLimit;
        return little_ram || profile.power == PowerSource::Battery
                   ? DeviceClass::IntegratedLowPower
                   : DeviceClass::Integrated;
    }
    case GpuKind::Discrete:
        if (profile.gpu.local_memory == 0) {
            return DeviceClass::Unknown;
        }
        return profile.gpu.local_memory >= DiscreteHighLocalMemory ? DeviceClass::DiscreteHigh
                                                                   : DeviceClass::DiscreteEntry;
    case GpuKind::Virtual:
    case GpuKind::Cpu:
    case GpuKind::Unknown:
        break;
    }
    return DeviceClass::Unknown;
}

std::string Label(const DeviceProfile& profile) {
    std::string label = ShortGpuName(profile.gpu.model);
    if (label.empty()) {
        label = "unknown GPU";
    }
    const auto add = [&label](std::string_view part) {
        label += ", ";
        label += part;
    };
    switch (profile.gpu.kind) {
    case GpuKind::Integrated:
        add("integrated");
        break;
    case GpuKind::Discrete:
        add(profile.gpu.local_memory != 0
                ? fmt::format("discrete {} GB", RoundedGiB(profile.gpu.local_memory))
                : std::string{"discrete"});
        break;
    case GpuKind::Virtual:
        add("virtual");
        break;
    case GpuKind::Cpu:
        add("software");
        break;
    case GpuKind::Unknown:
        add("unknown kind");
        break;
    }
    if (profile.ram != 0) {
        add(fmt::format("{} GB", RoundedGiB(profile.ram)));
    }
    if (profile.cpu.Hybrid()) {
        add("hybrid " + HybridCores(profile.cpu));
    } else if (profile.cpu.physical != 0) {
        add(fmt::format("{} cores", profile.cpu.physical));
    }
    if (profile.power == PowerSource::Battery) {
        add("on battery");
    }
    if (profile.npu == NpuPresence::Present) {
        add("NPU");
    }
    return label;
}

#ifdef _WIN32
// One entry per physical core, with its efficiency class (Windows 10 and later).
std::vector<CpuCore> HostCores() {
    DWORD length = 0;
    if (GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER || length == 0) {
        return {};
    }
    std::vector<u8> buffer(length);
    if (!GetLogicalProcessorInformationEx(
            RelationProcessorCore,
            reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()), &length)) {
        return {};
    }
    // Entries differ in size: each starts with its relationship and its size.
    constexpr DWORD header = sizeof(LOGICAL_PROCESSOR_RELATIONSHIP) + sizeof(DWORD);
    std::vector<CpuCore> cores;
    for (DWORD offset = 0; length - offset >= header;) {
        const auto* const info = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(
            buffer.data() + offset);
        if (info->Size < header || info->Size > length - offset) {
            break;
        }
        if (info->Relationship == RelationProcessorCore) {
            const PROCESSOR_RELATIONSHIP& core = info->Processor;
            u32 logical = 0;
            for (WORD group = 0; group < core.GroupCount; ++group) {
                logical += static_cast<u32>(
                    std::popcount(static_cast<u64>(core.GroupMask[group].Mask)));
            }
            cores.push_back({.logical = logical, .efficiency_class = core.EfficiencyClass});
        }
        offset += info->Size;
    }
    return cores;
}
#elif defined(__linux__)
std::optional<u32> ReadNumber(const std::string& path) {
    std::ifstream file{path};
    u32 value{};
    if (file >> value) {
        return value;
    }
    return std::nullopt;
}

std::vector<u32> ReadCpus(const std::string& path) {
    std::ifstream file{path};
    std::string list;
    if (!std::getline(file, list)) {
        return {};
    }
    return ParseCpuList(list);
}

// One entry per physical core: the online CPUs grouped by the core they share.
std::vector<CpuCore> HostCores() {
    const std::string root = "/sys/devices/system/cpu/";
    // Intel's hybrid CPUs list their efficiency cores, low-power ones included, as the cpu_atom
    // PMU's. Elsewhere cpu_capacity ranks the cores where the kernel has it (1024 is the fastest).
    const std::vector<u32> atom = ReadCpus("/sys/devices/cpu_atom/cpus");
    std::map<u32, CpuCore> cores; // by the core's first CPU
    for (const u32 cpu : ReadCpus(root + "online")) {
        const std::string dir = fmt::format("{}cpu{}/", root, cpu);
        std::vector<u32> siblings = ReadCpus(dir + "topology/core_cpus_list");
        if (siblings.empty()) {
            siblings = ReadCpus(dir + "topology/thread_siblings_list");
        }
        if (siblings.empty()) {
            return {};
        }
        const auto [slot, added] = cores.try_emplace(siblings.front());
        if (!added) {
            ++slot->second.logical;
            continue;
        }
        if (!atom.empty()) {
            const bool efficiency = std::ranges::find(atom, cpu) != atom.end();
            slot->second.efficiency_class = efficiency ? u8{0} : u8{1};
        } else {
            const u32 capacity =
                (std::min)(ReadNumber(dir + "cpu_capacity").value_or(1024U), 1024U);
            slot->second.efficiency_class = static_cast<u8>(capacity >> 3);
        }
    }
    std::vector<CpuCore> result;
    result.reserve(cores.size());
    for (const auto& item : cores) {
        result.push_back(item.second);
    }
    return result;
}
#endif

CpuProfile HostCpu() {
#if defined(_WIN32) || defined(__linux__)
    CpuProfile cpu = CountCores(HostCores());
#else
    CpuProfile cpu;
#endif
    if (cpu.logical == 0) {
        cpu = {};
        cpu.logical = std::thread::hardware_concurrency();
    }
    return cpu;
}

NpuPresence HostNpu() {
#ifdef _WIN32
    // Present devices of the "Neural processors" setup class (ComputeAccelerator,
    // GUID_DEVCLASS_COMPUTEACCELERATOR in recent SDKs' devguid.h), which NPU drivers such as
    // Intel's AI Boost install under. An NPU whose driver is not installed is not in it.
    static constexpr wchar_t ComputeAcceleratorClass[] = L"{f01a9d53-3ff6-48d2-9f97-c8a7004be10c}";
    constexpr ULONG flags = CM_GETIDLIST_FILTER_CLASS | CM_GETIDLIST_FILTER_PRESENT;
    ULONG size = 0;
    if (CM_Get_Device_ID_List_SizeW(&size, ComputeAcceleratorClass, flags) != CR_SUCCESS) {
        return NpuPresence::Unknown;
    }
    if (size <= 1) {
        return NpuPresence::Absent;
    }
    std::vector<wchar_t> ids(size);
    if (CM_Get_Device_ID_ListW(ComputeAcceleratorClass, ids.data(), size, flags) != CR_SUCCESS) {
        return NpuPresence::Unknown;
    }
    return ids.front() != L'\0' ? NpuPresence::Present : NpuPresence::Absent;
#else
    return NpuPresence::Unknown;
#endif
}

struct HostFacts {
    CpuProfile cpu;
    HostRam ram;
    NpuPresence npu{NpuPresence::Unknown};
};

const HostFacts& Facts() {
    static const HostFacts facts{
        .cpu = HostCpu(),
        .ram = GetHostRam(),
        .npu = HostNpu(),
    };
    return facts;
}

} // Anonymous namespace

CpuProfile CountCores(std::span<const CpuCore> cores) {
    CpuProfile cpu;
    if (cores.empty()) {
        return cpu;
    }
    u8 fastest = 0;
    u8 slowest = 255;
    for (const CpuCore& core : cores) {
        cpu.logical += core.logical;
        fastest = (std::max)(fastest, core.efficiency_class);
        slowest = (std::min)(slowest, core.efficiency_class);
    }
    cpu.physical = static_cast<u32>(cores.size());
    if (fastest == slowest) {
        return cpu;
    }
    const bool three_classes = std::ranges::any_of(cores, [&](const CpuCore& core) {
        return core.efficiency_class != fastest && core.efficiency_class != slowest;
    });
    for (const CpuCore& core : cores) {
        if (core.efficiency_class == fastest) {
            ++cpu.performance;
        } else if (three_classes && core.efficiency_class == slowest) {
            ++cpu.low_power;
        } else {
            ++cpu.efficiency;
        }
    }
    return cpu;
}

bool IsUnifiedMemory(GpuKind kind, u32 heaps, u32 device_local_heaps) {
    return kind == GpuKind::Integrated || kind == GpuKind::Cpu ||
           (heaps != 0 && device_local_heaps == heaps);
}

DeviceClassification Classify(const DeviceProfile& profile) {
    return {.device_class = ClassOf(profile), .label = Label(profile)};
}

std::string_view DeviceClassName(DeviceClass device_class) {
    switch (device_class) {
    case DeviceClass::IntegratedLowPower:
        return "IntegratedLowPower";
    case DeviceClass::Integrated:
        return "Integrated";
    case DeviceClass::DiscreteEntry:
        return "DiscreteEntry";
    case DeviceClass::DiscreteHigh:
        return "DiscreteHigh";
    case DeviceClass::Unknown:
        break;
    }
    return "Unknown";
}

std::string ShortGpuName(std::string_view model) {
    // Parenthesized parts ("(R)", "(TM)", "(16GB)", "(RADV NAVI33)") and the UTF-8 registered and
    // trademark signs go; each parenthesized part leaves a space, collapsed below.
    std::string text;
    for (size_t i = 0; i < model.size();) {
        const std::string_view rest = model.substr(i);
        if (rest.front() == '(') {
            const size_t close = rest.find(')');
            if (close == std::string_view::npos) {
                break;
            }
            text += ' ';
            i += close + 1;
        } else if (rest.starts_with("\xC2\xAE")) {
            i += 2;
        } else if (rest.starts_with("\xE2\x84\xA2")) {
            i += 3;
        } else {
            text += rest.front();
            ++i;
        }
    }
    std::string name;
    for (const char character : text) {
        if (character != ' ' || (!name.empty() && name.back() != ' ')) {
            name += character;
        }
    }
    while (!name.empty() && name.back() == ' ') {
        name.pop_back();
    }
    // "Intel Arc Graphics" is "Intel Arc", but "Intel Graphics" keeps its second word.
    constexpr std::array<std::string_view, 2> suffixes{" GPU", " Graphics"};
    for (const std::string_view suffix : suffixes) {
        if (name.ends_with(suffix) && name.find(' ') < name.size() - suffix.size()) {
            name.resize(name.size() - suffix.size());
            break;
        }
    }
    return name;
}

std::string GpuVendorName(u32 vendor_id) {
    switch (vendor_id) {
    case 0:
        return "unknown";
    case 0x8086:
        return "Intel";
    case 0x1002:
    case 0x1022:
        return "AMD";
    case 0x10DE:
        return "NVIDIA";
    case 0x5143:
        return "Qualcomm";
    case 0x13B5:
        return "ARM";
    case 0x106B:
        return "Apple";
    case 0x1010:
        return "Imagination";
    case 0x1414:
        return "Microsoft";
    case 0x10005:
        return "Mesa";
    default:
        return fmt::format("{:#06x}", vendor_id);
    }
}

std::string DescribeDeviceProfile(const DeviceProfile& profile,
                                  const DeviceClassification& classification) {
    const GpuProfile& gpu = profile.gpu;
    const CpuProfile& cpu = profile.cpu;
    return fmt::format(
        "class={} label=\"{}\" gpu_vendor={} gpu_model=\"{}\" gpu_type={} gpu_driver={} "
        "gpu_local_mib={} gpu_uma={} cpu_logical={} cpu_physical={} cpu_hybrid={} ram_mib={} "
        "ram_source={} power={} display_hz={} npu={}",
        DeviceClassName(classification.device_class), classification.label,
        GpuVendorName(gpu.vendor_id), gpu.model, GpuKindName(gpu.kind),
        gpu.driver.empty() ? std::string_view{"unknown"} : std::string_view{gpu.driver},
        gpu.local_memory >> 20, gpu.uma ? "yes" : "no", cpu.logical, cpu.physical,
        cpu.Hybrid() ? HybridCores(cpu) : std::string{"no"}, profile.ram >> 20,
        profile.ram == 0 ? "unknown" : (profile.ram_installed ? "installed" : "os"),
        PowerSourceName(profile.power),
        profile.refresh_hz > 0.0 ? fmt::format("{:.2f}", profile.refresh_hz)
                                 : std::string{"unknown"},
        NpuName(profile.npu));
}

DeviceProfile DetectHostProfile() {
    const HostFacts& facts = Facts();
    DeviceProfile profile;
    profile.cpu = facts.cpu;
    profile.ram = facts.ram.bytes;
    profile.ram_installed = facts.ram.installed;
    profile.npu = facts.npu;
    // ac_online is set where the host says which source it runs on (Windows).
    const int ac_online = GetPowerStatus().ac_online;
    profile.power = ac_online == 1   ? PowerSource::Ac
                    : ac_online == 0 ? PowerSource::Battery
                                     : PowerSource::Unknown;
    return profile;
}

} // namespace Common
