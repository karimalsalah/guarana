// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <utility>

#include "video_core/nx2_shown_frames.h"

namespace VideoCore::Nx2ShownFrames {

namespace detail {
std::atomic<bool> armed{};
std::atomic<bool> pictures{};
} // namespace detail

namespace {

struct State {
    std::mutex mutex;
    std::condition_variable changed;
    std::optional<Request> queued;
    /// The capture the presenting thread took and has not finished.
    std::optional<Request> running;
    bool running_cancelled{};
    std::optional<u64> finished_id;
    Result finished;
};

State state;

/// The contact sheet's strip colours, by Kind.
constexpr std::array<std::array<u8, 3>, 3> STRIP_RGB{{
    {64, 200, 64},  // real
    {240, 150, 32}, // predicted
    {64, 140, 240}, // interpolated
}};
constexpr std::array<u8, 3> NO_PICTURE_RGB{96, 96, 96};

/// Caller holds state.mutex.
void UpdateArmed() {
    const bool queued_pictures = state.queued && state.queued->height != 0;
    const bool running_pictures = state.running && state.running->height != 0;
    detail::armed.store(state.queued.has_value() || state.running.has_value(),
                        std::memory_order_relaxed);
    detail::pictures.store(queued_pictures || running_pictures, std::memory_order_relaxed);
}

/// Nanoseconds to microseconds, rounded to nearest (halves away from zero).
s64 Micros(s64 ns) {
    return ns >= 0 ? (ns + 500) / 1000 : -((-ns + 500) / 1000);
}

} // Anonymous namespace

std::string_view KindName(Kind kind) {
    switch (kind) {
    case Kind::Real:
        return "real";
    case Kind::Predicted:
        return "predicted";
    case Kind::Interpolated:
        return "interpolated";
    }
    return "real";
}

bool RequestCapture(const Request& request) {
    std::scoped_lock lock{state.mutex};
    // A cancelled capture still running is finished and dropped later; a new one may queue.
    if (state.queued || (state.running && !state.running_cancelled)) {
        return false;
    }
    state.queued = request;
    UpdateArmed();
    return true;
}

void CancelCapture(u64 id) {
    std::scoped_lock lock{state.mutex};
    if (state.queued && state.queued->id == id) {
        state.queued.reset();
    }
    if (state.running && state.running->id == id) {
        state.running_cancelled = true;
    }
    if (state.finished_id == id) {
        state.finished_id.reset();
        state.finished = {};
    }
    UpdateArmed();
}

std::optional<Result> WaitCapture(u64 id, std::chrono::milliseconds timeout) {
    std::unique_lock lock{state.mutex};
    state.changed.wait_for(lock, timeout, [id] { return state.finished_id == id; });
    if (state.finished_id != id) {
        return std::nullopt;
    }
    state.finished_id.reset();
    return std::exchange(state.finished, Result{});
}

std::optional<Request> TakeCapture() {
    std::scoped_lock lock{state.mutex};
    if (state.running || !state.queued) {
        return std::nullopt;
    }
    state.running = std::exchange(state.queued, std::nullopt);
    state.running_cancelled = false;
    UpdateArmed();
    return state.running;
}

void FinishCapture(u64 id, Result result) {
    {
        std::scoped_lock lock{state.mutex};
        if (!state.running || state.running->id != id) {
            return;
        }
        if (!state.running_cancelled) {
            state.finished_id = id;
            state.finished = std::move(result);
        }
        state.running.reset();
        state.running_cancelled = false;
        UpdateArmed();
    }
    state.changed.notify_all();
}

SheetLayout LayoutSheet(u32 count, u32 tile_width, u32 tile_height) {
    if (count == 0 || tile_width == 0 || tile_height == 0) {
        return {};
    }
    // Rows of four, or a near-square grid when there are more than 16 frames.
    const u32 square = static_cast<u32>(std::ceil(std::sqrt(static_cast<double>(count))));
    const u32 columns = (std::min)(count, (std::max)(4U, square));
    const u32 rows = (count + columns - 1) / columns;
    return SheetLayout{
        .columns = columns,
        .rows = rows,
        .width = SHEET_GAP + columns * (tile_width + SHEET_GAP),
        .height = SHEET_GAP + rows * (SHEET_STRIP + tile_height + SHEET_GAP),
    };
}

SheetTile TileAt(const SheetLayout& layout, u32 index, u32 tile_width, u32 tile_height) {
    const u32 column = layout.columns != 0 ? index % layout.columns : 0;
    const u32 row = layout.columns != 0 ? index / layout.columns : 0;
    return SheetTile{
        .x = SHEET_GAP + column * (tile_width + SHEET_GAP),
        .y = SHEET_GAP + row * (SHEET_STRIP + tile_height + SHEET_GAP) + SHEET_STRIP,
    };
}

std::vector<u8> ComposeSheet(const Result& result, const SheetLayout& layout) {
    const u32 width = result.width;
    const u32 height = result.height;
    const size_t picture_bytes = size_t{width} * height * 4;
    if (width == 0 || height == 0 || layout.width == 0 || result.frames.empty()) {
        return {};
    }
    const auto fill = [&layout](std::vector<u8>& target, u32 x, u32 y, u32 w, u32 h,
                                std::array<u8, 3> rgb) {
        for (u32 row = y; row < y + h && row < layout.height; ++row) {
            u8* out = target.data() + (size_t{row} * layout.width + x) * 3;
            for (u32 column = x; column < x + w && column < layout.width; ++column, out += 3) {
                out[0] = rgb[0];
                out[1] = rgb[1];
                out[2] = rgb[2];
            }
        }
    };
    // A dark background, so black frames (a failed draw) still stand out from the gaps.
    std::vector<u8> sheet(size_t{layout.width} * layout.height * 3, 32);
    const u32 count = (std::min)(static_cast<u32>(result.frames.size()), layout.columns * layout.rows);
    for (u32 index = 0; index < count; ++index) {
        const Shown& frame = result.frames[index];
        const SheetTile tile = TileAt(layout, index, width, height);
        fill(sheet, tile.x, tile.y - SHEET_STRIP, width, SHEET_STRIP,
             STRIP_RGB[static_cast<size_t>(frame.kind)]);
        const size_t offset = frame.picture >= 0 ? static_cast<size_t>(frame.picture) * picture_bytes
                                                 : result.pixels.size();
        if (frame.picture < 0 || offset + picture_bytes > result.pixels.size()) {
            fill(sheet, tile.x, tile.y, width, height, NO_PICTURE_RGB);
            continue;
        }
        for (u32 row = 0; row < height; ++row) {
            const u8* in = result.pixels.data() + offset + size_t{row} * width * 4;
            u8* out = sheet.data() + (size_t{tile.y + row} * layout.width + tile.x) * 3;
            for (u32 column = 0; column < width; ++column, in += 4, out += 3) {
                out[0] = in[2];
                out[1] = in[1];
                out[2] = in[0];
            }
        }
    }
    return sheet;
}

std::vector<Times> RelativeTimes(std::span<const Shown> frames) {
    std::vector<Times> times;
    if (frames.empty()) {
        return times;
    }
    times.reserve(frames.size());
    const s64 origin = frames.front().presented_ns;
    for (size_t index = 0; index < frames.size(); ++index) {
        const Shown& frame = frames[index];
        Times& entry = times.emplace_back();
        entry.presented_us = Micros(frame.presented_ns - origin);
        if (frame.scheduled_ns != 0) {
            entry.scheduled_us = Micros(frame.scheduled_ns - origin);
            // A copy-ahead frame's time is its copy's submission, about a refresh before its
            // present: no lateness or age is claimed for it.
            if (!frame.deferred) {
                entry.late_us = Micros(frame.presented_ns - frame.scheduled_ns);
            }
        }
        if (index != 0) {
            entry.interval_us = Micros(frame.presented_ns - frames[index - 1].presented_ns);
        }
        if (frame.recorded_ns != 0 && !frame.deferred) {
            entry.age_us = Micros(frame.presented_ns - frame.recorded_ns);
        }
    }
    return times;
}

} // namespace VideoCore::Nx2ShownFrames
