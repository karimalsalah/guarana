// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/game_presets.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include <fmt/format.h>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"

namespace Core::GamePresets {
namespace {

// Ocarina of Time on an Intel Arc 140T. Its anti-aliasing network is tensor math that runs as
// scalar shader code there, "No sharpening" replaces it; Max Resolution keeps the game off its
// 480p dynamic resolution floor. On Windows frame generation is on, 3 frames per real one toward
// 60: with OCARINA_IGPU_SWITCHES it predicts from the game's motion on the vblank-locked presenter
// (20 real frames a second, one per 20 Hz game tick). Elsewhere that presenter is not measured,
// so frame generation stays off.
constexpr std::array OCARINA_SOFTWARE_TENSOR_SETTINGS{
    SettingValue{"oot_anti_aliasing", "1"}, // No sharpening
    SettingValue{"ocarina_fsr", "false"},
#ifdef _WIN32
    SettingValue{"frame_gen", "true"},
    SettingValue{"frame_gen_multiplier", "3"},
    SettingValue{"frame_gen_target_rate", "60"},
#else
    SettingValue{"frame_gen", "false"},
#endif
    SettingValue{"vram_usage_mode", "1"},   // Aggressive
    SettingValue{"frame_pacing_mode", "0"}, // Auto
};

// Ocarina of Time on a Windows integrated GPU: the pacing measured best in Hyrule Field on the
// Arc 140T (docs/switches.md, "Defaults for Ocarina of Time"). FLUID=2 shows each real frame and
// predicts under one frame interval ahead, CADENCE=3 holds real frames to the game's 20 Hz tick,
// EXTRAPOLATE predicts from the game's motion vectors, EARLY_* download read-backs early. Against
// FLUID=1 in one paired session: slots shown on time 430 to 536 of 600, evenly paced frames 62%
// to 88%. FLUID_QUEUE=5 with HOST_WAIT copies each frame to the screen on a queue of its own, so
// the copy does not wait behind the game's rendering, and BUDGET_HOLD_RES keeps the resolution
// from rising into the freed GPU time: frames that reached the screen late went from about 200
// per 10 s to 0, slots on time to 586-599 of 600. GPL (pipeline library fast links) only on
// Intel, where it was measured.
#ifdef _WIN32
constexpr std::array OCARINA_IGPU_SWITCHES{
    SwitchValue{"EDEN_NX2_FLUID", "2"},
    SwitchValue{"EDEN_NX2_CADENCE", "3"},
    SwitchValue{"EDEN_NX2_EXTRAPOLATE", "1"},
    SwitchValue{"EDEN_NX2_EARLY_SUBMIT", "1"},
    SwitchValue{"EDEN_NX2_EARLY_PAGES", "256"},
    SwitchValue{"EDEN_NX2_EARLY_AFTER", "1"},
    SwitchValue{"EDEN_NX2_FLUID_QUEUE", "5"},
    SwitchValue{"EDEN_NX2_FLUID_QUEUE_HOST_WAIT", "1"},
    SwitchValue{"EDEN_NX2_BUDGET_HOLD_RES", "1"},
};
constexpr std::array OCARINA_INTEL_IGPU_SWITCHES{
    SwitchValue{"EDEN_NX2_FLUID", "2"},
    SwitchValue{"EDEN_NX2_CADENCE", "3"},
    SwitchValue{"EDEN_NX2_EXTRAPOLATE", "1"},
    SwitchValue{"EDEN_NX2_EARLY_SUBMIT", "1"},
    SwitchValue{"EDEN_NX2_EARLY_PAGES", "256"},
    SwitchValue{"EDEN_NX2_EARLY_AFTER", "1"},
    SwitchValue{"EDEN_NX2_FLUID_QUEUE", "5"},
    SwitchValue{"EDEN_NX2_FLUID_QUEUE_HOST_WAIT", "1"},
    SwitchValue{"EDEN_NX2_BUDGET_HOLD_RES", "1"},
    SwitchValue{"EDEN_NX2_GPL", "1"},
};
// A display at about 120 Hz: a frame every refresh on the vblank clock, the in-between frames
// interpolated from the last two real frames (optical flow, so Link and everything else moves by its
// own motion), 20 real frames a second (CADENCE=3 is a share of 60, six slots each at 120), the
// present thread time-critical in the MMCSS Games task. Arc 140T, Hyrule Field, while Link runs:
// 1181-1200 of 1200 slots on time per 10 s, 118-120 shown frames a second, p99 9.6-13.6 ms.
constexpr std::array OCARINA_INTEL_IGPU_120_SWITCHES{
    SwitchValue{"EDEN_NX2_FLUID", "2"},
    SwitchValue{"EDEN_NX2_FLUID_INTERP", "1"},
    SwitchValue{"EDEN_NX2_FLUID_RT", "1"},
    SwitchValue{"EDEN_NX2_SHOWN_RATE", "120"},
    SwitchValue{"EDEN_NX2_CADENCE", "3"},
    SwitchValue{"EDEN_NX2_EARLY_SUBMIT", "1"},
    SwitchValue{"EDEN_NX2_EARLY_PAGES", "256"},
    SwitchValue{"EDEN_NX2_EARLY_AFTER", "1"},
    SwitchValue{"EDEN_NX2_FLUID_QUEUE", "5"},
    SwitchValue{"EDEN_NX2_FLUID_QUEUE_HOST_WAIT", "1"},
    SwitchValue{"EDEN_NX2_BUDGET_HOLD_RES", "1"},
    SwitchValue{"EDEN_NX2_BUDGET_CEILING", "1"},
    SwitchValue{"EDEN_NX2_GPL", "1"},
};
// A display at about 144 Hz: frames paced to every refresh on the vblank clock (91.5 of 144 shown
// a second in the one run), the in-between frames interpolated, real frames held to 24 a second
// (CADENCE_DIVISOR=6: six slots per real frame at a shown rate of 144; the divisor is read only
// with CADENCE set, and CADENCE=3 alone would give 6 too, but only because 144/60 rounds to 2),
// the time-critical present thread. Exactly the setup
// measured: the 60 Hz Intel set with EXTRAPOLATE=0, FLUID_INTERP=1, FLUID_RT=1, SHOWN_RATE=144
// and CADENCE_DIVISOR=6, so no BUDGET_CEILING (the 120 Hz set has it; these runs did not).
// One device, single runs, Ocarina of Time in Kokiri Forest by Link's house: Panther Lake laptop,
// "Intel(R) Graphics" 8086:B0A0, 4 Xe3 cores, driver 32.0.101.8724, 2880x1800 at 144 Hz.
//   the 60 Hz set:           20.0 real fps, 50.5 shown a second, shown frames held one slot 0%
//                            (held 2 or 4+ refreshes), real frame age 57.0 ms
//   this set:                24.0 real fps, 91.5 shown a second, 917 of 1069 slots on their slot,
//                            84% held one slot, real frame age 89.9 ms
//   with CADENCE_DIVISOR=7:  20.6 real fps, 84.1 shown a second, 847 of 1048 slots, 90%, 108.1 ms
//   EXTRAPOLATE=1 (shipped nx2_switches.txt), divisor 7: 15.5 real fps, 58.5 shown a second,
//                            573 of 1439 slots, 56%, 75.5 ms, 655 of 1238 frames dropped unshown
// A scripted walk outdoors with this set held 17.6 to 24.1 real frames a second. The real frame
// shown is older: 89.9 ms against 57.0 ms. A paired A-B-A-B of the first
// two rows held in both pairs (79-83 shown a second against 44-51, 24 real against 20-22, real
// frame age 74-87 ms against 53-57 ms; one session, 14 November). With EDEN_NX2_EXTRAPOLATE=1
// set (the shipped nx2_switches.txt sets it, and a switch already set stays as set) the presenter
// extrapolates and FLUID_INTERP does nothing: that combination is the last row, not this one, and
// ApplySwitchDefaults says so in its log line.
constexpr std::array OCARINA_INTEL_IGPU_144_SWITCHES{
    SwitchValue{"EDEN_NX2_FLUID", "2"},
    SwitchValue{"EDEN_NX2_FLUID_INTERP", "1"},
    SwitchValue{"EDEN_NX2_FLUID_RT", "1"},
    SwitchValue{"EDEN_NX2_SHOWN_RATE", "144"},
    SwitchValue{"EDEN_NX2_CADENCE", "3"},
    SwitchValue{"EDEN_NX2_CADENCE_DIVISOR", "6"},
    SwitchValue{"EDEN_NX2_EARLY_SUBMIT", "1"},
    SwitchValue{"EDEN_NX2_EARLY_PAGES", "256"},
    SwitchValue{"EDEN_NX2_EARLY_AFTER", "1"},
    SwitchValue{"EDEN_NX2_FLUID_QUEUE", "5"},
    SwitchValue{"EDEN_NX2_FLUID_QUEUE_HOST_WAIT", "1"},
    SwitchValue{"EDEN_NX2_BUDGET_HOLD_RES", "1"},
    SwitchValue{"EDEN_NX2_GPL", "1"},
};

#ifdef _WIN32
/// The primary display's refresh rate in Hz, 0 when Windows does not say.
[[nodiscard]] u32 PrimaryDisplayRefreshHz() {
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode)) {
        return 0;
    }
    return mode.dmDisplayFrequency > 1 ? static_cast<u32>(mode.dmDisplayFrequency) : 0U;
}
#endif
#endif
constexpr u64 OCARINA_TITLE_ID = 0x0400486024DEC000;

