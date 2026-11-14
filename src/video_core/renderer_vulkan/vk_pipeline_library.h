// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"
#include "video_core/renderer_vulkan/unoptimized_first.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

// Graphics pipeline libraries (EDEN_NX2_GPL=1, VK_EXT_graphics_pipeline_library).
//
// A pipeline is linked from four parts: vertex input, pre-rasterization shaders, fragment shader
// and fragment output. The two shader parts are what the driver spends seconds on; they are built
// once per shader (and the little state the driver bakes into them) and shared by every pipeline
// that uses that shader, whatever its partner stage. The vertex input and fragment output parts
// are cheap and built per pipeline. A new pipeline whose shaders were seen before costs a link
// (a fraction of a millisecond with fast linking).
//
// For a shader library to be independent of its partner, each stage has its own descriptor set
// (set index = stage index, VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT) with its bindings
// numbered from 0, and the fragment stage's texture and image rescaling bits start at a fixed
// index instead of after the previous stages' ones.
//
// Libraries are keyed by the SPIR-V the stage was emitted to (a 128-bit hash), the stage's
// descriptor set layout, the push constant range, the state the part bakes in, the dynamic state
// list, the render pass and the creation flags. Keying by the emitted SPIR-V is always correct;
// it is less reuse than possible when the emission depends on the partner stage or on state (see
// MakeRuntimeInfo), which can be narrowed later in the emission.
//
// Based in part on suyu-main 3157ae0ce0 and f80bbc701a (library cache with build outside the
// lock, fast link then optimized link, link statistics); unlike there, the shader libraries here
// do not depend on the partner stage.

/// The fragment stage's first texture and image rescaling bit with libraries (the push constant
/// has 128 texture and 64 image bits; the pre-rasterization stages use the ones below).
constexpr u32 GPL_FRAGMENT_TEXTURE_SCALING_BASE = 64;
constexpr u32 GPL_FRAGMENT_IMAGE_SCALING_BASE = 32;

/// With EDEN_NX2_GPL=1, after the fast link an optimized link
/// (VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT) of the same libraries is built on a worker
/// and swapped in at a later draw. The shader libraries are then created with
/// VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT. EDEN_NX2_GPL_OPTIMIZE=0 leaves
/// the fast-linked pipelines in place.
bool GplOptimizeEnabled();

/// EDEN_NX2_UNOPT_FIRST=1 (unoptimized_first.h): whether it applies with `driver_id`, and
/// `libraries` (graphics pipeline libraries in use). Logs the outcome when it was asked for:
/// called once per pipeline cache, at startup.
bool UnoptimizedFirstEnabled(VkDriverId driver_id, bool libraries);

enum class GplPart : u32 {
    PreRasterization = 0,
    Fragment = 1,
};

/// A cached shader library and the layout objects it was created with.
struct GplLibrary {
    std::vector<vk::DescriptorSetLayout> set_layouts;
    vk::PipelineLayout layout;
    vk::Pipeline pipeline;
    /// How long the build took: what each later reuse of the library saves.
    u64 build_ns{};
};

/// Bytes of a library key. Only whole scalar values are added, never structs with padding.
class GplKeyWriter {
public:
    template <typename T>
        requires std::is_arithmetic_v<T> || std::is_enum_v<T> || std::is_pointer_v<T>
    void Add(T value) {
        char bytes[sizeof(T)];
        std::memcpy(bytes, &value, sizeof(T));
        data.append(bytes, sizeof(T));
    }

    void Add(const u128& value) {
        Add(value[0]);
        Add(value[1]);
    }

    void AddBytes(const std::string& bytes) {
        Add(static_cast<u64>(bytes.size()));
        data.append(bytes);
    }

    [[nodiscard]] const std::string& Data() const noexcept {
        return data;
    }

private:
    std::string data;
};

class GraphicsPipelineLibraryCache {
public:
    using Builder = std::function<GplLibrary()>;

    /// The library for `key`: built with `build` on this thread when nobody has built it yet,
    /// waited for when another thread is building it. Null when its build failed (a failed key
    /// is not retried). `name` (a shader hash) is only for the log. `unoptimized`: `build` makes
    /// it with optimization disabled (EDEN_NX2_UNOPT_FIRST), counted apart as well. `background`:
    /// EDEN_NX2_UNOPT_FIRST's optimized replacement asks, no draw waits for it, so it is left out
    /// of the builds, reuses and draw wait avoided of `Nx2Perf gpl` (it has its own counters).
    std::shared_ptr<const GplLibrary> GetOrCreate(GplPart part, const std::string& key,
                                                  const Builder& build, u64 name,
                                                  bool unoptimized = false,
                                                  bool background = false);

