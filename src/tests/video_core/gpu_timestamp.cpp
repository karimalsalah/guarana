// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <chrono>

#include <catch2/catch_test_macros.hpp>

#include "video_core/gpu_timestamp.h"

namespace {

struct Timing {
    std::chrono::nanoseconds time;
    u64 gpu_ticks;

    std::chrono::nanoseconds GetGlobalTimeNs() const {
        return time;
    }

    u64 GetGPUTicks() const {
        return gpu_ticks;
    }
};

constexpr std::array Clocks{Settings::GpuClock::Normal, Settings::GpuClock::Boost,
                            Settings::GpuClock::Overclock};

Timing At(u64 ns) {
    return {std::chrono::nanoseconds{ns}, ns * 384 / 625};
}

u64 Ampere(u64 now, Settings::GpuClock latched, u64 present_ns,
           Settings::GpuClock live = Settings::GpuClock::Normal) {
    return Tegra::ReadGpuTimestamp(At(now), true, live, Tegra::AmpereAnchor::At(present_ns, latched));
}

} // namespace

TEST_CASE("Ampere report timestamps use driver and VI nanoseconds", "[video_core][gpu_timestamp]") {
    constexpr u64 HostTimeNs = 293'036'000'000'000;
    for (const auto clock : Clocks) {
        CAPTURE(clock);
        REQUIRE(Ampere(HostTimeNs, clock, HostTimeNs) == HostTimeNs);
        REQUIRE(Ampere(HostTimeNs, clock, HostTimeNs + 1) == HostTimeNs);
    }
}

TEST_CASE("Ampere timestamps are exact before the first present", "[video_core][gpu_timestamp]") {
    constexpr u64 UptimeNs = 293'036'000'000'000;
    constexpr u64 FirstPresentNs = UptimeNs + 8'000'000;
    for (const auto clock : Clocks) {
        CAPTURE(clock);
        REQUIRE(Ampere(UptimeNs, clock, 0) == UptimeNs);
        const u64 before = Ampere(FirstPresentNs - 1, clock, 0);
        const u64 after = Ampere(FirstPresentNs + 4'000'000, clock, FirstPresentNs);
        REQUIRE(before == FirstPresentNs - 1);
        REQUIRE(after - before == 1 + 4'000'000 / Tegra::AmpereClockDivisor(clock));
    }
}

TEST_CASE("Ampere CPU GPU calibration stays valid between samples", "[video_core][gpu_timestamp]") {
    constexpr u64 HostStartNs = 293'036'000'000'000;
    constexpr u64 GuestStartNs = 95'000'000'000;
    constexpr u64 Offset = GuestStartNs - HostStartNs;
    constexpr u64 GpuDurationNs = 5'000'000;
    constexpr u64 PresentDelayNs = 200'000;
    constexpr u64 SincePresentNs = 3'000'000;
    for (const auto clock : Clocks) {
        const u64 divisor = Tegra::AmpereClockDivisor(clock);
        for (u64 elapsed = 0; elapsed <= 1'000'000'000; elapsed += 31'250'000) {
            CAPTURE(clock, elapsed);
            const u64 start_ns = HostStartNs + elapsed;
            const u64 end_ns = start_ns + GpuDurationNs;
            const u64 present_ns = start_ns - SincePresentNs;
            const u64 gpu_begin = Ampere(start_ns, clock, present_ns);
            const u64 gpu_end = Ampere(end_ns, clock, present_ns);
            const u64 present = end_ns + PresentDelayNs + Offset;
            REQUIRE(gpu_begin + Offset == GuestStartNs + elapsed - SincePresentNs +
                                              SincePresentNs / divisor);
            REQUIRE(gpu_end - gpu_begin == GpuDurationNs / divisor);
            REQUIRE(present - (gpu_end + Offset) ==
                    PresentDelayNs + (GpuDurationNs + SincePresentNs) -
                        (GpuDurationNs + SincePresentNs) / divisor);
        }
    }
}