// The display rates each locked set is chosen at, in Hz (both ends included). Each was measured at
// one rate only: 120 and 144.
constexpr u32 LOCKED_120_MIN_HZ = 115;
constexpr u32 LOCKED_120_MAX_HZ = 125;
constexpr u32 LOCKED_144_MIN_HZ = 140;
constexpr u32 LOCKED_144_MAX_HZ = 148;

constexpr std::array<std::string_view, 1> OCARINA_SOFTWARE_TENSOR_ADDONS{
    "Max Resolution (built-in)",
};

constexpr std::array PRESETS{
    Preset{
        .title_id = 0x0400486024DEC000,
        .gpu = {.vendor = GameSettings::GPUVendor::Intel,
                .integrated = true,
                .software_tensor_math = true},
        .settings = OCARINA_SOFTWARE_TENSOR_SETTINGS,
        .addons = OCARINA_SOFTWARE_TENSOR_ADDONS,
        .reason = "Ocarina of Time on an Intel integrated GPU without matrix cores",
    },
};

struct FirstBootTitle {
    u64 title_id{};
    std::string_view name;
};

// Titles whose first boot on an Intel or AMD integrated GPU gets one line with what the presets
// did, so that a first report shows the starting point.
constexpr std::array FIRST_BOOT_TITLES{
    FirstBootTitle{.title_id = 0x0400486024DEC000, .name = "Ocarina of Time"},
};

