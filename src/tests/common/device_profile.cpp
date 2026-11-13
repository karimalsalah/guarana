// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/device_profile.h"
#include "common/literals.h"

namespace {

using namespace Common::Literals;
using Common::CpuCore;
using Common::DeviceClass;
using Common::DeviceProfile;
using Common::GpuKind;
using Common::NpuPresence;
using Common::PowerSource;

void AddCores(std::vector<CpuCore>& cores, u32 count, u32 threads, u8 efficiency_class) {
    for (u32 i = 0; i < count; ++i) {
        cores.push_back({.logical = threads, .efficiency_class = efficiency_class});
    }
}

// A Core Ultra 9 285H laptop with its Arc 140T and 32 GB, on AC.
DeviceProfile Arc140T() {
    DeviceProfile profile;
    profile.gpu = {.vendor_id = 0x8086,
                   .model = "Intel(R) Arc(TM) 140T GPU (16GB)",
                   .driver = "101.9175",
                   .kind = GpuKind::Integrated,
                   .local_memory = 16_GiB,
                   .uma = true};
    profile.cpu = {
        .logical = 16, .physical = 16, .performance = 6, .efficiency = 8, .low_power = 2};
    profile.ram = 32_GiB;
    profile.ram_installed = true;
    profile.power = PowerSource::Ac;
    profile.refresh_hz = 60.0;
    profile.npu = NpuPresence::Present;
    return profile;
}

DeviceProfile Discrete(u64 local_memory) {
    DeviceProfile profile;
    profile.gpu = {.vendor_id = 0x10DE,
                   .model = "NVIDIA GeForce RTX 4070",
                   .kind = GpuKind::Discrete,
                   .local_memory = local_memory};
    profile.cpu = {.logical = 16, .physical = 8};
    profile.ram = 32_GiB;
    profile.ram_installed = true;
    profile.power = PowerSource::Ac;
    return profile;
}

DeviceClass ClassOf(const DeviceProfile& profile) {
    return Common::Classify(profile).device_class;
}

} // namespace

TEST_CASE("Device class: an integrated GPU is low power with little RAM or on battery",
          "[common][device_profile]") {
    DeviceProfile profile = Arc140T();
    CHECK(ClassOf(profile) == DeviceClass::Integrated);
    profile.power = PowerSource::Battery;
    CHECK(ClassOf(profile) == DeviceClass::IntegratedLowPower);
    // An unknown power source is not battery.
    profile.power = PowerSource::Unknown;
    CHECK(ClassOf(profile) == DeviceClass::Integrated);
    profile.ram = 8_GiB;
    CHECK(ClassOf(profile) == DeviceClass::IntegratedLowPower);
    profile.ram = 10_GiB - 1_MiB;
    CHECK(ClassOf(profile) == DeviceClass::IntegratedLowPower);
    profile.ram = 10_GiB;
    CHECK(ClassOf(profile) == DeviceClass::Integrated);
    // Unknown RAM is not little RAM.
    profile.ram = 0;
    CHECK(ClassOf(profile) == DeviceClass::Integrated);
    // A large shared heap does not make an integrated GPU discrete.
    profile.gpu.local_memory = 32_GiB;
    CHECK(ClassOf(profile) == DeviceClass::Integrated);
}

TEST_CASE("Device class: a discrete GPU by its memory, on AC or on battery",
          "[common][device_profile]") {
    CHECK(ClassOf(Discrete(12_GiB)) == DeviceClass::DiscreteHigh);
    // An 8 GB card that lists a little under 8 GiB.
    CHECK(ClassOf(Discrete(8176_MiB)) == DeviceClass::DiscreteHigh);
    CHECK(ClassOf(Discrete(Common::DiscreteHighLocalMemory)) == DeviceClass::DiscreteHigh);
    CHECK(ClassOf(Discrete(Common::DiscreteHighLocalMemory - 1)) == DeviceClass::DiscreteEntry);
    CHECK(ClassOf(Discrete(6_GiB)) == DeviceClass::DiscreteEntry);
    CHECK(ClassOf(Discrete(0)) == DeviceClass::Unknown);

    DeviceProfile laptop = Discrete(8_GiB);
    laptop.power = PowerSource::Battery;
    laptop.ram = 8_GiB;
    CHECK(ClassOf(laptop) == DeviceClass::DiscreteHigh);
    laptop.gpu.local_memory = 4_GiB;
    CHECK(ClassOf(laptop) == DeviceClass::DiscreteEntry);
}

