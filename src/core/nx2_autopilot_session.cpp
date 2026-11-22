// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include "common/device_power_state.h"
#include "common/logging.h"
#include "common/polyfill_thread.h"
#include "common/power_policy.h"
#include "common/settings.h"
#include "common/thread.h"
#include "common/vblank_clock.h"
#include "core/core.h"
#include "core/nx2_autopilot_session.h"
#include "video_core/autopilot_controller.h"
#include "video_core/autopilot_feed.h"
#include "video_core/frame_budget.h"
#include "video_core/gpu.h"
#include "video_core/renderer_base.h"

namespace Core {
namespace {

using Controller = VideoCore::AutopilotController;

constexpr auto TickInterval = std::chrono::milliseconds{500};
constexpr u32 TicksPerWindow = 4;
constexpr u32 SummaryWindows = 5;
constexpr u32 RefreshProbeWindows = 15;

std::mutex last_mutex;
Nx2AutopilotSession::State last_state;

/// A setting the autopilot steers: what it was, so that it can be put back while it still holds
/// the value the autopilot gave it.
template <typename SettingType>
struct Steered {
    bool applied{};
    bool was_global{};
    u32 original{};
    u32 value{};

    void Set(SettingType& setting, u32 wanted) {
        if (!applied) {
            applied = true;
            was_global = setting.UsingGlobal();
            original = setting.GetValue();
        }
        setting.SetGlobal(false);
        setting.SetValue(wanted);
        value = setting.GetValue();
        // A config save leaves a held value out.
        Settings::HoldPresetValue(setting);
    }

    [[nodiscard]] bool Holds(const SettingType& setting) const {
        return applied && !setting.UsingGlobal() && setting.GetValue() == value;
    }

    void Restore(SettingType& setting) {
        if (Holds(setting)) {
            if (was_global) {
                setting.SetGlobal(true);
            } else {
                setting.SetValue(original);
            }
        }
        applied = false;
    }
};

double Median(std::vector<double> values) {
    if (values.empty()) {
        return 0.0;
    }
    const auto middle = values.begin() + values.size() / 2;
    std::ranges::nth_element(values, middle);
    return *middle;
}

const char* PowerName(Controller::PowerSource power) {
    switch (power) {
    case Controller::PowerSource::Ac:
        return "ac";
    case Controller::PowerSource::Battery:
        return "battery";
    case Controller::PowerSource::Unknown:
        break;
    }
    return "unknown";
}

} // Anonymous namespace

struct Nx2AutopilotSession::Impl {
    explicit Impl(System& system_)
        : renderer{system_.GPU().Renderer()},
          multiplier_limit{Settings::FrameGenMultiplier()},
          presents_before{g_nx2_present_count.load(std::memory_order_relaxed)},
          scale_base{VideoCore::FrameBudget::GpuTimeScale()} {
        const auto& device = renderer.GetDeviceInfo();
        // With EDEN_NX2_POWER_POLICY=igpu Nx2PowerSession owns the power scheme.
        power_available = device.integrated && !Common::PowerPolicy::Requested();
        VideoCore::AutopilotFeed::SetActive(true);
        thread = std::jthread{[this](std::stop_token stop) { Run(stop); }};
    }

    ~Impl() {
        thread.request_stop();
        thread.join();
        VideoCore::AutopilotFeed::SetActive(false);
        if (scale_applied && VideoCore::FrameBudget::TargetFps() == 0.0 &&
            VideoCore::FrameBudget::GpuTimeScale() == applied_scale) {
            VideoCore::FrameBudget::SetGpuTimeScale(scale_base);
        }
        target_steer.Restore(Settings::values.frame_gen_target_rate);
        multiplier_steer.Restore(Settings::values.frame_gen_multiplier);
        if (power_applied) {
            Common::PowerPolicy::Restore();
            power_applied = false;
        }
        std::scoped_lock lock{last_mutex};
        last_state.running = false;
    }

    void Run(std::stop_token stop) {
        Common::SetCurrentThreadName("Nx2Autopilot");
        window_start = std::chrono::steady_clock::now();
        std::vector<double> busy_samples;
        while (!stop.stop_requested()) {
            for (u32 tick = 0; tick < TicksPerWindow; ++tick) {
                renderer.RequestGpuBusySample();
                if (!Common::StoppableTimedWait(stop, TickInterval)) {
                    return;
                }
                if (const auto busy = renderer.GetGpuBusyMs()) {
                    busy_samples.push_back(*busy);
                }
            }
            Window(busy_samples);
            busy_samples.clear();
        }
    }

    double FallbackRefreshHz() {
        if (windows % RefreshProbeWindows == 0) {
            // The primary display's mode; the present thread's measure is preferred.
            const Common::VblankClock probe{nullptr};
            const auto period = probe.ReportedPeriod();
            fallback_hz = period.count() > 0 ? 1.0e9 / static_cast<double>(period.count()) : 0.0;
        }
        return fallback_hz;
    }

