// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>

namespace Core::HID {
class HIDCore;
}

namespace InputCommon {
class InputSubsystem;
}

namespace ControllerAutoConfigure {

/// Maps player 1 (Pro Controller) to a connected SDL gamepad when player 1 is unmapped, uses the
/// default keyboard mapping or uses an SDL device that is not connected. Never touches a mapping
/// that points at a connected device or at a non-SDL engine. Returns true when it changed player 1.
/// `save` is called after a change so the caller can persist the config.
bool Run(InputCommon::InputSubsystem& input_subsystem, Core::HID::HIDCore& hid_core,
         const std::function<void()>& save);

/// Runs once on the next input pump and again every time a gamepad is attached or re-attached.
/// The references must outlive the input subsystem's callback (i.e. the main window).
void Install(InputCommon::InputSubsystem& input_subsystem, Core::HID::HIDCore& hid_core,
             std::function<void()> save);

} // namespace ControllerAutoConfigure
