// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <initializer_list>
#include <iterator>
#include <random>
#include <span>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "video_core/renderer_vulkan/fluid_slot_math.h"
#include "video_core/renderer_vulkan/vk_present_manager.h"

TEST_CASE("FLUID_INTERP_PHASE: on-time blends and real slot match main", "[video_core][fluid]") {
    using namespace Vulkan::FluidSlots;
    const auto batch = PlanInterpBatch(102, 100, 60.0, 10.0, 2, 5);
    const auto phases = PlanInterpPhases(batch, 100, 10.0, 60.0, 2);
    REQUIRE(phases.made == batch.wanted);
    REQUIRE(phases.made == 5);
    REQUIRE(phases.RealSlot(11) == 112);
    for (size_t index = 0; index < phases.made; ++index) {
        REQUIRE(phases.factors[index] ==
                Catch::Approx(static_cast<f64>(index + 1) / batch.span_slots));
    }
}

TEST_CASE("FLUID_INTERP_PHASE: late arrival keeps source moments and advances the real frame",
          "[video_core][fluid]") {
    using namespace Vulkan::FluidSlots;
    const auto batch = PlanInterpBatch(104, 100, 60.0, 10.0, 1, 5);
    const auto phases = PlanInterpPhases(batch, 100, 10.0, 60.0, 1);
    REQUIRE(phases.made == 2);
    REQUIRE(phases.factors[0] == Catch::Approx(4.0 / 6.0));
    REQUIRE(phases.factors[1] == Catch::Approx(5.0 / 6.0));
    REQUIRE(phases.RealSlot(11) == 106);
    REQUIRE(batch.first + batch.wanted == 109); // Main's real frame.
}

TEST_CASE("FLUID_INTERP_PHASE: exhausted pools preserve planned factors and bound real slots",
          "[video_core][fluid]") {
    using namespace Vulkan::FluidSlots;
    for (const u64 base : {102ULL, 108ULL}) {
        const auto batch = PlanInterpBatch(base, 100, 60.0, 10.0, 2, 5);
        const auto full = PlanInterpPhases(batch, 100, 10.0, 60.0, 2);
        for (size_t available = 0; available <= 7; ++available) {
            const auto phases = PlanInterpPhases(batch, 100, 10.0, 60.0, 2, available);
            REQUIRE(phases.made == (std::min)(available, full.made));
            REQUIRE(phases.RealSlot(available) == full.RealSlot(available));
            REQUIRE(phases.RealSlot(available) <=
                    batch.first + (std::min)(available, batch.wanted) * 2);
            for (size_t index = 0; index < phases.made; ++index) {
                REQUIRE(phases.factors[index] == full.factors[index]);
            }
        }
    }
}

TEST_CASE("FLUID_INTERP_PHASE: fractional 144 Hz interval is not rounded",
          "[video_core][fluid]") {
    using namespace Vulkan::FluidSlots;
    constexpr f64 slot_ns = 1.0e9 / 144.0;
    constexpr f64 interval_ns = 1.0e9 / 24.3;
    for (const u64 base : {101ULL, 102ULL}) {
        const auto batch = PlanInterpBatch(base, 100, interval_ns, slot_ns, 1, 5);
        const auto phases = PlanInterpPhases(batch, 100, slot_ns, interval_ns, 1);
        REQUIRE(batch.span_slots == 6.0);
        REQUIRE(phases.made == (base == 101 ? 5 : 4));
        REQUIRE(phases.RealSlot(11) == 106);
        for (size_t index = 0; index < phases.made; ++index) {
            const f64 source_slots = static_cast<f64>(base - 100 + index);
            REQUIRE(phases.factors[index] == Catch::Approx(source_slots * 24.3 / 144.0));
        }
    }
}

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

