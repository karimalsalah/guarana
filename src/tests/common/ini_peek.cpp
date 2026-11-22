// SPDX-FileCopyrightText: Copyright 2026 Guarana contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <optional>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "common/ini_peek.h"

using Common::IniBoolSetting;
using Common::IniValue;

namespace {

// As the configuration writes the file: other sections around, a key whose name starts with the
// one looked for, and the `\default` line before each value.
constexpr const char* Config = "[Renderer]\r\n"
                               "perform_vulkan_check=false\r\n"
                               "\r\n"
                               "[Debugging]\r\n"
                               "perform_vulkan_check_extra\\default=false\r\n"
                               "perform_vulkan_check_extra=true\r\n"
                               "perform_vulkan_check\\default=false\r\n"
                               "perform_vulkan_check=false\r\n"
                               "record_frame_times=true\r\n"
                               "\r\n"
                               "[UI]\r\n"
                               "fullscreen\\default=true\r\n"
                               "fullscreen=false\r\n";

} // Anonymous namespace

TEST_CASE("IniValue reads a key of its own section only", "[ini_peek]") {
    REQUIRE(IniValue(Config, "Debugging", "perform_vulkan_check") == std::optional<std::string>{"false"});
    REQUIRE(IniValue(Config, "Debugging", "perform_vulkan_check\\default") ==
            std::optional<std::string>{"false"});
    REQUIRE(IniValue(Config, "Debugging", "record_frame_times") == std::optional<std::string>{"true"});
    REQUIRE_FALSE(IniValue(Config, "Debugging", "fullscreen").has_value());
    REQUIRE_FALSE(IniValue(Config, "Debug", "perform_vulkan_check").has_value());
    REQUIRE_FALSE(IniValue(Config, "Debugging", "perform_vulkan").has_value());
    REQUIRE_FALSE(IniValue("", "Debugging", "perform_vulkan_check").has_value());
    // Line ends without a carriage return, and a last line without one
    REQUIRE(IniValue("[UI]\nconfirmStop=2", "UI", "confirmStop") == std::optional<std::string>{"2"});
}

TEST_CASE("IniBoolSetting follows the default flag as the configuration does", "[ini_peek]") {
    REQUIRE_FALSE(IniBoolSetting(Config, "Debugging", "perform_vulkan_check", true));
    // `\default=true`: the stored value does not count
    REQUIRE(IniBoolSetting(Config, "UI", "fullscreen", true));
    REQUIRE_FALSE(IniBoolSetting(Config, "UI", "fullscreen", false));
    // No `\default` line, or no file at all: the default
    REQUIRE(IniBoolSetting(Config, "Debugging", "record_frame_times", true));
    REQUIRE(IniBoolSetting("", "Debugging", "perform_vulkan_check", true));
    REQUIRE(IniBoolSetting("[Debugging]\nperform_vulkan_check\\default=false\nperform_vulkan_check=true\n",
                           "Debugging", "perform_vulkan_check", false));
    // A value other than true is false, as Setting<bool>::LoadString reads it
    REQUIRE_FALSE(IniBoolSetting("[Debugging]\nperform_vulkan_check\\default=false\nperform_vulkan_check=yes\n",
                                 "Debugging", "perform_vulkan_check", true));
}
