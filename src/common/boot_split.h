// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/common_types.h"

/// What boot to gameplay spends its time on besides the boot phases of Common::BootTiming: draws
/// held for pipelines still building, the pipelines built by kind, and the guest's file reads.
/// The debug server's boot_timing method (docs/nx2/debug-mcp.md) returns the totals when the first
/// guest frame was presented and at each mark a client asks for, so two samples show what
/// happened between them.
///
/// Diagnostic only, and counted only when the process started with EDEN_NX2_DEBUG_SERVER set
/// (Enabled): without it every Note function is one test of a flag read once, and the clock reads
/// around the timed work are skipped by their callers.
namespace Common::BootSplit {

/// The pipeline builds the driver time is counted for.
enum class BuildKind : std::size_t {
    GraphicsMonolithic,              ///< A whole graphics pipeline (no libraries)
    GraphicsLibraryPreRasterization, ///< EDEN_NX2_GPL: the vertex-to-geometry shader part
    GraphicsLibraryFragment,         ///< EDEN_NX2_GPL: the fragment shader part
    GraphicsFastLink,                ///< EDEN_NX2_GPL: a link without link time optimization
    GraphicsOptimizedLink,           ///< EDEN_NX2_GPL: a link time optimized link
    Compute,
};

inline constexpr std::size_t BuildKindCount = 6;
static_assert(BuildKindCount == static_cast<std::size_t>(BuildKind::Compute) + 1,
              "BuildKindCount and BuildKindNames follow BuildKind");

inline constexpr std::array<std::string_view, BuildKindCount> BuildKindNames{
    "graphics_monolithic", "graphics_library_pre_rasterization", "graphics_library_fragment",
    "graphics_fast_link",  "graphics_optimized_link",            "compute",
};

/// Cumulative counts since the process started.
struct Totals {
    u64 held_draws{};       ///< Draws that waited for their pipeline to be built
    u64 held_draw_ns{};     ///< Time those draws waited, on the GPU worker
    u64 held_dispatches{};  ///< Compute dispatches that waited for their pipeline
    u64 held_dispatch_ns{}; ///< Time those dispatches waited
    std::array<u64, BuildKindCount> builds{};   ///< Pipelines (or parts) built, by BuildKind
    std::array<u64, BuildKindCount> build_ns{}; ///< Their build time, summed over all threads
    u64 file_reads{};      ///< Guest storage and file reads
    u64 file_read_bytes{}; ///< Bytes they asked for
    u64 file_read_ns{};    ///< Time spent in the backend reads
};

/// `later` minus `earlier`, field by field; a field that went backwards gives 0.
[[nodiscard]] Totals Difference(const Totals& later, const Totals& earlier);

/// The totals at one moment: when the first guest frame was presented, or a mark.
struct Sample {
    std::string label;
    double at_ms{}; ///< Common::BootTiming::Now() when it was taken
    u64 presents{}; ///< Guest frames presented so far
    Totals totals;
};

/// Counters and samples. Counting is lock free (relaxed atomics: the GPU workers, the shader
/// workers and the guest threads add, the debug server reads); the samples are under a mutex.
class Recorder {
public:
    /// Marks kept; the oldest is dropped past this.
    static constexpr std::size_t MaxMarks = 16;

    void AddHeldDraw(u64 ns);
    void AddHeldDispatch(u64 ns);
    void AddBuild(BuildKind kind, u64 ns);
    void AddFileRead(u64 bytes, u64 ns);

    [[nodiscard]] Totals Current() const;

    /// Takes the sample of the first presented frame; a later call replaces it.
    void RecordFirstFrame(double at_ms, u64 presents);
    /// Takes a sample named `label` and returns it.
    Sample RecordMark(std::string label, double at_ms, u64 presents);

    [[nodiscard]] std::optional<Sample> FirstFrame() const;
    /// The marks, oldest first.
    [[nodiscard]] std::vector<Sample> Marks() const;

private:
    std::atomic<u64> held_draws{};
    std::atomic<u64> held_draw_ns{};
    std::atomic<u64> held_dispatches{};
    std::atomic<u64> held_dispatch_ns{};
    std::array<std::atomic<u64>, BuildKindCount> builds{};
    std::array<std::atomic<u64>, BuildKindCount> build_ns{};
    std::atomic<u64> file_reads{};
    std::atomic<u64> file_read_bytes{};
    std::atomic<u64> file_read_ns{};

    mutable std::mutex mutex;
    std::optional<Sample> first_frame;
    std::vector<Sample> marks;
};

/// Whether `value` (the text of EDEN_NX2_DEBUG_SERVER, null when unset) turns the counting on:
/// anything not empty.
[[nodiscard]] bool ParseEnabled(const char* value) noexcept;

/// True when the process started with EDEN_NX2_DEBUG_SERVER set and not empty. Read once.
[[nodiscard]] bool Enabled() noexcept;

/// The process's recorder.
[[nodiscard]] Recorder& Global();

/// A draw held for `ns` waiting for its pipeline. Nothing unless Enabled().
void NoteHeldDraw(u64 ns);
/// A compute dispatch held for `ns` waiting for its pipeline. Nothing unless Enabled().
void NoteHeldDispatch(u64 ns);
/// A pipeline (or part) of `kind` built in `ns`. Nothing unless Enabled().
void NoteBuild(BuildKind kind, u64 ns);
/// A guest read of `bytes` that took `ns`. Nothing unless Enabled().
void NoteFileRead(u64 bytes, u64 ns);
/// The first guest frame was presented (`presents` is 1). Nothing unless Enabled().
void NoteFirstFrame(u64 presents);
/// A client mark (the debug server's boot_timing with a mark): takes a sample now.
Sample NoteMark(std::string label, u64 presents);

} // namespace Common::BootSplit
