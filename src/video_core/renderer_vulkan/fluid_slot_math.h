// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <span>

#include "common/common_types.h"

/// EDEN_NX2_FLUID slot arithmetic without the Vulkan objects around it, so tests can replay it.
namespace Vulkan::FluidSlots {

/// EDEN_NX2_FLUID_INTERP: where a real frame and its interpolated frames go.
struct InterpBatch {
    u64 first{};      ///< the first interpolated frame's slot; 0 with no vblank counted yet
    size_t wanted{};  ///< interpolated frames to make; the real frame takes the slot after them
    f64 span_slots{}; ///< each interpolated frame's blend is its slot's share of this many slots
};

/// `base`: the first slot the batch's GPU work can be done for, 0 with no vblank counted yet.
/// `last_real_slot`: the slot of the real frame the last FLUID_INTERP batch made, 0 if none.
/// `interval_ns`: since that real frame was recorded. `generations`: the cadence's share.
[[nodiscard]] inline InterpBatch PlanInterpBatch(u64 base, u64 last_real_slot, f64 interval_ns,
                                                 f64 slot_ns, u64 divisor, size_t generations) {
    // As many in-between frames as the time since the last real frame spans in slots, so a real
    // frame that comes late stretches its batch rather than leaving slots empty, and each frame's
    // blend is its slot's share of that interval. Batches follow on from the last real frame's
    // slot.
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
            // Right after the last real frame's slot when the batch is done in time for it.
            const u64 after_last = last_real_slot + divisor;
            batch.first = (std::max)(base, after_last);
            // That is this many slots later than the batch's GPU work allows. One slot of lead is
            // kept, so real frames near a slot boundary still leave no slot empty; beyond it, make
            // as many fewer in-between frames, so the real frame is at most one slot after
            // base + (span - 1) slots rather than rounding up and late batches adding latency that
            // is never taken back.
            const u64 slots_after =
                after_last > base ? (after_last - base) / (std::max<u64>)(divisor, 1) : 0;
            const u64 late = slots_after > 1 ? slots_after - 1 : 0;
            batch.wanted -= static_cast<size_t>((std::min<u64>)(late, batch.wanted));
        }
    }
    // At most two more than the cadence's share: each extra frame costs GPU time, and a game that
    // slows down must not get still more frames to make (and so slow down further).
    batch.wanted = (std::min<size_t>)(batch.wanted, (std::min<size_t>)(generations + 2, 11));
    return batch;
}

/// A phase batch only contains source moments strictly between the two real frames.
struct InterpPhases {
    std::array<f32, 11> factors{};
    size_t made{};
    u64 first{};
    u64 divisor{};

    /// Preserve main's real-frame placement if the renderer runs out of free frames.
    [[nodiscard]] constexpr u64 RealSlot(size_t available) const {
        return first != 0 ? first + (std::min)(made, available) * divisor : 0;
    }
};

/// PlanInterpBatch's base already excludes elapsed display slots. Anchor the older source
/// at last_real_slot; slot_ns is one presentation slot (divisor refreshes). The current clock
/// is used for both indices, so a refresh-period change can shift this anchor for one batch.
[[nodiscard]] inline InterpPhases PlanInterpPhases(const InterpBatch& batch, u64 last_real_slot,
                                                  f64 slot_ns, f64 interval_ns, u64 divisor,
                                                  size_t available = 11) {
    InterpPhases result{.first = batch.first, .divisor = divisor};
    if (batch.first == 0 || last_real_slot == 0 || divisor == 0 ||
        !std::isfinite(slot_ns) || slot_ns <= 0.0 || !std::isfinite(interval_ns) ||
        interval_ns <= 0.0) {
        return result;
    }
    const size_t limit = (std::min)({batch.wanted, available, result.factors.size()});
    for (size_t index = 0; index < limit; ++index) {
        const u64 slot = batch.first + index * divisor;
        const f64 source_ns = (static_cast<f64>(slot) - static_cast<f64>(last_real_slot)) *
                              slot_ns / static_cast<f64>(divisor);
        if (source_ns <= 0.0 || source_ns >= interval_ns) {
            break;
        }
        result.factors[result.made++] = static_cast<f32>(source_ns / interval_ns);
    }
    return result;
}

