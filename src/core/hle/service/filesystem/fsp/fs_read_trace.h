// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <cstdlib>

#include "common/boot_split.h"
#include "common/common_types.h"

namespace Service::FileSystem {

enum class ReadTraceSource { Storage, File };

void RecordRead(ReadTraceSource source, u64 bytes, std::chrono::steady_clock::duration elapsed);

/// EDEN_NX2_FS_READ_TRACE=1 times the backend read of a guest storage or file read; about once a
/// second while reads happen, the log gets their count, bytes and latency percentiles. With
/// EDEN_NX2_DEBUG_SERVER set the reads are timed as well, for the boot_timing method's read totals
/// (Common::BootSplit), without the log lines.
class ReadTraceScope {
public:
    ReadTraceScope(ReadTraceSource source_, s64 bytes_) : source{source_}, bytes{bytes_} {
        if (Timed) {
            start = std::chrono::steady_clock::now();
        }
    }

    ~ReadTraceScope() {
        if (Timed) {
            const auto elapsed = std::chrono::steady_clock::now() - start;
            if (Enabled) {
                RecordRead(source, static_cast<u64>(bytes), elapsed);
            }
            // The guest's size is not checked on the file path: a negative one counts as 0.
            Common::BootSplit::NoteFileRead(
                bytes > 0 ? static_cast<u64>(bytes) : u64{0},
                static_cast<u64>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()));
        }
    }

    ReadTraceScope(const ReadTraceScope&) = delete;
    ReadTraceScope& operator=(const ReadTraceScope&) = delete;

private:
    static inline const bool Enabled = std::getenv("EDEN_NX2_FS_READ_TRACE") != nullptr;
    static inline const bool Timed = Enabled || Common::BootSplit::Enabled();

    ReadTraceSource source;
    s64 bytes;
    std::chrono::steady_clock::time_point start{};
};

} // namespace Service::FileSystem