TEST_CASE("Ampere boosted timestamps stay monotonic across presents and within a frame of CPU time",
          "[video_core][gpu_timestamp]") {
    constexpr u64 StartNs = 293'036'000'000'000;
    constexpr u64 FrameNs = 16'666'667;
    for (const auto clock : Clocks) {
        u64 present_ns = StartNs;
        u64 previous = 0;
        for (u64 now = StartNs; now <= StartNs + 10 * FrameNs; now += 1'000'003) {
            if (now >= present_ns + FrameNs) {
                present_ns += FrameNs;
            }
            CAPTURE(clock, now, present_ns);
            const u64 stamp = Ampere(now, clock, present_ns);
            REQUIRE(stamp >= previous);
            REQUIRE(stamp <= now);
            REQUIRE(now - stamp < FrameNs);
            previous = stamp;
        }
    }
}

TEST_CASE("Ampere GPU Clocks changes apply at the next present and stay monotonic",
          "[video_core][gpu_timestamp]") {
    constexpr u64 PresentNs = 293'036'000'000'000;
    constexpr u64 FrameNs = 16'666'667;
    for (const auto from : Clocks) {
        for (const auto to : Clocks) {
            CAPTURE(from, to);
            REQUIRE(Ampere(PresentNs + 12'000'000, from, PresentNs, to) ==
                    Ampere(PresentNs + 12'000'000, from, PresentNs, from));
            u64 previous = 0;
            for (u64 now = PresentNs; now < PresentNs + 2 * FrameNs; now += 500'001) {
                const bool switched = now >= PresentNs + FrameNs;
                const u64 stamp = switched ? Ampere(now, to, PresentNs + FrameNs, to)
                                           : Ampere(now, from, PresentNs, to);
                CAPTURE(now);
                REQUIRE(stamp >= previous);
                REQUIRE(stamp <= now);
                previous = stamp;
            }
        }
    }
}

TEST_CASE("Ampere GPU time scale multiplies the time since the present, up to the real time",
          "[video_core][gpu_timestamp]") {
    constexpr u64 PresentNs = 293'036'000'000'000;
    constexpr u64 SinceNs = 12'000'000;
    for (const auto clock : Clocks) {
        CAPTURE(clock);
        const u64 clocked = SinceNs / Tegra::AmpereClockDivisor(clock);
        REQUIRE(Tegra::AmpereTimeSincePresent(SinceNs, clock, 1.0) == clocked);
        REQUIRE(Tegra::AmpereTimeSincePresent(SinceNs, clock, 0.5) == clocked / 2);
        REQUIRE(Tegra::AmpereTimeSincePresent(SinceNs, clock, 1000.0) == SinceNs);
        const auto anchor = Tegra::AmpereAnchor::At(PresentNs, clock);
        REQUIRE(anchor.Scale() == 1.0);
        REQUIRE(Tegra::ReadGpuTimestamp(At(PresentNs + SinceNs), true, Settings::GpuClock::Normal,
                                        anchor) == PresentNs + clocked);
        const auto halved = Tegra::AmpereAnchor::At(PresentNs, clock, 0.5);
        REQUIRE(halved.Clock() == clock);
        REQUIRE(halved.Scale() == 0.5);
        REQUIRE(Tegra::ReadGpuTimestamp(At(PresentNs + SinceNs), true, Settings::GpuClock::Normal,
                                        halved) == PresentNs + clocked / 2);
    }
    REQUIRE(Tegra::AmpereTimeSincePresent(SinceNs, Settings::GpuClock::Boost, 2.0) == SinceNs / 2);
    REQUIRE(Tegra::AmpereTimeSincePresent(SinceNs, Settings::GpuClock::Normal, 2.0) == SinceNs);
}

TEST_CASE("Ampere anchors keep the GPU time scale to within a percent",
          "[video_core][gpu_timestamp]") {
    // Powers of two are exact, 1.0 is the code a default anchor has.
    REQUIRE(Tegra::AmpereScaleCode(1.0) == 0);
    for (const f64 scale : {1.0 / 256.0, 0.0625, 0.25, 0.5, 1.0, 2.0, 4.0, 16.0, 128.0}) {
        CAPTURE(scale);
        REQUIRE(Tegra::AmpereLatchedScale(scale) == scale);
    }
    for (f64 scale = 0.01; scale < 200.0; scale *= 1.037) {
        CAPTURE(scale);
        const f64 latched = Tegra::AmpereLatchedScale(scale);
        REQUIRE(latched > scale * 0.99);
        REQUIRE(latched < scale * 1.01);
        REQUIRE(Tegra::AmpereScaleCode(scale) < 1024);
        // A larger scale never latches as a smaller one.
        REQUIRE(Tegra::AmpereLatchedScale(scale * 1.037) >= latched);
    }
    // Out of range gives the nearest end; nothing positive, 1.
    REQUIRE(Tegra::AmpereLatchedScale(1e-9) == 1.0 / 256.0);
    REQUIRE(Tegra::AmpereLatchedScale(1e9) == 254.0);
    REQUIRE(Tegra::AmpereLatchedScale(0.0) == 1.0);
    REQUIRE(Tegra::AmpereLatchedScale(-2.0) == 1.0);
}

