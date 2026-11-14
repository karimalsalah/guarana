// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <initializer_list>

#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "video_core/renderer_vulkan/fluid_slot_math.h"
#include "video_core/renderer_vulkan/vk_present_manager.h"

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using Vulkan::PresentManager;
using Vulkan::FluidSlots::InterpBatch;
using std::chrono::nanoseconds;

constexpr nanoseconds Hz60{16'666'667};
constexpr nanoseconds Hz75{13'333'333};
constexpr nanoseconds Hz120{8'333'333};
constexpr nanoseconds Hz144{6'944'444};

/// RecordFluidInterpolated's plan before #104: the same, but a batch that starts late keeps all
/// its in-between frames.
InterpBatch PlanBefore104(u64 base, u64 last_real_slot, f64 interval_ns, f64 slot_ns, u64 divisor,
                          size_t generations) {
    InterpBatch batch{
        .first = base,
        .wanted = generations,
        .span_slots = static_cast<f64>(generations + 1),
    };
    if (base != 0 && last_real_slot != 0) {
        const f64 slots_between = slot_ns > 0.0 ? interval_ns / slot_ns : 0.0;
        if (slots_between >= 1.0 && slots_between <= 24.0) {
            batch.span_slots = (std::max)(1.0, std::round(slots_between));
            batch.wanted = static_cast<size_t>(batch.span_slots) - 1;
            batch.first = (std::max)(base, last_real_slot + divisor);
        }
    }
    batch.wanted = (std::min<size_t>)(batch.wanted, (std::min<size_t>)(generations + 2, 11));
    return batch;
}

struct Cadence {
    nanoseconds refresh;
    u64 divisor;
    nanoseconds interval;  ///< between real frames
    size_t generations;    ///< the cadence's share of in-between frames
    nanoseconds jitter{};  ///< odd real frames are recorded this much later, even ones earlier
    nanoseconds phase{};   ///< every real frame is recorded this much later
};

/// The vblank clock RecordFluidInterpolated reads, with the default 3.5 ms present offset and a
/// real frame's GPU work done 20 ms after it is recorded.
PresentManager::FluidClock ClockFor(const Cadence& cadence) {
    const nanoseconds offset = (std::min)(nanoseconds{3'500'000}, cadence.refresh * 42 / 100);
    return PresentManager::FluidClock{
        .vblank = Clock::time_point{} + 1s,
        .vblank_index = 1000,
        .refresh = cadence.refresh,
        .divisor = cadence.divisor,
        .lead = cadence.refresh - offset,
        .latency = 20ms,
    };
}

struct Replay {
    s64 most_late{}; ///< the most slots a real frame was shown after base + (span - 1) slots
    size_t gaps{};   ///< batches that left a slot empty after the last real frame
    size_t uneven{}; ///< real frames not generations + 1 slots after the last one
};

/// Replays `batches` real frames through `plan` with a frame pool that never runs out, as
/// RecordFluidInterpolated does. base + (span - 1) slots is where interpolation needs a real
/// frame at the latest.
template <typename Plan>
Replay Run(const Cadence& cadence, Plan plan, size_t batches) {
    const PresentManager::FluidClock slots = ClockFor(cadence);
    const f64 slot_ns =
        static_cast<f64>((cadence.refresh * static_cast<s64>(cadence.divisor)).count());
    u64 last_slot = 0;
    Clock::time_point last_recorded{};
    Replay replay{};
    for (size_t n = 1; n <= batches; ++n) {
        const Clock::time_point recorded = slots.vblank + cadence.interval * static_cast<s64>(n) +
                                           (n % 2 != 0 ? cadence.jitter : -cadence.jitter) +
                                           cadence.phase;
        const u64 base = slots.FirstSlotPresentedAfter(recorded + slots.latency);
        const InterpBatch batch =
            plan(base, last_recorded != Clock::time_point{} ? last_slot : 0,
                 static_cast<f64>(std::chrono::duration_cast<nanoseconds>(recorded - last_recorded)
                                      .count()),
                 slot_ns, cadence.divisor, cadence.generations);
        const u64 real = batch.first + static_cast<u64>(batch.wanted) * cadence.divisor;
        REQUIRE(real >= base);
        const s64 late = static_cast<s64>((real - base) / cadence.divisor) -
                         (static_cast<s64>(batch.span_slots) - 1);
        replay.most_late = (std::max)(replay.most_late, late);
        if (last_recorded != Clock::time_point{}) {
            const u64 step = (real - last_slot) / cadence.divisor;
            replay.gaps += batch.first > last_slot + cadence.divisor ? 1 : 0;
            replay.uneven += step != static_cast<u64>(cadence.generations) + 1 ? 1 : 0;
        }
        last_slot = real;
        last_recorded = recorded;
    }
    return replay;
}

template <typename Plan>
s64 MostLate(const Cadence& cadence, Plan plan, size_t batches) {
    return Run(cadence, plan, batches).most_late;
}

} // namespace

