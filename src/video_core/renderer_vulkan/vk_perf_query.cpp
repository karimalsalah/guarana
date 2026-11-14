// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>
#include <string_view>
#include <type_traits>

#include "common/logging.h"
#include "video_core/renderer_vulkan/vk_master_semaphore.h"
#include "video_core/renderer_vulkan/vk_perf_query.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {

namespace {

/// The counters sampled, by exact driver counter name (verified on the Arc 140T,
/// Vulkan 1.4.359: 1558 counters per queue family). A name the driver does not expose
/// is skipped, not an error.
constexpr std::array<std::string_view, 7> WantedCounters{
    "GpuBusy",
    "XveStall",
    "XveThreadOccupancy",
    "SamplerBottleneck",
    "AvgGpuCoreFrequencyMHz",
    "RenderBusy",
    "ComputeEngine0Busy",
};

/// Normalized units for the RPC: percentages and MHz where the counter's unit allows it.
std::string UnitName(VkPerformanceCounterUnitKHR unit) {
    switch (unit) {
    case VK_PERFORMANCE_COUNTER_UNIT_PERCENTAGE_KHR:
        return "pct";
    case VK_PERFORMANCE_COUNTER_UNIT_CYCLES_KHR:
        return "cycles";
    case VK_PERFORMANCE_COUNTER_UNIT_BYTES_KHR:
        return "bytes";
    case VK_PERFORMANCE_COUNTER_UNIT_BYTES_PER_SECOND_KHR:
        return "bytes_per_s";
    case VK_PERFORMANCE_COUNTER_UNIT_NANOSECONDS_KHR:
        return "ns";
    case VK_PERFORMANCE_COUNTER_UNIT_KELVIN_KHR:
        return "kelvin";
    case VK_PERFORMANCE_COUNTER_UNIT_WATTS_KHR:
        return "watts";
    case VK_PERFORMANCE_COUNTER_UNIT_VOLTS_KHR:
        return "volts";
    case VK_PERFORMANCE_COUNTER_UNIT_AMPS_KHR:
        return "amps";
    case VK_PERFORMANCE_COUNTER_UNIT_HERTZ_KHR:
        return "hz";
    case VK_PERFORMANCE_COUNTER_UNIT_GENERIC_KHR:
    default:
        return "raw";
    }
}

/// Bytes one counter result takes with VK_QUERY_RESULT_64_BIT | WITH_AVAILABILITY_BIT:
/// integers widen to 8, FLOAT32 stays 4, plus a 4-byte availability word. The layout is
/// per-counter {value, availability}; a wrong stride here reads past the buffer.
VkDeviceSize ResultStride(VkPerformanceCounterStorageKHR storage) {
    const VkDeviceSize value =
        storage == VK_PERFORMANCE_COUNTER_STORAGE_FLOAT32_KHR ? 4 : 8;
    return value + 4; // availability word (u32 with 64-bit results)
}

/// Reads one counter result of the given storage into a double. The value sits at the
/// start of the slot; only integers are widened to 64 bits by VK_QUERY_RESULT_64_BIT.
double ReadValue(const u8* data, VkPerformanceCounterStorageKHR storage) {
    switch (storage) {
    case VK_PERFORMANCE_COUNTER_STORAGE_INT32_KHR:
    case VK_PERFORMANCE_COUNTER_STORAGE_INT64_KHR: {
        s64 value{};
        std::memcpy(&value, data, sizeof(value));
        return static_cast<double>(value);
    }
    case VK_PERFORMANCE_COUNTER_STORAGE_UINT32_KHR:
    case VK_PERFORMANCE_COUNTER_STORAGE_UINT64_KHR: {
        u64 value{};
        std::memcpy(&value, data, sizeof(value));
        return static_cast<double>(value);
    }
    case VK_PERFORMANCE_COUNTER_STORAGE_FLOAT32_KHR: {
        float value{};
        std::memcpy(&value, data, sizeof(value));
        return static_cast<double>(value);
    }
    case VK_PERFORMANCE_COUNTER_STORAGE_FLOAT64_KHR: {
        f64 value{};
        std::memcpy(&value, data, sizeof(value));
        return value;
    }
    default:
        return 0.0;
    }
}

} // Anonymous namespace

