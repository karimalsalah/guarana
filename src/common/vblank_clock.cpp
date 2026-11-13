// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <thread>

#include "common/common_types.h"
#include "common/vblank_clock.h"

#ifdef _WIN32
#include <cwchar>
#include <vector>

#include <windows.h>

#include <dwmapi.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#endif

namespace Common {

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::chrono::nanoseconds DefaultPeriod{16'666'667};

#ifdef _WIN32
/// Waits that return before half a refresh passed this many times in a row give up the source.
constexpr u32 EarlyReturnLimit = 8;
/// How often the window's monitor and display mode are looked up again.
constexpr auto RecheckInterval = std::chrono::seconds{2};

[[nodiscard]] std::chrono::nanoseconds PeriodOf(u64 numerator, u64 denominator) {
    if (numerator == 0 || denominator == 0) {
        return {};
    }
    return std::chrono::nanoseconds{static_cast<s64>(u64{1'000'000'000} * denominator / numerator)};
}

// The refresh rate of the active display path whose source is the monitor named gdi_name.
[[nodiscard]] std::chrono::nanoseconds QueryPathPeriod(const wchar_t* gdi_name) {
    UINT32 path_count{}, mode_count{};
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) !=
        ERROR_SUCCESS) {
        return {};
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count,
                           modes.data(), nullptr) != ERROR_SUCCESS) {
        return {};
    }
    for (UINT32 i = 0; i < path_count; ++i) {
        const auto& path = paths[i];
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
            std::wcscmp(source.viewGdiDeviceName, gdi_name) != 0) {
            continue;
        }
        return PeriodOf(path.targetInfo.refreshRate.Numerator,
                        path.targetInfo.refreshRate.Denominator);
    }
    return {};
}

// DWM's composition rate: the primary display's.
[[nodiscard]] std::chrono::nanoseconds QueryDwmPeriod() {
    DWM_TIMING_INFO info{};
    info.cbSize = sizeof(info);
    if (FAILED(DwmGetCompositionTimingInfo(nullptr, &info))) {
        return {};
    }
    return PeriodOf(info.rateRefresh.uiNumerator, info.rateRefresh.uiDenominator);
}
#endif

} // Anonymous namespace

