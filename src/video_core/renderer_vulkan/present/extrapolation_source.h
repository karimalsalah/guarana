// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdlib>

#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

/// What the renderer knows about the frame being presented, for predicting the frames after it.
struct ExtrapolationSource {
    /// Half-size RGBA16F: xy is the motion to the previous frame in UV, z is 1 - depth.
    VkImageView guide{};
    /// The scene before and after the game drew its HUD, when both exist; null otherwise.
    VkImageView hud_reference{};
    VkImageView hud_composite{};
    /// hud_reference holds linear colour while hud_composite is sRGB-encoded.
    bool hud_reference_linear{};
    /// Both hold linear colour and are the same pixels wherever the game drew no HUD.
    bool hud_exact{};
    /// The camera jumped: the motion describes no movement to continue.
    bool cut{};
};

/// EDEN_NX2_EXTRAPOLATE=1: predict generated frames forward from the game's motion vectors.
[[nodiscard]] inline bool ExtrapolationRequested() {
    static const bool requested = [] {
        const char* value = std::getenv("EDEN_NX2_EXTRAPOLATE");
        return value && *value != '\0' && *value != '0';
    }();
    return requested;
}

} // namespace Vulkan
