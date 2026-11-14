// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/settings_enums.h"

namespace Vulkan {

/// The present mode the EDEN_NX2_FLUID presenter asks for. It times each present to be shown at
/// the next vblank, so FIFO. With EDEN_NX2_FLUID_MAILBOX (`mailbox_requested`) it is MAILBOX when
/// the surface lists it (`has_mailbox`), so a present that misses its vblank does not queue ahead
/// of the later ones: a later present that arrives before it is shown replaces it. With
/// EDEN_NX2_FLUID_IMMEDIATE (`immediate_requested`) it is IMMEDIATE when the surface lists it
/// (`has_immediate`): a present is shown at once, mid-scan-out, so nothing queues and a tear line
/// can show where the present lands. MAILBOX wins when both are asked for and listed; a mode the
/// surface does not list falls through to the next, and the last resort is FIFO, as without the
/// switches.
[[nodiscard]] constexpr Settings::VSyncMode ChooseFluidPresentMode(bool mailbox_requested,
                                                                   bool has_mailbox,
                                                                   bool immediate_requested,
                                                                   bool has_immediate) {
    if (mailbox_requested && has_mailbox) {
        return Settings::VSyncMode::Mailbox;
    }
    if (immediate_requested && has_immediate) {
        return Settings::VSyncMode::Immediate;
    }
    return Settings::VSyncMode::Fifo;
}

} // namespace Vulkan