    void Window(const std::vector<double>& busy_samples) {
        const auto now = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(now - window_start).count();
        window_start = now;
        const u64 presents = g_nx2_present_count.load(std::memory_order_relaxed);
        const double real_fps =
            seconds > 0.0 ? static_cast<double>(presents - presents_before) / seconds : 0.0;
        presents_before = presents;
        const Controller::ShownSummary shown =
            Controller::Summarize(VideoCore::AutopilotFeed::TakeShownIntervals());

        double refresh_hz = VideoCore::AutopilotFeed::RefreshHz();
        const char* refresh_source = "present thread";
        if (refresh_hz <= 0.0) {
            refresh_hz = FallbackRefreshHz();
            refresh_source = refresh_hz > 0.0 ? "display" : "unknown";
        }
        double gpu_busy = -1.0;
        if (!busy_samples.empty() && real_fps > 0.0) {
            gpu_busy = std::clamp(Median(busy_samples) / (1000.0 / real_fps), 0.0, 1.0);
        }

        auto& values = Settings::values;
        // A multiplier the user set meanwhile becomes the new limit.
        if (!multiplier_steer.Holds(values.frame_gen_multiplier)) {
            multiplier_limit = Settings::FrameGenMultiplier();
        }
        // A scale set by hand (the debug server) or a target of the frame budget ends ours.
        const double current_scale = VideoCore::FrameBudget::GpuTimeScale();
        const bool budget_owned = VideoCore::FrameBudget::TargetFps() > 0.0;
        if (scale_applied && (budget_owned || current_scale != applied_scale)) {
            scale_applied = false;
        }
        if (!scale_applied) {
            scale_base = current_scale;
        }
        const auto power_status = Common::GetPowerStatus();
        Controller::PowerSource power = Controller::PowerSource::Unknown;
        if (power_status.ac_online >= 0) {
            power = power_status.ac_online == 1 ? Controller::PowerSource::Ac
                                                : Controller::PowerSource::Battery;
        } else if (!power_status.has_battery) {
            power = Controller::PowerSource::Ac;
        } else if (power_status.percentage >= 0) {
            power = power_status.charging ? Controller::PowerSource::Ac
                                          : Controller::PowerSource::Battery;
        }

        Controller::Input input;
        input.real_fps = real_fps;
        input.shown_p50_ms = shown.p50_ms;
        input.shown_p95_ms = shown.p95_ms;
        input.shown_even_percent = shown.even_percent;
        input.shown_samples = shown.samples;
        input.refresh_hz = refresh_hz;
        input.gpu_busy = gpu_busy;
        input.power = power;
        input.fg_on = values.frame_gen.GetValue();
        input.fg_target = values.frame_gen_target_rate.GetValue();
        input.fg_multiplier = Settings::FrameGenMultiplier();
        input.fg_multiplier_limit = multiplier_limit;
        input.resolution_available = !budget_owned;
        input.scale = current_scale;
        input.scale_base = scale_base;
        input.scale_limit = (std::max)(VideoCore::FrameBudget::GetStatus().max_scale, scale_base);
        input.power_available = power_available;
        input.power_applied = power_applied;

        const Controller::Decision decision = controller.Update(input);
        const auto numbers = [&] {
            return fmt::format("real {:.1f} fps, shown p50 {:.1f} p95 {:.1f} ms, {:.0f}% within "
                               "2 ms, refresh {:.2f} Hz ({})",
                               real_fps, shown.p50_ms, shown.p95_ms, shown.even_percent,
                               refresh_hz, refresh_source);
        };

        if (decision.fg_target) {
            target_steer.Set(values.frame_gen_target_rate, *decision.fg_target);
            Change(fmt::format("frame generation target {} -> {} fps ({}; {})", input.fg_target,
                               *decision.fg_target, decision.fg_reason, numbers()));
            input.fg_target = *decision.fg_target;
        }
        if (decision.fg_multiplier) {
            multiplier_steer.Set(values.frame_gen_multiplier, *decision.fg_multiplier);
            Change(fmt::format("frame generation multiplier {} -> {} ({}; {})", input.fg_multiplier,
                               *decision.fg_multiplier, decision.fg_reason, numbers()));
            input.fg_multiplier = *decision.fg_multiplier;
        }
        if (decision.resolution != Controller::ResolutionAction::Hold) {
            VideoCore::FrameBudget::SetGpuTimeScale(decision.scale);
            applied_scale = VideoCore::FrameBudget::GpuTimeScale();
            scale_applied = std::abs(applied_scale - scale_base) > 1e-9;
            Change(fmt::format(
                "resolution {} (GPU time scale {:.3f} -> {:.3f}; {}; {}, goal {:.1f} fps, GPU "
                "busy {:.0f}%)",
                decision.resolution == Controller::ResolutionAction::Lower ? "lower" : "raise",
                current_scale, decision.scale, decision.resolution_reason, numbers(),
                decision.goal_fps, gpu_busy * 100.0));
        }
        if (decision.power == Controller::PowerAction::Apply) {
            if (Common::PowerPolicy::Apply()) {
                power_applied = true;
                Change(fmt::format("power policy igpu applied ({}; {})", decision.power_reason,
                                   numbers()));
            } else {
                controller.NotePowerFailed();
                LOG_WARNING(Core, "Autopilot: the power policy could not be applied, not tried "
                                  "again this session");
            }
        } else if (decision.power == Controller::PowerAction::Restore) {
            Common::PowerPolicy::Restore();
            power_applied = false;
            Change(fmt::format("power policy restored ({}; {})", decision.power_reason,
                               numbers()));
        }
        // The FLUID presenter spaces its slots by the shown rate steered towards.
        VideoCore::AutopilotFeed::SetSlotRate(
            input.fg_on && input.fg_target != 0 && target_steer.Holds(values.frame_gen_target_rate)
                ? static_cast<double>(input.fg_target)
                : 0.0);

        ++windows;
        if (windows % SummaryWindows == 0) {
            LOG_INFO(Core,
                     "Autopilot: 10 s: {}, GPU busy {:.0f}%, power {}, frame generation {} target "
                     "{} multiplier {}, GPU time scale {:.3f} (base {:.3f}), power policy {}, {} "
                     "changes",
                     numbers(), gpu_busy * 100.0, PowerName(power), input.fg_on ? "on" : "off",
                     input.fg_target, input.fg_multiplier, VideoCore::FrameBudget::GpuTimeScale(),
                     scale_base, power_applied ? "applied" : "off", changes);
        }
        std::scoped_lock lock{last_mutex};
        last_state.running = true;
        last_state.windows = windows;
        last_state.changes = changes;
        last_state.real_fps = real_fps;
        last_state.shown_p50_ms = shown.p50_ms;
        last_state.shown_p95_ms = shown.p95_ms;
        last_state.shown_even_percent = shown.even_percent;
        last_state.shown_samples = shown.samples;
        last_state.refresh_hz = refresh_hz;
        last_state.refresh_source = refresh_source;
        last_state.gpu_busy = gpu_busy;
        last_state.power = PowerName(power);
        last_state.frame_gen_on = input.fg_on;
        last_state.frame_gen_target = input.fg_target;
        last_state.frame_gen_multiplier = input.fg_multiplier;
        last_state.scale = VideoCore::FrameBudget::GpuTimeScale();
        last_state.scale_base = scale_base;
        last_state.resolution_available = input.resolution_available;
        last_state.power_available = power_available;
        last_state.power_applied = power_applied;
        if (!last_change.empty()) {
            last_state.last_change = last_change;
        }
    }

