// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "common/logging.h"
#include "common/param_package.h"
#include "common/settings.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "input_common/main.h"
#include "lichii/configuration/controller_auto_configure.h"
#include "qt_common/config/qt_config.h"

namespace ControllerAutoConfigure {
namespace {

enum class PlayerState {
    Unmapped,
    KeyboardDefault,
    DisconnectedDevice,
    UserMapped,
};

bool IsSameDevice(const Common::ParamPackage& a, const Common::ParamPackage& b) {
    return a.Get("guid", "") == b.Get("guid", "") && a.Get("port", 0) == b.Get("port", 0);
}

bool LooksLikeJoycon(const std::string& display) {
    std::string lower = display;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.find("joy-con") != std::string::npos || lower.find("joycon") != std::string::npos;
}

PlayerState ClassifyPlayerOne(const Settings::PlayerInput& player,
                              const std::vector<Common::ParamPackage>& devices) {
    bool any_mapped = false;
    bool all_keyboard_default = true;
    bool any_sdl = false;
    bool all_sdl_disconnected = true;
    bool other = false;

    for (std::size_t i = 0; i < player.buttons.size(); ++i) {
        if (player.buttons[i].empty()) {
            continue;
        }
        const Common::ParamPackage param{player.buttons[i]};
        const std::string engine = param.Get("engine", "");
        if (engine.empty()) {
            continue;
        }
        any_mapped = true;
        if (engine == "keyboard") {
            all_keyboard_default =
                all_keyboard_default && param.Get("code", -1) == QtConfig::default_buttons[i];
        } else {
            all_keyboard_default = false;
            if (engine == "sdl") {
                any_sdl = true;
                const bool connected = std::any_of(
                    devices.begin(), devices.end(), [&](const Common::ParamPackage& device) {
                        return device.Get("engine", "") == "sdl" && IsSameDevice(device, param);
                    });
                all_sdl_disconnected = all_sdl_disconnected && !connected;
            } else {
                other = true;
            }
        }
    }

    if (!any_mapped) {
        return PlayerState::Unmapped;
    }
    if (all_keyboard_default) {
        return PlayerState::KeyboardDefault;
    }
    if (!other && any_sdl && all_sdl_disconnected) {
        return PlayerState::DisconnectedDevice;
    }
    return PlayerState::UserMapped;
}

std::optional<Common::ParamPackage> PickDevice(InputCommon::InputSubsystem& input_subsystem,
                                               const std::vector<Common::ParamPackage>& devices) {
    std::vector<Common::ParamPackage> candidates;
    for (const auto& device : devices) {
        // Dual JoyCon entries and single JoyCons are not Pro Controller candidates
        if (device.Get("engine", "") != "sdl" || device.Has("guid2") ||
            LooksLikeJoycon(device.Get("display", ""))) {
            continue;
        }
        candidates.push_back(device);
    }
    if (candidates.empty()) {
        return std::nullopt;
    }
    if (candidates.size() == 1) {
        return candidates.front();
    }
    const auto recent = input_subsystem.GetMostRecentDevice();
    for (const auto& candidate : candidates) {
        if (IsSameDevice(candidate, recent)) {
            return candidate;
        }
    }
    return std::nullopt;
}

void SwapParams(InputCommon::ButtonMapping& mapping, Settings::NativeButton::Values a,
                Settings::NativeButton::Values b) {
    auto it_a = mapping.find(a);
    auto it_b = mapping.find(b);
    if (it_a == mapping.end() || it_b == mapping.end()) {
        return;
    }
    std::swap(it_a->second, it_b->second);
}

} // Anonymous namespace

bool Run(InputCommon::InputSubsystem& input_subsystem, Core::HID::HIDCore& hid_core,
         const std::function<void()>& save) {
    if (!Settings::values.controller_auto_configure.GetValue()) {
        return false;
    }
    // Per-game input configuration is never rewritten
    if (!Settings::values.players.UsingGlobal()) {
        return false;
    }

    const auto devices = input_subsystem.GetInputDevices();
    auto& player = Settings::values.players.GetValue()[0];
    const auto state = ClassifyPlayerOne(player, devices);
    if (state == PlayerState::UserMapped) {
        return false;
    }

    const auto device = PickDevice(input_subsystem, devices);
    if (!device) {
        return false;
    }

    auto button_mapping = input_subsystem.GetButtonMappingForDevice(*device);
    auto analog_mapping = input_subsystem.GetAnalogMappingForDevice(*device);
    if (button_mapping.empty() || analog_mapping.empty()) {
        // Not usable as a gamepad; leave the current mapping alone
        return false;
    }
    auto motion_mapping = input_subsystem.GetMotionMappingForDevice(*device);
    const bool has_motion = !motion_mapping.empty() && input_subsystem.DeviceHasMotion(*device);

    if (!Settings::values.controller_positional_face_buttons.GetValue()) {
        // Label layout: the south button is A, like Xbox/PlayStation glyphs
        SwapParams(button_mapping, Settings::NativeButton::A, Settings::NativeButton::B);
        SwapParams(button_mapping, Settings::NativeButton::X, Settings::NativeButton::Y);
    }

    for (auto& button : player.buttons) {
        button.clear();
    }
    for (auto& analog : player.analogs) {
        analog.clear();
    }
    for (auto& motion : player.motions) {
        motion.clear();
    }
    for (const auto& [index, param] : button_mapping) {
        player.buttons[static_cast<std::size_t>(index)] = param.Serialize();
    }
    for (const auto& [index, param] : analog_mapping) {
        player.analogs[static_cast<std::size_t>(index)] = param.Serialize();
    }
    if (has_motion) {
        for (const auto& [index, param] : motion_mapping) {
            player.motions[static_cast<std::size_t>(index)] = param.Serialize();
        }
    }
    player.connected = true;
    player.controller_type = Settings::ControllerType::ProController;
    player.vibration_enabled = true;
    if (player.vibration_strength <= 0) {
        player.vibration_strength = 100;
    }

    if (auto* controller = hid_core.GetEmulatedController(Core::HID::NpadIdType::Player1)) {
        controller->ReloadFromSettings();
    }

    LOG_INFO(Input, "Controller auto-configured: {} ({}) as player 1",
             device->Get("display", ""), device->Get("guid", ""));

    if (save) {
        save();
    }
    return true;
}

void Install(InputCommon::InputSubsystem& input_subsystem, Core::HID::HIDCore& hid_core,
             std::function<void()> save) {
    input_subsystem.SetDeviceAddedCallback(
        [&input_subsystem, &hid_core, save = std::move(save)]() {
            Run(input_subsystem, hid_core, save);
        });
}

} // namespace ControllerAutoConfigure
