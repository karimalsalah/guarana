// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

namespace Vulkan {

// One descriptor set per graphics stage, including holes for absent stages.
constexpr u32 GraphicsPipelineLibraryDescriptorSetCount = 5;

// Only fragment inputs carry Flat or NoPerspective, so the libraries also need the driver to
// accept outputs of the last pre-rasterization stage without matching interpolation decorations.
constexpr bool CanUseGraphicsPipelineLibraries(bool feature, bool fast_linking, u32 max_sets,
                                               bool independent_interpolation) {
    return feature && fast_linking && max_sets >= GraphicsPipelineLibraryDescriptorSetCount &&
           independent_interpolation;
}

/// What a draw does about the optimized link (EDEN_NX2_GPL_OPTIMIZE) of its pipeline.
enum class OptimizedLinkAtDraw : u8 {
    None,      ///< Not a fast-linked pipeline, or its optimized link is already queued
    Queue,     ///< Queue the optimized link now (EDEN_NX2_GPL_OPTIMIZE_CACHED)
    StaysFast, ///< Fast-linked with no optimized link coming (counted for the log)
};

/// What the draw knows about its pipeline. `has_optimized_build` and `linked` are only read once
/// `built` holds: the build thread writes them before it completes.
struct OptimizedLinkState {
    bool built{};               ///< The pipeline's build is done
    bool linked{};              ///< Linked from libraries (not the monolithic fallback)
    bool fast_link{};           ///< The device fast-links (else the link was already optimized)
    bool optimize{};            ///< EDEN_NX2_GPL_OPTIMIZE (on unless 0)
    bool has_optimized_build{}; ///< Its optimized link was queued (at its build, or a draw)
    bool optimize_cached{};     ///< EDEN_NX2_GPL_OPTIMIZE_CACHED=1
    bool has_pool{};            ///< The optimized links have their own threads (not THREADS=0)
};

/// A pipeline whose build had no shader jobs (one loaded from the shader cache, built by its
/// owner) gets no optimized link at its build. With EDEN_NX2_GPL_OPTIMIZE_CACHED its first draw
/// queues it on the optimized links' own threads, as a build with jobs would have.
constexpr OptimizedLinkAtDraw DecideOptimizedLinkAtDraw(const OptimizedLinkState& state) {
    if (!state.built || !state.linked || !state.fast_link || !state.optimize ||
        state.has_optimized_build) {
        return OptimizedLinkAtDraw::None;
    }
    return state.optimize_cached && state.has_pool ? OptimizedLinkAtDraw::Queue
                                                   : OptimizedLinkAtDraw::StaysFast;
}

} // namespace Vulkan
