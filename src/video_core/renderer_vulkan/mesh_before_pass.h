// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdlib>

namespace Vulkan::MeshBeforePass {

/// EDEN_NX2_MESH_BEFORE_PASS: unset, empty, 0 and false are off; any other number is on.
[[nodiscard]] inline bool ParseSwitch(const char* value) noexcept {
    return value != nullptr && std::strtoul(value, nullptr, 10) != 0;
}

/// What the scheduler knows when a mesh emulation draw wants its dispatches to run before the
/// active render pass begins instead of cutting the pass.
struct State {
    bool pass_active{};                 ///< A render pass is open.
    bool deferred_clear_matches{};      ///< A parked clear of this draw's framebuffer would become it.
    bool has_prelude{};                 ///< The pass's begin has not reached the worker yet.
    bool pass_has_stores{};             ///< A draw in the pass wrote storage.
    bool uploads_pending{};             ///< Batched uploads wait to be recorded before anything else.
    bool same_framebuffer{};            ///< The pass draws into this draw's framebuffer.
    bool same_render_area{};            ///< And has this draw's render area.
};

enum class Verdict {
    NoPass,       ///< No pass to cut: the caller records the command outside any pass.
    RealizeClear, ///< Begin the parked clear as the pass, then decide again.
    Refuse,       ///< End the pass and record the command after it, as without the switch.
    Hoist,        ///< Record the command into the pass's prelude.
};

/// The draw's own sampling of its render targets is decided by the caller (it keeps the dispatch
/// in place); copies and other dispatches end the pass, so the pass holds neither.
[[nodiscard]] constexpr Verdict Decide(const State& state) noexcept {
    if (!state.pass_active) {
        return state.deferred_clear_matches ? Verdict::RealizeClear : Verdict::NoPass;
    }
    if (!state.has_prelude || state.pass_has_stores || state.uploads_pending ||
        !state.same_framebuffer || !state.same_render_area) {
        return Verdict::Refuse;
    }
    return Verdict::Hoist;
}

/// Storage writes must also stay after earlier draws' reads, even in a pass with no stores.
[[nodiscard]] constexpr bool MayHoistDraw(bool enabled, bool samples_own_targets,
                                        bool writes_storage) noexcept {
    return enabled && !samples_own_targets && !writes_storage;
}

} // namespace Vulkan::MeshBeforePass