#ifdef _WIN32
struct VblankClock::Impl {
    explicit Impl(HWND window_) : window{window_} {
        timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                       TIMER_ALL_ACCESS);
        if (timer == nullptr) {
            // Before Windows 10 1803.
            timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        }
        Locate(WindowMonitor());
    }

    ~Impl() {
        if (timer != nullptr) {
            CloseHandle(timer);
        }
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    [[nodiscard]] HMONITOR WindowMonitor() const {
        if (window != nullptr) {
            return MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
        }
        return MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    }

    [[nodiscard]] std::chrono::nanoseconds QueryPeriod() const {
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        if (monitor != nullptr && GetMonitorInfoW(monitor, &info)) {
            if (const auto period_ = QueryPathPeriod(info.szDevice); period_.count() > 0) {
                return period_;
            }
        }
        return QueryDwmPeriod();
    }

    /// Takes the monitor, its DXGI output and its refresh period.
    void Locate(HMONITOR new_monitor) {
        monitor = new_monitor;
        output.Reset();
        period = QueryPeriod();
        Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
        if (monitor != nullptr && SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
            for (UINT a = 0; !output && factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND;
                 ++a) {
                Microsoft::WRL::ComPtr<IDXGIOutput> candidate;
                for (UINT o = 0; adapter->EnumOutputs(o, &candidate) != DXGI_ERROR_NOT_FOUND;
                     ++o) {
                    DXGI_OUTPUT_DESC desc{};
                    if (SUCCEEDED(candidate->GetDesc(&desc)) && desc.Monitor == monitor) {
                        output = candidate;
                        break;
                    }
                }
            }
        }
        source = output ? Source::Dxgi : Source::Dwm;
        early_returns = 0;
    }

    void Recheck(Clock::time_point now) {
        if (now < recheck) {
            return;
        }
        recheck = now + RecheckInterval;
        if (const HMONITOR current = WindowMonitor(); current != monitor) {
            Locate(current);
            return;
        }
        // The mode may have changed, and a source given up on gets another chance.
        if (const auto reported = QueryPeriod(); reported.count() > 0) {
            period = reported;
        }
        const Source best = output ? Source::Dxgi : Source::Dwm;
        if (source != best) {
            source = best;
            early_returns = 0;
        }
    }

    void Demote() {
        source = source == Source::Dxgi ? Source::Dwm : Source::Timer;
        early_returns = 0;
    }

    [[nodiscard]] std::chrono::nanoseconds Period() const {
        return period.count() > 0 ? period : DefaultPeriod;
    }

    Clock::time_point Wait() {
        Recheck(Clock::now());
        bool waited = false;
        switch (source) {
        case Source::Dxgi:
            waited = SUCCEEDED(output->WaitForVBlank());
            break;
        case Source::Dwm:
            waited = SUCCEEDED(DwmFlush());
            break;
        case Source::Timer:
            break;
        }
        if (!waited) {
            if (source != Source::Timer) {
                Demote();
            }
            SleepUntil(last + Period());
        }
        Clock::time_point returned = Clock::now();
        if (last != Clock::time_point{} && returned - last < Period() / 2) {
            // No vblank follows the last one this soon: the display is off, or the source broke.
            if (++early_returns >= EarlyReturnLimit) {
                Demote();
            }
            SleepUntil(last + Period());
            returned = Clock::now();
        } else if (waited) {
            early_returns = 0;
        }
        last = returned;
        return returned;
    }

    void SleepUntil(Clock::time_point deadline) {
        const auto left =
            std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - Clock::now());
        if (left.count() <= 0) {
            return;
        }
        if (timer != nullptr) {
            // Relative, in 100 ns units.
            LARGE_INTEGER due{};
            due.QuadPart = -(std::max<LONGLONG>)(1, static_cast<LONGLONG>(left.count() / 100));
            if (SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0) &&
                WaitForSingleObject(timer, INFINITE) == WAIT_OBJECT_0) {
                return;
            }
        }
        std::this_thread::sleep_until(deadline);
    }

    HWND window{};
    HMONITOR monitor{};
    Microsoft::WRL::ComPtr<IDXGIOutput> output;
    HANDLE timer{};
    std::chrono::nanoseconds period{};
    Source source{Source::Timer};
    Clock::time_point last{};
    Clock::time_point recheck{};
    u32 early_returns{};
};

VblankClock::VblankClock(void* window) : impl{std::make_unique<Impl>(static_cast<HWND>(window))} {}
#else
struct VblankClock::Impl {
    Clock::time_point Wait() {
        if (last != Clock::time_point{}) {
            std::this_thread::sleep_until(last + DefaultPeriod);
        }
        last = Clock::now();
        return last;
    }

    void SleepUntil(Clock::time_point deadline) {
        std::this_thread::sleep_until(deadline);
    }

    Clock::time_point last{};
    std::chrono::nanoseconds period{};
    Source source{Source::Timer};
};

VblankClock::VblankClock(void*) : impl{std::make_unique<Impl>()} {}
#endif

VblankClock::~VblankClock() = default;

std::chrono::steady_clock::time_point VblankClock::Wait() {
    return impl->Wait();
}

void VblankClock::SleepUntil(std::chrono::steady_clock::time_point deadline) {
    impl->SleepUntil(deadline);
}

std::chrono::nanoseconds VblankClock::ReportedPeriod() const {
    return impl->period;
}

VblankClock::Source VblankClock::CurrentSource() const {
    return impl->source;
}

const char* VblankClock::SourceName(Source source) {
    switch (source) {
    case Source::Dxgi:
        return "DXGI WaitForVBlank";
    case Source::Dwm:
        return "DwmFlush";
    case Source::Timer:
        return "a timer";
    }
    return "?";
}

} // namespace Common
