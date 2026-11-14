// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/game_presets.h"

namespace {

using namespace Core::GamePresets;
using Core::GameSettings::GPUVendor;

constexpr u64 OcarinaTitleId = 0x0400486024DEC000;
constexpr u64 OtherTitleId = 0x0100152000022000;
constexpr GpuInfo Arc140T{.vendor = GPUVendor::Intel,
                          .integrated = true,
                          .software_tensor_math = true};

struct Combo {
    u64 title_id{};
    GPUVendor vendor{};
    bool integrated{};
    bool software_tensor_math{};
    double refresh_hz{};
};

// Every title, vendor, GPU kind and display rate that main's presets and pacing sets tell apart:
// the edges of the 120 and 144 Hz ranges, the fractional rates a display path reports, and a rate
// the host did not report.
std::vector<Combo> AllCombos() {
    std::vector<Combo> combos;
    for (const u64 title : {OcarinaTitleId, OtherTitleId}) {
        for (const GPUVendor vendor :
             {GPUVendor::Unknown, GPUVendor::Intel, GPUVendor::AMD, GPUVendor::Nvidia}) {
            for (const bool integrated : {false, true}) {
                for (const bool tensor : {false, true}) {
                    for (const double hz : {0.0, 1.0, 24.0, 59.94, 60.0, 100.0, 114.4, 115.0, 119.88,
                                            120.0, 125.4, 125.5, 139.4, 139.5, 143.98, 144.0, 148.4,
                                            148.5, 240.0}) {
                        combos.push_back({title, vendor, integrated, tensor, hz});
                    }
                }
            }
        }
    }
    return combos;
}

// The set main chooses, written out from the documented rule and not from the code under test.
PacingSet ExpectedSet(const Combo& combo) {
    if (combo.title_id != OcarinaTitleId || !combo.integrated) {
        return PacingSet::None;
    }
    if (combo.vendor == GPUVendor::AMD) {
        return PacingSet::Hz60;
    }
    if (combo.vendor != GPUVendor::Intel) {
        return PacingSet::None;
    }
    const u32 hz = combo.refresh_hz >= 1.5 ? static_cast<u32>(combo.refresh_hz + 0.5) : 0U;
    if (hz >= 115 && hz <= 125) {
        return PacingSet::Hz120;
    }
    if (hz >= 140 && hz <= 148) {
        return PacingSet::Hz144;
    }
    return PacingSet::Hz60;
}

// The settings of the Ocarina of Time preset on an Intel integrated GPU without matrix cores, as
// main has them.
std::vector<std::pair<std::string_view, std::string_view>> ExpectedOcarinaSettings() {
    return {
        {"oot_anti_aliasing", "1"},
        {"ocarina_fsr", "false"},
#ifdef _WIN32
        {"frame_gen", "true"},
        {"frame_gen_multiplier", "3"},
        {"frame_gen_target_rate", "60"},
#else
        {"frame_gen", "false"},
#endif
        {"vram_usage_mode", "1"},
        {"frame_pacing_mode", "0"},
    };
}

ResolveInput InputOf(const Combo& combo) {
    ResolveInput input;
    input.title_id = combo.title_id;
    input.gpu = {.vendor = combo.vendor,
                 .integrated = combo.integrated,
                 .software_tensor_math = combo.software_tensor_math};
    input.display.refresh_hz = combo.refresh_hz;
    return input;
}

std::vector<std::pair<std::string, std::string>> SettingPairs(const Resolution& resolution) {
    std::vector<std::pair<std::string, std::string>> pairs;
    for (const SettingEntry& entry : resolution.settings) {
        pairs.emplace_back(std::string{entry.label}, std::string{entry.value});
    }
    return pairs;
}

std::vector<std::pair<std::string, std::string>> SwitchPairs(const Resolution& resolution) {
    std::vector<std::pair<std::string, std::string>> pairs;
    for (const SwitchEntry& entry : resolution.switches) {
        pairs.emplace_back(std::string{entry.name}, std::string{entry.value});
    }
    return pairs;
}

const SettingEntry* FindSetting(const Resolution& resolution, std::string_view label) {
    const auto it = std::ranges::find(resolution.settings, label, &SettingEntry::label);
    return it != resolution.settings.end() ? &*it : nullptr;
}

[[maybe_unused]] const SwitchEntry* FindSwitch(const Resolution& resolution, std::string_view name) {
    const auto it = std::ranges::find(resolution.switches, name, &SwitchEntry::name);
    return it != resolution.switches.end() ? &*it : nullptr;
}

} // namespace

