// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>
#include <vector>

#include "common/common_types.h"

namespace VideoCore {

/// EDEN_NX2_AUTOPILOT=1: the decision logic of the autopilot, free of settings, Vulkan and
/// logging. Every 2 s window the session (core/nx2_autopilot_session.cpp) hands it what it
/// measured; it answers with at most one bounded step for each knob: the frame generation target
/// (or multiplier), the resolution (the GPU time scale of the frame budget) and the power policy.
/// It never touches the guest.
class AutopilotController {
public:
    enum class PowerSource : u8 { Unknown, Ac, Battery };
    enum class PowerAction : u8 { None, Apply, Restore };
    enum class ResolutionAction : u8 { Hold, Lower, Raise };

    struct Config {
        /// Frame generation: shown rates considered are the integer divisors of the refresh
        /// (rounded to a whole Hz) between these.
        double min_target_fps{20.0};
        double max_target_fps{75.0};
        /// A target needs real fps of at least target / this (3 generated frames at most are
        /// asked of a real one in quality terms: 60 needs 20, 40 needs 13.3).
        double max_ratio{3.0};
        /// A higher target needs this much more real fps than its minimum, a kept one may fall
        /// this far below it.
        double up_margin{1.10};
        double down_margin{0.90};
        u32 up_dwell{5};   ///< Windows between a change and a step up
        u32 down_dwell{2}; ///< Windows between a change and a step down
        /// Shown frames within 2 ms of the median below this share for judder_windows windows
        /// step the target down once; the target left is not tried again for judder_block windows.
        double judder_even_percent{75.0};
        u32 judder_windows{3};
        u32 judder_min_samples{20};
        u32 judder_block{30};
        u32 min_multiplier{2};
        u32 max_multiplier{4};
        /// A fixed multiplier is kept while the wanted ratio (target / real fps) is within this.
        double multiplier_band{0.6};
        /// Windows with fewer real fps than this decide nothing (a loading screen, a stall).
        double min_real_fps{5.0};

        /// Resolution: the real fps wanted without frame generation, and GPU busy shares.
        double goal_fps{30.0};
        double busy_high{0.85}; ///< GPU bound: busy over frame time at or above this
        double busy_low{0.70};  ///< Headroom: busy over frame time below this
        double goal_slack{0.95};
        double scale_step{1.25};       ///< Per step, up or down
        double max_scale_factor{2.0};  ///< Never above this times the scale the session began with
        u32 resolution_dwell{3};       ///< Windows between two steps (the game's own scaling follows)
        u32 headroom_windows{5};       ///< 10 s of headroom before resolution comes back
        double effect_gain{1.04};      ///< A step has to raise real fps by this much
        u32 no_effect_limit{2};        ///< Steps without it before the run is undone
        u32 no_effect_hold{30};        ///< Windows without lowering after an undone run

        /// Power: windows on the same source before the policy follows.
        u32 power_windows{2};
    };

    struct Input {
        double real_fps{};       ///< Guest presents per second over the window, 0 when none
        double shown_p50_ms{};
        double shown_p95_ms{};
        double shown_even_percent{100.0};
        u32 shown_samples{};
        double refresh_hz{};     ///< 0 when unknown: the frame generation target is held
        double gpu_busy{-1.0};   ///< Host GPU busy time over the real frame time, <0 unknown
        PowerSource power{PowerSource::Unknown};

        bool fg_on{};            ///< frame_gen is on (owned by the user or a preset, never changed)
        u32 fg_target{};         ///< frame_gen_target_rate, 0 for a fixed multiplier
        u32 fg_multiplier{};
        u32 fg_multiplier_limit{}; ///< The multiplier is never raised above this (the user's)

        bool resolution_available{}; ///< The frame budget has no target of its own
        double scale{1.0};           ///< The GPU time scale now
        double scale_base{1.0};      ///< The scale the session began with
        double scale_limit{4.0};     ///< The frame budget's highest useful scale

        bool power_available{};  ///< Windows, an integrated GPU, EDEN_NX2_POWER_POLICY unset
        bool power_applied{};
    };

    struct Decision {
        std::optional<u32> fg_target;
        std::optional<u32> fg_multiplier;
        const char* fg_reason{""};
        ResolutionAction resolution{ResolutionAction::Hold};
        double scale{};              ///< The new GPU time scale for Lower and Raise
        const char* resolution_reason{""};
        double goal_fps{};           ///< The real fps the resolution step aims at
        PowerAction power{PowerAction::None};
        const char* power_reason{""};
    };

    struct ShownSummary {
        u32 samples{};
        double p5_ms{};
        double p50_ms{};
        double p95_ms{};
        double even_percent{100.0};
    };

    AutopilotController();
    explicit AutopilotController(const Config& config_);

    /// One window. Mutates the hysteresis state.
    [[nodiscard]] Decision Update(const Input& input);

    /// The session could not apply the power policy: it is not tried again.
    void NotePowerFailed() {
        power_gave_up = true;
    }

    void Reset();

    /// The shown rates a refresh allows, highest first: its integer divisors (the refresh rounded
    /// to a whole Hz, so 119.88 counts as 120) within the configured range.
    [[nodiscard]] std::vector<u32> Candidates(double refresh_hz) const;

    /// The highest candidate that real_fps carries with the given margin on its minimum, 0 when
    /// none or the refresh is unknown.
    [[nodiscard]] u32 BestTarget(double refresh_hz, double real_fps, double margin) const;

    /// Median, 5th/95th percentile and the share within 2 ms of the median of shown-frame
    /// intervals in ms.
    [[nodiscard]] static ShownSummary Summarize(std::vector<double> intervals_ms);

    [[nodiscard]] const Config& GetConfig() const {
        return config;
    }

private:
    [[nodiscard]] u32 PickFrom(const std::vector<u32>& candidates, double real_fps,
                               double margin) const;
    [[nodiscard]] u32 StepTarget(const std::vector<u32>& candidates, u32 current,
                                 double real_fps) const;
    void DecideFrameGen(const Input& input, Decision& decision);
    void DecideResolution(const Input& input, Decision& decision);
    void DecidePower(const Input& input, Decision& decision);

    Config config;

    u32 fg_since{1'000'000}; ///< Windows since the last frame generation change
    u32 virtual_target{};    ///< With a fixed multiplier: the shown rate it aims at
    u32 judder_run{};
    u32 blocked_target{}; ///< Targets at or above this are not stepped up to
    u32 blocked_left{};

    u32 resolution_since{1'000'000};
    u32 headroom_run{};
    u32 no_effect_run{};
    u32 hold_left{};
    double run_fps{};       ///< Real fps before the last lowering step of the run
    double run_start_scale{};
    bool in_run{};

    PowerSource power_seen{PowerSource::Unknown};
    u32 power_run{};
    bool power_gave_up{};
};

} // namespace VideoCore
