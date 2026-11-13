// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <memory>

namespace Common {

/// Waits for the vertical blanks of the display a window is on: on Windows through the window's
/// DXGI output (IDXGIOutput::WaitForVBlank), else DwmFlush, else a high resolution timer at the
/// refresh period; elsewhere through the timer only. Used by one thread.
class VblankClock {
public:
    enum class Source { Dxgi, Dwm, Timer };

    /// window: the native window (an HWND on Windows); may be null.
    explicit VblankClock(void* window);
    ~VblankClock();

    VblankClock(const VblankClock&) = delete;
    VblankClock& operator=(const VblankClock&) = delete;

    /// Blocks until the next vblank and returns when it returned. Returns are at least half a
    /// refresh apart: a wait that returns sooner (display off) sleeps the refresh out, and one
    /// that keeps doing so moves to the next source. The window's monitor and display mode are
    /// looked up again every 2 s.
    std::chrono::steady_clock::time_point Wait();

    /// Sleeps until deadline, to a fraction of a millisecond where the system allows.
    void SleepUntil(std::chrono::steady_clock::time_point deadline);

    /// The refresh period of the window's display mode, 0 when it is unknown.
    [[nodiscard]] std::chrono::nanoseconds ReportedPeriod() const;

    [[nodiscard]] Source CurrentSource() const;

    [[nodiscard]] static const char* SourceName(Source source);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Common
