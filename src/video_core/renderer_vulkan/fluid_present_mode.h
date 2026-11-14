// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/settings_enums.h"

namespace Vulkan {

/// The present mode the EDEN_NX2_FLUID presenter asks for. It times each present to be shown at
/// the next vblank, so FIFO; with EDEN_NX2_FLUID_MAILBOX (`mailbox_requested`) MAILBOX when the
/// surface lists it, so a present that misses its vblank does not queue ahead of the later ones:
/// a later present that arrives before it is shown replaces it.
[[nodiscard]] constexpr Settings::VSyncMode ChooseFluidPresentMode(bool mailbox_requested,
                                                                   bool has_mailbox) {
    return mailbox_requested && has_mailbox ? Settings::VSyncMode::Mailbox
                                            : Settings::VSyncMode::Fifo;
}

} // namespace Vulkan
