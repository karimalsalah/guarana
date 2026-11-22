// SPDX-FileCopyrightText: Copyright 2026 Guarana contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/common_types.h"
#include "common/device_profile.h"

// What the community's device reports measured: the effects of paired tools/bench rows per GPU
// family and game, from site/data/community-evidence.json (tools/ingest/evidence.py, docs/FEEDBACK.md).
// Each build carries the copy its source tree had. The resolver cites them in its evidence text;
// nothing here changes a default (#24).
namespace Core::CommunityEvidence {

/// One effect: the rows that set `set` against the rows that set `against`, on one GPU family, game,
/// scene, build and power source, with everything else alike.
struct Effect {
    Common::GpuFamily family{Common::GpuFamily::None};
    u64 title_id{}; ///< 0 when the reports did not name the game's program id.
    std::string scene;
    /// EDEN_NX2_* switches, and settings as "setting:<section>/<key>", with their values.
    std::vector<std::pair<std::string, std::string>> set;
    std::vector<std::pair<std::string, std::string>> against;
    std::vector<std::string> context; ///< What both rows set alike, as NAME=value.
    std::string build;
    std::string power; ///< "ac", "battery", or empty when not known.
    u32 devices{};
    u32 pairs{};
    /// Game fps of `set` against `against` in percent: the median, lowest and highest over devices.
    std::optional<double> fps_change_median;
    std::optional<double> fps_change_min;
    std::optional<double> fps_change_max;
    std::optional<double> p99_change_median; ///< In percent; lower is better.
    std::vector<u32> issues;                 ///< The device reports, by issue number.
};

/// The effects of a community-evidence.json document on the GPU families the engine knows; empty
/// when the document cannot be read. Pure.
[[nodiscard]] std::vector<Effect> Parse(std::string_view json);

/// The effects of the community-evidence.json this build carries, read once.
[[nodiscard]] std::span<const Effect> Embedded();

/// The effect that measures `name` (an EDEN_NX2_* switch, or a setting's label) at `value` on this
/// family and title: one that changes that switch or setting alone, with `value` on either side.
/// Of several, the one with the most devices, then the most pairs, then on mains power, then the
/// first. nullptr when there is none. Pure.
[[nodiscard]] const Effect* Find(std::span<const Effect> effects, Common::GpuFamily family,
                                 u64 title_id, std::string_view name, std::string_view value);

/// The effect in one line for the log and the debug server: "EDEN_NX2_GM=11 against EDEN_NX2_GM=0
/// in oot-hyrule-field: game fps +48.0% median (+31.0% to +55.0%), p99 -40.0%, 3 devices, 4 pairs,
/// v20.5.0, mains, community reports #12 #40 #52". Pure.
[[nodiscard]] std::string Describe(const Effect& effect);

} // namespace Core::CommunityEvidence