bool Enabled() {
    const char* off = std::getenv("EDEN_NX2_NO_PRESETS");
    return Settings::values.auto_presets.GetValue() && (off == nullptr || off[0] != '1');
}

/// Returns the outcome for the summary line.
std::string ApplySetting(const Preset& preset, const SettingValue& item) {
    const auto& by_key = Settings::values.linkage.by_key;
    const auto it = by_key.find(std::string{item.label});
    if (it == by_key.end() || !it->second->Switchable()) {
        LOG_ERROR(Core, "Preset {}: {} is not a per-game setting", preset.reason, item.label);
        return fmt::format("{} is not a per-game setting", item.label);
    }
    Settings::BasicSetting& setting = *it->second;
    switch (Decide(item.value, !setting.UsingGlobal(), setting.ToStringGlobal(),
                   setting.DefaultToString())) {
    case Decision::Apply:
        setting.SetGlobal(false);
        setting.LoadString(std::string{item.value});
        Settings::HoldPresetValue(setting);
        LOG_INFO(Core, "Preset {}: {}={}", preset.reason, item.label, setting.ToString());
        return fmt::format("{}={}", item.label, setting.ToString());
    case Decision::AlreadySet:
        LOG_INFO(Core, "Preset {}: {}={} (already set)", preset.reason, item.label, item.value);
        return fmt::format("{}={} (already set)", item.label, item.value);
    case Decision::UserPerGame:
        LOG_INFO(Core, "Preset {}: {}={} skipped, the game's configuration sets {}",
                 preset.reason, item.label, item.value, setting.ToString());
        return fmt::format("{}={} skipped (game configuration: {})", item.label, item.value,
                           setting.ToString());
    case Decision::UserGlobal:
        LOG_INFO(Core, "Preset {}: {}={} skipped, the global configuration sets {}",
                 preset.reason, item.label, item.value, setting.ToStringGlobal());
        return fmt::format("{}={} skipped (global configuration: {})", item.label, item.value,
                           setting.ToStringGlobal());
    }
    return {};
}