    /// Whether the library of `key` is built and usable now. Never waits for a build.
    bool HasBuilt(const std::string& key);

    /// EDEN_NX2_UNOPT_FIRST: set at startup (UnoptimizedFirstEnabled), before any build.
    void SetUnoptimizedFirst(bool enabled) noexcept {
        unoptimized_first = enabled;
    }
    [[nodiscard]] bool UnoptimizedFirst() const noexcept {
        return unoptimized_first;
    }
    /// A pipeline was linked from at least one unoptimized library.
    void RecordUnoptimizedStart() {
        started_unoptimized.fetch_add(1, std::memory_order_relaxed);
    }
    /// The Background build of a pipeline's optimized libraries and their link ended; `built`
    /// when it gave a pipeline to swap in.
    void RecordReplacementBuild(bool built, std::chrono::nanoseconds time);
    /// GPU thread: a pipeline linked from unoptimized libraries is swapped for its optimized
    /// build, `since_first_draw` after its first draw. Logs one line.
    void RecordUnoptimizedSwap(u64 pipeline_hash, std::chrono::nanoseconds since_first_draw);

    void RecordLink(bool optimized, std::chrono::nanoseconds time);
    void RecordHeldDraw(std::chrono::nanoseconds time);
    void RecordFallback();

    /// A draw recorded with a pipeline linked from libraries, before its optimized link.
    void RecordFastDraw() {
        fast_draws.fetch_add(1, std::memory_order_relaxed);
    }
    /// The optimized link replaced a fast-linked pipeline.
    void RecordReplacement() {
        replacements.fetch_add(1, std::memory_order_relaxed);
    }

    /// Logs the counters of the last 10 s (Nx2Perf gpl), when there were any.
    void Report();

    /// Releases the built libraries no pipeline holds any more (their pipelines were evicted), so
    /// their driver memory goes with them. A key that failed stays, so it is not retried.
    /// Returns how many were released.
    size_t PruneUnused();

private:
    struct Entry {
        std::mutex mutex;
        std::condition_variable condition;
        bool done{};
        std::shared_ptr<const GplLibrary> library;
    };

    /// The EDEN_NX2_UNOPT_FIRST line of Report, when it is on and anything happened.
    void ReportUnoptimizedFirst();

    std::mutex mutex;
    std::unordered_map<std::string, std::shared_ptr<Entry>> entries;

    std::array<std::atomic<u32>, 2> built{};
    std::array<std::atomic<u32>, 2> failed{};
    std::array<std::atomic<u32>, 2> hits{};
    std::array<std::atomic<u32>, 2> waited{};
    std::array<std::atomic<u64>, 2> build_ns{};
    std::array<std::atomic<u64>, 2> build_max_ns{};
    std::array<std::atomic<u32>, 2> link_count{};
    std::array<std::atomic<u64>, 2> link_ns{};
    std::array<std::atomic<u64>, 2> link_max_ns{};
    std::atomic<u64> held_ns{};
    std::atomic<u64> avoided_ns{};
    std::atomic<u64> fast_draws{};
    std::atomic<u32> replacements{};
    std::atomic<u32> fallbacks{};
    std::atomic<u32> logged_builds{};
    std::atomic<size_t> total_libraries{};
    std::chrono::steady_clock::time_point window = std::chrono::steady_clock::now();

    // EDEN_NX2_UNOPT_FIRST
    bool unoptimized_first{};
    std::array<std::atomic<u32>, 2> unoptimized_built{};
    std::array<std::atomic<u64>, 2> unoptimized_build_ns{};
    std::array<std::atomic<u64>, 2> unoptimized_build_max_ns{};
    std::atomic<u32> replacement_builds{};
    std::atomic<u32> replacement_failures{};
    std::atomic<u64> replacement_build_ns{};
    std::atomic<u64> replacement_build_max_ns{};
    std::atomic<u64> started_unoptimized{}; ///< Since startup, not reset by Report
    std::atomic<u64> swapped_unoptimized{}; ///< Since startup, not reset by Report
    std::mutex swap_mutex;
    SwapDelays swap_delays;
};

} // namespace Vulkan
