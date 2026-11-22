// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace Guarana::Overlay {

/// Present intervals are drawn against these budgets. 16.7 ms is one frame at 60 Hz.
inline constexpr double Budget120Ms = 1000.0 / 120.0;
inline constexpr double Budget90Ms = 1000.0 / 90.0;
inline constexpr double Budget60Ms = 1000.0 / 60.0;
inline constexpr double Budget45Ms = 1000.0 / 45.0;
inline constexpr double Budget30Ms = 1000.0 / 30.0;
/// The trace never stretches past this, so one stalled present cannot flatten the rest.
inline constexpr double ScaleCapMs = 80.0;
inline constexpr float SpikeRatio = 1.2f;
inline constexpr int HistogramBins = 8;

inline constexpr double PacingCv = 0.22;
inline constexpr double PacingHitchFraction = 0.04;
inline constexpr double BoundPercent = 70.0;
inline constexpr double BoundGap = 12.0;
inline constexpr double HitchAgeS = 0.15;

/// Height of the trace for the busiest present in view. At least 1.35× the 60 Hz budget, at most
/// ScaleCapMs, with a little room above the busiest bar.
inline double ScaleFromPeak(float peak_ms) {
    const double scaled = std::max(static_cast<double>(peak_ms) * 1.08, Budget60Ms * 1.35);
    return std::min(ScaleCapMs, scaled);
}

inline double ScaleMs(std::span<const float> frame_ms) {
    float peak = 0;
    for (const float ms : frame_ms) {
        if (ms > peak) {
            peak = ms;
        }
    }
    return ScaleFromPeak(peak);
}

/// One present on the trace. x is 0 at the start of the window and 1 at now. Height is 0..1 of the
/// scale. Width is the present interval, clipped to the window.
struct IntervalMark {
    double x0 = 0;
    double x1 = 0;
    double height = 0;
    bool visible = false;
};

inline IntervalMark MarkInterval(double age_end_s, double ms, double window_s, double scale_ms) {
    IntervalMark mark;
    if (!(age_end_s > -0.05) || !(ms > 0.0) || !(window_s > 0.0) || !(scale_ms > 0.0)) {
        return mark;
    }
    const double age_end = std::max(0.0, age_end_s);
    if (age_end >= window_s) {
        return mark;
    }
    const double age_start = std::min(window_s, age_end + ms / 1000.0);
    mark.x0 = 1.0 - age_start / window_s;
    mark.x1 = 1.0 - age_end / window_s;
    mark.height = std::min(1.0, ms / scale_ms);
    mark.visible = mark.x1 > mark.x0;
    return mark;
}

/// Slowest `percent` of presents, as a frame rate. One present is enough; the sample count is what
/// was averaged.
struct LowFrame {
    double fps = 0;
    double ms = 0;
    std::size_t samples = 0;
};

inline std::optional<LowFrame> LowPercent(std::span<const float> frame_ms, double percent = 1.0) {
    if (!(percent > 0.0) || percent > 100.0) {
        return std::nullopt;
    }
    std::vector<float> slowest;
    slowest.reserve(frame_ms.size());
    for (const float ms : frame_ms) {
        if (ms > 0.f && std::isfinite(ms)) {
            slowest.push_back(ms);
        }
    }
    if (slowest.empty()) {
        return std::nullopt;
    }
    std::ranges::sort(slowest, std::ranges::greater{});
    const double fraction = static_cast<double>(slowest.size()) * percent / 100.0;
    const auto take = std::min(
        slowest.size(), std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(fraction))));
    double sum = 0;
    for (std::size_t i = 0; i < take; ++i) {
        sum += slowest[i];
    }
    const double mean = sum / static_cast<double>(take);
    if (!(mean > 0.0)) {
        return std::nullopt;
    }
    return LowFrame{.fps = 1000.0 / mean, .ms = mean, .samples = take};
}

struct Pacing {
    double cv = 0;
    double hitch_fraction = 0;
    float median_ms = 0;
    int hitches = 0;
};

inline std::optional<Pacing> MeasurePacing(std::span<const float> frame_ms) {
    std::vector<float> samples;
    samples.reserve(frame_ms.size());
    for (const float ms : frame_ms) {
        if (ms > 0.f && std::isfinite(ms)) {
            samples.push_back(ms);
        }
    }
    if (samples.size() < 8) {
        return std::nullopt;
    }
    std::vector<float> sorted = samples;
    std::ranges::sort(sorted);
    const std::size_t mid = sorted.size() / 2;
    const float median =
        sorted.size() % 2 != 0 ? sorted[mid] : (sorted[mid - 1] + sorted[mid]) / 2.f;
    if (!(median > 0.f)) {
        return std::nullopt;
    }
    double sum = 0;
    for (const float ms : samples) {
        sum += ms;
    }
    const double mean = sum / static_cast<double>(samples.size());
    double var = 0;
    int hitches = 0;
    const float spike = median * SpikeRatio;
    for (const float ms : samples) {
        const double delta = static_cast<double>(ms) - mean;
        var += delta * delta;
        if (ms > spike) {
            ++hitches;
        }
    }
    var /= static_cast<double>(samples.size());
    return Pacing{
        .cv = std::sqrt(var) / median,
        .hitch_fraction = static_cast<double>(hitches) / static_cast<double>(samples.size()),
        .median_ms = median,
        .hitches = hitches,
    };
}

inline bool NewestIsHitch(float newest_ms, float median_ms, double newest_age_s) {
    return newest_age_s >= 0 && newest_age_s < HitchAgeS && median_ms > 0.f &&
           newest_ms > median_ms * SpikeRatio;
}

enum class Limiter { Unknown, Steady, Cpu, Gpu, Pacing, Shaders };

