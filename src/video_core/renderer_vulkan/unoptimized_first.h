// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

#include "video_core/vulkan_common/vulkan.h"

namespace Vulkan {

// EDEN_NX2_UNOPT_FIRST=1 (experiment, issue #13). With graphics pipeline libraries
// (EDEN_NX2_GPL) on Intel's Windows driver, a shader library whose optimized build no pipeline has
// finished yet this session is first built with VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT,
// meant to cut the driver work a waiting draw waits for (unproven on this driver: step 1 of the
// plan in docs/switches.md). The optimized libraries are then built and linked on the
// EDEN_NX2_GPL_OPTIMIZE_THREADS pool (a Background job when it is 0) and swapped in at a later
// draw, where the optimized link of EDEN_NX2_GPL_OPTIMIZE is swapped in
// (GraphicsPipeline::ConfigureDraw). The monolithic route is not changed. The pieces here are the
// ones that do not need a device, for the tests.

/// Whether EDEN_NX2_UNOPT_FIRST applies: asked for, Intel's Windows driver, and graphics
/// pipelines linked from libraries (the only route it changes).
constexpr bool UnoptimizedFirstApplies(bool requested, VkDriverId driver_id, bool libraries) {
    return requested && libraries && driver_id == VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS;
}

/// Whether a shader library is built unoptimized first, once UnoptimizedFirstApplies: only when
/// its optimized build can follow in the background (`replaceable`: the pipeline has a shader job
/// scheduler to queue it on) and no pipeline has built the optimized library yet, which is then
/// used as it is.
constexpr bool BuildsLibraryUnoptimizedFirst(bool replaceable, bool optimized_built) {
    return replaceable && !optimized_built;
}

/// Create flags of a shader library from the `base` flags all libraries take: optimization
/// disabled when it is built unoptimized first. The flags are part of the library's key, so an
/// unoptimized library never stands in for an optimized one.
constexpr VkPipelineCreateFlags ShaderLibraryCreateFlags(VkPipelineCreateFlags base,
                                                         bool unoptimized) {
    return unoptimized ? (base | VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT) : base;
}

/// Puts `replacement` in place of `current` and keeps the replaced handle in `retired`. The owner
/// holds `retired` until it is destroyed itself; a graphics pipeline is destroyed only once the GPU
/// has finished the command buffers recorded before it went (PipelineCache's evicted pipelines,
/// freed when Scheduler::IsFree of their tick), so a command buffer that bound the replaced handle
/// never sees it destroyed. Once per owner: false, with nothing moved, when there is no
/// replacement or `retired` holds one already.
template <typename Handle>
bool SwapInReplacement(Handle& current, Handle& replacement, Handle& retired) {
    if (!replacement || retired) {
        return false;
    }
    retired = std::move(current);
    current = std::move(replacement);
    return true;
}

/// Times from a pipeline's first draw to its swap, for the 10 s summary. Not thread safe: the
/// owner locks around it.
class SwapDelays {
public:
    struct Summary {
        size_t count{};
        double p50_ms{};
        double p99_ms{};
        double max_ms{};
    };

    void Add(double ms) {
        delays.push_back(ms);
    }

    /// The delays added since the last call, as nearest-rank percentiles, and starts over.
    Summary Take() {
        Summary summary{.count = delays.size()};
        if (delays.empty()) {
            return summary;
        }
        std::ranges::sort(delays);
        summary.p50_ms = Rank(50);
        summary.p99_ms = Rank(99);
        summary.max_ms = delays.back();
        delays.clear();
        return summary;
    }

private:
    /// The smallest delay with at least `percent` percent of the delays at or below it.
    double Rank(size_t percent) const {
        const size_t rank = (percent * delays.size() + 99) / 100;
        return delays[std::max<size_t>(rank, 1) - 1];
    }

    std::vector<double> delays;
};

} // namespace Vulkan
