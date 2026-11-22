// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

#include "common/common_types.h"
#include "common/logging.h"
#include "common/period_worker.h"
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

// EDEN_NX2_VBLANK_RECHECK_ASYNC=1: the 2 s display-mode recheck (QueryDisplayConfig, which can take
// milliseconds) runs on a helper thread that publishes the period, instead of on the present
// thread just before its vblank wait. Read like the neighbouring switches: unset, empty, 0 and
// false are off, and off is the original code path.
[[nodiscard]] bool RecheckAsyncRequested() {
    const char* value = std::getenv("EDEN_NX2_VBLANK_RECHECK_ASYNC");
    return value != nullptr && std::strtoul(value, nullptr, 10) != 0;
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

// The refresh period of a monitor's display mode; DWM's when the path lookup finds none.
[[nodiscard]] std::chrono::nanoseconds QueryMonitorPeriod(HMONITOR monitor) {
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (monitor != nullptr && GetMonitorInfoW(monitor, &info)) {
        if (const auto period_ = QueryPathPeriod(info.szDevice); period_.count() > 0) {
            return period_;
        }
    }
    return QueryDwmPeriod();
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
        if (worker != nullptr) {
            // Joined before anything it could still read goes away.
            worker->Stop();
            const auto stats = worker->GetStats();
            if (stats.queries != 0) {
                const double total_ms =
                    std::chrono::duration<double, std::milli>(stats.total).count();
                const double longest_ms =
                    std::chrono::duration<double, std::milli>(stats.longest).count();
                LOG_INFO(Common,
                         "EDEN_NX2_VBLANK_RECHECK_ASYNC: {} display-mode queries on the helper "
                         "thread, {:.2f} ms on average and {:.2f} ms at most: that time was spent "
                         "off the present thread",
                         stats.queries, total_ms / static_cast<double>(stats.queries),
                         longest_ms);
            }
            worker.reset();
        }
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

    /// The window's monitor as Windows describes it; false when there is none.
    [[nodiscard]] bool QueryMonitorInfo(MONITORINFOEXW& info) const {
        info = {};
        info.cbSize = sizeof(info);
        return monitor != nullptr && GetMonitorInfoW(monitor, &info);
    }

    [[nodiscard]] std::string DisplayName() const {
        MONITORINFOEXW info{};
        if (!QueryMonitorInfo(info)) {
            return {};
        }
        // GDI device names are ASCII, such as DISPLAY1 after the \\.\ prefix.
        std::string name;
        for (const wchar_t* c = info.szDevice; *c != L'\0'; ++c) {
            name.push_back(*c < 0x80 ? static_cast<char>(*c) : '?');
        }
        return name;
    }

    [[nodiscard]] bool OnPrimaryDisplay() const {
        MONITORINFOEXW info{};
        return QueryMonitorInfo(info) && (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    }

    /// The pixels of the monitor's current display mode, which a process that is not DPI aware
    /// does not see scaled.
    [[nodiscard]] Pixels ReportedPixels() const {
        MONITORINFOEXW info{};
        if (!QueryMonitorInfo(info)) {
            return {};
        }
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        if (!EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode)) {
            return {};
        }
        return {static_cast<u32>(mode.dmPelsWidth), static_cast<u32>(mode.dmPelsHeight)};
    }

    [[nodiscard]] std::chrono::nanoseconds QueryPeriod() const {
        return QueryMonitorPeriod(monitor);
    }

    /// Starts the helper thread of the display-mode recheck, at the first Wait: a clock that is
    /// built only to read the period (ReportedPeriod) never starts a thread. Its first query is
    /// one interval away; until a result lands the period of Locate stays.
    void StartRecheckWorker() {
        worker = std::make_unique<PeriodWorker>(
            [this] { return QueryMonitorPeriod(target_monitor.load(std::memory_order_acquire)); },
            RecheckInterval);
        LOG_INFO(Common,
                 "EDEN_NX2_VBLANK_RECHECK_ASYNC: the display mode is looked up every {} s on a "
                 "helper thread",
                 std::chrono::duration_cast<std::chrono::seconds>(RecheckInterval).count());
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
        if (async_recheck) {
            // The helper reads the monitor after the generation it is told about: store first.
            target_monitor.store(monitor, std::memory_order_release);
            if (worker != nullptr) {
                worker->Retarget();
            }
        }
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
        // The mode may have changed, and a source given up on gets another chance. With the helper
        // thread the query is not made here: its latest result is taken, 0 before the first.
        if (const auto reported = worker != nullptr ? worker->Latest() : QueryPeriod();
            reported.count() > 0) {
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
        if (async_recheck && worker == nullptr) {
            StartRecheckWorker();
        }
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
    const bool async_recheck{RecheckAsyncRequested()};
    /// The monitor the helper thread queries; written by Locate on the present thread.
    std::atomic<HMONITOR> target_monitor{};
    // Last: destroyed first, and the thread it runs reads target_monitor.
    std::unique_ptr<PeriodWorker> worker;
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

    [[nodiscard]] std::string DisplayName() const {
        return {};
    }

    [[nodiscard]] bool OnPrimaryDisplay() const {
        return false;
    }

    [[nodiscard]] Pixels ReportedPixels() const {
        return {};
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

std::string VblankClock::DisplayName() const {
    return impl->DisplayName();
}

bool VblankClock::OnPrimaryDisplay() const {
    return impl->OnPrimaryDisplay();
}

VblankClock::Pixels VblankClock::ReportedPixels() const {
    return impl->ReportedPixels();
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