inline Limiter Classify(int shaders, const std::optional<Pacing>& pacing, std::optional<double> cpu,
                        std::optional<double> gpu) {
    if (shaders > 0) {
        return Limiter::Shaders;
    }
    if (pacing && (pacing->cv >= PacingCv || pacing->hitch_fraction >= PacingHitchFraction)) {
        return Limiter::Pacing;
    }
    const bool cpu_hot = cpu && *cpu >= BoundPercent;
    const bool gpu_hot = gpu && *gpu >= BoundPercent;
    if (cpu && gpu) {
        if (*cpu >= *gpu + BoundGap && cpu_hot) {
            return Limiter::Cpu;
        }
        if (*gpu >= *cpu + BoundGap && gpu_hot) {
            return Limiter::Gpu;
        }
    } else if (cpu_hot) {
        return Limiter::Cpu;
    } else if (gpu_hot) {
        return Limiter::Gpu;
    }
    if (!pacing && !cpu && !gpu) {
        return Limiter::Unknown;
    }
    return Limiter::Steady;
}

/// Bins, slowest last: 120, 90, 60, 45, 30 Hz, then 50 ms, 100 ms, and anything slower.
inline int BinIndex(double ms) {
    constexpr std::array<double, HistogramBins - 1> upper{
        Budget120Ms, Budget90Ms, Budget60Ms, Budget45Ms, Budget30Ms, 50.0, 100.0};
    for (int i = 0; i < HistogramBins - 1; ++i) {
        if (ms <= upper[static_cast<std::size_t>(i)] + 0.05) {
            return i;
        }
    }
    return HistogramBins - 1;
}

struct Histogram {
    std::array<int, HistogramBins> counts{};
    int total = 0;

    [[nodiscard]] int Max() const {
        return *std::ranges::max_element(counts);
    }
};

inline Histogram BuildHistogram(std::span<const float> frame_ms) {
    Histogram histogram;
    for (const float ms : frame_ms) {
        if (!(ms > 0.f) || !std::isfinite(ms)) {
            continue;
        }
        ++histogram.counts[static_cast<std::size_t>(BinIndex(ms))];
        ++histogram.total;
    }
    return histogram;
}

/// `cpu` and `wall` are the same unit. Unknown when either side is missing.
struct RoleLoad {
    bool known = false;
    double percent = 0;
};

inline RoleLoad PercentOfWall(std::optional<double> cpu, double wall) {
    if (!cpu || !(wall > 0.0) || !std::isfinite(*cpu) || !std::isfinite(wall)) {
        return {};
    }
    return RoleLoad{true, std::min(100.0, *cpu / wall * 100.0)};
}

/// Below these the room left turns amber: the host starts paging, or video memory spills into RAM.
/// Both show as stutter. Fritchii's thresholds (Frizti/Fritchii 58f55cb7d9, 8627ff4233).
inline constexpr std::uint64_t RamLowBytes = std::uint64_t{2} << 30;
inline constexpr std::uint64_t VramLowBytes = std::uint64_t{1} << 30;

/// One memory readout in GiB: what this process uses, and the room left when it is known.
struct MemoryFigure {
    double used_gib = 0;
    std::optional<double> free_gib;
    bool low = false;
};

inline double Gib(std::uint64_t bytes) {
    return static_cast<double>(bytes) / static_cast<double>(std::uint64_t{1} << 30);
}

/// This process's resident memory and the RAM the host can still hand out, in bytes; 0 is not
/// reported. Nothing without the first.
inline std::optional<MemoryFigure> RamFigure(std::uint64_t working_set, std::uint64_t available) {
    if (working_set == 0) {
        return std::nullopt;
    }
    MemoryFigure figure;
    figure.used_gib = Gib(working_set);
    if (available != 0) {
        figure.free_gib = Gib(available);
        figure.low = available < RamLowBytes;
    }
    return figure;
}

/// Device memory in use and the driver's budget for it (0: none reported). An integrated GPU's
/// device memory is system RAM, so its room is also bounded by the RAM the host can hand out.
inline MemoryFigure VramFigure(std::uint64_t used, std::uint64_t budget, bool shared,
                               std::uint64_t ram_available) {
    MemoryFigure figure;
    figure.used_gib = Gib(used);
    std::optional<std::uint64_t> room;
    if (budget != 0) {
        room = budget > used ? budget - used : 0;
    }
    if (shared && ram_available != 0) {
        room = room ? std::min(*room, ram_available) : ram_available;
    }
    if (room) {
        figure.free_gib = Gib(*room);
        figure.low = *room < VramLowBytes;
    }
    return figure;
}

/// Critically damped. The first step snaps, so a readout never eases up from zero.
struct Spring {
    double y = 0;
    double v = 0;
    bool armed = false;

    void Disarm() {
        y = 0;
        v = 0;
        armed = false;
    }

    void Snap(double target) {
        if (!std::isfinite(target)) {
            return;
        }
        y = target;
        v = 0;
        armed = true;
    }

    void Step(double target, double dt) {
        if (!std::isfinite(target)) {
            return;
        }
        if (!armed) {
            Snap(target);
            return;
        }
        if (!(dt > 0.0)) {
            return;
        }
        dt = std::min(dt, 0.05);
        constexpr double omega = 14.0;
        const double x = y - target;
        const double decay = std::exp(-omega * dt);
        const double b = v + omega * x;
        const double next_x = (x + b * dt) * decay;
        y = target + next_x;
        v = (b - omega * (x + b * dt)) * decay;
        if (std::abs(y - target) < 0.015 && std::abs(v) < 0.015) {
            y = target;
            v = 0;
        }
    }
};

} // namespace Guarana::Overlay
