// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>
#include <exception>

#include "common/logging.h"
#include "video_core/renderer_vulkan/vk_pipeline_library.h"

namespace Vulkan {

namespace {
/// Raises `target` to `value` when it is larger, safe against concurrent callers.
void StoreMax(std::atomic<u64>& target, u64 value) {
    u64 previous = target.load(std::memory_order_relaxed);
    while (previous < value &&
           !target.compare_exchange_weak(previous, value, std::memory_order_relaxed)) {
    }
}

double Ms(u64 ns) {
    return static_cast<double>(ns) / 1'000'000.0;
}

constexpr std::array<const char*, 2> PART_NAMES{"pre-rasterization", "fragment"};
} // Anonymous namespace

bool GplOptimizeEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_GPL_OPTIMIZE");
        return value == nullptr || *value == 0 || *value != '0';
    }();
    return enabled;
}

bool UnoptimizedFirstEnabled(VkDriverId driver_id, bool libraries) {
    static const bool requested = [] {
        const char* value = std::getenv("EDEN_NX2_UNOPT_FIRST");
        return value != nullptr && value[0] == '1';
    }();
    if (!requested) {
        return false;
    }
    const bool enabled = UnoptimizedFirstApplies(requested, driver_id, libraries);
    if (enabled) {
        LOG_INFO(Render_Vulkan,
                 "EDEN_NX2_UNOPT_FIRST=1: shader libraries whose optimized build no pipeline has "
                 "finished yet are built unoptimized first, and their optimized build is swapped "
                 "in from the background");
    } else if (!libraries) {
        LOG_INFO(Render_Vulkan, "EDEN_NX2_UNOPT_FIRST=1 ignored: graphics pipeline libraries "
                                "(EDEN_NX2_GPL) are not in use");
    } else {
        LOG_INFO(Render_Vulkan,
                 "EDEN_NX2_UNOPT_FIRST=1 ignored: only for Intel's Windows driver");
    }
    return enabled;
}

std::shared_ptr<const GplLibrary> GraphicsPipelineLibraryCache::GetOrCreate(
    GplPart part, const std::string& key, const Builder& build, u64 name, bool unoptimized,
    bool background) {
    const size_t index = static_cast<size_t>(part);
    std::shared_ptr<Entry> entry;
    bool owner = false;
    {
        std::scoped_lock lock{mutex};
        auto [it, inserted] = entries.try_emplace(key);
        if (inserted) {
            it->second = std::make_shared<Entry>();
            owner = true;
        }
        entry = it->second;
    }
    if (!owner) {
        std::unique_lock lock{entry->mutex};
        if (!entry->done) {
            // Another worker is building the same library: wait for it instead of building it
            // twice.
            if (!background) {
                waited[index].fetch_add(1, std::memory_order_relaxed);
            }
            entry->condition.wait(lock, [&] { return entry->done; });
        }
        if (background) {
            return entry->library;
        }
        hits[index].fetch_add(1, std::memory_order_relaxed);
        if (entry->library) {
            // What this pipeline would have waited for building the library itself.
            avoided_ns.fetch_add(entry->library->build_ns, std::memory_order_relaxed);
        }
        return entry->library;
    }
    // Built outside the map lock: other libraries are built at the same time on other workers.
    const auto begin = std::chrono::steady_clock::now();
    std::shared_ptr<const GplLibrary> library;
    try {
        GplLibrary built_library = build();
        built_library.build_ns = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                      std::chrono::steady_clock::now() - begin)
                                                      .count());
        if (built_library.pipeline) {
            library = std::make_shared<const GplLibrary>(std::move(built_library));
        }
    } catch (const std::exception& exception) {
        LOG_ERROR(Render_Vulkan, "Nx2Perf gpl: {} library {:016x} failed: {}", PART_NAMES[index],
                  name, exception.what());
        library.reset();
    } catch (...) {
        LOG_ERROR(Render_Vulkan, "Nx2Perf gpl: {} library {:016x} failed", PART_NAMES[index],
                  name);
        library.reset();
    }
    const u64 ns = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now() - begin)
                                        .count());
    if (library) {
        // The total counts every library the cache holds, the background ones too.
        total_libraries.fetch_add(1, std::memory_order_relaxed);
        if (!background) {
            built[index].fetch_add(1, std::memory_order_relaxed);
            build_ns[index].fetch_add(ns, std::memory_order_relaxed);
            StoreMax(build_max_ns[index], ns);
        }
        if (unoptimized) {
            unoptimized_built[index].fetch_add(1, std::memory_order_relaxed);
            unoptimized_build_ns[index].fetch_add(ns, std::memory_order_relaxed);
            StoreMax(unoptimized_build_max_ns[index], ns);
        }
        if (ns > 2'000'000 && logged_builds.fetch_add(1, std::memory_order_relaxed) < 400) {
            LOG_INFO(Render_Vulkan, "Nx2Perf gpl: {} library {:016x} built in {:.1f} ms{}",
                     PART_NAMES[index], name, Ms(ns),
                     unoptimized ? " (unoptimized)" : (background ? " (background)" : ""));
        }
    } else {
        failed[index].fetch_add(1, std::memory_order_relaxed);
    }
    {
        std::scoped_lock lock{entry->mutex};
        entry->library = library;
        entry->done = true;
    }
    entry->condition.notify_all();
    return library;
}