/// Returns the outcome for the summary line.
std::string ReportAddon(const Preset& preset, std::string_view addon) {
    const auto& all = Settings::values.disabled_addons;
    const auto it = all.find(preset.title_id);
    const bool disabled =
        it != all.end() && std::ranges::any_of(it->second, [addon](const std::string& name) {
            return name == addon;
        });
    if (disabled) {
        LOG_INFO(Core, "Preset {}: add-on \"{}\" skipped, it is disabled in Add-Ons",
                 preset.reason, addon);
        return fmt::format("add-on \"{}\" skipped (disabled in Add-Ons)", addon);
    }
    LOG_INFO(Core, "Preset {}: add-on \"{}\" enabled", preset.reason, addon);
    return fmt::format("add-on \"{}\" enabled", addon);
}

std::filesystem::path FirstBootList() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / "presets_first_boot.txt";
}

} // Anonymous namespace

std::span<const Preset> Presets() {
    return PRESETS;
}

bool Matches(const GpuMatch& match, const GpuInfo& gpu) {
    return (match.vendor == GameSettings::GPUVendor::Unknown || match.vendor == gpu.vendor) &&
           (!match.integrated || gpu.integrated) &&
           (!match.software_tensor_math || gpu.software_tensor_math) &&
           (match.device_class == Common::DeviceClass::Unknown ||
            match.device_class == gpu.device_class) &&
           (match.gpu_family == Common::GpuFamily::None || match.gpu_family == gpu.gpu_family);
}

const Preset* Find(std::span<const Preset> presets, u64 title_id, const GpuInfo& gpu) {
    const auto it = std::ranges::find_if(presets, [&](const Preset& preset) {
        return preset.title_id == title_id && Matches(preset.gpu, gpu);
    });
    return it != presets.end() ? &*it : nullptr;
}

Decision Decide(std::string_view preset_value, bool per_game, std::string_view global_value,
                std::string_view default_value) {
    if (per_game) {
        return Decision::UserPerGame;
    }
    if (global_value == preset_value) {
        return Decision::AlreadySet;
    }
    return global_value == default_value ? Decision::Apply : Decision::UserGlobal;
}

std::string Apply(u64 title_id, const GpuInfo& gpu) {
    const Preset* const preset = Find(PRESETS, title_id, gpu);
    if (preset == nullptr) {
        return {};
    }
    if (!Enabled()) {
        LOG_INFO(Core, "Preset {}: not used, automatic presets are off", preset->reason);
        return fmt::format("preset \"{}\" not used, automatic presets are off", preset->reason);
    }
    std::string summary = fmt::format("preset \"{}\":", preset->reason);
    std::string_view separator = " ";
    for (const SettingValue& item : preset->settings) {
        summary += separator;
        summary += ApplySetting(*preset, item);
        separator = ", ";
    }
    for (const std::string_view addon : preset->addons) {
        summary += separator;
        summary += ReportAddon(*preset, addon);
        separator = ", ";
    }
    return summary;
}

