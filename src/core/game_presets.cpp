// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/game_presets.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <string>

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

bool Enabled() {
    const char* off = std::getenv("EDEN_NX2_NO_PRESETS");
    return Settings::values.auto_presets.GetValue() && (off == nullptr || off[0] != '1');
}

void ApplySetting(const Preset& preset, const SettingValue& item) {
    const auto& by_key = Settings::values.linkage.by_key;
    const auto it = by_key.find(std::string{item.label});
    if (it == by_key.end() || !it->second->Switchable()) {
        LOG_ERROR(Core, "Preset {}: {} is not a per-game setting", preset.reason, item.label);
        return;
    }
    Settings::BasicSetting& setting = *it->second;
    switch (Decide(item.value, !setting.UsingGlobal(), setting.ToStringGlobal(),
                   setting.DefaultToString())) {
    case Decision::Apply:
        setting.SetGlobal(false);
        setting.LoadString(std::string{item.value});
        Settings::HoldPresetValue(setting);
        LOG_INFO(Core, "Preset {}: {}={}", preset.reason, item.label, setting.ToString());
        break;
    case Decision::AlreadySet:
        LOG_INFO(Core, "Preset {}: {}={} (already set)", preset.reason, item.label, item.value);
        break;
    case Decision::UserPerGame:
        LOG_INFO(Core, "Preset {}: {}={} skipped, the game's configuration sets {}",
                 preset.reason, item.label, item.value, setting.ToString());
        break;
    case Decision::UserGlobal:
        LOG_INFO(Core, "Preset {}: {}={} skipped, the global configuration sets {}",
                 preset.reason, item.label, item.value, setting.ToStringGlobal());
        break;
    }
}

void ReportAddon(const Preset& preset, std::string_view addon) {
    const auto& all = Settings::values.disabled_addons;
    const auto it = all.find(preset.title_id);
    const bool disabled =
        it != all.end() && std::ranges::any_of(it->second, [addon](const std::string& name) {
            return name == addon;
        });
    if (disabled) {
        LOG_INFO(Core, "Preset {}: add-on \"{}\" skipped, it is disabled in Add-Ons",
                 preset.reason, addon);
    } else {
        LOG_INFO(Core, "Preset {}: add-on \"{}\" enabled", preset.reason, addon);
    }
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

void Apply(u64 title_id, const GpuInfo& gpu) {
    const Preset* const preset = Find(PRESETS, title_id, gpu);
    if (preset == nullptr) {
        return;
    }
    if (!Enabled()) {
        LOG_INFO(Core, "Preset {}: not used, automatic presets are off", preset->reason);
        return;
    }
    for (const SettingValue& item : preset->settings) {
        ApplySetting(*preset, item);
    }
    for (const std::string_view addon : preset->addons) {
        ReportAddon(*preset, addon);
    }
}

} // namespace Core::GamePresets