TEST_CASE("The resolver gives today's values for every title, GPU and display rate",
          "[core][game_resolver]") {
    for (const Combo& combo : AllCombos()) {
        CAPTURE(combo.title_id, static_cast<int>(combo.vendor), combo.integrated,
                combo.software_tensor_math, combo.refresh_hz);
        const Resolution resolution = Resolve(InputOf(combo));

        // The pacing set and its switches are main's: ChoosePacingSet and PacingSwitches.
        const PacingSet expected_set = ExpectedSet(combo);
        CHECK(resolution.pacing == expected_set);
        const GpuInfo gpu = InputOf(combo).gpu;
        const std::span<const SwitchValue> expected_switches = PacingSwitches(expected_set, gpu);
        REQUIRE(resolution.switches.size() == expected_switches.size());
        for (size_t i = 0; i < expected_switches.size(); ++i) {
            CHECK(resolution.switches[i].name == expected_switches[i].name);
            CHECK(resolution.switches[i].value == expected_switches[i].value);
        }

        // The settings preset applies to Ocarina of Time on an Intel integrated GPU without
        // matrix cores, and to nothing else.
        const bool preset = combo.title_id == OcarinaTitleId && combo.vendor == GPUVendor::Intel &&
                            combo.integrated && combo.software_tensor_math;
        if (preset) {
            const auto expected = ExpectedOcarinaSettings();
            REQUIRE(resolution.settings.size() == expected.size());
            for (size_t i = 0; i < expected.size(); ++i) {
                CHECK(resolution.settings[i].label == expected[i].first);
                CHECK(resolution.settings[i].value == expected[i].second);
            }
            REQUIRE(resolution.addons.size() == 1);
            CHECK(resolution.addons[0].name == "Max Resolution (built-in)");
            CHECK(resolution.preset_reason ==
                  "Ocarina of Time on an Intel integrated GPU without matrix cores");
        } else {
            CHECK(resolution.settings.empty());
            CHECK(resolution.addons.empty());
            CHECK(resolution.preset_reason.empty());
        }
    }
}

TEST_CASE("The resolver's switches are the documented sets", "[core][game_resolver]") {
#ifdef _WIN32
    const auto resolve_set = [](GPUVendor vendor, double hz) {
        ResolveInput input;
        input.title_id = OcarinaTitleId;
        input.gpu = {.vendor = vendor, .integrated = true};
        input.display.refresh_hz = hz;
        return Resolve(input);
    };
    const Resolution intel60 = resolve_set(GPUVendor::Intel, 60.0);
    CHECK(intel60.switches.size() == 10);
    REQUIRE(FindSwitch(intel60, "EDEN_NX2_GPL") != nullptr);
    CHECK(FindSwitch(intel60, "EDEN_NX2_GPL")->value == "1");
    CHECK(FindSwitch(intel60, "EDEN_NX2_EXTRAPOLATE")->value == "1");

    const Resolution amd = resolve_set(GPUVendor::AMD, 144.0);
    CHECK(amd.pacing == PacingSet::Hz60);
    CHECK(amd.switches.size() == 9);
    CHECK(FindSwitch(amd, "EDEN_NX2_GPL") == nullptr);

    const Resolution intel120 = resolve_set(GPUVendor::Intel, 120.0);
    CHECK(intel120.switches.size() == 14);
    CHECK(FindSwitch(intel120, "EDEN_NX2_SHOWN_RATE")->value == "120");
    CHECK(FindSwitch(intel120, "EDEN_NX2_BUDGET_CEILING")->value == "1");
    CHECK(FindSwitch(intel120, "EDEN_NX2_EXTRAPOLATE")->value == "0");

    const Resolution intel144 = resolve_set(GPUVendor::Intel, 143.98);
    CHECK(intel144.pacing == PacingSet::Hz144);
    CHECK(intel144.switches.size() == 14);
    CHECK(FindSwitch(intel144, "EDEN_NX2_SHOWN_RATE")->value == "144");
    CHECK(FindSwitch(intel144, "EDEN_NX2_CADENCE_DIVISOR")->value == "6");
    CHECK(FindSwitch(intel144, "EDEN_NX2_BUDGET_CEILING") == nullptr);
#else
    // The sets exist on Windows only: the same set is chosen and no switch is resolved.
    ResolveInput input;
    input.title_id = OcarinaTitleId;
    input.gpu = Arc140T;
    input.display.refresh_hz = 144.0;
    const Resolution resolution = Resolve(input);
    CHECK(resolution.pacing == PacingSet::Hz144);
    CHECK(resolution.switches.empty());
#endif
}

