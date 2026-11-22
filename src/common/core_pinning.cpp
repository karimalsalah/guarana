// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <charconv>

#include "common/core_pinning.h"

namespace Common {

GpuThreadPinning PlanGpuThreadPinning(std::span<const LogicalProcessor> processors) {
    GpuThreadPinning plan;
    if (processors.empty()) {
        return plan;
    }
    const u8 fastest =
        std::ranges::max(processors, {}, &LogicalProcessor::efficiency_class).efficiency_class;
    std::vector<u32> cores;
    for (const LogicalProcessor& processor : processors) {
        if (processor.efficiency_class == fastest) {
            cores.push_back(processor.core);
        }
    }
    std::ranges::sort(cores);
    cores.erase(std::unique(cores.begin(), cores.end()), cores.end());
    plan.performance_cores = cores.size();
    if (cores.size() < MinCoresForGpuThreadPinning) {
        return plan;
    }
    const u32 front_core = cores[1];
    const u32 back_core = cores[2];
    for (const LogicalProcessor& processor : processors) {
        if (processor.efficiency_class != fastest) {
            continue;
        }
        if (processor.core == front_core) {
            plan.front.push_back(processor.id);
        } else if (processor.core == back_core) {
            plan.back.push_back(processor.id);
        } else {
            plan.guest.push_back(processor.id);
        }
    }
    return plan;
}

HybridPlan PlanHybridCores(std::span<const LogicalProcessor> processors, bool pin_gpu_threads) {
    HybridPlan plan;
    if (processors.empty()) {
        return plan;
    }
    u8 fastest = 0;
    u8 lowest = 255;
    for (const LogicalProcessor& processor : processors) {
        fastest = (std::max)(fastest, processor.efficiency_class);
        lowest = (std::min)(lowest, processor.efficiency_class);
    }
    if (lowest == fastest) {
        return plan;
    }
    std::vector<u32> cores;
    std::vector<u32> caches;
    bool middle_class = false;
    for (const LogicalProcessor& processor : processors) {
        if (processor.efficiency_class == fastest) {
            cores.push_back(processor.core);
            caches.push_back(processor.last_level_cache);
        } else if (processor.efficiency_class != lowest) {
            middle_class = true;
        }
    }
    std::ranges::sort(cores);
    cores.erase(std::unique(cores.begin(), cores.end()), cores.end());
    plan.performance_cores = cores.size();
    if (cores.size() < MinHybridPerformanceCores) {
        return plan;
    }
    plan.four_p = cores.size() < SixPHybridPerformanceCores;
    std::ranges::sort(caches);
    for (const LogicalProcessor& processor : processors) {
        if (processor.efficiency_class == fastest) {
            plan.performance.push_back(processor.id);
        } else if (!std::ranges::binary_search(caches, processor.last_level_cache) ||
                   (middle_class && processor.efficiency_class == lowest)) {
            plan.low_power.push_back(processor.id);
        } else {
            plan.efficiency.push_back(processor.id);
        }
    }
    // Four-P: when no efficiency core shares the performance cores' cache (Lunar Lake's four sit
    // on an island), the island is not a low-power add-on to keep clear of but the CPU's only
    // efficiency cores.
    if (plan.four_p && plan.efficiency.empty()) {
        plan.efficiency = std::move(plan.low_power);
        plan.low_power.clear();
    }
    plan.responsive = plan.performance;
    plan.responsive.insert(plan.responsive.end(), plan.efficiency.begin(), plan.efficiency.end());
    if (plan.four_p) {
        // Four P cores cannot hold the six guest cores, the GPU front and back, the Vulkan worker
        // and the helpers. The three threads each frame waits on in turn keep the P cores; the
        // guest cores and the latency threads, which mostly sleep and need a prompt wake-up more
        // than a fast core, may also use the efficiency cores, with high QoS so Windows still
        // prefers a free P core. No core is dedicated: that needs MinCoresForGpuThreadPinning.
        plan.render = plan.performance;
        plan.front = plan.performance;
        plan.back = plan.performance;
        plan.latency = plan.responsive;
        plan.active = true;
        return plan;
    }
    GpuThreadPinning pinning = PlanGpuThreadPinning(processors);
    if (pin_gpu_threads && !pinning.front.empty()) {
        plan.front = std::move(pinning.front);
        plan.back = std::move(pinning.back);
        plan.latency = std::move(pinning.guest);
    } else {
        plan.front = plan.performance;
        plan.back = plan.performance;
        plan.latency = plan.performance;
    }
    plan.render = plan.latency;
    plan.active = true;
    return plan;
}

std::vector<u32> ParseCpuList(std::string_view list) {
    std::vector<u32> cpus;
    while (!list.empty() && (list.back() == '\n' || list.back() == ' ')) {
        list.remove_suffix(1);
    }
    const auto number = [](std::string_view text, u32& value) {
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        return error == std::errc{} && end == text.data() + text.size();
    };
    while (!list.empty()) {
        const size_t comma = list.find(',');
        const std::string_view range = list.substr(0, comma);
        list = comma == std::string_view::npos ? std::string_view{} : list.substr(comma + 1);
        const size_t dash = range.find('-');
        u32 first{};
        u32 last{};
        if (!number(range.substr(0, dash), first) ||
            !number(dash == std::string_view::npos ? range.substr(0, dash) : range.substr(dash + 1),
                    last) ||
            last < first || last - first >= 4096) {
            return {};
        }
        // 64-bit, so a range ending at the largest u32 doesn't wrap to 0 and loop forever.
        for (u64 cpu = first; cpu <= last; ++cpu) {
            cpus.push_back(static_cast<u32>(cpu));
        }
    }
    return cpus;
}

} // namespace Common