bool PerfQuery::Enabled() {
    static const bool enabled = [] {
        const char* const value = std::getenv("EDEN_NX2_GPU_PERF");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

PerfQuery::PerfQuery(const Device& device_) : device{device_} {
    if (!Enabled()) {
        reason = "EDEN_NX2_GPU_PERF is not set";
        return;
    }
    if (!device.IsKhrPerformanceQueryEnabled()) {
        reason = "VK_KHR_performance_query is not enabled on this device";
        return;
    }

    const vk::DeviceDispatch& dld = device.GetDispatchLoader();
    PFN_vkEnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHR enumerate = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyPerformanceQueryPassesKHR get_passes = nullptr;
    {
        const VkInstance instance = device.GetInstance();
        const auto load = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(
                dld.vkGetInstanceProcAddr(instance, name));
        };
        load(enumerate, "vkEnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHR");
        load(get_passes, "vkGetPhysicalDeviceQueueFamilyPerformanceQueryPassesKHR");
        vkAcquireProfilingLockKHR = reinterpret_cast<PFN_vkAcquireProfilingLockKHR>(
            dld.vkGetDeviceProcAddr(*device.GetLogical(), "vkAcquireProfilingLockKHR"));
        vkReleaseProfilingLockKHR = reinterpret_cast<PFN_vkReleaseProfilingLockKHR>(
            dld.vkGetDeviceProcAddr(*device.GetLogical(), "vkReleaseProfilingLockKHR"));
    }
    if (!enumerate || !get_passes || !vkAcquireProfilingLockKHR || !vkReleaseProfilingLockKHR) {
        reason = "VK_KHR_performance_query entry points are missing";
        return;
    }

    // Enumerate the graphics family's counters and pick the whitelist by name. The
    // description array is filled by the same call when passed alongside the counters.
    const u32 family = device.GetGraphicsFamily();
    u32 count = 0;
    if (enumerate(device.GetPhysical(), family, &count, nullptr, nullptr) != VK_SUCCESS ||
        count == 0) {
        reason = "the graphics queue family exposes no performance counters";
        return;
    }
    std::vector<VkPerformanceCounterKHR> all(count);
    for (VkPerformanceCounterKHR& counter : all) {
        counter.sType = VK_STRUCTURE_TYPE_PERFORMANCE_COUNTER_KHR;
        counter.pNext = nullptr;
    }
    std::vector<VkPerformanceCounterDescriptionKHR> descriptions(count);
    for (VkPerformanceCounterDescriptionKHR& description : descriptions) {
        description.sType = VK_STRUCTURE_TYPE_PERFORMANCE_COUNTER_DESCRIPTION_KHR;
        description.pNext = nullptr;
    }
    if (enumerate(device.GetPhysical(), family, &count, all.data(), descriptions.data()) !=
        VK_SUCCESS) {
        reason = "performance counter enumeration failed";
        return;
    }
    // Collect every wanted counter the driver exposes, in priority order.
    std::vector<Counter> wanted;
    for (const std::string_view name_wanted : WantedCounters) {
        for (u32 i = 0; i < count; ++i) {
            const std::string_view name{descriptions[i].name};
            if (name == name_wanted) {
                wanted.push_back({std::string{name}, i, all[i].unit, all[i].storage});
                break;
            }
        }
    }
    if (wanted.empty()) {
        reason = "none of the wanted counters (GpuBusy, XveStall, XveThreadOccupancy, "
                 "SamplerBottleneck, AvgGpuCoreFrequencyMHz) is exposed";
        return;
    }

    // The hardware groups counters into passes; a query pool may only span one pass.
    // Greedily keep the largest priority-ordered subset that fits a single pass, so a
    // multi-pass wishlist still samples its most important counters instead of nothing.
    const auto passes_for = [&](const std::vector<u32>& idx) -> u32 {
        const VkQueryPoolPerformanceCreateInfoKHR ci{
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_PERFORMANCE_CREATE_INFO_KHR,
            .pNext = nullptr,
            .queueFamilyIndex = family,
            .counterIndexCount = static_cast<u32>(idx.size()),
            .pCounterIndices = idx.data(),
        };
        u32 n = 0;
        get_passes(device.GetPhysical(), &ci, &n);
        return n;
    };
    std::vector<u32> indices;
    for (const Counter& counter : wanted) {
        indices.push_back(counter.index);
        if (passes_for(indices) != 1) {
            indices.pop_back(); // this counter would force a second pass: skip it
        } else {
            counters.push_back(counter);
        }
    }
    if (counters.empty()) {
        reason = "no single-pass subset of the wanted counters is available";
        return;
    }
    const VkQueryPoolPerformanceCreateInfoKHR performance_ci{
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_PERFORMANCE_CREATE_INFO_KHR,
        .pNext = nullptr,
        .queueFamilyIndex = family,
        .counterIndexCount = static_cast<u32>(indices.size()),
        .pCounterIndices = indices.data(),
    };

    try {
        pool = device.GetLogical().CreateQueryPool({
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .pNext = &performance_ci,
            .flags = 0,
            .queryType = VK_QUERY_TYPE_PERFORMANCE_QUERY_KHR,
            .queryCount = MaxSlots,
            .pipelineStatistics = 0,
        });
    } catch (const vk::Exception& error) {
        reason = std::string{"performance query pool creation failed: "} + error.what();
        counters.clear();
        return;
    }

    sums.resize(counters.size(), 0.0);
    supported = true;
    std::string joined;
    for (const Counter& counter : counters) {
        joined += (joined.empty() ? "" : ", ") + counter.name;
    }
    LOG_INFO(Render_Vulkan, "gpu_perf: sampling {} performance counters ({})", counters.size(),
             joined);
}

PerfQuery::~PerfQuery() {
    ReleaseLock();
}

bool PerfQuery::Supported() const {
    return supported;
}

std::string PerfQuery::Reason() const {
    std::scoped_lock lock{mutex};
    return reason;
}

bool PerfQuery::RequestSampling(u32 frames) {
    std::scoped_lock lock{mutex};
    if (!supported) {
        return false;
    }
    if (frames == 0) {
        frames_left = 0;
        if (pending_slots.empty()) {
            ReleaseLock();
        }
        return true;
    }
    if (!lock_held) {
        const VkAcquireProfilingLockInfoKHR lock_info{
            .sType = VK_STRUCTURE_TYPE_ACQUIRE_PROFILING_LOCK_INFO_KHR,
            .pNext = nullptr,
            .flags = 0,
            .timeout = 1000000000, // 1 s: the lock is exclusive, never wait long for it
        };
        if (vkAcquireProfilingLockKHR(*device.GetLogical(), &lock_info) != VK_SUCCESS) {
            reason = "the profiling lock is held by another profiler";
            return false;
        }
        lock_held = true;
        reason.clear();
    }
    frames_left = frames;
    frames_sampled = 0;
    collected = 0;
    next_slot = 0;
    pending_slots.clear();
    scope_open = false;
    std::fill(sums.begin(), sums.end(), 0.0);
    return true;
}

bool PerfQuery::Sampling() const {
    std::scoped_lock lock{mutex};
    return frames_left != 0 || !pending_slots.empty();
}

void PerfQuery::Begin(vk::CommandBuffer cmdbuf) {
    std::scoped_lock lock{mutex};
    ++begin_calls;
    if (!supported) {
        ++begin_reject_unsupported;
        return;
    }
    if (frames_left == 0) {
        ++begin_reject_noframes;
        return;
    }
    if (scope_open) {
        ++begin_reject_scopeopen;
        return;
    }
    // A restarted window may reach a slot whose earlier query is still pending on the
    // GPU; resetting it would be invalid, so that submission is simply not sampled.
    for (const PendingSlot& slot : pending_slots) {
        if (slot.index == next_slot) {
            ++begin_reject_collision;
            return;
        }
    }
    // The slot's previous results were collected when its tick completed; reset it here
    // so a fresh scope never reads stale counters.
    device.GetLogical().ResetQueryPool(*pool, next_slot, 1);
    cmdbuf.BeginQuery(*pool, next_slot, 0);
    scope_open = true;
    ++begin_opened;
}

void PerfQuery::End(vk::CommandBuffer cmdbuf, u64 tick) {
    std::scoped_lock lock{mutex};
    if (!scope_open) {
        return;
    }
    cmdbuf.EndQuery(*pool, next_slot);
    scope_open = false;
    pending_slots.push_back({next_slot, tick});
    next_slot = (next_slot + 1) % MaxSlots;
    --frames_left;
}

void PerfQuery::CollectLocked() {
    if (!semaphore) {
        return;
    }
    // IsFree reads the known GPU tick, which only advances on Refresh(); without this
    // the pending slots would never look complete from the debug thread.
    semaphore->Refresh();
    ++collect_calls;
    // Only read once a submission's tick has completed on the GPU; the profiling lock
    // stays held until the window's last result is read (the spec requires the lock
    // while queried command buffers are pending).
    while (!pending_slots.empty() && semaphore->IsFree(pending_slots.front().tick)) {
        const PendingSlot slot = pending_slots.front();
        pending_slots.erase(pending_slots.begin());
        // Per-counter {value, availability} layout; size each slot by its storage so a
        // FLOAT32 counter (4 bytes, not widened) never overruns the buffer.
        VkDeviceSize total = 0;
        for (const Counter& counter : counters) {
            total += ResultStride(counter.storage);
        }
        std::vector<u8> data(total);
        const VkResult result = device.GetLogical().GetQueryResults(
            *pool, slot.index, 1, data.size(), data.data(), 0,
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        if (result != VK_SUCCESS) {
            ++collect_not_ready;
            last_query_result = static_cast<int>(result);
            LOG_WARNING(Render_Vulkan, "gpu_perf: query result {}", static_cast<int>(result));
            continue;
        }
        VkDeviceSize offset = 0;
        bool all_available = true;
        for (size_t i = 0; i < counters.size(); ++i) {
            const VkDeviceSize stride = ResultStride(counters[i].storage);
            const u8* slot_data = data.data() + offset;
            // The availability word follows the value; 0 means the GPU has not written it.
            u32 available{};
            std::memcpy(&available, slot_data + (stride - 4), sizeof(available));
            if (available != 0) {
                sums[i] += ReadValue(slot_data, counters[i].storage);
            } else {
                all_available = false;
            }
            offset += stride;
        }
        if (all_available) {
            ++collected;
            ++frames_sampled;
        }
    }
    if (!pending_slots.empty()) {
        last_pending_tick = pending_slots.front().tick;
    }
    if (frames_left == 0 && pending_slots.empty()) {
        ReleaseLock();
    }
}

std::vector<PerfQuery::Sample> PerfQuery::Read() {
    std::scoped_lock lock{mutex};
    if (!supported) {
        return {};
    }
    CollectLocked();
    if (collected == 0) {
        return {};
    }
    std::vector<Sample> samples;
    samples.reserve(counters.size());
    for (size_t i = 0; i < counters.size(); ++i) {
        const Counter& counter = counters[i];
        double value = sums[i] / static_cast<double>(collected);
        std::string unit = UnitName(counter.unit);
        // Hertz counters reported in MHz (AvgGpuCoreFrequencyMHz) stay MHz; plain Hertz
        // values are scaled to MHz so freq_mhz is comparable.
        if (counter.unit == VK_PERFORMANCE_COUNTER_UNIT_HERTZ_KHR &&
            counter.name.find("MHz") == std::string::npos) {
            value /= 1000000.0;
            unit = "mhz";
        } else if (counter.unit == VK_PERFORMANCE_COUNTER_UNIT_HERTZ_KHR) {
            unit = "mhz";
        }
        samples.push_back({counter.name, value, std::move(unit)});
    }
    return samples;
}

void PerfQuery::SetSemaphore(MasterSemaphore& semaphore_) {
    semaphore = &semaphore_;
}

u64 PerfQuery::FramesSampled() const {
    std::scoped_lock lock{mutex};
    return frames_sampled;
}

std::string PerfQuery::Diag() const {
    std::scoped_lock lock{mutex};
    const u64 gpu_tick = semaphore ? semaphore->KnownGpuTick() : 0;
    return fmt::format(
        "pending={} begin_calls={} opened={} rej_noframes={} rej_collision={} "
        "frames_left={} scope_open={} lock_held={} gpu_tick={} collect_calls={} "
        "collect_not_ready={} last_qresult={} last_pending_tick={}",
        pending_slots.size(), begin_calls, begin_opened, begin_reject_noframes,
        begin_reject_collision, frames_left, scope_open, lock_held, gpu_tick, collect_calls,
        collect_not_ready, last_query_result, last_pending_tick);
}

void PerfQuery::ReleaseLock() {
    if (lock_held && vkReleaseProfilingLockKHR) {
        vkReleaseProfilingLockKHR(*device.GetLogical());
    }
    lock_held = false;
}

} // namespace Vulkan