    void Change(std::string text) {
        ++changes;
        LOG_INFO(Core, "Autopilot: {}", text);
        last_change = std::move(text);
    }

    VideoCore::RendererBase& renderer;
    Controller controller;
    u32 multiplier_limit;
    u64 presents_before;
    std::chrono::steady_clock::time_point window_start;
    u64 windows{};
    u64 changes{};
    std::string last_change;
    double fallback_hz{};
    bool power_available{};
    bool power_applied{};
    double scale_base{1.0};
    double applied_scale{};
    bool scale_applied{};
    Steered<decltype(Settings::values.frame_gen_target_rate)> target_steer;
    Steered<decltype(Settings::values.frame_gen_multiplier)> multiplier_steer;
    std::jthread thread;
};

bool Nx2AutopilotSession::Requested() {
    static const bool requested = [] {
        const char* const value = std::getenv("EDEN_NX2_AUTOPILOT");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return requested;
}

std::unique_ptr<Nx2AutopilotSession> Nx2AutopilotSession::MaybeStart(System& system) {
    if (!Requested()) {
        return nullptr;
    }
    {
        std::scoped_lock lock{last_mutex};
        last_state = {};
    }
    return std::unique_ptr<Nx2AutopilotSession>{new Nx2AutopilotSession{system}};
}

Nx2AutopilotSession::Nx2AutopilotSession(System& system) {
    impl = std::make_unique<Impl>(system);
    LOG_INFO(Core,
             "Autopilot: on. Every 2 s it steers the frame generation target (or fixed "
             "multiplier), the GPU time scale of the frame budget and, on battery, the power "
             "policy. Game state is not touched.");
}

Nx2AutopilotSession::~Nx2AutopilotSession() {
    impl.reset();
}

Nx2AutopilotSession::State Nx2AutopilotSession::LastState() {
    std::scoped_lock lock{last_mutex};
    return last_state;
}

} // namespace Core