namespace {

using WakeRing = std::array<s64, 15>;

/// FluidCountVblanks's period before EDEN_NX2_FLUID_MODE_PERIOD, as it was written inline.
s64 PeriodBeforeModeSwitch(const WakeRing& intervals, size_t interval_count, nanoseconds reported) {
    nanoseconds period = reported.count() > 0 ? reported : nanoseconds{16'666'667};
    if (interval_count >= 8) {
        auto sorted = intervals;
        const auto count = static_cast<std::ptrdiff_t>(interval_count);
        std::nth_element(sorted.begin(), sorted.begin() + count / 2, sorted.begin() + count);
        const s64 median = sorted[static_cast<size_t>(count / 2)];
        const s64 nominal = period.count();
        if (std::abs(median - nominal) * 10 > nominal && median >= 2'000'000 &&
            median <= 50'000'000) {
            period = nanoseconds{median};
        }
    }
    return period.count();
}

/// FluidCountVblanks's elapsed vblanks before EDEN_NX2_FLUID_MODE_PERIOD, for a wake after the
/// first.
s64 ElapsedBeforeModeSwitch(s64 since, s64 period) {
    const f64 refreshes = static_cast<f64>(since) / static_cast<f64>(period);
    return (std::max<s64>)(1, static_cast<s64>(std::llround(refreshes)));
}

WakeRing RingOf(std::initializer_list<s64> values) {
    WakeRing ring{};
    std::copy(values.begin(), values.end(), ring.begin());
    return ring;
}

WakeRing Filled(s64 interval) {
    WakeRing ring{};
    ring.fill(interval);
    return ring;
}

s64 Choose(const WakeRing& ring, size_t count, nanoseconds reported, bool keep_mode) {
    return Vulkan::FluidSlots::ChooseRefreshPeriodNs(std::span<const s64>{ring.data(), count},
                                                     reported.count(), keep_mode);
}

} // namespace

TEST_CASE("FLUID_MODE_PERIOD: wakes that sleep through vblanks keep the mode's period",
          "[video_core][fluid]") {
    // Every other vblank slept through at 144 Hz: the median is two refreshes.
    const WakeRing every_other = Filled(13'888'889);
    REQUIRE(Choose(every_other, 15, Hz144, false) == 13'888'889);
    REQUIRE(Choose(every_other, 15, Hz144, true) == Hz144.count());
    // Nine wakes two refreshes apart and six one apart.
    const WakeRing mixed =
        RingOf({13'888'889, 6'944'444, 13'888'889, 13'888'889, 6'944'444, 13'888'889, 6'944'444,
                13'888'889, 13'888'889, 6'944'444, 13'888'889, 6'944'444, 13'888'889, 6'944'444,
                13'888'889});
    REQUIRE(Choose(mixed, 15, Hz144, false) == 13'888'889);
    REQUIRE(Choose(mixed, 15, Hz144, true) == Hz144.count());
    // Three refreshes each.
    const WakeRing every_third = Filled(20'833'333);
    REQUIRE(Choose(every_third, 15, Hz144, false) == 20'833'333);
    REQUIRE(Choose(every_third, 15, Hz144, true) == Hz144.count());
}

TEST_CASE("FLUID_MODE_PERIOD: a display whose refresh really changed is still followed",
          "[video_core][fluid]") {
    // A variable refresh display at 120 Hz while the mode says 144 (1.2 refreshes, no multiple).
    const WakeRing variable = Filled(Hz120.count());
    REQUIRE(Choose(variable, 15, Hz144, false) == Hz120.count());
    REQUIRE(Choose(variable, 15, Hz144, true) == Hz120.count());
    // Wakes within 2% of the mode's period: both keep it.
    const WakeRing near_mode =
        RingOf({7'000'000, 6'900'000, 6'944'444, 7'050'000, 6'850'000, 6'944'444, 7'000'000,
                6'900'000, 6'944'444, 7'050'000, 6'850'000, 6'944'444, 7'000'000, 6'900'000,
                6'944'444});
    REQUIRE(Choose(near_mode, 15, Hz144, false) == Hz144.count());
    REQUIRE(Choose(near_mode, 15, Hz144, true) == Hz144.count());
    // A median 1.2 refreshes long whose lower quartile is the mode's period: kept.
    const WakeRing quartile_on_mode =
        RingOf({6'944'444, 6'944'444, 6'944'444, 6'944'444, 6'944'444, 8'333'333, 8'333'333,
                8'333'333, 8'333'333, 8'333'333, 8'333'333, 8'333'333, 8'333'333, 8'333'333,
                8'333'333});
    REQUIRE(Choose(quartile_on_mode, 15, Hz144, false) == Hz120.count());
    REQUIRE(Choose(quartile_on_mode, 15, Hz144, true) == Hz144.count());
}