TEST_CASE("The output size and the GPU's core count change no resolved value",
          "[core][game_resolver]") {
    for (const Combo& combo : AllCombos()) {
        CAPTURE(combo.title_id, static_cast<int>(combo.vendor), combo.integrated,
                combo.software_tensor_math, combo.refresh_hz);
        const Resolution base = Resolve(InputOf(combo));
        ResolveInput bigger = InputOf(combo);
        bigger.display.width_px = 2880;
        bigger.display.height_px = 1800;
        bigger.gpu.cores = 4;
        bigger.gpu.cores_unit = "Xe3 cores";
        const Resolution with_size = Resolve(bigger);
        CHECK(SettingPairs(with_size) == SettingPairs(base));
        CHECK(SwitchPairs(with_size) == SwitchPairs(base));
        CHECK(with_size.pacing == base.pacing);
        CHECK(with_size.addons.size() == base.addons.size());
        // The inputs are recorded.
        CHECK(with_size.inputs.output_width == 2880);
        CHECK(with_size.inputs.output_height == 1800);
        CHECK(with_size.inputs.gpu_cores == 4);
        CHECK(with_size.inputs.pixels_per_core == 1296000);
        CHECK(base.inputs.pixels_per_core == 0);
    }
}

TEST_CASE("Output pixels per GPU core", "[core][game_resolver]") {
    CHECK(PixelsPerGpuCore(2880, 1800, 4) == 1296000);
    CHECK(PixelsPerGpuCore(1920, 1080, 12) == 172800);
    // Unknown on either side: no figure.
    CHECK(PixelsPerGpuCore(0, 1800, 4) == 0);
    CHECK(PixelsPerGpuCore(2880, 0, 4) == 0);
    CHECK(PixelsPerGpuCore(2880, 1800, 0) == 0);
    // No overflow in 32 bits.
    CHECK(PixelsPerGpuCore(7680, 4320, 1) == u64{7680} * 4320);
}

TEST_CASE("Every resolved value has a reason and evidence", "[core][game_resolver]") {
    for (const Combo& combo : AllCombos()) {
        CAPTURE(combo.title_id, static_cast<int>(combo.vendor), combo.integrated,
                combo.software_tensor_math, combo.refresh_hz);
        const Resolution resolution = Resolve(InputOf(combo));
        CHECK_FALSE(resolution.pacing_why.empty());
        for (const SettingEntry& entry : resolution.settings) {
            CAPTURE(entry.label);
            CHECK_FALSE(entry.reason.empty());
            CHECK_FALSE(entry.evidence.where.empty());
            // The preset has no paired run behind it (docs/switches.md, EDEN_NX2_NO_PRESETS).
            CHECK_FALSE(entry.evidence.measured);
        }
        for (const AddonEntry& entry : resolution.addons) {
            CHECK_FALSE(entry.reason.empty());
            CHECK_FALSE(entry.evidence.where.empty());
            CHECK_FALSE(entry.evidence.measured);
        }
        for (const SwitchEntry& entry : resolution.switches) {
            CAPTURE(entry.name);
            CHECK_FALSE(entry.reason.empty());
            CHECK_FALSE(entry.evidence.where.empty());
            // The sets were measured on Intel; AMD's 60 Hz set was not.
            CHECK(entry.evidence.measured == (combo.vendor == GPUVendor::Intel));
        }
    }
    for (const Preset& preset : Presets()) {
        CHECK_FALSE(preset.reason.empty());
    }
}

