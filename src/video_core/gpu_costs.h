// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

#include "common/common_types.h"

namespace VideoCore {

/// Host GPU time of one sampled frame, by shaders and by render target. frame is 0 when no
/// frame was sampled yet.
struct GpuCostSnapshot {
    static constexpr std::size_t MaxWork = 256;
    static constexpr std::size_t MaxPasses = 64;

    struct Entry {
        u64 vertex_hash{};   ///< The compute hash of a dispatch
        u64 fragment_hash{}; ///< Zero for a dispatch
        /// '\0' draw, m mesh, e emulated mesh, i indirect count, b indirect byte count
        char kind{};
        u32 draws{};
        /// Direct draws of at most 6 vertices and one instance (full-screen and UI quads).
        u32 small_draws{};
        /// Draws into another render target than the main scene target (has_main_target).
        u32 side_draws{};
        double total_ms{};
        double max_ms{};

        [[nodiscard]] bool IsCompute() const {
            return fragment_hash == 0;
        }
    };
    /// Emulator-issued work of one kind (image copies, uploads, its own compute passes, ...).
    struct EmuEntry {
        std::string category;
        u32 count{};
        double total_ms{};
        double max_ms{};
    };
    struct Pass {
        u32 width{};
        u32 height{};
        std::string color_format; ///< First color target's, "-" without one
        u32 num_colors{};
        std::string depth_format;
        u32 samples{};
        u32 passes{};
        double total_ms{};
        double max_ms{};
    };

    u64 frame{};
    /// Whether a main scene target was found: the target with depth and colour whose draws took
    /// the most GPU time.
    bool has_main_target{};
    double frame_span_ms{}; ///< First submission's start to the last one's end, -1 when unknown
    double submission_sum_ms{};
    double timed_sum_ms{};    ///< Sum of the timed guest draws and dispatches
    double emu_sum_ms{};      ///< Sum of the timed emulator work
    std::vector<Entry> work;  ///< Slowest first
    std::vector<EmuEntry> emu; ///< Slowest first
    std::vector<Pass> passes; ///< Slowest first
    std::chrono::steady_clock::time_point when{};
};

} // namespace VideoCore