TEST_CASE("FLUID_MODE_PERIOD: no mode period or too few wakes", "[video_core][fluid]") {
    // No mode period: both follow the old rule, also for a whole multiple of 60 Hz.
    const s64 no_mode_intervals[] = {Hz120.count(), Hz60.count(), 33'333'333, 60'000'000};
    for (const s64 interval : no_mode_intervals) {
        const WakeRing ring = Filled(interval);
        const s64 before = PeriodBeforeModeSwitch(ring, 15, nanoseconds{0});
        REQUIRE(Choose(ring, 15, nanoseconds{0}, false) == before);
        REQUIRE(Choose(ring, 15, nanoseconds{0}, true) == before);
    }
    REQUIRE(Choose(Filled(Hz120.count()), 15, nanoseconds{0}, true) == Hz120.count());
    REQUIRE(Choose(Filled(33'333'333), 15, nanoseconds{0}, true) == 33'333'333);
    // Seven wakes: the mode's period, or 60 Hz without one.
    const WakeRing every_other = Filled(13'888'889);
    REQUIRE(Choose(every_other, 7, Hz144, false) == Hz144.count());
    REQUIRE(Choose(every_other, 7, Hz144, true) == Hz144.count());
    REQUIRE(Choose(every_other, 7, nanoseconds{0}, true) == Hz60.count());
}

TEST_CASE("FLUID_MODE_PERIOD: elapsed vblanks", "[video_core][fluid]") {
    using Vulkan::FluidSlots::ElapsedVblanks;
    REQUIRE(ElapsedVblanks(13'900'000, Hz144.count()) == 2);
    REQUIRE(ElapsedVblanks(3'000'000, Hz144.count()) == 1);
    REQUIRE(ElapsedVblanks(0, Hz144.count()) == 1);
    REQUIRE(ElapsedVblanks(13'900'000, 0) == 1);
    REQUIRE(ElapsedVblanks(20'833'333, Hz144.count()) == 3);
}

TEST_CASE("FLUID_MODE_PERIOD: with the switch off the period is the old rule's",
          "[video_core][fluid]") {
    // Seeded, so every run checks the same table.
    std::mt19937_64 rng{136};
    const auto below = [&rng](u64 bound) { return rng() % bound; };
    constexpr s64 bases[] = {1'000'000,  6'944'444,  8'333'333,  13'888'889,
                             16'666'667, 20'833'333, 33'333'333, 60'000'000};
    const nanoseconds reported_choices[] = {nanoseconds{0}, nanoseconds{-1}, Hz60, Hz75, Hz120,
                                            Hz144};
    for (int trial = 0; trial < 4000; ++trial) {
        WakeRing ring{};
        const size_t count = static_cast<size_t>(below(ring.size() + 1));
        const s64 base = bases[below(std::size(bases))];
        for (size_t index = 0; index < count; ++index) {
            // A wake within 25% of the base, or now and then one of the other bases.
            const s64 pick = below(5) == 0 ? bases[below(std::size(bases))] : base;
            const s64 spread = static_cast<s64>(below(static_cast<u64>(pick / 2) + 1));
            ring[index] = pick - pick / 4 + spread;
        }
        const nanoseconds reported = reported_choices[below(std::size(reported_choices))];
        const s64 period = Choose(ring, count, reported, false);
        REQUIRE(period == PeriodBeforeModeSwitch(ring, count, reported));
        // With the switch on: the old rule's period or the mode's, never a third one.
        const s64 kept = Choose(ring, count, reported, true);
        REQUIRE((kept == period || (reported.count() > 0 && kept == reported.count())));
        const s64 since = static_cast<s64>(below(100'000'000));
        REQUIRE(Vulkan::FluidSlots::ElapsedVblanks(since, period) ==
                ElapsedBeforeModeSwitch(since, period));
    }
}