TEST_CASE("FLUID_INTERP: a real frame that rounds up does not push later batches later",
          "[video_core][fluid]") {
    // The replay of #104: a real frame every 56.8 ms (6.8 slots, rounded to 7) at 120 Hz.
    const Cadence cadence{
        .refresh = Hz120, .divisor = 1, .interval = 56'800'000ns, .generations = 6};
    REQUIRE(MostLate(cadence, PlanBefore104, 200) >= 20); // about 0.18 slot more each batch
    REQUIRE(MostLate(cadence, Vulkan::FluidSlots::PlanInterpBatch, 200) <= 1);
}

TEST_CASE("FLUID_INTERP: late and early real frames do not add up", "[video_core][fluid]") {
    // 20 real frames a second at 120 Hz, alternately 1.5 ms late and early.
    const Cadence jittered{.refresh = Hz120,
                           .divisor = 1,
                           .interval = Hz120 * 6,
                           .generations = 5,
                           .jitter = 1'500'000ns};
    REQUIRE(MostLate(jittered, PlanBefore104, 200) >= 1);
    REQUIRE(MostLate(jittered, Vulkan::FluidSlots::PlanInterpBatch, 200) <= 1);
    // 20 real frames a second shown at 72 on a 144 Hz display: a slot is two refreshes.
    const Cadence two_refreshes{
        .refresh = Hz144, .divisor = 2, .interval = 50'000'000ns, .generations = 3};
    REQUIRE(MostLate(two_refreshes, PlanBefore104, 200) >= 20);
    REQUIRE(MostLate(two_refreshes, Vulkan::FluidSlots::PlanInterpBatch, 200) <= 1);
}

