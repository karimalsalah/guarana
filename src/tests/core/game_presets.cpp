// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "common/settings.h"
#include "core/game_presets.h"

namespace {

using namespace Core::GamePresets;
using Core::GameSettings::GPUVendor;

constexpr u64 OcarinaTitleId = 0x0400486024DEC000;
constexpr GpuInfo Arc140T{.vendor = GPUVendor::Intel,
                          .integrated = true,
                          .software_tensor_math = true};

// Puts back the settings the Ocarina preset touches.
struct SettingsScope {
    ~SettingsScope() {
        Settings::ClearPresetValues();
        auto& values = Settings::values;
        values.oot_anti_aliasing.SetGlobal(true);
        values.oot_anti_aliasing.SetValue(anti_aliasing);
        values.vram_usage_mode.SetGlobal(true);
        values.vram_usage_mode.SetValue(vram);
        values.ocarina_fsr.SetGlobal(true);
        values.ocarina_fsr.SetValue(fsr);
        values.auto_presets.SetValue(enabled);
    }

    Settings::OotAntiAliasing anti_aliasing{Settings::values.oot_anti_aliasing.GetValue(true)};
    Settings::VramUsageMode vram{Settings::values.vram_usage_mode.GetValue(true)};
    bool fsr{Settings::values.ocarina_fsr.GetValue(true)};
    bool enabled{Settings::values.auto_presets.GetValue()};
};

void ResetToDefaults() {
    auto& values = Settings::values;
    Settings::ClearPresetValues();
    values.oot_anti_aliasing.SetGlobal(true);
    values.oot_anti_aliasing.SetValue(values.oot_anti_aliasing.GetDefault());
    values.vram_usage_mode.SetGlobal(true);
    values.vram_usage_mode.SetValue(values.vram_usage_mode.GetDefault());
    values.ocarina_fsr.SetGlobal(true);
    values.ocarina_fsr.SetValue(values.ocarina_fsr.GetDefault());
    values.auto_presets.SetValue(true);
}

} // namespace

TEST_CASE("Game presets match on GPU capabilities", "[core][game_presets]") {
    const GpuMatch match{.vendor = GPUVendor::Intel,
                         .integrated = true,
                         .software_tensor_math = true};
    REQUIRE(Matches(match, Arc140T));
    REQUIRE_FALSE(Matches(match, {.vendor = GPUVendor::AMD,
                                  .integrated = true,
                                  .software_tensor_math = true}));
    REQUIRE_FALSE(Matches(match, {.vendor = GPUVendor::Intel,
                                  .integrated = false,
                                  .software_tensor_math = true}));
    REQUIRE_FALSE(Matches(match, {.vendor = GPUVendor::Intel,
                                  .integrated = true,
                                  .software_tensor_math = false}));
    // An empty match takes any GPU.
    REQUIRE(Matches({}, {}));
    REQUIRE(Matches({}, Arc140T));
}

TEST_CASE("Game presets are found by title and GPU", "[core][game_presets]") {
    const std::array presets{
        Preset{.title_id = 1, .gpu = {.integrated = true}, .reason = "integrated"},
        Preset{.title_id = 1, .gpu = {}, .reason = "any"},
        Preset{.title_id = 2, .gpu = {.vendor = GPUVendor::Nvidia}, .reason = "nvidia"},
    };
    REQUIRE(Find(presets, 1, Arc140T)->reason == "integrated");
    REQUIRE(Find(presets, 1, {})->reason == "any");
    REQUIRE(Find(presets, 2, Arc140T) == nullptr);
    REQUIRE(Find(presets, 3, Arc140T) == nullptr);

    REQUIRE(Find(Presets(), OcarinaTitleId, Arc140T) != nullptr);
    REQUIRE(Find(Presets(), OcarinaTitleId, {.vendor = GPUVendor::Nvidia}) == nullptr);
}

TEST_CASE("A game preset value yields to the user's", "[core][game_presets]") {
    REQUIRE(Decide("1", false, "0", "0") == Decision::Apply);
    REQUIRE(Decide("1", false, "1", "0") == Decision::AlreadySet);
    REQUIRE(Decide("0", false, "0", "0") == Decision::AlreadySet);
    REQUIRE(Decide("1", false, "2", "0") == Decision::UserGlobal);
    REQUIRE(Decide("1", true, "0", "0") == Decision::UserPerGame);
    REQUIRE(Decide("1", true, "1", "0") == Decision::UserPerGame);
}

TEST_CASE("Game presets name per-game settings and values they keep", "[core][game_presets]") {
    SettingsScope scope;
    const auto& by_key = Settings::values.linkage.by_key;
    for (const Preset& preset : Presets()) {
        for (const SettingValue& item : preset.settings) {
            const auto it = by_key.find(std::string{item.label});
            REQUIRE(it != by_key.end());
            Settings::BasicSetting& setting = *it->second;
            REQUIRE(setting.Switchable());
            const bool global = setting.UsingGlobal();
            const std::string previous = setting.ToStringGlobal();
            setting.SetGlobal(true);
            setting.LoadString(std::string{item.value});
            CHECK(setting.ToString() == item.value);
            setting.LoadString(previous);
            setting.SetGlobal(global);
        }
    }
}

TEST_CASE("Game presets fill in the settings left at their default", "[core][game_presets]") {
    SettingsScope scope;
    auto& values = Settings::values;

    ResetToDefaults();
    Apply(OcarinaTitleId, Arc140T);
    REQUIRE(values.oot_anti_aliasing.GetValue() == Settings::OotAntiAliasing::NoSharpening);
    REQUIRE(values.vram_usage_mode.GetValue() == Settings::VramUsageMode::Aggressive);
    REQUIRE(Settings::IsPresetValue(values.oot_anti_aliasing));
    // The preset's value is the default: nothing to hold.
    REQUIRE(values.ocarina_fsr.UsingGlobal());
    // The global values stay the user's.
    REQUIRE(values.oot_anti_aliasing.GetValue(true) == Settings::OotAntiAliasing::Game);
    REQUIRE(values.vram_usage_mode.GetValue(true) == Settings::VramUsageMode::Conservative);

    // A value the user then changes is theirs.
    values.vram_usage_mode.SetValue(Settings::VramUsageMode::Conservative);
    REQUIRE_FALSE(Settings::IsPresetValue(values.vram_usage_mode));
    Settings::ClearPresetValues();
    REQUIRE(values.oot_anti_aliasing.UsingGlobal());
    REQUIRE_FALSE(values.vram_usage_mode.UsingGlobal());

    // Set for the game, even to the default.
    ResetToDefaults();
    values.oot_anti_aliasing.SetGlobal(false);
    values.oot_anti_aliasing.SetValue(Settings::OotAntiAliasing::Game);
    // Changed globally.
    values.ocarina_fsr.SetValue(true);
    Apply(OcarinaTitleId, Arc140T);
    REQUIRE(values.oot_anti_aliasing.GetValue() == Settings::OotAntiAliasing::Game);
    REQUIRE_FALSE(Settings::IsPresetValue(values.oot_anti_aliasing));
    REQUIRE(values.ocarina_fsr.GetValue());
    REQUIRE(values.vram_usage_mode.GetValue() == Settings::VramUsageMode::Aggressive);

    ResetToDefaults();
    Apply(OcarinaTitleId, {.vendor = GPUVendor::Nvidia});
    REQUIRE(values.oot_anti_aliasing.UsingGlobal());

    values.auto_presets.SetValue(false);
    Apply(OcarinaTitleId, Arc140T);
    REQUIRE(values.oot_anti_aliasing.UsingGlobal());
}