TEST_CASE("The resolver decides by what the user chose", "[core][game_resolver]") {
    ResolveInput input;
    input.title_id = OcarinaTitleId;
    input.gpu = Arc140T;
    input.user.setting = [](std::string_view label) {
        SettingState state;
        if (label == "oot_anti_aliasing") {
            state = {true, true, "1", "1", "3"}; // the game's own configuration says 3
        } else if (label == "ocarina_fsr") {
            state = {true, false, "true", "false", "true"}; // the global value was changed
        } else if (label == "vram_usage_mode") {
            state = {true, false, "1", "0", "1"}; // already the preset's
        } else if (label == "frame_pacing_mode") {
            state = {true, false, "2", "2", "2"}; // at its default
        }
        return state;
    };
    input.user.addon_disabled = [](u64 title_id, std::string_view addon) {
        return title_id == OcarinaTitleId && addon == "Max Resolution (built-in)";
    };
    const Resolution resolution = Resolve(input);

    const SettingEntry* per_game = FindSetting(resolution, "oot_anti_aliasing");
    REQUIRE(per_game != nullptr);
    CHECK(per_game->decision == Decision::UserPerGame);
    CHECK(per_game->current == "3");
    const SettingEntry* global = FindSetting(resolution, "ocarina_fsr");
    REQUIRE(global != nullptr);
    CHECK(global->decision == Decision::UserGlobal);
    CHECK(global->current == "true");
    const SettingEntry* already = FindSetting(resolution, "vram_usage_mode");
    REQUIRE(already != nullptr);
    CHECK(already->decision == Decision::AlreadySet);
    const SettingEntry* fresh = FindSetting(resolution, "frame_pacing_mode");
    REQUIRE(fresh != nullptr);
    CHECK(fresh->decision == Decision::Apply);
    CHECK(fresh->available);
    CHECK(fresh->current.empty());
    // A setting the function does not know is not available.
    const SettingEntry* unknown = FindSetting(resolution, "frame_gen");
    REQUIRE(unknown != nullptr);
    CHECK_FALSE(unknown->available);
    // The user's disable of an add-on is kept and reported.
    REQUIRE(resolution.addons.size() == 1);
    CHECK(resolution.addons[0].disabled);

    // Nothing chosen: every setting at its default.
    ResolveInput untouched;
    untouched.title_id = OcarinaTitleId;
    untouched.gpu = Arc140T;
    const Resolution plain = Resolve(untouched);
    for (const SettingEntry& entry : plain.settings) {
        CHECK(entry.available);
        CHECK(entry.decision == Decision::Apply);
    }
    CHECK_FALSE(plain.addons[0].disabled);

    // Presets off: the values are still resolved, and the resolution says they are off.
    ResolveInput off = untouched;
    off.user.presets_enabled = false;
    const Resolution resolved_off = Resolve(off);
    CHECK_FALSE(resolved_off.presets_enabled);
    CHECK(SettingPairs(resolved_off) == SettingPairs(plain));

#ifdef _WIN32
    const std::map<std::string, std::string, std::less<>> environment{
        {"EDEN_NX2_FLUID", "0"}, {"EDEN_NX2_EXTRAPOLATE", "1"}};
    const std::map<std::string, std::string, std::less<>> shipped{
        {"EDEN_NX2_EXTRAPOLATE", "1"}, {"EDEN_NX2_CADENCE", "3"}};
    const auto lookup = [](const std::map<std::string, std::string, std::less<>>& values) {
        return [&values](std::string_view name) -> std::optional<std::string> {
            const auto it = values.find(name);
            if (it == values.end()) {
                return std::nullopt;
            }
            return it->second;
        };
    };
    input.display.refresh_hz = 144.0;
    input.user.environment = lookup(environment);
    input.user.shipped_only = lookup(shipped);
    const Resolution switches = Resolve(input);
    REQUIRE(switches.pacing == PacingSet::Hz144);
    // Set by the player, to anything: kept.
    const SwitchEntry* fluid = FindSwitch(switches, "EDEN_NX2_FLUID");
    REQUIRE(fluid != nullptr);
    CHECK(fluid->decision == SwitchDecision::KeepPlayers);
    CHECK(fluid->player_value == "0");
    CHECK_FALSE(fluid->shipped.has_value());
    // Only the shipped file set it: replaced.
    const SwitchEntry* extrapolate = FindSwitch(switches, "EDEN_NX2_EXTRAPOLATE");
    REQUIRE(extrapolate != nullptr);
    CHECK(extrapolate->decision == SwitchDecision::ReplaceShipped);
    CHECK(extrapolate->value == "0");
    CHECK(extrapolate->shipped == std::optional<std::string>{"1"});
    // Nothing set it: set, with the shipped value (unused by the environment) on record.
    const SwitchEntry* cadence = FindSwitch(switches, "EDEN_NX2_CADENCE");
    REQUIRE(cadence != nullptr);
    CHECK(cadence->decision == SwitchDecision::Set);
    const SwitchEntry* gpl = FindSwitch(switches, "EDEN_NX2_GPL");
    REQUIRE(gpl != nullptr);
    CHECK(gpl->decision == SwitchDecision::Set);
    CHECK_FALSE(gpl->shipped.has_value());
#endif
}

