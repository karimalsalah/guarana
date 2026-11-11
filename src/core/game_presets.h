// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <span>
#include <string_view>

#include "common/common_types.h"
#include "core/game_settings.h"

// Settings a game is known to run better with on a kind of GPU, for the ones the user left alone.
// A preset value is a per-game value of the running session only: no config file takes it
// (Settings::HoldPresetValue). Off with the auto_presets setting or EDEN_NX2_NO_PRESETS=1.
namespace Core::GamePresets {

struct GpuInfo {
    GameSettings::GPUVendor vendor{GameSettings::GPUVendor::Unknown};
    bool integrated{};
    /// The guest's tensor operations (HMMA) have no cooperative matrix shape to run on, so they
    /// are emulated with scalar shader math.
    bool software_tensor_math{};
};

/// What a preset asks of the GPU. Unknown takes any vendor, a false flag takes either state.
struct GpuMatch {
    GameSettings::GPUVendor vendor{GameSettings::GPUVendor::Unknown};
    bool integrated{};
    bool software_tensor_math{};
};

/// A switchable setting by its label, and the value as BasicSetting::ToString gives it.
struct SettingValue {
    std::string_view label;
    std::string_view value;
};

struct Preset {
    u64 title_id{};
    GpuMatch gpu{};
    std::span<const SettingValue> settings{};
    /// Built-in add-ons the preset counts on. They are on unless the user disabled them in
    /// Add-Ons, which is kept: this only reports it.
    std::span<const std::string_view> addons{};
    std::string_view reason{};
};

enum class Decision {
    Apply,
    AlreadySet,  ///< The global value is the preset's.
    UserPerGame, ///< The title's config sets it.
    UserGlobal,  ///< The global value is not the default.
};

[[nodiscard]] std::span<const Preset> Presets();

[[nodiscard]] bool Matches(const GpuMatch& match, const GpuInfo& gpu);

/// The first preset of the title this GPU matches, or nullptr.
[[nodiscard]] const Preset* Find(std::span<const Preset> presets, u64 title_id,
                                 const GpuInfo& gpu);

/// What to do with one preset value, from the setting's state as its strings.
[[nodiscard]] Decision Decide(std::string_view preset_value, bool per_game,
                              std::string_view global_value, std::string_view default_value);

/// Applies the title's preset for this GPU to Settings::values. Call it once the GPU is known,
/// before the settings are read.
void Apply(u64 title_id, const GpuInfo& gpu);

} // namespace Core::GamePresets
