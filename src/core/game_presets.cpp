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
constexpr u64 OCARINA_TITLE_ID = 0x0400486024DEC000;

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

std::span<const SwitchValue> SwitchDefaults([[maybe_unused]] u64 title_id,
                                            [[maybe_unused]] const GpuInfo& gpu) {
#ifdef _WIN32
    if (title_id == OCARINA_TITLE_ID && gpu.integrated) {
        if (gpu.vendor == GameSettings::GPUVendor::Intel) {
            // Measured at 120 Hz only: other high refresh rates keep the 60 Hz configuration.
            const u32 refresh = PrimaryDisplayRefreshHz();
            if (refresh >= 115 && refresh <= 125) {
                return OCARINA_INTEL_IGPU_120_SWITCHES;
            }
            return OCARINA_INTEL_IGPU_SWITCHES;
        }
        if (gpu.vendor == GameSettings::GPUVendor::AMD) {
            return OCARINA_IGPU_SWITCHES;
        }
    }
#endif
    // TODO(measure): defaults by device class or GPU family (gpu.device_class, gpu.gpu_family) go
    // here; settings presets by class or family set GpuMatch's device_class or gpu_family. Each
    // needs a paired run on a verified scene of that class or family first (AGENTS.md, rule 1);
    // until then neither changes anything.
    return {};
}

std::string ApplySwitchDefaults(u64 title_id, const GpuInfo& gpu) {
    const std::span<const SwitchValue> defaults = SwitchDefaults(title_id, gpu);
    if (defaults.empty()) {
        return {};
    }
    if (!Enabled()) {
        LOG_INFO(Core, "Switch defaults for this game and GPU: not used, automatic presets are off");
        return "switch defaults not used, automatic presets are off";
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
    LOG_INFO(Core, "Switch defaults for this game and GPU:{}{}",
             set.empty() ? std::string{" none needed"} : set,
             kept.empty() ? std::string{} : fmt::format("; set by the user:{}", kept));
    return fmt::format("switch defaults:{}{}", set.empty() ? std::string{" none needed"} : set,
                       kept.empty() ? std::string{} : fmt::format(" (user set:{})", kept));
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
