// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <stop_token>
#include <thread>

#include <fmt/format.h>

#include "common/logging.h"
#include "common/polyfill_thread.h"
#include "common/power_governor.h"
#include "common/power_meter.h"
#include "common/power_policy.h"
#include "common/power_tuning.h"
#include "common/thread.h"
#include "core/core.h"
#include "core/nx2_power_session.h"
#include "video_core/auto_shading_rate.h"
#include "video_core/frame_budget.h"
#include "video_core/gpu.h"
#include "video_core/renderer_base.h"

namespace Core {
namespace {

std::mutex last_mutex;
Nx2PowerSession::GovernorState last_state;

/// EDEN_NX2_POWER_POLICY_FPS, else the frame budget's target (EDEN_NX2_TARGET_FPS), else the
/// target of the shading-rate governor, else 30.
double TargetFps() {
    if (const char* value = std::getenv("EDEN_NX2_POWER_POLICY_FPS")) {
        const double fps = std::strtod(value, nullptr);
        if (fps >= 1.0 && fps <= 1000.0) {
            return fps;
        }
        LOG_WARNING(Core, "EDEN_NX2_POWER_POLICY_FPS: bad target \"{}\"", value);
    }
    if (const double budget = VideoCore::FrameBudget::TargetFps(); budget > 0.0) {
        return budget;
    }
    const double automatic = VideoCore::AutoShadingRate::TargetFps();
    return automatic > 0.0 ? automatic : 30.0;
}

} // Anonymous namespace

struct Nx2PowerSession::Impl {
    explicit Impl(VideoCore::RendererBase& renderer_)
        : renderer{renderer_},
          initial_percent{Common::PowerTuning::spin_percent.load(std::memory_order_relaxed)},
          target_fps{TargetFps()} {
        Common::SpinGovernor::Config config;
        config.ceiling = initial_percent;
        governor = Common::SpinGovernor{config};
        g_nx2_present_time_services.fetch_add(1, std::memory_order_relaxed);
        thread = std::jthread{[this](std::stop_token stop) { Run(stop); }};
    }

    ~Impl() {
        thread.request_stop();
        thread.join();
        g_nx2_present_time_services.fetch_sub(1, std::memory_order_relaxed);
        // Only what the governor set is undone: a power_set made meanwhile stays.
        if (Common::PowerTuning::spin_percent.load(std::memory_order_relaxed) == applied_percent) {
            Common::PowerTuning::spin_percent.store(initial_percent, std::memory_order_relaxed);
        }
        std::scoped_lock lock{last_mutex};
        last_state.running = false;
        last_state.state = "off";
        last_state.spin_percent = Common::PowerTuning::spin_percent.load(std::memory_order_relaxed);
    }

    void Run(std::stop_token stop) {
        Common::SetCurrentThreadName("Nx2Power");
        auto& meter = Common::PowerMeter::Instance();
        if (!meter.Available()) {
            LOG_WARNING(Core, "Power policy: no host power meter, the spin scale stays at {}%",
                        initial_percent);
        }
        static_cast<void>(meter.Read());
        while (!stop.stop_requested()) {
            renderer.RequestGpuBusySample();
            if (!Common::StoppableTimedWait(stop, std::chrono::seconds{1})) {
                break;
            }
            Sample(meter.Read());
        }
    }

    void Sample(const Common::PowerMeter::Sample& power) {
        const double frame_ms = Nx2RecentFrameMs();
        const auto busy_ms = renderer.GetGpuBusyMs();
        const u32 current = Common::PowerTuning::spin_percent.load(std::memory_order_relaxed);
        const Common::SpinGovernor::Input input{
            .package_w = power.package_w,
            .cpu_w = power.cpu_w,
            .power_valid = power.valid,
            .gpu_busy = busy_ms && frame_ms > 0.0 ? (std::min)(*busy_ms / frame_ms, 1.0) : -1.0,
            .frame_ms = frame_ms,
            .target_ms = 1000.0 / target_fps,
        };
        const Common::SpinGovernor::Decision decision = governor.Update(input, current);
        std::string change;
        if (decision.action != Common::SpinGovernor::Action::Hold) {
            Common::PowerTuning::spin_percent.store(decision.spin_percent,
                                                    std::memory_order_relaxed);
            applied_percent = decision.spin_percent;
            change = fmt::format(
                "{} spin scale {}% -> {}% ({}, GPU busy {:.0f}%, frame {:.2f} ms, target {:.2f} "
                "ms, package {:.1f} W, CPU {:.1f} W, GPU {:.1f} W)",
                decision.action == Common::SpinGovernor::Action::Lower ? "lower" : "raise",
                current, decision.spin_percent, Common::SpinGovernorStateName(decision.state),
                input.gpu_busy * 100.0, frame_ms, input.target_ms, power.package_w, power.cpu_w,
                power.gpu_w);
            LOG_INFO(Core, "Power policy: {}", change);
        }
        std::scoped_lock lock{last_mutex};
        last_state.running = true;
        last_state.state = Common::SpinGovernorStateName(decision.state);
        last_state.spin_percent = decision.spin_percent;
        last_state.ceiling = initial_percent;
        last_state.target_fps = target_fps;
        last_state.frame_ms = frame_ms;
        last_state.gpu_busy = input.gpu_busy;
        last_state.package_w = power.valid ? power.package_w : 0.0;
        last_state.cpu_w = power.valid ? power.cpu_w : 0.0;
        last_state.gpu_w = power.valid ? power.gpu_w : 0.0;
        if (!change.empty()) {
            ++last_state.changes;
            last_state.last_change = std::move(change);
        }
    }

    VideoCore::RendererBase& renderer;
    const u32 initial_percent;
    u32 applied_percent{~u32{0}};
    const double target_fps;
    Common::SpinGovernor governor;
    std::jthread thread;
};

std::unique_ptr<Nx2PowerSession> Nx2PowerSession::MaybeStart(System& system) {
    if (!Common::PowerPolicy::Requested()) {
        return nullptr;
    }
    const auto& device = system.GPU().Renderer().GetDeviceInfo();
    if (!device.integrated) {
        LOG_INFO(Core, "Power policy igpu: {} is not an integrated GPU, nothing is changed",
                 device.model);
        return nullptr;
    }
    return std::unique_ptr<Nx2PowerSession>{new Nx2PowerSession{system}};
}

Nx2PowerSession::Nx2PowerSession(System& system) {
    // A host that refuses the scheme change still gets the spin governor.
    static_cast<void>(Common::PowerPolicy::Apply());
    impl = std::make_unique<Impl>(system.GPU().Renderer());
    LOG_INFO(Core, "Power policy igpu: spin governor at {:.1f} fps target, spin scale {}%",
             impl->target_fps, impl->initial_percent);
}

Nx2PowerSession::~Nx2PowerSession() {
    impl.reset();
    Common::PowerPolicy::Restore();
}

Nx2PowerSession::GovernorState Nx2PowerSession::LastState() {
    std::scoped_lock lock{last_mutex};
    return last_state;
}

} // namespace Core