bool GraphicsPipelineLibraryCache::HasBuilt(const std::string& key) {
    std::shared_ptr<Entry> entry;
    {
        std::scoped_lock lock{mutex};
        const auto it = entries.find(key);
        if (it == entries.end()) {
            return false;
        }
        entry = it->second;
    }
    std::scoped_lock lock{entry->mutex};
    return entry->done && entry->library != nullptr;
}

void GraphicsPipelineLibraryCache::RecordReplacementBuild(bool built,
                                                          std::chrono::nanoseconds time) {
    if (!built) {
        replacement_failures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const u64 ns = static_cast<u64>(time.count());
    replacement_builds.fetch_add(1, std::memory_order_relaxed);
    replacement_build_ns.fetch_add(ns, std::memory_order_relaxed);
    StoreMax(replacement_build_max_ns, ns);
}

void GraphicsPipelineLibraryCache::RecordUnoptimizedSwap(u64 pipeline_hash,
                                                         std::chrono::nanoseconds since_first_draw) {
    const double ms = Ms(static_cast<u64>(since_first_draw.count()));
    swapped_unoptimized.fetch_add(1, std::memory_order_relaxed);
    {
        std::scoped_lock lock{swap_mutex};
        swap_delays.Add(ms);
    }
    LOG_INFO(Render_Vulkan,
             "Nx2Perf unopt_first: pipeline {:016x} swapped to its optimized build {:.0f} ms after "
             "its first draw",
             pipeline_hash, ms);
}

/// Counts one link (unoptimized or optimized) and its time for the next report.
void GraphicsPipelineLibraryCache::RecordLink(bool optimized, std::chrono::nanoseconds time) {
    const size_t index = optimized ? 1 : 0;
    const u64 ns = static_cast<u64>(time.count());
    link_count[index].fetch_add(1, std::memory_order_relaxed);
    link_ns[index].fetch_add(ns, std::memory_order_relaxed);
    StoreMax(link_max_ns[index], ns);
}

void GraphicsPipelineLibraryCache::RecordHeldDraw(std::chrono::nanoseconds time) {
    held_ns.fetch_add(static_cast<u64>(time.count()), std::memory_order_relaxed);
}

void GraphicsPipelineLibraryCache::RecordFallback() {
    fallbacks.fetch_add(1, std::memory_order_relaxed);
}

void GraphicsPipelineLibraryCache::Report() {
    const auto now = std::chrono::steady_clock::now();
    if (now - window < std::chrono::seconds{10}) {
        return;
    }
    window = now;
    ReportUnoptimizedFirst();
    const auto take = [](auto& counter) { return counter.exchange(0, std::memory_order_relaxed); };
    const u32 pre_built = take(built[0]), frag_built = take(built[1]);
    const u32 pre_hits = take(hits[0]), frag_hits = take(hits[1]);
    const u32 fast_links = take(link_count[0]), optimized_links = take(link_count[1]);
    const u32 pre_failed = take(failed[0]), frag_failed = take(failed[1]);
    const u32 fallback_count = take(fallbacks);
    const u64 held = take(held_ns);
    const u64 avoided = take(avoided_ns);
    const u64 draws = take(fast_draws);
    const u32 replaced = take(replacements);
    if (pre_built == 0 && frag_built == 0 && pre_hits == 0 && frag_hits == 0 && fast_links == 0 &&
        optimized_links == 0 && held == 0 && pre_failed == 0 && frag_failed == 0 &&
        fallback_count == 0 && draws == 0 && replaced == 0) {
        return;
    }
    const u32 pre_waited = take(waited[0]), frag_waited = take(waited[1]);
    const u64 pre_ns = take(build_ns[0]), frag_ns = take(build_ns[1]);
    const u64 pre_max = take(build_max_ns[0]), frag_max = take(build_max_ns[1]);
    const u64 fast_ns = take(link_ns[0]), optimized_ns = take(link_ns[1]);
    const u64 fast_max = take(link_max_ns[0]), optimized_max = take(link_max_ns[1]);
    const auto mean = [](u64 total, u32 count) {
        return count == 0 ? 0.0 : Ms(total) / static_cast<double>(count);
    };
    LOG_INFO(Render_Vulkan,
             "Nx2Perf gpl in the last 10 s: fast-linked draws {}; libraries built "
             "pre-rasterization {} ({:.0f} ms, max {:.1f} ms), fragment {} ({:.0f} ms, max "
             "{:.1f} ms); libraries reused pre-rasterization {} (waited on a build {}), fragment "
             "{} (waited {}); failed {}/{}; fast links {} (mean {:.3f} ms, max {:.3f} ms); "
             "background replacements {} (optimized links {}, mean {:.1f} ms, max {:.1f} ms); "
             "monolithic fallbacks {}; draw wait avoided {:.0f} ms (library builds not repeated), "
             "draws held {:.1f} ms; {} libraries cached",
             draws, pre_built, Ms(pre_ns), Ms(pre_max), frag_built, Ms(frag_ns), Ms(frag_max),
             pre_hits, pre_waited, frag_hits, frag_waited, pre_failed, frag_failed, fast_links,
             mean(fast_ns, fast_links), Ms(fast_max), replaced, optimized_links,
             mean(optimized_ns, optimized_links), Ms(optimized_max), fallback_count, Ms(avoided),
             Ms(held), total_libraries.load(std::memory_order_relaxed));
}

void GraphicsPipelineLibraryCache::ReportUnoptimizedFirst() {
    if (!unoptimized_first) {
        return;
    }
    const auto take = [](auto& counter) { return counter.exchange(0, std::memory_order_relaxed); };
    const u32 pre_built = take(unoptimized_built[0]), frag_built = take(unoptimized_built[1]);
    const u32 replaced = take(replacement_builds);
    const u32 replace_failed = take(replacement_failures);
    SwapDelays::Summary swaps;
    {
        std::scoped_lock lock{swap_mutex};
        swaps = swap_delays.Take();
    }
    if (pre_built == 0 && frag_built == 0 && replaced == 0 && replace_failed == 0 &&
        swaps.count == 0) {
        return;
    }
    const u64 pre_ns = take(unoptimized_build_ns[0]), frag_ns = take(unoptimized_build_ns[1]);
    const u64 pre_max = take(unoptimized_build_max_ns[0]);
    const u64 frag_max = take(unoptimized_build_max_ns[1]);
    const u64 replace_ns = take(replacement_build_ns);
    const u64 replace_max = take(replacement_build_max_ns);
    const double replace_mean =
        replaced == 0 ? 0.0 : Ms(replace_ns) / static_cast<double>(replaced);
    LOG_INFO(Render_Vulkan,
             "Nx2Perf unopt_first in the last 10 s: unoptimized libraries built "
             "pre-rasterization {} ({:.0f} ms, max {:.1f} ms), fragment {} ({:.0f} ms, max "
             "{:.1f} ms); optimized replacements built {} (libraries and link, mean {:.1f} ms, "
             "max {:.1f} ms), failed {}; swaps {} (first draw to swap p50 {:.0f} ms, p99 {:.0f} "
             "ms, max {:.0f} ms); since startup {} pipelines started unoptimized, {} swapped",
             pre_built, Ms(pre_ns), Ms(pre_max), frag_built, Ms(frag_ns), Ms(frag_max), replaced,
             replace_mean, Ms(replace_max), replace_failed, swaps.count, swaps.p50_ms,
             swaps.p99_ms, swaps.max_ms, started_unoptimized.load(std::memory_order_relaxed),
             swapped_unoptimized.load(std::memory_order_relaxed));
}

size_t GraphicsPipelineLibraryCache::PruneUnused() {
    std::scoped_lock lock{mutex};
    const size_t pruned = std::erase_if(entries, [](const auto& item) {
        Entry& entry = *item.second;
        std::scoped_lock entry_lock{entry.mutex};
        // Only the map holds it. A thread that took the entry before this still holds the entry
        // itself, and with it the library it returns.
        return entry.done && entry.library && entry.library.use_count() == 1;
    });
    total_libraries.fetch_sub(pruned, std::memory_order_relaxed);
    return pruned;
}

} // namespace Vulkan