/// The present thread is back from a screen copy wait this long before the next wake it must not
/// miss, to present and get to its vblank wait in time.
inline constexpr std::chrono::nanoseconds CopyWaitMargin{500'000};

/// Vblanks from the one the present thread woke for to the next wake it must not sleep through:
/// the vblank before the next slot after the one this wake presents for, if any.
[[nodiscard]] constexpr u64 VblanksToNextSlotWake(u64 vblank, u64 divisor) {
    const u64 slots = (std::max<u64>)(divisor, 1);
    const u64 to = slots - 1 - vblank % slots;
    return to == 0 ? slots : to;
}

/// From the wake for `vblank`, until when a screen copy wait may last.
[[nodiscard]] constexpr std::chrono::nanoseconds CopyWaitDue(std::chrono::nanoseconds period,
                                                             u64 vblank, u64 divisor) {
    return period * static_cast<s64>(VblanksToNextSlotWake(vblank, divisor)) - CopyWaitMargin;
}

/// A screen copy wait's timeout: `bound_ns`, cut to the time `left` until CopyWaitDue; 0 (a look
/// at the fence) once that has passed.
[[nodiscard]] constexpr u64 CopyWaitNs(u64 bound_ns, std::chrono::nanoseconds left) {
    return left.count() <= 0 ? 0 : (std::min)(bound_ns, static_cast<u64>(left.count()));
}

/// EDEN_NX2_SHOWN_STATS: a frame shown for `shown_vblank` after one shown for `last_shown_vblank`
/// held the screen exactly one slot.
[[nodiscard]] constexpr bool HeldOneSlot(u64 shown_vblank, u64 last_shown_vblank, u64 divisor) {
    return shown_vblank - last_shown_vblank == divisor;
}

/// FluidCountVblanks reads at most this many wake intervals.
inline constexpr size_t MaxWakeIntervals = 32;

/// The rule FluidCountVblanks has always used: a median of the wake intervals replaces `period`
/// when it differs from it by more than 10% and lies in 2 to 50 ms (a dynamic refresh rate).
[[nodiscard]] inline bool WakesReplacePeriod(s64 median, s64 period) {
    return std::abs(median - period) * 10 > period && median >= 2'000'000 &&
           median <= 50'000'000;
}

/// EDEN_NX2_FLUID: the refresh period vblanks are counted with, from the display mode's period
/// `reported_ns` (60 Hz when not known) and the last wake intervals (the first
/// MaxWakeIntervals of them are read). With fewer than 8 intervals, that period as is; with
/// `keep_mode` off or no mode period, the median of the intervals by the rule of
/// WakesReplacePeriod.
/// EDEN_NX2_FLUID_MODE_PERIOD (`keep_mode`): sleeping through vblanks only lengthens intervals,
/// so a median within 10% of a whole multiple (2 or more) of the mode period is refused, and any
/// other median is taken only if the lower quartile of the intervals differs from the mode period
/// by more than 10% too, as it does on a display whose refresh really changed.
[[nodiscard]] inline s64 ChooseRefreshPeriodNs(std::span<const s64> intervals, s64 reported_ns,
                                               bool keep_mode) {
    const s64 period = reported_ns > 0 ? reported_ns : 16'666'667;
    if (intervals.size() < 8) {
        return period;
    }
    std::array<s64, MaxWakeIntervals> sorted{};
    const size_t count = (std::min)(intervals.size(), MaxWakeIntervals);
    std::copy_n(intervals.begin(), count, sorted.begin());
    const auto first = sorted.begin();
    const auto last = first + static_cast<std::ptrdiff_t>(count);
    const auto middle = first + static_cast<std::ptrdiff_t>(count / 2);
    std::nth_element(first, middle, last);
    const s64 median = *middle;
    if (!keep_mode || reported_ns <= 0) {
        return WakesReplacePeriod(median, period) ? median : period;
    }
    const s64 multiple = static_cast<s64>(
        std::llround(static_cast<f64>(median) / static_cast<f64>(reported_ns)));
    if (multiple >= 2 && std::abs(median - multiple * reported_ns) * 10 <= reported_ns) {
        return reported_ns;
    }
    if (!WakesReplacePeriod(median, reported_ns)) {
        return reported_ns;
    }
    const auto quarter = first + static_cast<std::ptrdiff_t>(count / 4);
    std::nth_element(first, quarter, last);
    const s64 quartile = *quarter;
    return std::abs(quartile - reported_ns) * 10 > reported_ns ? median : reported_ns;
}

/// The vblanks a wake `since_ns` after the last one counts: the nearest whole number of refreshes,
/// at least 1 (also with no period known).
[[nodiscard]] inline s64 ElapsedVblanks(s64 since_ns, s64 period_ns) {
    if (period_ns <= 0) {
        return 1;
    }
    const f64 refreshes = static_cast<f64>(since_ns) / static_cast<f64>(period_ns);
    return (std::max<s64>)(1, static_cast<s64>(std::llround(refreshes)));
}

} // namespace Vulkan::FluidSlots
