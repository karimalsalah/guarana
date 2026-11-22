// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/common_types.h"

/// The debug server's shown_frames method (EDEN_NX2_DEBUG_SERVER, docs/nx2/debug-mcp.md): the
/// next frames the presenter shows, generated ones included, with their timing and a small copy
/// of each picture. The server posts a request; the thread that presents (the present thread, or
/// the renderer without one) takes it at the next present and answers once it has shown `count`
/// frames. While nothing is asked the presenter's cost is two relaxed atomic loads per present
/// (one at the screen copy, one after the present).
namespace VideoCore::Nx2ShownFrames {

/// What a shown frame is: drawn by the game, or made by frame generation.
enum class Kind : u8 {
    Real,
    Predicted,    ///< extrapolated from the last real frame (EDEN_NX2_EXTRAPOLATE)
    Interpolated, ///< between two real frames (Lossless Scaling, EDEN_NX2_FLUID_INTERP)
};

[[nodiscard]] std::string_view KindName(Kind kind);

struct Request {
    u64 id;
    u32 count;  ///< frames to capture, 1 to 32
    u32 height; ///< of each picture, 144 to 540; 0: timing only
};

/// One frame as the presenter showed it. Times are steady-clock nanoseconds since its epoch.
struct Shown {
    Kind kind{};
    /// The vblank slot it was shown for (EDEN_NX2_FLUID); 0: shown in arrival order.
    u64 slot{};
    /// The real frame it is or derives from (EDEN_NX2_FLUID numbering); 0: not numbered.
    u64 batch{};
    s64 scheduled_ns{}; ///< when the slot's vblank was due; 0 without a slot
    s64 presented_ns{}; ///< when the present call returned on the CPU (not the scanout)
    s64 recorded_ns{};  ///< when the real frame it derives from was recorded; 0: unknown
    /// "fluid" (the vblank clock), "present_thread" (arrival order) or "renderer_thread".
    std::string_view presenter;
    /// EDEN_NX2_FLUID_COPY_AHEAD: copied now, presented at the next slot's wake.
    bool deferred{};
    /// Index of its picture in Result::pixels; -1: none.
    s32 picture{-1};
};

struct Result {
    std::vector<Shown> frames;
    u32 width{};  ///< of each picture
    u32 height{}; ///< of each picture; 0: no pictures
    /// BGRA8, picture i at i * width * height * 4, rows top to bottom.
    std::vector<u8> pixels;
    bool hdr{}; ///< some frames were HDR: their pictures are the encoded values cut to 8 bits
    /// Why some or all frames have no picture; empty when all have one.
    std::string picture_note;
};

namespace detail {
extern std::atomic<bool> armed;
extern std::atomic<bool> pictures;
} // namespace detail

/// Presenting thread: whether a capture waits or runs. One relaxed atomic load.
[[nodiscard]] inline bool Armed() noexcept {
    return detail::armed.load(std::memory_order_relaxed);
}

/// Presenting thread: whether the capture wants pictures, so a copy that would run on the
/// dedicated FLUID queue (which cannot blit) runs on the graphics queue instead.
[[nodiscard]] inline bool PicturesArmed() noexcept {
    return detail::pictures.load(std::memory_order_relaxed);
}

/// Server thread: queues a capture. False when another one waits or runs.
[[nodiscard]] bool RequestCapture(const Request& request);
/// Server thread: withdraws a capture that timed out; its answer, if it still comes, is dropped.
void CancelCapture(u64 id);
/// Server thread: the capture's answer, or empty when it is not there within the timeout.
[[nodiscard]] std::optional<Result> WaitCapture(u64 id, std::chrono::milliseconds timeout);
/// Presenting thread: the queued capture, which then runs until FinishCapture.
[[nodiscard]] std::optional<Request> TakeCapture();
/// Presenting thread: the answer of the capture taken.
void FinishCapture(u64 id, Result result);

/// Contact sheet: every picture in order, left to right and top to bottom, each under a strip in
/// its kind's colour (real green, predicted orange, interpolated blue), grey where a frame has
/// no picture.
inline constexpr u32 SHEET_GAP = 4;
inline constexpr u32 SHEET_STRIP = 6;

struct SheetLayout {
    u32 columns{};
    u32 rows{};
    u32 width{};
    u32 height{};
};

struct SheetTile {
    u32 x{}; ///< left of the picture
    u32 y{}; ///< top of the picture, below its strip
};

[[nodiscard]] SheetLayout LayoutSheet(u32 count, u32 tile_width, u32 tile_height);
[[nodiscard]] SheetTile TileAt(const SheetLayout& layout, u32 index, u32 tile_width,
                               u32 tile_height);
/// RGB8 rows of the contact sheet of `result` (empty when it has no pictures).
[[nodiscard]] std::vector<u8> ComposeSheet(const Result& result, const SheetLayout& layout);

/// One frame's times in microseconds, rounded to nearest, relative to the first frame's present.
struct Times {
    std::optional<s64> scheduled_us;
    s64 presented_us{};
    std::optional<s64> late_us;     ///< presented less scheduled
    std::optional<s64> interval_us; ///< since the previous frame's present
    std::optional<s64> age_us;      ///< presented less when its real frame was recorded
};

[[nodiscard]] std::vector<Times> RelativeTimes(std::span<const Shown> frames);

} // namespace VideoCore::Nx2ShownFrames