TEST_CASE("Device class: virtual, software and unknown GPUs are Unknown",
          "[common][device_profile]") {
    CHECK(ClassOf({}) == DeviceClass::Unknown);
    for (const GpuKind kind : {GpuKind::Virtual, GpuKind::Cpu, GpuKind::Unknown}) {
        DeviceProfile profile = Discrete(16_GiB);
        profile.gpu.kind = kind;
        CHECK(ClassOf(profile) == DeviceClass::Unknown);
    }
}

TEST_CASE("Device class names", "[common][device_profile]") {
    CHECK(Common::DeviceClassName(DeviceClass::Unknown) == "Unknown");
    CHECK(Common::DeviceClassName(DeviceClass::IntegratedLowPower) == "IntegratedLowPower");
    CHECK(Common::DeviceClassName(DeviceClass::Integrated) == "Integrated");
    CHECK(Common::DeviceClassName(DeviceClass::DiscreteEntry) == "DiscreteEntry");
    CHECK(Common::DeviceClassName(DeviceClass::DiscreteHigh) == "DiscreteHigh");
}

TEST_CASE("Device class label: the machine in a few words", "[common][device_profile]") {
    CHECK(Common::Classify(Arc140T()).label ==
          "Intel Arc 140T, integrated, 32 GB, hybrid 6P+8E+2LPE, NPU");
    CHECK(Common::Classify(Discrete(8176_MiB)).label ==
          "NVIDIA GeForce RTX 4070, discrete 8 GB, 32 GB, 8 cores");

    DeviceProfile handheld = Arc140T();
    handheld.gpu.model = "AMD Radeon(TM) 780M";
    handheld.cpu = {.logical = 16, .physical = 8};
    handheld.ram = 16_GiB;
    handheld.power = PowerSource::Battery;
    handheld.npu = NpuPresence::Absent;
    CHECK(Common::Classify(handheld).label ==
          "AMD Radeon 780M, integrated, 16 GB, 8 cores, on battery");

    CHECK(Common::Classify({}).label == "unknown GPU, unknown kind");
}

TEST_CASE("GPU names lose trademark signs and suffixes", "[common][device_profile]") {
    using Common::ShortGpuName;
    CHECK(ShortGpuName("Intel(R) Arc(TM) 140T GPU (16GB)") == "Intel Arc 140T");
    CHECK(ShortGpuName("Intel(R) Arc(TM) Graphics") == "Intel Arc");
    CHECK(ShortGpuName("Intel(R) Graphics") == "Intel Graphics");
    CHECK(ShortGpuName("AMD Radeon(TM) 780M") == "AMD Radeon 780M");
    CHECK(ShortGpuName("AMD Radeon 780M Graphics") == "AMD Radeon 780M");
    CHECK(ShortGpuName("AMD Radeon\xE2\x84\xA2 890M") == "AMD Radeon 890M");
    CHECK(ShortGpuName("AMD Radeon RX 7600 (RADV NAVI33)") == "AMD Radeon RX 7600");
    CHECK(ShortGpuName("AMD Custom GPU 0405 (RADV VANGOGH)") == "AMD Custom GPU 0405");
    CHECK(ShortGpuName("NVIDIA GeForce RTX 4070 Laptop GPU") == "NVIDIA GeForce RTX 4070 Laptop");
    CHECK(ShortGpuName("llvmpipe (LLVM 17.0.6, 256 bits)") == "llvmpipe");
    CHECK(ShortGpuName("").empty());
}

TEST_CASE("GPU vendors by PCI vendor ID", "[common][device_profile]") {
    CHECK(Common::GpuVendorName(0x8086) == "Intel");
    CHECK(Common::GpuVendorName(0x1002) == "AMD");
    CHECK(Common::GpuVendorName(0x10DE) == "NVIDIA");
    CHECK(Common::GpuVendorName(0x1234) == "0x1234");
    CHECK(Common::GpuVendorName(0) == "unknown");
}