PacingSet ChoosePacingSet(u64 title_id, const GpuInfo& gpu, u32 refresh_hz) {
    if (title_id != OCARINA_TITLE_ID || !gpu.integrated) {
        return PacingSet::None;
    }
    if (gpu.vendor == GameSettings::GPUVendor::AMD) {
        return PacingSet::Hz60;
    }
    if (gpu.vendor != GameSettings::GPUVendor::Intel) {
        return PacingSet::None;
    }
    // Measured at 120 and 144 Hz only: other refresh rates, and an unknown one, keep the 60 Hz set.
    if (refresh_hz >= LOCKED_120_MIN_HZ && refresh_hz <= LOCKED_120_MAX_HZ) {
        return PacingSet::Hz120;
    }
    if (refresh_hz >= LOCKED_144_MIN_HZ && refresh_hz <= LOCKED_144_MAX_HZ) {
        return PacingSet::Hz144;
    }
    // TODO(measure): defaults by device class or GPU family (gpu.device_class, gpu.gpu_family) go
    // here; settings presets by class or family set GpuMatch's device_class or gpu_family. Each
    // needs a paired run on a verified scene of that class or family first (AGENTS.md, rule 1);
    // until then neither changes anything.
    return PacingSet::Hz60;
}

std::string DescribePacingChoice(const GpuInfo& gpu, u32 refresh_hz, PacingSet set) {
    const std::string display = refresh_hz == 0
                                    ? std::string{"primary display rate unknown"}
                                    : fmt::format("primary display at {} Hz", refresh_hz);
    switch (set) {
    case PacingSet::None:
        return fmt::format("{}, no pacing set for this game and GPU", display);
    case PacingSet::Hz60:
        if (gpu.vendor != GameSettings::GPUVendor::Intel) {
            return fmt::format(
                "{}, the 60 Hz set (the only set given for this GPU vendor; measured on Intel)",
                display);
        }
        return fmt::format("{}, the 60 Hz set (the locked sets are chosen at {} to {} and {} "
                           "to {} Hz only)",
                           display, LOCKED_120_MIN_HZ, LOCKED_120_MAX_HZ, LOCKED_144_MIN_HZ,
                           LOCKED_144_MAX_HZ);
    case PacingSet::Hz120:
        return fmt::format("{}, the 120 Hz set (chosen at {} to {} Hz, measured at 120)", display,
                           LOCKED_120_MIN_HZ, LOCKED_120_MAX_HZ);
    case PacingSet::Hz144:
        return fmt::format("{}, the 144 Hz set (chosen at {} to {} Hz, measured at 144)", display,
                           LOCKED_144_MIN_HZ, LOCKED_144_MAX_HZ);
    }
    return display;
}

std::span<const SwitchValue> PacingSwitches([[maybe_unused]] PacingSet set,
                                            [[maybe_unused]] const GpuInfo& gpu) {
#ifdef _WIN32
    switch (set) {
    case PacingSet::None:
        return {};
    case PacingSet::Hz60:
        if (gpu.vendor == GameSettings::GPUVendor::Intel) {
            return OCARINA_INTEL_IGPU_SWITCHES;
        }
        return OCARINA_IGPU_SWITCHES;
    case PacingSet::Hz120:
        return OCARINA_INTEL_IGPU_120_SWITCHES;
    case PacingSet::Hz144:
        return OCARINA_INTEL_IGPU_144_SWITCHES;
    }
#else
    // The sets are measured on Windows only.
#endif
    return {};
}

std::span<const SwitchValue> SwitchDefaults(u64 title_id, const GpuInfo& gpu) {
#ifdef _WIN32
    return PacingSwitches(ChoosePacingSet(title_id, gpu, PrimaryDisplayRefreshHz()), gpu);
#else
    return PacingSwitches(ChoosePacingSet(title_id, gpu, 0), gpu);
#endif
}