TEST_CASE("FLUID_INTERP: real frames near a slot boundary leave no slot empty",
          "[video_core][fluid]") {
    // The default presets: 20 real frames at 120 Hz and 24 at 144 Hz, six slots each, with real
    // frames alternately late and early across every sixteenth of a slot.
    for (const nanoseconds refresh : {Hz120, Hz144}) {
        for (const nanoseconds jitter : {500'000ns, 1'500'000ns}) {
            for (s64 sixteenth = 0; sixteenth < 16; ++sixteenth) {
                const Cadence cadence{.refresh = refresh,
                                      .divisor = 1,
                                      .interval = refresh * 6,
                                      .generations = 5,
                                      .jitter = jitter,
                                      .phase = refresh * sixteenth / 16};
                const Replay replay = Run(cadence, Vulkan::FluidSlots::PlanInterpBatch, 200);
                REQUIRE(replay.gaps == 0);
                REQUIRE(replay.uneven == 0);
                REQUIRE(replay.most_late <= 1);
            }
        }
    }
}

TEST_CASE("FLUID_INTERP: real frames on whole slots at 60 and 120 Hz plan as before",
          "[video_core][fluid]") {
    const Cadence cadences[] = {
        {.refresh = Hz120, .divisor = 1, .interval = Hz120 * 2, .generations = 1}, // 60 at 120
        {.refresh = Hz120, .divisor = 1, .interval = Hz120 * 6, .generations = 5}, // 20 at 120
        {.refresh = Hz120, .divisor = 2, .interval = Hz120 * 6, .generations = 2}, // 20, 60 shown
        {.refresh = Hz60, .divisor = 1, .interval = Hz60 * 2, .generations = 1},   // 30 at 60
        {.refresh = Hz60, .divisor = 1, .interval = Hz60 * 3, .generations = 2},   // 20 at 60
    };
    for (const Cadence& cadence : cadences) {
        const PresentManager::FluidClock slots = ClockFor(cadence);
        const f64 slot_ns =
            static_cast<f64>((cadence.refresh * static_cast<s64>(cadence.divisor)).count());
        u64 last_slot = 0;
        Clock::time_point last_recorded{};
        for (size_t n = 1; n <= 200; ++n) {
            const Clock::time_point recorded =
                slots.vblank + cadence.interval * static_cast<s64>(n);
            const u64 base = slots.FirstSlotPresentedAfter(recorded + slots.latency);
            const u64 last = last_recorded != Clock::time_point{} ? last_slot : 0;
            const f64 interval_ns = static_cast<f64>(
                std::chrono::duration_cast<nanoseconds>(recorded - last_recorded).count());
            const InterpBatch now = Vulkan::FluidSlots::PlanInterpBatch(
                base, last, interval_ns, slot_ns, cadence.divisor, cadence.generations);
            const InterpBatch before = PlanBefore104(base, last, interval_ns, slot_ns,
                                                     cadence.divisor, cadence.generations);
            REQUIRE(now.first == before.first);
            REQUIRE(now.wanted == before.wanted);
            // Whole numbers of slots: compared as integers.
            REQUIRE(static_cast<s64>(now.span_slots) == static_cast<s64>(before.span_slots));
            last_slot = now.first + static_cast<u64>(now.wanted) * cadence.divisor;
            last_recorded = recorded;
        }
    }
}

TEST_CASE("FLUID: the copy wait ends before the next slot's wake", "[video_core][fluid]") {
    using Vulkan::FluidSlots::CopyWaitDue;
    using Vulkan::FluidSlots::CopyWaitNs;
    using Vulkan::FluidSlots::VblanksToNextSlotWake;
    // A slot every refresh: the next wake is the next vblank.
    REQUIRE(VblanksToNextSlotWake(7, 1) == 1);
    // Every other refresh: from the wake before a slot (odd), two; from the slot's own (even), one.
    REQUIRE(VblanksToNextSlotWake(7, 2) == 2);
    REQUIRE(VblanksToNextSlotWake(8, 2) == 1);
    REQUIRE(VblanksToNextSlotWake(3, 4) == 4);
    REQUIRE(VblanksToNextSlotWake(4, 4) == 3);
    REQUIRE(VblanksToNextSlotWake(5, 0) == 1);
    // Once the time is up, only a look at the fence.
    REQUIRE(CopyWaitNs(12'000'000, nanoseconds{0}) == 0);
    REQUIRE(CopyWaitNs(12'000'000, nanoseconds{-1}) == 0);
    REQUIRE(CopyWaitNs(12'000'000, nanoseconds{5'000'000}) == 5'000'000);

    struct Mode {
        nanoseconds period;
        u64 divisor;
        u64 bound_ns; ///< FluidHostWaitNs's default for the shown rate
    };
    // The wait starts at the present offset after the slot's wake, or up to 0.3 ms after it.
    const auto wait_at = [](const Mode& mode, nanoseconds started) {
        const u64 wake_vblank = mode.divisor - 1; // the vblank before a slot
        return CopyWaitNs(mode.bound_ns,
                          CopyWaitDue(mode.period, wake_vblank, mode.divisor) - started);
    };
    const auto offset_of = [](nanoseconds period) {
        return (std::min)(nanoseconds{3'500'000}, period * 42 / 100);
    };

    // 60 shown at 60 and 120 Hz, 120 shown at 120 Hz: the bound as before.
    const Mode unchanged[] = {
        {.period = Hz60, .divisor = 1, .bound_ns = 12'000'000},
        {.period = Hz120, .divisor = 2, .bound_ns = 12'000'000},
        {.period = Hz120, .divisor = 1, .bound_ns = 4'000'000},
    };
    for (const Mode& mode : unchanged) {
        for (const nanoseconds late : {0ns, 300'000ns}) {
            REQUIRE(wait_at(mode, offset_of(mode.period) + late) == mode.bound_ns);
        }
    }

    // 60 shown at 75 and 144 Hz, 144 shown at 144 Hz: the bound overran the next slot's wake
    // (12 ms from 3.5 ms into a 13.3 ms slot, from 2.9 ms into a 13.9 ms one; 4 ms from 2.9 ms
    // into a 6.9 ms one). Now the thread is back half a ms before it.
    const Mode cut[] = {
        {.period = Hz75, .divisor = 1, .bound_ns = 12'000'000},
        {.period = Hz144, .divisor = 2, .bound_ns = 12'000'000},
        {.period = Hz144, .divisor = 1, .bound_ns = 4'000'000},
    };
    for (const Mode& mode : cut) {
        for (const nanoseconds late : {0ns, 300'000ns}) {
            const nanoseconds started = offset_of(mode.period) + late;
            const u64 wait = wait_at(mode, started);
            REQUIRE(wait < mode.bound_ns);
            const nanoseconds next_wake = mode.period * static_cast<s64>(mode.divisor);
            REQUIRE(started + nanoseconds{static_cast<s64>(wait)} ==
                    next_wake - Vulkan::FluidSlots::CopyWaitMargin);
        }
    }
}

TEST_CASE("FLUID: one slot held counts slots of any length", "[video_core][fluid]") {
    using Vulkan::FluidSlots::HeldOneSlot;
    // A slot of four refreshes (240 Hz at 60): shown for slots 4, 8 and 16, one slot was missed.
    REQUIRE(HeldOneSlot(8, 4, 4));
    REQUIRE_FALSE(HeldOneSlot(16, 8, 4));
    // A slot of six refreshes (144 Hz at 24): an even sequence is all one slot.
    REQUIRE(HeldOneSlot(12, 6, 6));
    REQUIRE(HeldOneSlot(18, 12, 6));
    // A slot of one or two refreshes, as held[divisor - 1] counted it.
    REQUIRE(HeldOneSlot(11, 10, 1));
    REQUIRE_FALSE(HeldOneSlot(12, 10, 1));
    REQUIRE(HeldOneSlot(12, 10, 2));
    REQUIRE_FALSE(HeldOneSlot(11, 10, 2));
    REQUIRE_FALSE(HeldOneSlot(13, 10, 2));
}