TEST_CASE("The resolution is described in one line", "[core][game_resolver]") {
    ResolveInput input;
    input.title_id = OcarinaTitleId;
    input.gpu = Arc140T;
    input.display = {.refresh_hz = 144.0,
                     .name = "\\\\.\\DISPLAY1",
                     .primary = true,
                     .width_px = 2880,
                     .height_px = 1800};
    const std::string without_cores = DescribeResolution(Resolve(input));
    CHECK(without_cores.find('\n') == std::string::npos);
    CHECK(without_cores.find("title 0400486024DEC000") != std::string::npos);
    CHECK(without_cores.find("Intel integrated GPU without matrix cores") != std::string::npos);
    CHECK(without_cores.find("output 2880x1800") != std::string::npos);
    CHECK(without_cores.find("GPU cores not reported by the driver") != std::string::npos);
    CHECK(without_cores.find("oot_anti_aliasing=1 (set)") != std::string::npos);
    CHECK(without_cores.find("(0 of ") != std::string::npos);
    CHECK(without_cores.find("144 Hz") != std::string::npos);

    input.gpu.cores = 4;
    input.gpu.cores_unit = "compute units";
    const std::string with_cores = DescribeResolution(Resolve(input));
    CHECK(with_cores.find("GPU cores 4 compute units, 1296000 output pixels per core") !=
          std::string::npos);

    // No preset, no switches, no size.
    ResolveInput other;
    other.title_id = OtherTitleId;
    other.gpu = Arc140T;
    const std::string nothing = DescribeResolution(Resolve(other));
    CHECK(nothing.find("settings: no preset") != std::string::npos);
    CHECK(nothing.find("switches: none") != std::string::npos);
    CHECK(nothing.find("output not reported") != std::string::npos);

    ResolveInput off = input;
    off.user.presets_enabled = false;
    const std::string off_text = DescribeResolution(Resolve(off));
    CHECK(off_text.find("automatic presets off") != std::string::npos);
    // Nothing is applied with the presets off, and the line does not say "set".
    CHECK(off_text.find("oot_anti_aliasing=1 (not used)") != std::string::npos);
    CHECK(off_text.find("(set)") == std::string::npos);
}

TEST_CASE("The names of the decisions are distinct", "[core][game_resolver]") {
    CHECK(DecisionName(Decision::Apply) == "apply");
    CHECK(DecisionName(Decision::AlreadySet) == "already_set");
    CHECK(DecisionName(Decision::UserPerGame) == "user_per_game");
    CHECK(DecisionName(Decision::UserGlobal) == "user_global");
    CHECK(SwitchDecisionName(SwitchDecision::Set) == "set");
    CHECK(SwitchDecisionName(SwitchDecision::ReplaceShipped) == "replace_shipped");
    CHECK(SwitchDecisionName(SwitchDecision::KeepPlayers) == "keep_players");
    CHECK(PacingSetName(PacingSet::None) == "none");
    CHECK(PacingSetName(PacingSet::Hz60) == "60hz");
    CHECK(PacingSetName(PacingSet::Hz120) == "120hz");
    CHECK(PacingSetName(PacingSet::Hz144) == "144hz");
    CHECK(VendorName(GPUVendor::Intel) == "Intel");
    CHECK(VendorName(GPUVendor::AMD) == "AMD");
    CHECK(VendorName(GPUVendor::Nvidia) == "NVIDIA");
}

TEST_CASE("The boot resolution is recorded for the debug server", "[core][game_resolver]") {
    // A title with no preset and no switches: nothing in the settings or the environment changes.
    GpuInfo gpu = Arc140T;
    gpu.cores = 4;
    gpu.cores_unit = "Xe3 cores";
    const DisplayInfo display{.refresh_hz = 144.0,
                              .name = {},
                              .primary = true,
                              .width_px = 2880,
                              .height_px = 1800};
    ApplySwitchDefaults(OtherTitleId, gpu, display);
    Apply(OtherTitleId, gpu);
    const std::optional<Resolution> last = LastResolution();
    REQUIRE(last.has_value());
    CHECK(last->title_id == OtherTitleId);
    CHECK(last->pacing == PacingSet::None);
    CHECK(last->switches.empty());
    CHECK(last->settings.empty());
    CHECK(last->preset_reason.empty());
    CHECK(last->inputs.output_width == 2880);
    CHECK(last->inputs.output_height == 1800);
    CHECK(last->inputs.gpu_cores == 4);
    CHECK(last->inputs.pixels_per_core == 1296000);
    // The settings stage knows what the switch stage does not: the guest's tensor math.
    CHECK(last->inputs.software_tensor_math);
}
