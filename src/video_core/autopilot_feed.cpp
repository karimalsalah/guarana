// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <atomic>

#include "video_core/autopilot_feed.h"

namespace VideoCore::AutopilotFeed {
namespace {

constexpr u64 Capacity = 2048;

std::atomic<bool> active{};
std::atomic<u64> shown_count{};
std::array<std::atomic<u64>, Capacity> shown_times_ns{};
std::atomic<s64> refresh_period_ns{};
std::atomic<double> slot_rate{};
/// Only the autopilot's thread reads.
u64 taken{};

u64 NowNs() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

} // Anonymous namespace

void SetActive(bool value) {
    active.store(value, std::memory_order_relaxed);
    if (!value) {
        slot_rate.store(0.0, std::memory_order_relaxed);
    }
    taken = shown_count.load(std::memory_order_relaxed);
}

bool Active() {
    return active.load(std::memory_order_relaxed);
}

void NoteShown() {
    if (!active.load(std::memory_order_relaxed)) {
        return;
    }
    // Present threads are one at a time (the swapchain lock), so the slot is written before the
    // count that publishes it.
    const u64 index = shown_count.load(std::memory_order_relaxed);
    shown_times_ns[index % Capacity].store(NowNs(), std::memory_order_relaxed);
    shown_count.store(index + 1, std::memory_order_release);
}

std::vector<double> TakeShownIntervals() {
    const u64 last = shown_count.load(std::memory_order_acquire);
    u64 first = taken;
    if (last - first > Capacity - 1) {
        first = last - (Capacity - 1);
    }
    std::vector<double> intervals;
    for (u64 index = first + 1; index < last; ++index) {
        const u64 before = shown_times_ns[(index - 1) % Capacity].load(std::memory_order_relaxed);
        const u64 after = shown_times_ns[index % Capacity].load(std::memory_order_relaxed);
        if (after > before) {
            intervals.push_back(static_cast<double>(after - before) / 1.0e6);
        }
    }
    // The next window starts from the last frame seen, so no interval is lost between them.
    taken = last > 0 ? last - 1 : 0;
    return intervals;
}

void SetRefreshPeriod(std::chrono::nanoseconds period) {
    refresh_period_ns.store(period.count(), std::memory_order_relaxed);
}

double RefreshHz() {
    const s64 period = refresh_period_ns.load(std::memory_order_relaxed);
    return period > 0 ? 1.0e9 / static_cast<double>(period) : 0.0;
}

void SetSlotRate(double fps) {
    slot_rate.store(fps > 0.0 ? fps : 0.0, std::memory_order_relaxed);
}

double SlotRate() {
    return slot_rate.load(std::memory_order_relaxed);
}

} // namespace VideoCore::AutopilotFeed
