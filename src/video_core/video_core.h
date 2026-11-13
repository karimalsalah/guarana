// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2014 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <optional>

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

} // namespace VideoCore
