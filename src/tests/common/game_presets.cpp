// SPDX-License-Identifier: GPL-3.0-or-later

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <catch2/catch_test_macros.hpp>
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