TEST_CASE("Ampere anchors keep the present time past their 52 bits",
          "[video_core][gpu_timestamp]") {
    constexpr u64 SinceNs = 12'000'000;
    for (const u64 present_ns : {u64{1}, u64{293'036'000'000'000}, (u64{1} << 52) - 1,
                                (u64{1} << 52) + 5, (u64{3} << 52) + 777, (u64{1} << 60) + 1}) {
        CAPTURE(present_ns);
        const auto anchor = Tegra::AmpereAnchor::At(present_ns, Settings::GpuClock::Boost, 2.0);
        REQUIRE(anchor.Clock() == Settings::GpuClock::Boost);
        REQUIRE(anchor.Scale() == 2.0);
        REQUIRE(anchor.PresentNs(present_ns + SinceNs) == present_ns);
        REQUIRE(anchor.PresentNs(present_ns) == present_ns);
        // A reader a little behind the present gets its own time, and so an exact timestamp.
        REQUIRE(anchor.PresentNs(present_ns - 1) == present_ns - 1);
        REQUIRE(Tegra::ReadGpuTimestamp(At(present_ns + SinceNs), true, Settings::GpuClock::Normal,
                                        anchor) == present_ns + SinceNs / 2);
        REQUIRE(Tegra::ReadGpuTimestamp(At(present_ns - 1), true, Settings::GpuClock::Normal,
                                        anchor) == present_ns - 1);
    }
    REQUIRE(Tegra::AmpereAnchor{}.PresentNs(293'036'000'000'000) == 0);
}

TEST_CASE("Ampere timestamps stay monotonic when the GPU time scale changes at a present",
          "[video_core][gpu_timestamp]") {
    constexpr u64 StartNs = 293'036'000'000'000;
    constexpr u64 FrameNs = 33'333'333;
    constexpr std::array Scales{1.0, 4.0, 0.05, 2.44, 0.3, 4.0, 1.25};
    for (const auto clock : Clocks) {
        u64 present_ns = StartNs;
        u64 previous = 0;
        size_t frame = 0;
        for (u64 now = StartNs; now <= StartNs + 6 * FrameNs; now += 1'000'003) {
            if (now >= present_ns + FrameNs) {
                present_ns += FrameNs;
                ++frame;
            }
            CAPTURE(clock, now, present_ns, frame);
            const u64 stamp = Tegra::ReadGpuTimestamp(
                At(now), true, Settings::GpuClock::Normal,
                Tegra::AmpereAnchor::At(present_ns, clock, Scales[frame]));
            REQUIRE(stamp >= previous);
            REQUIRE(stamp <= now);
            previous = stamp;
        }
    }
}

TEST_CASE("Maxwell retains its tick units and clock scaling", "[video_core][gpu_timestamp]") {
    for (const u64 ticks : {u64{0}, u64{1}, u64{255}, u64{256}, u64{511},
                           u64{512}, u64{614'400'000}, u64{180'041'318'400'000}}) {
        CAPTURE(ticks);
        const Timing timing{std::chrono::nanoseconds{293'036'000'000'000}, ticks};
        for (const auto latched : Clocks) {
            for (const u64 present_ns : {u64{0}, u64{293'035'990'000'000}}) {
                const auto anchor = Tegra::AmpereAnchor::At(present_ns, latched);
                REQUIRE(Tegra::ReadGpuTimestamp(timing, false, Settings::GpuClock::Normal,
                                                anchor) == ticks);
                REQUIRE(Tegra::ReadGpuTimestamp(timing, false, Settings::GpuClock::Boost,
                                                anchor) == ticks / 256);
                REQUIRE(Tegra::ReadGpuTimestamp(timing, false, Settings::GpuClock::Overclock,
                                                anchor) == ticks / 512);
            }
        }
    }
}
