// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cmath>

// EDEN_NX2_FG_GAME_MOTION: an in-between frame warped from the game's motion instead of optical
// flow (docs/design/game-reconstruction-and-motion.md, 3.2). The arithmetic of
// host_shaders/game_motion_interpolation.comp on plain values, line for line, so a test can run it.
//
// A guide texel holds the motion of a surface point to the previous real frame, in UV of the scene,
// and its nearness (1 - depth: 0 at the far plane, larger when nearer). `t` places the in-between
// frame from the previous real frame (0) to the newest (1).
namespace Vulkan::GameMotionWarp {

struct Vec2 {
    float x{};
    float y{};
};

struct GuideTexel {
    Vec2 motion;    ///< UV to the previous real frame
    float nearness; ///< 1 - depth
};

/// A solve whose last step moved less than this many output pixels has found its source.
constexpr float CONVERGED_PIXELS = 2.0f;
/// Two nearness values within this ratio (plus the bias) are the same surface.
constexpr float SAME_SURFACE_RATIO = 1.1f;
constexpr float SAME_SURFACE_BIAS = 1e-3f;

[[nodiscard]] inline Vec2 Add(Vec2 a, Vec2 b) {
    return {a.x + b.x, a.y + b.y};
}

[[nodiscard]] inline Vec2 Scale(Vec2 a, float s) {
    return {a.x * s, a.y * s};
}

/// Where a surface point lies at the in-between frame's time, solved from one real frame: a point
/// at p of that frame is at q when p = q + advance * motion(p). Two fixed-point steps, as
/// frame_extrapolation.comp takes them.
struct Solution {
    Vec2 position;    ///< source UV in the real frame
    Vec2 previous;    ///< the estimate before it
    float nearness{}; ///< of the texel the last step was taken from
    bool converged{}; ///< the last step moved less than CONVERGED_PIXELS
};

template <typename Guide>
[[nodiscard]] Solution Solve(const Guide& guide, Vec2 target, float advance, Vec2 screen_size) {
    GuideTexel sampled = guide(target);
    Vec2 before = target;
    Vec2 after = Add(target, Scale(sampled.motion, advance));
    for (int step = 0; step < 2; ++step) {
        before = after;
        sampled = guide(before);
        after = Add(target, Scale(sampled.motion, advance));
    }
    const float dx = (after.x - before.x) * screen_size.x;
    const float dy = (after.y - before.y) * screen_size.y;
    return {after, before, sampled.nearness, std::sqrt(dx * dx + dy * dy) < CONVERGED_PIXELS};
}

[[nodiscard]] inline bool SameSurface(float a, float b) {
    return (std::max)(a, b) <= (std::min)(a, b) * SAME_SURFACE_RATIO + SAME_SURFACE_BIAS;
}

/// What an in-between pixel shows: the newest frame at `newest`, the previous frame at `previous`,
/// mixed with `newest_weight` on the newest (1: the newest alone, 0: the previous alone).
struct Pick {
    Vec2 newest;
    Vec2 previous;
    float newest_weight{};
};

/// The in-between pixel at `target` (scene UV) at time `t`.
///
/// - From the newest frame the point's source solves p = q - (1 - t) m(p) on the newest guide; the
///   same point in the previous frame is at p + m(p), exactly, by the guide's definition.
/// - From the previous frame the forward motion is unknown; it is taken as the previous guide's
///   motion reversed (constant velocity), so the source solves p' = q + t m'(p').
/// - Both solved on the same surface, or only the newest solved: the newest frame's point, mixed
///   with its trajectory point in the previous frame by t where the previous frame saw that point
///   (its nearness there is the same surface), else alone.
/// - Both solved on different surfaces: the nearer is in front at time t and wins alone.
/// - Only the previous solved: what the newest frame no longer shows (a disocclusion) comes from the
///   previous frame alone.
/// - Neither: the farther of the two last estimates, as the background behind whatever moved.
template <typename Guide>
[[nodiscard]] Pick Interpolate(const Guide& newest_guide, const Guide& previous_guide, Vec2 target,
                               float t, Vec2 screen_size) {
    const Solution newest = Solve(newest_guide, target, -(1.0f - t), screen_size);
    const Solution previous = Solve(previous_guide, target, t, screen_size);
    const bool newest_wins =
        newest.converged &&
        (!previous.converged || SameSurface(newest.nearness, previous.nearness) ||
         newest.nearness > previous.nearness);
    if (newest_wins) {
        const Vec2 trail = Add(newest.position, newest_guide(newest.position).motion);
        const bool seen_before = SameSurface(previous_guide(trail).nearness, newest.nearness);
        return {newest.position, trail, seen_before ? t : 1.0f};
    }
    if (previous.converged) {
        return {previous.position, previous.position, 0.0f};
    }
    // Neither converged: each solve alternates between what moved away and what was behind it.
    const auto behind = [](const Guide& guide, const Solution& solution) {
        return guide(solution.position).nearness < guide(solution.previous).nearness
                   ? solution.position
                   : solution.previous;
    };
    const Vec2 newest_behind = behind(newest_guide, newest);
    const Vec2 previous_behind = behind(previous_guide, previous);
    if (newest_guide(newest_behind).nearness <= previous_guide(previous_behind).nearness) {
        return {newest_behind, newest_behind, 1.0f};
    }
    return {previous_behind, previous_behind, 0.0f};
}

/// What GuardHud did to a pick.
enum class HudGuard {
    None,          ///< no source the pick uses lies on the HUD
    OtherFrame,    ///< the frame whose source lies on the HUD gave way to the other one
    SceneUnderHud, ///< every source it uses does: the scene under the HUD at the source
    Unmoved,       ///< the same without `fill`: the newest frame at the pixel itself
};

/// Scene pixels must not drag the HUD along (frame_extrapolation.comp's rule): an in-between pixel
/// outside the HUD whose source in a real frame lies on the HUD would copy the HUD into the scene
/// beside it, as far as the motion. `is_hud` is the newest frame's HUD test in scene UV, standing
/// for both frames: the HUD stays put between them. A source on the HUD gives way to the other
/// frame's when the pick uses that one too and it is clear; otherwise, with `fill`
/// (EDEN_NX2_EXTRAPOLATE_HUD_FILL), the newest frame's scene under the HUD at the source the pick
/// used (the previous frame's position read in the newest frame when it used only that one), and
/// without it the newest frame at `target`, unmoved.
template <typename IsHud>
[[nodiscard]] HudGuard GuardHud(const IsHud& is_hud, Pick& pick, Vec2 target, bool fill) {
    const bool newest_used = pick.newest_weight > 0.0f;
    const bool previous_used = pick.newest_weight < 1.0f;
    const bool newest_on = newest_used && is_hud(pick.newest);
    const bool previous_on = previous_used && is_hud(pick.previous);
    if (!newest_on && !previous_on) {
        return HudGuard::None;
    }
    if (newest_on && previous_used && !previous_on) {
        pick = {pick.previous, pick.previous, 0.0f};
        return HudGuard::OtherFrame;
    }
    if (previous_on && newest_used && !newest_on) {
        pick = {pick.newest, pick.newest, 1.0f};
        return HudGuard::OtherFrame;
    }
    if (fill) {
        const Vec2 source = newest_used ? pick.newest : pick.previous;
        pick = {source, source, 1.0f};
        return HudGuard::SceneUnderHud;
    }
    pick = {target, target, 1.0f};
    return HudGuard::Unmoved;
}

} // namespace Vulkan::GameMotionWarp
