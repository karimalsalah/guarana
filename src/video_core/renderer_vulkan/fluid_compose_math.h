// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdlib>

#include "common/common_types.h"

/// Pure logic of the FLUID presenter's slot pass (EDEN_NX2_FLUID_COMPOSE): no Vulkan types, so
/// the tests under src/tests/video_core/fluid_compose_math.cpp cover it.
namespace Vulkan::FluidComposeMath {

/// The largest side of a frame the slot pass is asked to read.
constexpr u32 MaxExtent = 16384;

/// The slot pass's work group is this many pixels on a side (fluid_compose.comp).
constexpr u32 GroupSize = 16;

struct Extent {
    u32 width{};
    u32 height{};
};

/// A rectangle of output pixels.
struct Rect {
    u32 x{};
    u32 y{};
    u32 width{};
    u32 height{};
};

/// EDEN_NX2_FLUID_COMPOSE as the environment gives it: 0 (unset, empty, "0", "false" and anything
/// strtoul reads as 0) is off, any other number is 1. Parsed as EDEN_NX2_FLUID is.
[[nodiscard]] inline u32 ModeOf(const char* value) {
    if (value == nullptr) {
        return 0U;
    }
    return std::strtoul(value, nullptr, 10) != 0 ? 1U : 0U;
}

/// The size of the frames the slot pass reads: the guest's, rounded up to an even size, and never
/// smaller than `previous` in either axis, so a game that changes its render size does not
/// recreate the frame pool each time. A guest without an area leaves `previous` as it is.
[[nodiscard]] constexpr Extent InternalExtent(Extent guest, Extent previous) {
    if (guest.width == 0 || guest.height == 0) {
        return previous;
    }
    const auto grow = [](u32 wanted, u32 current) {
        const u32 capped = (std::min)(wanted, MaxExtent);
        const u32 even = capped + (capped & 1U);
        return (std::max)(even, current);
    };
    return Extent{grow(guest.width, previous.width), grow(guest.height, previous.height)};
}

/// The rectangle of a layout's screen (left, top, right and bottom edges) inside an output of
/// the given size: clamped to it; empty when the edges are reversed or lie outside.
[[nodiscard]] constexpr Rect ScreenRect(u32 output_width, u32 output_height, u32 left, u32 top,
                                        u32 right, u32 bottom) {
    const u32 x = (std::min)(left, output_width);
    const u32 y = (std::min)(top, output_height);
    const u32 end_x = (std::min)((std::max)(right, x), output_width);
    const u32 end_y = (std::min)((std::max)(bottom, y), output_height);
    return Rect{x, y, end_x - x, end_y - y};
}

/// Whether a frame of the internal size can be scaled into the rectangle with two bilinear taps
/// without visible aliasing: the rectangle is at least three quarters of the frame on each side.
/// Else the classic path (which filters at the output size) is used.
[[nodiscard]] constexpr bool Worthwhile(Rect rect, Extent internal) {
    return internal.width != 0 && internal.height != 0 && rect.width != 0 && rect.height != 0 &&
           u64{rect.width} * 4 >= u64{internal.width} * 3 &&
           u64{rect.height} * 4 >= u64{internal.height} * 3;
}

/// The slot pass's push constants, in the order and with the types of the block in
/// fluid_compose.comp (all 32-bit scalars, so std430 packs them without padding).
struct PushConstants {
    s32 out_width;
    s32 out_height;
    s32 rect_x;
    s32 rect_y;
    s32 rect_width;
    s32 rect_height;
};
static_assert(sizeof(PushConstants) == 24);
static_assert(sizeof(PushConstants) % 4 == 0 && sizeof(PushConstants) <= 128);
static_assert(offsetof(PushConstants, out_width) == 0);
static_assert(offsetof(PushConstants, out_height) == 4);
static_assert(offsetof(PushConstants, rect_x) == 8);
static_assert(offsetof(PushConstants, rect_y) == 12);
static_assert(offsetof(PushConstants, rect_width) == 16);
static_assert(offsetof(PushConstants, rect_height) == 20);

[[nodiscard]] constexpr PushConstants MakePushConstants(u32 out_width, u32 out_height, Rect rect) {
    return PushConstants{
        .out_width = static_cast<s32>(out_width),
        .out_height = static_cast<s32>(out_height),
        .rect_x = static_cast<s32>(rect.x),
        .rect_y = static_cast<s32>(rect.y),
        .rect_width = static_cast<s32>(rect.width),
        .rect_height = static_cast<s32>(rect.height),
    };
}

/// Work groups to cover `extent` pixels on one axis.
[[nodiscard]] constexpr u32 GroupCount(u32 extent) {
    return extent / GroupSize + (extent % GroupSize != 0 ? 1U : 0U);
}

/// Where the slot pass samples the source for an output pixel: the whole source maps onto the
/// rectangle, sampled at the pixel's centre; `inside` is false for the letterbox bars, which the
/// pass fills with black. The same arithmetic as fluid_compose.comp.
struct Uv {
    f32 u{};
    f32 v{};
    bool inside{};
};

[[nodiscard]] constexpr Uv SourceUv(s32 pixel_x, s32 pixel_y, Rect rect) {
    const s64 dx = s64{pixel_x} - s64{rect.x};
    const s64 dy = s64{pixel_y} - s64{rect.y};
    if (dx < 0 || dy < 0 || dx >= s64{rect.width} || dy >= s64{rect.height}) {
        return Uv{};
    }
    return Uv{
        .u = (static_cast<f32>(dx) + 0.5f) / static_cast<f32>(rect.width),
        .v = (static_cast<f32>(dy) + 0.5f) / static_cast<f32>(rect.height),
        .inside = true,
    };
}

/// Whether the configured frame generation lets frames be drawn at the internal size: its
/// interpolated frames go to the FLUID presenter's slots (EDEN_NX2_FLUID_INTERP), or it is off.
[[nodiscard]] constexpr bool FrameGenerationFits(bool fluid_interpolation, bool frame_generation) {
    return fluid_interpolation || !frame_generation;
}

/// What the slot pass needs of a composite. Every field that is not what the pass needs returns
/// the composite to the classic path: the frame drawn at the window's size and copied by the
/// screen copy.
struct Gate {
    bool available{};              ///< Switch on, FLUID presenter, storage swapchain, compute queue
    bool hdr{};                    ///< HDR output
    bool extrapolation{};          ///< Extrapolation is requested or running
    bool streamline{};             ///< DLSS Frame Generation presents the frames
    bool frame_generation_fits{};  ///< FrameGenerationFits
    bool pictures_armed{};         ///< The debug server's shown_frames wants pictures
    bool worthwhile{};             ///< Worthwhile
};

[[nodiscard]] constexpr bool Active(const Gate& gate) {
    return gate.available && !gate.hdr && !gate.extrapolation && !gate.streamline &&
           gate.frame_generation_fits && !gate.pictures_armed && gate.worthwhile;
}

} // namespace Vulkan::FluidComposeMath
