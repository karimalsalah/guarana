// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/game_presets.h"

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

// Ocarina of Time on an Intel Arc 140T: 9.5 to 15 fps. Its anti-aliasing network is tensor math
// that runs as scalar shader code there, "No sharpening" replaces it; Max Resolution keeps the
// game off its 480p dynamic resolution floor.
constexpr std::array OCARINA_SOFTWARE_TENSOR_SETTINGS{
    SettingValue{"oot_anti_aliasing", "1"}, // No sharpening
    SettingValue{"ocarina_fsr", "false"},
    SettingValue{"frame_gen", "false"},
    SettingValue{"vram_usage_mode", "1"},   // Aggressive
    SettingValue{"frame_pacing_mode", "0"}, // Auto
};
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
           (!match.software_tensor_math || gpu.software_tensor_math);
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
