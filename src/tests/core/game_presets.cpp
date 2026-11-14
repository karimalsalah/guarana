// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstdlib>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/settings.h"
#include "common/switch_origin.h"
#include "core/game_presets.h"
#include "video_core/nx2_async_readback.h"
#include "video_core/video_core.h"

namespace {

using namespace Core::GamePresets;
using Common::DeviceClass;
using Common::GpuFamily;
using Core::GameSettings::GPUVendor;

constexpr u64 OcarinaTitleId = 0x0400486024DEC000;
constexpr GpuInfo Arc140T{.vendor = GPUVendor::Intel,
                          .integrated = true,
                          .software_tensor_math = true};

constexpr std::array AllClasses{DeviceClass::Unknown, DeviceClass::IntegratedLowPower,
                                DeviceClass::Integrated, DeviceClass::DiscreteEntry,
                                DeviceClass::DiscreteHigh};

// Every family, None included, from the enum itself.
std::vector<GpuFamily> AllFamilies() {
    std::vector<GpuFamily> families;
    for (u32 i = 0; i < static_cast<u32>(GpuFamily::Count); ++i) {
        families.push_back(static_cast<GpuFamily>(i));
    }
    return families;
}

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

TEST_CASE("Game presets match on the device class and the GPU family", "[core][game_presets]") {
    GpuInfo panther_lake = Arc140T;
    panther_lake.device_class = DeviceClass::Integrated;
    panther_lake.gpu_family = GpuFamily::IntelGraphicsPantherLake;
    GpuInfo arc_b390 = panther_lake;
    arc_b390.gpu_family = GpuFamily::IntelArcB390;

    const GpuMatch by_family{.gpu_family = GpuFamily::IntelGraphicsPantherLake};
    REQUIRE(Matches(by_family, panther_lake));
    REQUIRE_FALSE(Matches(by_family, arc_b390));
    REQUIRE_FALSE(Matches(by_family, Arc140T));
    REQUIRE_FALSE(Matches(by_family, {}));

    const GpuMatch by_class{.device_class = DeviceClass::Integrated};
    REQUIRE(Matches(by_class, panther_lake));
    REQUIRE(Matches(by_class, arc_b390));
    REQUIRE_FALSE(Matches(by_class, Arc140T));
    GpuInfo on_battery = panther_lake;
    on_battery.device_class = DeviceClass::IntegratedLowPower;
    REQUIRE_FALSE(Matches(by_class, on_battery));

    // A class or a family goes with the other conditions.
    const GpuMatch all{.vendor = GPUVendor::Intel,
                       .integrated = true,
                       .software_tensor_math = true,
                       .device_class = DeviceClass::Integrated,
                       .gpu_family = GpuFamily::IntelGraphicsPantherLake};
    REQUIRE(Matches(all, panther_lake));
    REQUIRE_FALSE(Matches(all, on_battery));
    GpuInfo hardware_tensor_math = panther_lake;
    hardware_tensor_math.software_tensor_math = false;
    REQUIRE_FALSE(Matches(all, hardware_tensor_math));

    // An empty match takes any class and any family.
    for (const DeviceClass any_class : AllClasses) {
        for (const GpuFamily any_family : AllFamilies()) {
            REQUIRE(Matches({}, {.device_class = any_class, .gpu_family = any_family}));
        }
    }
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

TEST_CASE("A game preset sums up what it did in one line", "[core][game_presets]") {
    SettingsScope scope;
    ResetToDefaults();
    const std::string summary = Apply(OcarinaTitleId, Arc140T);
    CHECK(summary.starts_with(
        "preset \"Ocarina of Time on an Intel integrated GPU without matrix cores\": "));
    CHECK(summary.find("oot_anti_aliasing=1") != std::string::npos);
    CHECK(summary.find("vram_usage_mode=1") != std::string::npos);
    CHECK(summary.find("add-on \"Max Resolution (built-in)\" enabled") != std::string::npos);
    CHECK_FALSE(summary.ends_with(", "));

    ResetToDefaults();
    CHECK(Apply(OcarinaTitleId, {.vendor = GPUVendor::Nvidia}).empty());
}

TEST_CASE("Ocarina of Time on a Windows integrated GPU gets the pacing switches", "[core][game_presets]") {
#ifdef _WIN32
    const auto names = [](std::span<const SwitchValue> items) {
        std::string joined;
        for (const SwitchValue& item : items) {
            joined += std::string{item.name} + "=" + std::string{item.value} + " ";
        }
        return joined;
    };
    const std::string intel = names(SwitchDefaults(OcarinaTitleId, Arc140T));
    CHECK(intel.find("EDEN_NX2_FLUID=2 ") != std::string::npos);
    CHECK(intel.find("EDEN_NX2_CADENCE=3 ") != std::string::npos);
    CHECK(intel.find("EDEN_NX2_GPL=1 ") != std::string::npos);
    CHECK(intel.find("EDEN_NX2_FLUID_QUEUE=5 ") != std::string::npos);
    CHECK(intel.find("EDEN_NX2_BUDGET_HOLD_RES=1 ") != std::string::npos);
    const std::string amd = names(SwitchDefaults(
        OcarinaTitleId, {.vendor = GPUVendor::AMD, .integrated = true}));
    CHECK(amd.find("EDEN_NX2_FLUID=2 ") != std::string::npos);
    CHECK(amd.find("EDEN_NX2_GPL") == std::string::npos);
#else
    CHECK(SwitchDefaults(OcarinaTitleId, Arc140T).empty());
#endif
    // Discrete GPUs and other games keep the switches as they are.
    CHECK(SwitchDefaults(OcarinaTitleId, {.vendor = GPUVendor::Nvidia}).empty());
    CHECK(SwitchDefaults(OcarinaTitleId, {.vendor = GPUVendor::Intel, .integrated = false}).empty());
    CHECK(SwitchDefaults(0x0100152000022000, Arc140T).empty());
}

TEST_CASE("Ocarina of Time's pacing set follows the GPU and the display rate", "[core][game_presets]") {
    const GpuInfo amd{.vendor = GPUVendor::AMD, .integrated = true};
    for (const u32 hz : {59U, 60U, 75U, 100U, 114U, 126U, 139U, 149U, 165U, 240U, 0U}) {
        CAPTURE(hz);
        CHECK(ChoosePacingSet(OcarinaTitleId, Arc140T, hz) == PacingSet::Hz60);
    }
    for (const u32 hz : {115U, 120U, 125U}) {
        CAPTURE(hz);
        CHECK(ChoosePacingSet(OcarinaTitleId, Arc140T, hz) == PacingSet::Hz120);
    }
    for (const u32 hz : {140U, 144U, 148U}) {
        CAPTURE(hz);
        CHECK(ChoosePacingSet(OcarinaTitleId, Arc140T, hz) == PacingSet::Hz144);
    }
    // The locked sets are measured on Intel only; AMD integrated GPUs keep the 60 Hz set.
    for (const u32 hz : {0U, 60U, 120U, 144U, 240U}) {
        CAPTURE(hz);
        CHECK(ChoosePacingSet(OcarinaTitleId, amd, hz) == PacingSet::Hz60);
        // Discrete GPUs and other titles get no set at any rate.
        CHECK(ChoosePacingSet(OcarinaTitleId, {.vendor = GPUVendor::Intel}, hz) == PacingSet::None);
        CHECK(ChoosePacingSet(OcarinaTitleId, {.vendor = GPUVendor::AMD}, hz) == PacingSet::None);
        CHECK(ChoosePacingSet(OcarinaTitleId, {.vendor = GPUVendor::Nvidia}, hz) ==
              PacingSet::None);
        CHECK(ChoosePacingSet(OcarinaTitleId, {.vendor = GPUVendor::Nvidia, .integrated = true},
                              hz) == PacingSet::None);
        CHECK(ChoosePacingSet(0x0100152000022000, Arc140T, hz) == PacingSet::None);
        CHECK(ChoosePacingSet(0x0100152000022000, amd, hz) == PacingSet::None);
    }
}

TEST_CASE("The pacing choice names the display rate and the set", "[core][game_presets]") {
    CHECK(DescribePacingChoice(Arc140T, 144, PacingSet::Hz144) ==
          "primary display at 144 Hz, the 144 Hz set (chosen at 140 to 148 Hz, measured at 144)");
    CHECK(DescribePacingChoice(Arc140T, 120, PacingSet::Hz120) ==
          "primary display at 120 Hz, the 120 Hz set (chosen at 115 to 125 Hz, measured at 120)");
    CHECK(DescribePacingChoice(Arc140T, 165, PacingSet::Hz60).starts_with(
        "primary display at 165 Hz, the 60 Hz set"));
    CHECK(DescribePacingChoice(Arc140T, 0, PacingSet::Hz60).starts_with(
        "primary display rate unknown, the 60 Hz set"));
    CHECK(DescribePacingChoice({.vendor = GPUVendor::AMD, .integrated = true}, 144,
                               PacingSet::Hz60) ==
          "primary display at 144 Hz, the 60 Hz set (the only set given for this GPU vendor; "
          "measured on Intel)");
}

TEST_CASE("The pacing sets hold the switches measured", "[core][game_presets]") {
    const auto names = [](std::span<const SwitchValue> items) {
        std::string joined;
        for (const SwitchValue& item : items) {
            joined += std::string{item.name} + "=" + std::string{item.value} + " ";
        }
        return joined;
    };
    CHECK(PacingSwitches(PacingSet::None, Arc140T).empty());
#ifdef _WIN32
    const std::string hz144 = names(PacingSwitches(PacingSet::Hz144, Arc140T));
    // Exactly as measured: the 60 Hz Intel set with EXTRAPOLATE=0, FLUID_INTERP, FLUID_RT,
    // SHOWN_RATE=144 and CADENCE_DIVISOR=6; no BUDGET_CEILING.
    CHECK(hz144 == "EDEN_NX2_FLUID=2 EDEN_NX2_FLUID_INTERP=1 EDEN_NX2_FLUID_RT=1 "
                   "EDEN_NX2_SHOWN_RATE=144 EDEN_NX2_CADENCE=3 EDEN_NX2_CADENCE_DIVISOR=6 "
                   "EDEN_NX2_EXTRAPOLATE=0 "
                   "EDEN_NX2_EARLY_SUBMIT=1 EDEN_NX2_EARLY_PAGES=256 EDEN_NX2_EARLY_AFTER=1 "
                   "EDEN_NX2_FLUID_QUEUE=5 EDEN_NX2_FLUID_QUEUE_HOST_WAIT=1 "
                   "EDEN_NX2_BUDGET_HOLD_RES=1 EDEN_NX2_GPL=1 ");

    const std::string hz120 = names(PacingSwitches(PacingSet::Hz120, Arc140T));
    CHECK(hz120.find("EDEN_NX2_SHOWN_RATE=120 ") != std::string::npos);
    CHECK(hz120.find("EDEN_NX2_CADENCE=3 ") != std::string::npos);
    CHECK(hz120.find("EDEN_NX2_CADENCE_DIVISOR") == std::string::npos);
    CHECK(hz120.find("EDEN_NX2_BUDGET_CEILING=1 ") != std::string::npos);
    // Interpolated in-between frames: EXTRAPOLATE=1 (the shipped nx2_switches.txt) would turn
    // FLUID_INTERP off.
    CHECK(hz120.find("EDEN_NX2_EXTRAPOLATE=0 ") != std::string::npos);
    CHECK(hz120.find("EDEN_NX2_FLUID_INTERP=1 ") != std::string::npos);

    const std::string hz60 = names(PacingSwitches(PacingSet::Hz60, Arc140T));
    CHECK(hz60.find("EDEN_NX2_EXTRAPOLATE=1 ") != std::string::npos);
    CHECK(hz60.find("EDEN_NX2_SHOWN_RATE") == std::string::npos);
    CHECK(hz60.find("EDEN_NX2_GPL=1 ") != std::string::npos);
    const std::string amd60 =
        names(PacingSwitches(PacingSet::Hz60, {.vendor = GPUVendor::AMD, .integrated = true}));
    CHECK(amd60.find("EDEN_NX2_GPL") == std::string::npos);
#else
    // The sets exist on Windows only.
    CHECK(names(PacingSwitches(PacingSet::Hz60, Arc140T)).empty());
    CHECK(PacingSwitches(PacingSet::Hz120, Arc140T).empty());
    CHECK(PacingSwitches(PacingSet::Hz144, Arc140T).empty());
#endif
}

TEST_CASE("A switch default yields to the player's value, not to the shipped file's",
          "[core][game_presets]") {
    // Nothing set it.
    CHECK(DecideSwitch(std::nullopt, std::nullopt) == SwitchDecision::Set);
    // Only the shipped nx2_switches.txt set it.
    CHECK(DecideSwitch("1", "1") == SwitchDecision::ReplaceShipped);
    // The player set it: no shipped-only record, whatever the value (0 and empty included).
    CHECK(DecideSwitch("1", std::nullopt) == SwitchDecision::KeepPlayers);
    CHECK(DecideSwitch("0", std::nullopt) == SwitchDecision::KeepPlayers);
    CHECK(DecideSwitch("", std::nullopt) == SwitchDecision::KeepPlayers);
    // A shipped-only record whose value something has changed since is no longer the file's.
    CHECK(DecideSwitch("0", "1") == SwitchDecision::KeepPlayers);
    // A record with no value in the environment (removed since): nothing set it now.
    CHECK(DecideSwitch(std::nullopt, "1") == SwitchDecision::Set);
}

TEST_CASE("A switch default replaces only what the shipped switch file alone set",
          "[core][game_presets]") {
    using Common::SwitchOrigin::ShippedOnly;
    using Common::SwitchOrigin::SwitchMap;
    const SwitchMap shipped{{"EDEN_NX2_EXTRAPOLATE", "1"}, {"EDEN_NX2_SPLIT", "2"}};
    const auto nothing_in_environment = [](const std::string&) { return false; };
    // The decision for EDEN_NX2_EXTRAPOLATE as LoadNx2Switches and ApplySwitchDefaults reach it:
    // `merged` and `named_by_player` from the files, `environment` the real one at start, and the
    // value the switch then has in the environment.
    const auto decide = [&shipped](const SwitchMap& merged,
                                   const std::set<std::string>& named_by_player,
                                   const std::optional<std::string>& environment) {
        const SwitchMap record =
            ShippedOnly(shipped, merged, named_by_player,
                        [&environment](const std::string& name) {
                            return environment.has_value() && name == "EDEN_NX2_EXTRAPOLATE";
                        });
        const auto it = record.find("EDEN_NX2_EXTRAPOLATE");
        std::optional<std::string_view> shipped_only;
        if (it != record.end()) {
            shipped_only = it->second;
        }
        std::optional<std::string_view> current;
        if (environment) {
            current = *environment;
        } else if (const auto entry = merged.find("EDEN_NX2_EXTRAPOLATE");
                   entry != merged.end()) {
            current = entry->second;
        }
        return DecideSwitch(current, shipped_only);
    };

    // Nothing set: no file names it, the environment does not set it.
    CHECK(decide({{"EDEN_NX2_SPLIT", "2"}}, {}, std::nullopt) == SwitchDecision::Set);
    // Set by the shipped file only.
    CHECK(decide(shipped, {}, std::nullopt) == SwitchDecision::ReplaceShipped);
    // Set by the shipped file and overridden by the player's file, to another value or the same.
    CHECK(decide({{"EDEN_NX2_EXTRAPOLATE", "0"}, {"EDEN_NX2_SPLIT", "2"}},
                 {"EDEN_NX2_EXTRAPOLATE"}, std::nullopt) == SwitchDecision::KeepPlayers);
    CHECK(decide(shipped, {"EDEN_NX2_EXTRAPOLATE"}, std::nullopt) == SwitchDecision::KeepPlayers);
    // Set in the real environment, to another value or to the shipped file's.
    CHECK(decide(shipped, {}, "0") == SwitchDecision::KeepPlayers);
    CHECK(decide(shipped, {}, "1") == SwitchDecision::KeepPlayers);

    // The other shipped switch stays shipped-only when the player names a different one.
    const SwitchMap record =
        ShippedOnly(shipped, shipped, {"EDEN_NX2_EXTRAPOLATE"}, nothing_in_environment);
    const SwitchMap expected{{"EDEN_NX2_SPLIT", "2"}};
    CHECK(record == expected);
}

TEST_CASE("A game's read-back switch defaults are in effect from its first start",
          "[core][game_presets]") {
    using namespace VideoCore::AsyncReadback;
    constexpr std::array early{"EDEN_NX2_EARLY_SUBMIT", "EDEN_NX2_EARLY_PAGES",
                               "EDEN_NX2_EARLY_AFTER"};
    // The switch defaults go into this process's environment: only where none of them is set.
    for (const SwitchValue& item : SwitchDefaults(OcarinaTitleId, Arc140T)) {
        if (std::getenv(std::string{item.name}.c_str()) != nullptr) {
            SKIP("A switch of Ocarina of Time's defaults is set in the environment");
        }
    }
    for (const char* name : early) {
        if (std::getenv(name) != nullptr) {
            SKIP("A read-back switch is set in the environment");
        }
    }
    if (std::getenv("EDEN_NX2_NO_PRESETS") != nullptr) {
        SKIP("EDEN_NX2_NO_PRESETS is set");
    }
    SettingsScope scope;
    Settings::values.auto_presets.SetValue(true);
    Common::SwitchOrigin::RecordShippedOnly({});

    // VideoCore::CreateGPU reads them before the renderer knows the GPU.
    ReadSwitch();
    REQUIRE(EarlySubmit() == 0U);
    REQUIRE(EarlyPages() == 64U);
    REQUIRE(EarlyAfter() == 2);

    // The renderer's call: the first start runs with the game's defaults, as a later one does.
    void(VideoCore::ApplyGameSwitchDefaults(OcarinaTitleId, Arc140T));
#ifdef _WIN32
    CHECK(EarlySubmit() == 1U);
    CHECK(EarlyPages() == 256U);
    CHECK(EarlyAfter() == 1);
#else
    // The sets exist on Windows only.
    CHECK(EarlySubmit() == 0U);
    CHECK(EarlyPages() == 64U);
    CHECK(EarlyAfter() == 2);
#endif

    // Another game started next in the same process: CreateGPU reads what the first game's
    // defaults left in the environment, and this game's call puts back what they replaced.
    ReadSwitch();
    void(VideoCore::ApplyGameSwitchDefaults(0x0100152000022000, Arc140T));
    CHECK(EarlySubmit() == 0U);
    CHECK(EarlyPages() == 64U);
    CHECK(EarlyAfter() == 2);
    for (const SwitchValue& item : SwitchDefaults(OcarinaTitleId, Arc140T)) {
        CAPTURE(item.name);
        CHECK(std::getenv(std::string{item.name}.c_str()) == nullptr);
    }
}

TEST_CASE("The device class changes no preset or switch default yet", "[core][game_presets]") {
    const Preset* const preset = Find(Presets(), OcarinaTitleId, Arc140T);
    const std::span<const SwitchValue> switches = SwitchDefaults(OcarinaTitleId, Arc140T);
    for (const DeviceClass device_class : AllClasses) {
        GpuInfo gpu = Arc140T;
        gpu.device_class = device_class;
        CHECK(Find(Presets(), OcarinaTitleId, gpu) == preset);
        const std::span<const SwitchValue> with_class = SwitchDefaults(OcarinaTitleId, gpu);
        CHECK(with_class.data() == switches.data());
        CHECK(with_class.size() == switches.size());
        CHECK(ReportsFirstBoot(OcarinaTitleId, gpu));
    }
}

TEST_CASE("The GPU family changes no preset or switch default yet", "[core][game_presets]") {
    // No preset asks for a class or a family: each needs measured rows on that device first.
    for (const Preset& listed : Presets()) {
        CHECK(listed.gpu.device_class == DeviceClass::Unknown);
        CHECK(listed.gpu.gpu_family == GpuFamily::None);
    }
    const GpuInfo amd{.vendor = GPUVendor::AMD, .integrated = true};
    const GpuInfo nvidia{.vendor = GPUVendor::Nvidia};
    for (const GpuInfo& base : {Arc140T, amd, nvidia}) {
        const Preset* const preset = Find(Presets(), OcarinaTitleId, base);
        const std::span<const SwitchValue> switches = SwitchDefaults(OcarinaTitleId, base);
        const bool reports = ReportsFirstBoot(OcarinaTitleId, base);
        for (const GpuFamily gpu_family : AllFamilies()) {
            GpuInfo gpu = base;
            gpu.gpu_family = gpu_family;
            CHECK(Find(Presets(), OcarinaTitleId, gpu) == preset);
            const std::span<const SwitchValue> with_family = SwitchDefaults(OcarinaTitleId, gpu);
            CHECK(with_family.data() == switches.data());
            CHECK(with_family.size() == switches.size());
            CHECK(ReportsFirstBoot(OcarinaTitleId, gpu) == reports);
        }
    }
}

TEST_CASE("Ocarina of Time's first boot on an integrated GPU is reported", "[core][game_presets]") {
    REQUIRE(ReportsFirstBoot(OcarinaTitleId, Arc140T));
    REQUIRE(ReportsFirstBoot(OcarinaTitleId, {.vendor = GPUVendor::AMD, .integrated = true}));
    REQUIRE_FALSE(ReportsFirstBoot(OcarinaTitleId, {.vendor = GPUVendor::AMD}));
    REQUIRE_FALSE(ReportsFirstBoot(OcarinaTitleId,
                                   {.vendor = GPUVendor::Nvidia, .integrated = true}));
    REQUIRE_FALSE(ReportsFirstBoot(0x0400C3F00006E000, Arc140T));
}
