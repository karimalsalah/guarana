// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/device_profile.h"
#include "common/literals.h"

namespace {

using namespace Common::Literals;
using Common::CpuCore;
using Common::DeviceClass;
using Common::DeviceProfile;
using Common::GpuFamily;
using Common::GpuKind;
using Common::NpuPresence;
using Common::PowerSource;

// VkDriverId values.
constexpr u32 AmdWindows = 1;   // VK_DRIVER_ID_AMD_PROPRIETARY
constexpr u32 Radv = 3;         // VK_DRIVER_ID_MESA_RADV
constexpr u32 Nvidia = 4;       // VK_DRIVER_ID_NVIDIA_PROPRIETARY
constexpr u32 IntelWindows = 5; // VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS
constexpr u32 Anv = 6;          // VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA
constexpr u32 Venus = 22;       // VK_DRIVER_ID_MESA_VENUS, a virtual GPU with the host's IDs

void AddCores(std::vector<CpuCore>& cores, u32 count, u32 threads, u8 efficiency_class) {
    for (u32 i = 0; i < count; ++i) {
        cores.push_back({.logical = threads, .efficiency_class = efficiency_class});
    }
}

GpuFamily IntelFamily(u32 device_id, std::string_view name, u32 driver = IntelWindows) {
    return Common::ClassifyGpuFamily(0x8086, device_id, name, driver, GpuKind::Integrated);
}

GpuFamily AmdFamily(u32 device_id, std::string_view name, u32 driver = AmdWindows) {
    return Common::ClassifyGpuFamily(0x1002, device_id, name, driver, GpuKind::Integrated);
}

// Every family but None, from the enum itself.
std::vector<GpuFamily> AllFamilies() {
    std::vector<GpuFamily> families;
    for (u32 i = 1; i < static_cast<u32>(GpuFamily::Count); ++i) {
        families.push_back(static_cast<GpuFamily>(i));
    }
    return families;
}

// A Core Ultra 9 285H laptop with its Arc 140T (8086:7D51) and 32 GB, on AC.
DeviceProfile Arc140T() {
    DeviceProfile profile;
    profile.gpu = {.vendor_id = 0x8086,
                   .device_id = 0x7D51,
                   .model = "Intel(R) Arc(TM) 140T GPU (16GB)",
                   .driver = "101.9175",
                   .driver_id = IntelWindows,
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
          "class=Integrated gpu_family=IntelArrowLakeH "
          "label=\"Intel Arc 140T, integrated, 32 GB, hybrid 6P+8E+2LPE, NPU\" "
          "gpu_vendor=Intel gpu_model=\"Intel(R) Arc(TM) 140T GPU (16GB)\" gpu_type=integrated "
          "gpu_driver=101.9175 gpu_local_mib=16384 gpu_uma=yes cpu_logical=16 cpu_physical=16 "
          "cpu_hybrid=6P+8E+2LPE ram_mib=32768 ram_source=installed power=ac display_hz=60.00 "
          "npu=yes");

    const DeviceProfile unknown{};
    CHECK(Common::DescribeDeviceProfile(unknown, Common::Classify(unknown)) ==
          "class=Unknown gpu_family=None label=\"unknown GPU, unknown kind\" gpu_vendor=unknown "
          "gpu_model=\"\" gpu_type=unknown gpu_driver=unknown gpu_local_mib=0 gpu_uma=no "
          "cpu_logical=0 cpu_physical=0 cpu_hybrid=no ram_mib=0 ram_source=unknown "
          "power=unknown display_hz=unknown npu=unknown");
}

TEST_CASE("GPU family: known integrated GPUs map to their family", "[common][device_profile]") {
    // Panther Lake: the "Intel(R) Graphics" with 4 Xe3 cores at B0A0 (Windows driver and ANV) is a
    // family of its own, apart from the Arc B390 and B370.
    CHECK(IntelFamily(0xB0A0, "Intel(R) Graphics") == GpuFamily::IntelGraphicsPantherLake);
    CHECK(IntelFamily(0xB0A0, "Intel(R) Graphics", Anv) == GpuFamily::IntelGraphicsPantherLake);
    CHECK(IntelFamily(0xB090, "Intel(R) Graphics") == GpuFamily::IntelGraphicsPantherLake);
    CHECK(IntelFamily(0xB080, "Intel(R) Arc(TM) B390 GPU") == GpuFamily::IntelArcB390);
    CHECK(IntelFamily(0xB086, "Intel(R) Arc(tm) Pro B390", Anv) == GpuFamily::IntelArcB390);
    CHECK(IntelFamily(0xB081, "Intel(R) Arc(TM) B370 GPU") == GpuFamily::IntelArcB370);
    CHECK(IntelFamily(0xB087, "Intel(R) Arc(tm) Pro B370", Anv) == GpuFamily::IntelArcB370);

    // Arrow Lake-H, Lunar Lake and Meteor Lake by ID, whatever name the driver gives.
    CHECK(IntelFamily(0x7D51, "Intel(R) Arc(TM) 140T GPU (16GB)") == GpuFamily::IntelArrowLakeH);
    CHECK(IntelFamily(0x7D51, "Intel(R) Graphics", Anv) == GpuFamily::IntelArrowLakeH);
    CHECK(IntelFamily(0x64A0, "Intel(R) Arc(TM) 140V GPU (16GB)") ==
          GpuFamily::IntelArcLunarLake);
    CHECK(IntelFamily(0x64A0, "Intel(R) Graphics", Anv) == GpuFamily::IntelArcLunarLake);
    CHECK(IntelFamily(0x7D55, "Intel(R) Arc(TM) Graphics") == GpuFamily::IntelArcMeteorLake);

    // IDs Intel does not list go by the marketing name.
    CHECK(IntelFamily(0x7DD1, "Intel(R) Arc(TM) 130T GPU") == GpuFamily::IntelArrowLakeH);
    CHECK(IntelFamily(0x64B0, "Intel(R) Arc(TM) 140V GPU") == GpuFamily::IntelArcLunarLake);

    // AMD shares one ID between several parts: the name decides.
    CHECK(AmdFamily(0x15BF, "AMD Radeon 780M Graphics") == GpuFamily::AmdRadeon780M);
    CHECK(AmdFamily(0x15BF, "AMD Radeon 780M Graphics (RADV PHOENIX)", Radv) ==
          GpuFamily::AmdRadeon780M);
    CHECK(AmdFamily(0x1900, "AMD Radeon 780M Graphics") == GpuFamily::AmdRadeon780M);
    CHECK(AmdFamily(0x150E, "AMD Radeon 880M Graphics") == GpuFamily::AmdRadeon880M);
    CHECK(AmdFamily(0x150E, "AMD Radeon 890M Graphics") == GpuFamily::AmdRadeon890M);
    CHECK(AmdFamily(0x1586, "AMD Radeon 8060S Graphics") == GpuFamily::AmdRadeon8060S);
    CHECK(AmdFamily(0x163F, "AMD Custom GPU 0405 (RADV VANGOGH)", Radv) ==
          GpuFamily::AmdCustomGpu0405);
    CHECK(AmdFamily(0x1435, "AMD Custom GPU 0932") == GpuFamily::AmdCustomGpu0932);
}

TEST_CASE("GPU family: unknown GPUs, foreign drivers and other kinds are in none",
          "[common][device_profile]") {
    // Smaller parts of the same generations, and IDs Intel does not list without a known name.
    CHECK(IntelFamily(0x6420, "Intel(R) Graphics") == GpuFamily::None);
    CHECK(IntelFamily(0x7DD5, "Intel(R) Graphics") == GpuFamily::None);
    CHECK(IntelFamily(0x7D40, "Intel(R) Graphics") == GpuFamily::None);
    CHECK(IntelFamily(0x7D41, "Intel(R) Graphics") == GpuFamily::None);
    CHECK(IntelFamily(0xB0B0, "Intel(R) Graphics") == GpuFamily::None);
    CHECK(IntelFamily(0xB08F, "Intel(R) Graphics") == GpuFamily::None);
    CHECK(IntelFamily(0x7DD1, "Intel(R) Graphics") == GpuFamily::None);
    CHECK(IntelFamily(0x64B0, "Intel(R) Graphics") == GpuFamily::None);
    CHECK(AmdFamily(0x15BF, "AMD Radeon 760M Graphics") == GpuFamily::None);
    CHECK(AmdFamily(0x1900, "AMD Radeon 760M Graphics") == GpuFamily::None);
    CHECK(AmdFamily(0x15BF, "AMD Radeon Graphics (RADV PHOENIX)", Radv) == GpuFamily::None);
    CHECK(AmdFamily(0x1586, "AMD Radeon 8050S Graphics") == GpuFamily::None);
    // A known name on an ID it does not belong to.
    CHECK(IntelFamily(0x1234, "Intel(R) Arc(TM) 140V GPU") == GpuFamily::None);
    CHECK(AmdFamily(0x1234, "AMD Radeon 780M Graphics") == GpuFamily::None);
    CHECK(AmdFamily(0xB0A0, "Intel(R) Graphics", IntelWindows) == GpuFamily::None);
    // A driver that is not the vendor's own: another vendor's, a virtual GPU's, or none.
    CHECK(Common::ClassifyGpuFamily(0x10DE, 0xB0A0, "Intel(R) Graphics", Nvidia,
                                    GpuKind::Integrated) == GpuFamily::None);
    CHECK(IntelFamily(0xB0A0, "Intel(R) Graphics", Radv) == GpuFamily::None);
    CHECK(IntelFamily(0xB0A0, "Intel(R) Graphics", Venus) == GpuFamily::None);
    CHECK(AmdFamily(0x163F, "AMD Custom GPU 0405", Venus) == GpuFamily::None);
    CHECK(AmdFamily(0x163F, "AMD Custom GPU 0405", 0) == GpuFamily::None);
    // Not integrated.
    for (const GpuKind kind :
         {GpuKind::Discrete, GpuKind::Virtual, GpuKind::Cpu, GpuKind::Unknown}) {
        CHECK(Common::ClassifyGpuFamily(0x8086, 0xB0A0, "Intel(R) Graphics", IntelWindows, kind) ==
              GpuFamily::None);
    }
    CHECK(Common::ClassifyGpuFamily(0, 0, "", 0, GpuKind::Unknown) == GpuFamily::None);
}

TEST_CASE("GPU family names", "[common][device_profile]") {
    CHECK(Common::GpuFamilyName(GpuFamily::None) == "None");
    CHECK(Common::GpuFamilyName(GpuFamily::Count) == "None");
    CHECK(Common::GpuFamilyName(GpuFamily::IntelGraphicsPantherLake) ==
          "IntelGraphicsPantherLake");
    // Every family has a name of its own.
    const std::vector<GpuFamily> families = AllFamilies();
    CHECK(families.size() == std::size_t{12});
    for (std::size_t i = 0; i < families.size(); ++i) {
        const std::string_view name = Common::GpuFamilyName(families[i]);
        CHECK_FALSE(name.empty());
        CHECK(name != "None");
        for (std::size_t j = i + 1; j < families.size(); ++j) {
            CHECK(name != Common::GpuFamilyName(families[j]));
        }
    }
}

TEST_CASE("GPU family: the profile carries it apart from the class", "[common][device_profile]") {
    CHECK(Common::Classify(Arc140T()).gpu_family == GpuFamily::IntelArrowLakeH);

    // The Panther Lake part with 4 Xe3 cores is in the same class as the Arc 140T; its family
    // tells them apart.
    DeviceProfile panther_lake = Arc140T();
    panther_lake.gpu.device_id = 0xB0A0;
    panther_lake.gpu.model = "Intel(R) Graphics";
    const Common::DeviceClassification classified = Common::Classify(panther_lake);
    CHECK(classified.device_class == DeviceClass::Integrated);
    CHECK(classified.gpu_family == GpuFamily::IntelGraphicsPantherLake);

    // The family does not follow the power source or the RAM; the class does.
    panther_lake.power = PowerSource::Battery;
    CHECK(Common::Classify(panther_lake).device_class == DeviceClass::IntegratedLowPower);
    CHECK(Common::Classify(panther_lake).gpu_family == GpuFamily::IntelGraphicsPantherLake);

    // Without the driver ID there is no family.
    panther_lake.gpu.driver_id = 0;
    CHECK(Common::Classify(panther_lake).gpu_family == GpuFamily::None);

    CHECK(Common::Classify(Discrete(12_GiB)).gpu_family == GpuFamily::None);
    CHECK(Common::Classify({}).gpu_family == GpuFamily::None);
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
