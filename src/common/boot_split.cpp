// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/boot_split.h"

#include <cstdlib>
#include <utility>

#include "common/boot_timing.h"

namespace Common::BootSplit {
namespace {

u64 Minus(u64 later, u64 earlier) {
    return later > earlier ? later - earlier : 0;
}

} // Anonymous namespace

Totals Difference(const Totals& later, const Totals& earlier) {
    Totals result;
    result.held_draws = Minus(later.held_draws, earlier.held_draws);
    result.held_draw_ns = Minus(later.held_draw_ns, earlier.held_draw_ns);
    result.held_dispatches = Minus(later.held_dispatches, earlier.held_dispatches);
    result.held_dispatch_ns = Minus(later.held_dispatch_ns, earlier.held_dispatch_ns);
    for (std::size_t kind = 0; kind < BuildKindCount; ++kind) {
        result.builds[kind] = Minus(later.builds[kind], earlier.builds[kind]);
        result.build_ns[kind] = Minus(later.build_ns[kind], earlier.build_ns[kind]);
    }
    result.file_reads = Minus(later.file_reads, earlier.file_reads);
    result.file_read_bytes = Minus(later.file_read_bytes, earlier.file_read_bytes);
    result.file_read_ns = Minus(later.file_read_ns, earlier.file_read_ns);
    return result;
}

void Recorder::AddHeldDraw(u64 ns) {
    held_draws.fetch_add(1, std::memory_order_relaxed);
    held_draw_ns.fetch_add(ns, std::memory_order_relaxed);
}

void Recorder::AddHeldDispatch(u64 ns) {
    held_dispatches.fetch_add(1, std::memory_order_relaxed);
    held_dispatch_ns.fetch_add(ns, std::memory_order_relaxed);
}

void Recorder::AddBuild(BuildKind kind, u64 ns) {
    const std::size_t index = static_cast<std::size_t>(kind);
    if (index >= BuildKindCount) {
        return;
    }
    builds[index].fetch_add(1, std::memory_order_relaxed);
    build_ns[index].fetch_add(ns, std::memory_order_relaxed);
}

void Recorder::AddFileRead(u64 bytes, u64 ns) {
    file_reads.fetch_add(1, std::memory_order_relaxed);
    file_read_bytes.fetch_add(bytes, std::memory_order_relaxed);
    file_read_ns.fetch_add(ns, std::memory_order_relaxed);
}

Totals Recorder::Current() const {
    Totals result;
    result.held_draws = held_draws.load(std::memory_order_relaxed);
    result.held_draw_ns = held_draw_ns.load(std::memory_order_relaxed);
    result.held_dispatches = held_dispatches.load(std::memory_order_relaxed);
    result.held_dispatch_ns = held_dispatch_ns.load(std::memory_order_relaxed);
    for (std::size_t kind = 0; kind < BuildKindCount; ++kind) {
        result.builds[kind] = builds[kind].load(std::memory_order_relaxed);
        result.build_ns[kind] = build_ns[kind].load(std::memory_order_relaxed);
    }
    result.file_reads = file_reads.load(std::memory_order_relaxed);
    result.file_read_bytes = file_read_bytes.load(std::memory_order_relaxed);
    result.file_read_ns = file_read_ns.load(std::memory_order_relaxed);
    return result;
}

void Recorder::RecordFirstFrame(double at_ms, u64 presents) {
    Sample sample{"first_frame", at_ms, presents, Current()};
    std::scoped_lock lock{mutex};
    first_frame = std::move(sample);
}

Sample Recorder::RecordMark(std::string label, double at_ms, u64 presents) {
    Sample sample{std::move(label), at_ms, presents, Current()};
    std::scoped_lock lock{mutex};
    marks.push_back(sample);
    if (marks.size() > MaxMarks) {
        marks.erase(marks.begin());
    }
    return sample;
}

std::optional<Sample> Recorder::FirstFrame() const {
    std::scoped_lock lock{mutex};
    return first_frame;
}

std::vector<Sample> Recorder::Marks() const {
    std::scoped_lock lock{mutex};
    return marks;
}

bool ParseEnabled(const char* value) noexcept {
    return value != nullptr && *value != '\0';
}

bool Enabled() noexcept {
    static const bool enabled = ParseEnabled(std::getenv("EDEN_NX2_DEBUG_SERVER"));
    return enabled;
}

Recorder& Global() {
    static Recorder recorder;
    return recorder;
}

void NoteHeldDraw(u64 ns) {
    if (Enabled()) {
        Global().AddHeldDraw(ns);
    }
}

void NoteHeldDispatch(u64 ns) {
    if (Enabled()) {
        Global().AddHeldDispatch(ns);
    }
}

void NoteBuild(BuildKind kind, u64 ns) {
    if (Enabled()) {
        Global().AddBuild(kind, ns);
    }
}

void NoteFileRead(u64 bytes, u64 ns) {
    if (Enabled()) {
        Global().AddFileRead(bytes, ns);
    }
}

void NoteFirstFrame(u64 presents) {
    if (Enabled()) {
        Global().RecordFirstFrame(BootTiming::Now(), presents);
    }
}

Sample NoteMark(std::string label, u64 presents) {
    return Global().RecordMark(std::move(label), BootTiming::Now(), presents);
}

} // namespace Common::BootSplit
