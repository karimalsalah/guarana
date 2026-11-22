// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2014 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <optional>
#include <string>

#include "common/common_types.h"
#include "core/game_presets.h"

namespace Core {
class System;
}

namespace Core::Frontend {
class EmuWindow;
}

namespace Tegra {
class GPU;
}

namespace VideoCore {

class RendererBase;
/// Starts what the renderer can do before the game is known (the Vulkan instance), on other
/// threads, so that it overlaps the rest of the load. Call before CreateGPU.
void PrepareRenderer(Core::Frontend::EmuWindow& emu_window);
void CreateGPU(std::optional<Tegra::GPU>& gpu, Core::Frontend::EmuWindow& emu_window, Core::System& system);

/// Puts the game's switch defaults in the environment (Core::GamePresets::ApplySwitchDefaults),
/// then reads again the switches CreateGPU read before they were there: the read-back's
/// EDEN_NX2_EARLY_* (AsyncReadback::ReadSwitch). The renderer calls it once the GPU is known, so
/// a game's first start runs with its defaults, and a later start without an earlier game's.
/// Returns ApplySwitchDefaults' line. `display` is the display the window is on.
std::string ApplyGameSwitchDefaults(u64 title_id, const Core::GamePresets::GpuInfo& gpu,
                                    const Core::GamePresets::DisplayInfo& display = {});

} // namespace VideoCore
