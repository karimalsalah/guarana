// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class MasterSemaphore;

/// EDEN_NX2_GPU_PERF=1: samples a small whitelist of VK_KHR_performance_query hardware
/// counters (GpuBusy, XveStall, XveThreadOccupancy, SamplerBottleneck,
/// AvgGpuCoreFrequencyMHz, RenderBusy, ComputeEngine0Busy) over command-buffer scopes
/// of the scheduler's submissions.
/// The profiling lock is exclusive: it is held only while a sampling window runs and is
/// released once the window's last result is read. Without the switch nothing is created;
/// without the extension or the lock the sampler reports supported=false with a reason.
class PerfQuery {
public:
    /// One counter of a sample, normalized to percentages/MHz where its unit allows it.
    struct Sample {
        std::string name;
        double value{};
        std::string unit;
    };

    explicit PerfQuery(const Device& device_);
    ~PerfQuery();

    /// EDEN_NX2_GPU_PERF=1 was set at startup (read once).
    [[nodiscard]] static bool Enabled();

    /// Whether counters can be sampled on this device.
    [[nodiscard]] bool Supported() const;
    /// Why not, when Supported() is false.
    [[nodiscard]] std::string Reason() const;

    /// Any thread: samples the next `frames` submissions (0 stops and releases the lock).
    /// False when unsupported or the profiling lock could not be acquired.
    bool RequestSampling(u32 frames);
    [[nodiscard]] bool Sampling() const;

    /// Worker thread: opens the query scope of a submission's command buffer.
    void Begin(vk::CommandBuffer cmdbuf);
    /// Worker thread: closes the scope; `tick` is the submission's signal value.
    void End(vk::CommandBuffer cmdbuf, u64 tick);

    /// Any thread: reads finished results (once the submission's GPU tick completed),
    /// averages them into one sample and releases the profiling lock when the window is
    /// done. Empty until the first result is read.
    std::vector<Sample> Read();

    /// The scheduler's timeline, to know when a sampled submission finished on the GPU.
    void SetSemaphore(const MasterSemaphore& semaphore_);
    /// Submissions whose counters were collected since the window started.
    [[nodiscard]] u64 FramesSampled() const;

private:
    struct Counter {
        std::string name;
        u32 index{}; ///< Index in the queue family's counter list
        VkPerformanceCounterUnitKHR unit{};
        VkPerformanceCounterStorageKHR storage{};
    };
    struct PendingSlot {
        u32 index{}; ///< Query pool slot
        u64 tick{};  ///< The submission's signal value
    };

    void ReleaseLock();
    /// Caller holds mutex: drains finished results into sums.
    void CollectLocked();

    const Device& device;
    const MasterSemaphore* semaphore{};
    PFN_vkAcquireProfilingLockKHR vkAcquireProfilingLockKHR{};
    PFN_vkReleaseProfilingLockKHR vkReleaseProfilingLockKHR{};

    mutable std::mutex mutex;
    std::string reason;
    bool supported{};
    bool lock_held{};
    bool scope_open{};
    u32 frames_left{};
    u64 frames_sampled{};
    u32 next_slot{};
    std::vector<PendingSlot> pending_slots;
    std::vector<Counter> counters;
    std::vector<double> sums;
    u64 collected{};
    vk::QueryPool pool;
    static constexpr u32 MaxSlots = 8;
};

} // namespace Vulkan
