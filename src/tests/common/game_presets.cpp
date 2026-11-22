// SPDX-License-Identifier: GPL-3.0-or-later

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <catch2/catch_test_macros.hpp>
#include "common/game_presets.h"
#include "common/settings.h"
#include "common/switch_origin.h"

TEST_CASE("Display filter defaults use ScaleForce and FXAA", "[game_presets]") {
    auto settings = std::make_unique<Settings::Values>();
    CHECK(settings->scaling_filter.GetDefault() == Settings::ScalingFilter::ScaleForce);
    CHECK(settings->anti_aliasing.GetDefault() == Settings::AntiAliasing::Fxaa);
}

TEST_CASE("Switches from the shipped switch file alone are told apart", "[game_presets]") {
    using Common::SwitchOrigin::ShippedOnly;
    using Common::SwitchOrigin::SwitchMap;
    const auto none = [](const std::string&) { return false; };
    const SwitchMap shipped{{"EDEN_NX2_EXTRAPOLATE", "1"}, {"EDEN_NX2_SPLIT", "2"}};

    // Nothing but the shipped file: all of it.
    CHECK(ShippedOnly(shipped, shipped, {}, none) == shipped);
    // No shipped file: nothing.
    const SwitchMap players{{"EDEN_NX2_EXTRAPOLATE", "0"}};
    CHECK(ShippedOnly({}, players, {"EDEN_NX2_EXTRAPOLATE"}, none).empty());

    // A player's file that names a switch takes it, to another value or to the shipped one.
    const SwitchMap overridden{{"EDEN_NX2_EXTRAPOLATE", "0"}, {"EDEN_NX2_SPLIT", "2"}};
    const SwitchMap split_only{{"EDEN_NX2_SPLIT", "2"}};
    CHECK(ShippedOnly(shipped, overridden, {"EDEN_NX2_EXTRAPOLATE"}, none) == split_only);
    CHECK(ShippedOnly(shipped, shipped, {"EDEN_NX2_EXTRAPOLATE"}, none) == split_only);

    // The real environment takes it, whatever its value.
    const auto extrapolate_set = [](const std::string& name) {
        return name == "EDEN_NX2_EXTRAPOLATE";
    };
    CHECK(ShippedOnly(shipped, shipped, {}, extrapolate_set) == split_only);

    // A name the loader's filter dropped is not in the environment at all.
    const SwitchMap with_host{{"EDEN_STREAMLINE_DIR", "C:\\sl"}, {"EDEN_NX2_SPLIT", "2"}};
    CHECK(ShippedOnly(with_host, split_only, {}, none) == split_only);
}

TEST_CASE("The shipped-only record answers by name", "[game_presets]") {
    using Common::SwitchOrigin::RecordShippedOnly;
    using Common::SwitchOrigin::ShippedOnlyValue;
    RecordShippedOnly({{"EDEN_NX2_EXTRAPOLATE", "1"}});
    CHECK(ShippedOnlyValue("EDEN_NX2_EXTRAPOLATE") == std::optional<std::string>{"1"});
    CHECK_FALSE(ShippedOnlyValue("EDEN_NX2_SPLIT").has_value());
    // A frontend that reads no switch file leaves it empty: every switch is the player's.
    RecordShippedOnly({});
    CHECK_FALSE(ShippedOnlyValue("EDEN_NX2_EXTRAPOLATE").has_value());
}

TEST_CASE("Names the process set itself are kept for the process started in its place",
          "[game_presets]") {
    using Common::SwitchOrigin::RecordSetByProcess;
    using Common::SwitchOrigin::SetByProcess;
    // Not a switch's name, so the record this leaves behind changes nothing.
    const std::string name{"GUARANA_TEST_SET_BY_PROCESS"};
    CHECK_FALSE(SetByProcess().contains(name));
    RecordSetByProcess(name);
    RecordSetByProcess(name);
    CHECK(SetByProcess().contains(name));
    CHECK(SetByProcess().count(name) == 1);
}

TEST_CASE("A game override keeps CPU inner invalidation on despite the skip hack",
          "[game_presets]") {
    auto& values = Settings::values;
    const bool skip = values.skip_cpu_inner_invalidation.GetValue();
    const bool keep = values.keep_cpu_inner_invalidation;
    values.skip_cpu_inner_invalidation.SetValue(true);
    values.keep_cpu_inner_invalidation = false;
    CHECK(Settings::SkipCpuInnerInvalidation());
    values.keep_cpu_inner_invalidation = true;
    CHECK_FALSE(Settings::SkipCpuInnerInvalidation());
    values.skip_cpu_inner_invalidation.SetValue(skip);
    values.keep_cpu_inner_invalidation = keep;
}

TEST_CASE("Game tweaks refer to valid per-game settings", "[game_presets]") {
    auto settings = std::make_unique<Settings::Values>();
    for (const auto& [program_id, keys] : Settings::game_tweaks) {
        INFO(program_id);
        REQUIRE(Settings::FindGameTweaks(program_id) == &keys);
        for (const auto& key : keys) {
            INFO(key);
            const auto it = settings->linkage.by_key.find(key);
            REQUIRE(it != settings->linkage.by_key.end());
            CHECK(it->second->Switchable());
            CHECK(Settings::IsGameTweak(key));
        }
    }
    CHECK_FALSE(Settings::IsGameTweak("resolution_setup"));
    CHECK(Settings::FindGameTweaks(0) == nullptr);
}

TEST_CASE("A game tweak belongs to its game only", "[game_presets]") {
    constexpr u64 OcarinaProgramId = 0x0400486024DEC000ULL;
    using Settings::HasGameTweak;
    using Settings::HyruleWarriorsProgramId;
    using Settings::MarioKartWorldProgramId;

    for (const auto& [program_id, keys] : Settings::game_tweaks) {
        for (const auto& key : keys) {
            INFO(key);
            CHECK(HasGameTweak(program_id, key));
            // Another game, and a file without a program id.
            CHECK_FALSE(HasGameTweak(OcarinaProgramId, key));
            CHECK_FALSE(HasGameTweak(0, key));
        }
    }
    CHECK(HasGameTweak(MarioKartWorldProgramId, "mkw_60fps"));
    CHECK_FALSE(HasGameTweak(HyruleWarriorsProgramId, "mkw_60fps"));
    CHECK(HasGameTweak(HyruleWarriorsProgramId, "hyrule_30fps"));
    CHECK_FALSE(HasGameTweak(MarioKartWorldProgramId, "hyrule_30fps"));
    // A setting for every game is nobody's tweak.
    CHECK_FALSE(HasGameTweak(MarioKartWorldProgramId, "use_video_framerate"));
}

TEST_CASE("Every Mario Kart World and Hyrule Warriors option is a game tweak", "[game_presets]") {
    // A new option named after one of these games needs a row in game_tweaks, or it is listed
    // for every game.
    const auto settings = std::make_unique<Settings::Values>();
    for (const auto& [key, setting] : settings->linkage.by_key) {
        if (key.starts_with("mkw_") || key.starts_with("hyrule_")) {
            INFO(key);
            CHECK(Settings::IsGameTweak(key));
            CHECK(setting->GetCategory() == Settings::Category::RendererAdvanced);
        }
    }
}
