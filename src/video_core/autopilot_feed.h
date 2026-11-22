// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <vector>

#include "common/common_types.h"

/// What the renderer tells the autopilot (EDEN_NX2_AUTOPILOT=1), and what the autopilot tells the
/// renderer. Every call is a relaxed atomic access: the present threads pay one load a frame
/// while the autopilot is off.
namespace VideoCore::AutopilotFeed {

/// The session turns the feed on at game boot and off at stop.
void SetActive(bool active);
[[nodiscard]] bool Active();

/// A present thread, once for every frame that reached the swapchain (generated ones included).
void NoteShown();

/// The intervals in ms between the frames shown since the last call, oldest first. One caller.
[[nodiscard]] std::vector<double> TakeShownIntervals();

/// The display's refresh period as the present thread measured it; 0 is unknown.
void SetRefreshPeriod(std::chrono::nanoseconds period);
[[nodiscard]] double RefreshHz();

/// The shown rate the autopilot steers towards, 0 when it does not. The FLUID presenter spaces
/// its slots by it.
void SetSlotRate(double fps);
[[nodiscard]] double SlotRate();

} // namespace VideoCore::AutopilotFeed