TEST_CASE("Cores: a hybrid CPU by its efficiency classes", "[common][device_profile]") {
    // A Core Ultra 9 285H as Windows reports it.
    std::vector<CpuCore> arrow_lake;
    AddCores(arrow_lake, 6, 1, 2);
    AddCores(arrow_lake, 8, 1, 1);
    AddCores(arrow_lake, 2, 1, 0);
    const Common::CpuProfile ultra = Common::CountCores(arrow_lake);
    CHECK(ultra.logical == 16);
    CHECK(ultra.physical == 16);
    CHECK(ultra.performance == 6);
    CHECK(ultra.efficiency == 8);
    CHECK(ultra.low_power == 2);
    CHECK(ultra.Hybrid());

    // Two classes: P cores with SMT and E cores (Core i7-12700H).
    std::vector<CpuCore> alder_lake;
    AddCores(alder_lake, 6, 2, 1);
    AddCores(alder_lake, 8, 1, 0);
    const Common::CpuProfile alder = Common::CountCores(alder_lake);
    CHECK(alder.logical == 20);
    CHECK(alder.physical == 14);
    CHECK(alder.performance == 6);
    CHECK(alder.efficiency == 8);
    CHECK(alder.low_power == 0);
    CHECK(alder.Hybrid());
}

TEST_CASE("Cores: a CPU with one efficiency class is not hybrid", "[common][device_profile]") {
    std::vector<CpuCore> uniform;
    AddCores(uniform, 8, 2, 0);
    const Common::CpuProfile cpu = Common::CountCores(uniform);
    CHECK(cpu.logical == 16);
    CHECK(cpu.physical == 8);
    CHECK(cpu.performance == 0);
    CHECK(cpu.efficiency == 0);
    CHECK(cpu.low_power == 0);
    CHECK_FALSE(cpu.Hybrid());

    const Common::CpuProfile none = Common::CountCores({});
    CHECK(none.logical == 0);
    CHECK(none.physical == 0);
    CHECK_FALSE(none.Hybrid());
}

TEST_CASE("Unified memory: integrated GPUs, software renderers and one shared heap",
          "[common][device_profile]") {
    using Common::IsUnifiedMemory;
    // An integrated GPU may list a host heap besides its device-local ones.
    CHECK(IsUnifiedMemory(GpuKind::Integrated, 3, 2));
    CHECK(IsUnifiedMemory(GpuKind::Cpu, 1, 1));
    CHECK(IsUnifiedMemory(GpuKind::Virtual, 1, 1));
    CHECK_FALSE(IsUnifiedMemory(GpuKind::Discrete, 3, 2));
    CHECK_FALSE(IsUnifiedMemory(GpuKind::Virtual, 2, 1));
    CHECK_FALSE(IsUnifiedMemory(GpuKind::Unknown, 0, 0));
}

TEST_CASE("Device profile: one log line of fields", "[common][device_profile]") {
    const DeviceProfile profile = Arc140T();
    CHECK(Common::DescribeDeviceProfile(profile, Common::Classify(profile)) ==
          "class=Integrated label=\"Intel Arc 140T, integrated, 32 GB, hybrid 6P+8E+2LPE, NPU\" "
          "gpu_vendor=Intel gpu_model=\"Intel(R) Arc(TM) 140T GPU (16GB)\" gpu_type=integrated "
          "gpu_driver=101.9175 gpu_local_mib=16384 gpu_uma=yes cpu_logical=16 cpu_physical=16 "
          "cpu_hybrid=6P+8E+2LPE ram_mib=32768 ram_source=installed power=ac display_hz=60.00 "
          "npu=yes");

    const DeviceProfile unknown{};
    CHECK(Common::DescribeDeviceProfile(unknown, Common::Classify(unknown)) ==
          "class=Unknown label=\"unknown GPU, unknown kind\" gpu_vendor=unknown gpu_model=\"\" "
          "gpu_type=unknown gpu_driver=unknown gpu_local_mib=0 gpu_uma=no cpu_logical=0 "
          "cpu_physical=0 cpu_hybrid=no ram_mib=0 ram_source=unknown power=unknown "
          "display_hz=unknown npu=unknown");
}

TEST_CASE("Device profile: the host reports its processors", "[common][device_profile]") {
    const DeviceProfile host = Common::DetectHostProfile();
    CHECK(host.cpu.logical >= 1);
    CHECK(host.cpu.physical <= host.cpu.logical);
    if (host.cpu.Hybrid()) {
        CHECK(host.cpu.performance + host.cpu.efficiency + host.cpu.low_power ==
              host.cpu.physical);
    }
    // The GPU and the display are the renderer's to fill in.
    CHECK(host.gpu.kind == GpuKind::Unknown);
    CHECK(host.refresh_hz == 0.0);
}
