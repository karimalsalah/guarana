// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstddef>
#include <numeric>
#include <span>
#include <vector>

#include "common/common_types.h"

namespace Vulkan {

/// Built graphics pipelines kept when the memory query fails.
inline constexpr size_t DefaultKeptPipelines = 4096;

/// Host memory one built graphics pipeline holds, rounded up: building OoT's 12,900 cached
/// pipelines at boot added 3.4 GiB of committed memory (~0.27 MiB each, NVIDIA).
inline constexpr u64 PipelineBytes = 300 * 1024;

/// Built graphics pipelines kept on a machine with `total_memory` bytes of RAM before unused ones
/// are destroyed: as many as fit in a quarter of it (8 GiB: 6,990; 32 GiB: 27,962). A failed
/// memory query (`total_memory` 0) gives DefaultKeptPipelines.
constexpr size_t KeptPipelinesForMemory(u64 total_memory) {
    if (total_memory == 0) {
        return DefaultKeptPipelines;
    }
    return static_cast<size_t>(total_memory / 4 / PipelineBytes);
}

/// Low memory mode (Common::LowMemoryMode) keeps at most 1 GiB of built pipelines (3,495).
inline constexpr size_t LowMemoryKeptPipelines =
    static_cast<size_t>((u64{1} << 30) / PipelineBytes);

/// Low memory mode's budget for the compressed SPIR-V kept of evicted pipelines (~3,800 of them).
inline constexpr u64 LowMemoryKeptSpirvBytes = u64{96} << 20;

/// Low memory mode's budget of prebuilt pipelines no draw has used, where only those are kept.
inline constexpr size_t LowMemoryUnusedPrebuiltPipelines = 500;

/// Without a kept limit (`kept` 0, Android), prebuilt pipelines no draw has used stay up to this
/// budget (~0.9 GB of driver memory): evicting them throws the prebuild away (OoT: 1174 of 1346
/// evicted on the title screen, 684 rebuilt in 10 s on entering the field).
inline constexpr size_t MaxUnusedPrebuiltPipelines = 2000;

/// Cached pipelines the background prebuild may hold built until a draw takes them: what the kept
/// limit leaves past the `boot_built` pipelines the boot builds, or MaxUnusedPrebuiltPipelines
/// without a kept limit.
constexpr size_t PrebuildLimit(size_t kept, size_t boot_built) {
    if (kept == 0) {
        return MaxUnusedPrebuiltPipelines;
    }
    return kept - std::min(kept, boot_built);
}

/// Whether a boot load saves the driver pipeline cache once its boot builds have ended. Never
/// while async boot builds still run. Where reading the cache during a build crashes (Intel's
/// Windows driver), only when no build can run during the save: reads wait for builds
/// (EDEN_NX2_CACHE_SAFE), or the background prebuild starts after it (`load_save`,
/// EDEN_NX2_LOAD_SAVE, Lichii #1838).
constexpr bool SaveDriverCacheAtLoadEnd(bool async_boot, bool can_read_during_builds,
                                        bool serializes_reads, bool load_save) {
    return !async_boot && (can_read_during_builds || serializes_reads || load_save);
}

/// Whether a boot load starts the background prebuild only after its driver cache save (and the
/// precache): with a precache, as its jobs would queue ahead of the precache's; where reads of the
/// cache wait for the builds into it; and with `load_save` where such reads crash, so that the
/// save runs with no build in flight. Otherwise the prebuild runs beside the boot builds.
constexpr bool PrebuildAfterLoadSave(bool async_boot, bool precache, bool can_read_during_builds,
                                     bool serializes_reads, bool load_save) {
    return !async_boot && (precache || serializes_reads || (load_save && !can_read_during_builds));
}

/// Whether a boot load starts the background prebuild before waiting for its boot builds, so the
/// two run together. With a precache it starts after it, as its jobs would queue ahead of the
/// precache's. Where reading the driver pipeline cache during a build crashes (Intel's Windows
/// driver), it starts after the load's driver cache saves, which then run with no build in flight:
/// otherwise those builds reach the file only at a clean close.
constexpr bool PrebuildStartsWithBootBuilds(bool precache, bool cache_read_races_builds) {
    return !precache && !cache_read_races_builds;
}

/// Picks which unused pipelines to destroy so that at most `kept` stay built.
/// @param last_uses Frame each candidate was last used (0 for a prebuilt one no draw has used).
///                  Candidates are built pipelines old enough to evict and not the current one.
/// @param built     Built graphics pipelines in the cache, candidates included.
/// @param kept      How many built pipelines to keep.
/// @return Indices into `last_uses`, least recently used first (ties in input order): only as
///         many as it takes to get back to `kept`, or every candidate when that is not enough.
inline std::vector<size_t> PipelinesToEvict(std::span<const u64> last_uses, size_t built,
                                            size_t kept) {
    if (built <= kept) {
        return {};
    }
    std::vector<size_t> order(last_uses.size());
    std::iota(order.begin(), order.end(), size_t{0});
    std::ranges::stable_sort(order, [&](size_t a, size_t b) { return last_uses[a] < last_uses[b]; });
    order.resize(std::min(built - kept, order.size()));
    return order;
}

} // namespace Vulkan
