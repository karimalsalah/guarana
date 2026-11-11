// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#version 450

// ocarina_actor_motion.frag for the half-size extrapolation guide: motion in UV, written where
// the actor is the nearest of the pixel's 2x2 block of scene depth.

layout(location = 0) in vec4 previous_clip;
layout(location = 0) out vec2 motion;

layout(binding = 3) uniform sampler2D scene_depth;
layout(push_constant) uniform DepthComparison {
    uint depth_tolerance;
} comparison;

layout(std140, binding = 2, row_major) uniform Transforms {
    mat4 current_view_projection;
    mat4 previous_view_projection;
    vec4 current_origin;
    vec4 previous_origin;
    vec2 extent; // of the scene, twice the target's
    vec2 jitter;
} transforms;

void main() {
    ivec2 base = ivec2(gl_FragCoord.xy) * 2;
    ivec2 limit = textureSize(scene_depth, 0) - 1;
    float nearest = 2.0;
    vec2 offset = vec2(0);
    for (int i = 0; i < 4; ++i) {
        ivec2 corner = ivec2(i & 1, i >> 1);
        float depth = texelFetch(scene_depth, min(base + corner, limit), 0).r;
        if (depth < nearest) {
            nearest = depth;
            // From this fragment's centre to that texel's, in target pixels.
            offset = (vec2(corner) - 0.5) * 0.5;
        }
    }
    // Depth is planar across a triangle, so this is the actor's depth at that texel; the slope
    // allows for a neighbouring triangle.
    vec2 slope = vec2(dFdx(gl_FragCoord.z), dFdy(gl_FragCoord.z));
    float actual = gl_FragCoord.z + dot(slope, offset);
    float tolerance = 0.5 * (abs(slope.x) + abs(slope.y)) +
                      float(comparison.depth_tolerance) * 2.5e-7;
    if (abs(nearest - actual) > tolerance || previous_clip.w <= 1e-6) {
        discard;
    }
    vec2 previous_uv = previous_clip.xy / previous_clip.w * vec2(0.5, -0.5) + 0.5;
    vec2 current_uv = (gl_FragCoord.xy * 2.0 - transforms.jitter) / transforms.extent;
    vec2 displacement = previous_uv - current_uv;
    if (!all(lessThanEqual(abs(displacement), vec2(16)))) {
        discard;
    }
    motion = displacement;
}