std::string ApplySwitchDefaults(u64 title_id, const GpuInfo& gpu) {
#ifdef _WIN32
    const u32 refresh_hz = PrimaryDisplayRefreshHz();
#else
    const u32 refresh_hz = 0;
#endif
    const PacingSet pacing = ChoosePacingSet(title_id, gpu, refresh_hz);
    const std::span<const SwitchValue> defaults = PacingSwitches(pacing, gpu);
    if (defaults.empty()) {
        return {};
    }
    const std::string why = DescribePacingChoice(gpu, refresh_hz, pacing);
    if (!Enabled()) {
        LOG_INFO(Core,
                 "Switch defaults for this game and GPU ({}): not used, automatic presets are off",
                 why);
        return fmt::format("switch defaults ({}) not used, automatic presets are off", why);
    }
    std::string set;
    std::string kept;
    for (const SwitchValue& item : defaults) {
        const std::string name{item.name};
        if (const char* current = std::getenv(name.c_str()); current != nullptr) {
            kept += fmt::format(" {}={}", name, current);
            continue;
        }
        const std::string value{item.value};
#ifdef _WIN32
        _putenv_s(name.c_str(), value.c_str());
#else
        setenv(name.c_str(), value.c_str(), 0);
#endif
        set += fmt::format(" {}={}", name, value);
    }
    // The locked sets leave EXTRAPOLATE out, so the loop above never names it; set elsewhere (the
    // shipped nx2_switches.txt, the launcher) it wins over FLUID_INTERP (ExtrapolationRequested).
    std::string extrapolating;
    if (pacing == PacingSet::Hz120 || pacing == PacingSet::Hz144) {
        if (const char* extrapolate = std::getenv("EDEN_NX2_EXTRAPOLATE");
            extrapolate != nullptr && *extrapolate != '\0' && *extrapolate != '0') {
            extrapolating = fmt::format(
                "; EDEN_NX2_EXTRAPOLATE={} is set, so frames are extrapolated and FLUID_INTERP "
                "does nothing",
                extrapolate);
        }
    }
    LOG_INFO(Core, "Switch defaults for this game and GPU ({}):{}{}{}", why,
             set.empty() ? std::string{" none needed"} : set,
             kept.empty() ? std::string{} : fmt::format("; set by the user:{}", kept),
             extrapolating);
    return fmt::format("switch defaults ({}):{}{}{}", why,
                       set.empty() ? std::string{" none needed"} : set,
                       kept.empty() ? std::string{} : fmt::format(" (user set:{})", kept),
                       extrapolating);
}

bool ReportsFirstBoot(u64 title_id, const GpuInfo& gpu) {
    return gpu.integrated &&
           (gpu.vendor == GameSettings::GPUVendor::Intel ||
            gpu.vendor == GameSettings::GPUVendor::AMD) &&
           std::ranges::any_of(FIRST_BOOT_TITLES, [title_id](const FirstBootTitle& title) {
               return title.title_id == title_id;
           });
}

void ReportFirstBoot(u64 title_id, const GpuInfo& gpu, std::string_view gpu_name,
                     std::string_view summary) {
    if (!ReportsFirstBoot(title_id, gpu)) {
        return;
    }
    const auto title = std::ranges::find_if(FIRST_BOOT_TITLES, [title_id](const auto& entry) {
        return entry.title_id == title_id;
    });
    const std::string id = fmt::format("{:016X}", title_id);
    const std::filesystem::path list = FirstBootList();
    {
        std::ifstream file{list};
        std::string line;
        while (std::getline(file, line)) {
            if (line == id) {
                return;
            }
        }
    }
    LOG_INFO(Core, "First boot of {} on {} ({} integrated GPU{}): {}", title->name, gpu_name,
             gpu.vendor == GameSettings::GPUVendor::Intel ? "Intel" : "AMD",
             gpu.software_tensor_math ? " without matrix cores" : "",
             summary.empty() ? std::string_view{"no preset for this GPU, settings as configured"}
                             : summary);
    std::error_code error;
    if (std::filesystem::is_directory(list.parent_path(), error)) {
        std::ofstream file{list, std::ios::app};
        file << id << '\n';
    }
}

} // namespace Core::GamePresets
