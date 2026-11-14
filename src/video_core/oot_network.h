// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <mutex>

#include "common/common_types.h"
#include "common/settings.h"
#include "video_core/engines/maxwell_3d.h"

// The Legend of Zelda: Ocarina of Time's anti-aliasing. After tonemapping, the game runs a
// single-frame tensor network on the tonemapped image and an output pass that sharpens the result
// hard. The No sharpening setting replaces the output pass with a copy of the tonemapped image
// and can also skip the update's network passes (docs/nx2/oot-network-bypass.md).
//
// The update and the base game use different native output programs.
namespace VideoCommon::OotNetwork {

constexpr u64 ProgramId = 0x0400486024DEC000ULL;

// Native program keys of the network's compute passes.
constexpr u64 PrepassProgramKey = 0x8475d671e6967428ULL;
constexpr u64 NetworkProgramKey = 0x42593194bb0b2b32ULL;
constexpr u64 OutputProgramKey = 0xda6df3e54c716df2ULL;

// Base v65536 output passes sample cb0+0xc0 and store to cb0+0xd0. Match them
// before translation because the translator may reject their per-tap gathers.
struct BaseOutputProgram {
    u32 first_word;
    u32 size;
    u64 key;
};

constexpr std::array BaseOutputPrograms{
    BaseOutputProgram{0x006b7919, 0x57a0, 0x1e49acb491d9fb59ULL},
    BaseOutputProgram{0x00397919, 0x6430, 0x2142429c43740683ULL},
    BaseOutputProgram{0x003c7919, 0x6690, 0xd01939a775921a6cULL},
};

/// Constant buffer 0 offset of the output pass's texture handle for the B10G11R11 image it writes
/// (the scene image the rest of the frame uses).
constexpr u32 OutputImageHandleOffset = 0xd0;

/// Shader hashes of the output pass in the game's other modes, used only with
/// EDEN_NX2_OOT_BYPASS_ANY and EDEN_NX2_OOT_FSR_ANY: those the temporal replacement knows
/// (ComputePipeline::CanReplaceOcarinaUpscale), and those measured writing the 1920x1080
/// output in handheld without Max Resolution. Each dynamic resolution step has its own program.
constexpr bool IsOutputShader(u64 hash) {
    return hash == 0x6bd913e1f34e83ebULL || // docked, Max Resolution
           hash == 0xc033c3ddc9f0897bULL || // handheld, Max Resolution
           hash == 0xe40f5efb5480e692ULL || // 3/4 scale
           hash == 0xef542df87cacfe9eULL || // 3/4 scale, Linux build
           hash == 0x9302bc0d6d1d9bc1ULL || // handheld, 1024x576
           hash == 0x03bf0ffc7d2d0bedULL;   // handheld, 854x480 of 864x480
}

/// EDEN_NX2_OOT_FSR_ANY=1 (experiment): the FSR 3.1 temporal replacement takes the game's upscale
/// at every dynamic resolution step, not only the passes and the 4/3 scale it knows
/// (docs/native-dlss-replacement.md).
[[nodiscard]] inline bool FsrAnyStepRequested() {
    static const bool requested = [] {
        const char* const value = std::getenv("EDEN_NX2_OOT_FSR_ANY");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return requested;
}

/// Fragment shaders of the game's tonemap passes; render target 0 of their draws is the
/// network's input.
constexpr bool IsTonemapShader(u64 hash) {
    return hash == 0xbded5ce917183214ULL || // outdoors
           hash == 0x6cb06edf14be1d79ULL || // indoors
           hash == 0x3ac011bd2d772994ULL;   // outdoors at another time of day
}

/// Texels of a render target; x1 and y1 are exclusive.
struct Area {
    u32 x0, y0, x1, y1;
};

/// The part of render target 0 the current draw can reach: the render area (surface clip) cut by
/// viewport 0 and its scissor, as the Vulkan rasterizer sets them at 1x. With dynamic resolution
/// the game may draw into part of a larger target. The whole target when nothing is left.
inline Area DrawArea(const Tegra::Engines::Maxwell3D::Regs& regs) {
    using Regs = Tegra::Engines::Maxwell3D::Regs;
    const auto& target = regs.rt[0];
    const Area whole{0, 0, target.width, target.height};
    f32 x0 = 0.0f;
    f32 y0 = 0.0f;
    f32 x1 = static_cast<f32>((std::min)(target.width, regs.surface_clip.width.Value()));
    f32 y1 = static_cast<f32>((std::min)(target.height, regs.surface_clip.height.Value()));
    if (regs.viewport_scale_offset_enabled != 0) {
        const bool lower_left = regs.window_origin.mode != Regs::WindowOrigin::Mode::UpperLeft;
        const f32 clip_height = static_cast<f32>(regs.surface_clip.height.Value());
        const auto& viewport = regs.viewport_transform[0];
        const f32 x = viewport.translate_x - viewport.scale_x;
        const f32 width = viewport.scale_x * 2.0f;
        f32 y = viewport.translate_y - viewport.scale_y;
        f32 height = viewport.scale_y * 2.0f;
        if (lower_left) {
            y += clip_height;
            height = -height;
        }
        x0 = (std::max)(x0, (std::min)(x, x + width));
        x1 = (std::min)(x1, (std::max)(x, x + width));
        y0 = (std::max)(y0, (std::min)(y, y + height));
        y1 = (std::min)(y1, (std::max)(y, y + height));
        const auto& scissor = regs.scissor_test[0];
        if (scissor.enable != 0) {
            const f32 min_y = static_cast<f32>(scissor.min_y.Value());
            const f32 max_y = static_cast<f32>(scissor.max_y.Value());
            x0 = (std::max)(x0, static_cast<f32>(scissor.min_x.Value()));
            x1 = (std::min)(x1, static_cast<f32>(scissor.max_x.Value()));
            y0 = (std::max)(y0, lower_left ? clip_height - max_y : min_y);
            y1 = (std::min)(y1, lower_left ? clip_height - min_y : max_y);
        }
    }
    // Neither NaN nor outside the target: x0 and y0 start at 0, x1 and y1 at its size at most.
    if (!(x0 >= 0.0f && y0 >= 0.0f && x1 - x0 >= 1.0f && y1 - y0 >= 1.0f)) {
        return whole;
    }
    const Area area{static_cast<u32>(x0 + 0.5f), static_cast<u32>(y0 + 0.5f),
                    static_cast<u32>(x1 + 0.5f), static_cast<u32>(y1 + 0.5f)};
    return area.x1 > area.x0 && area.y1 > area.y0 ? area : whole;
}

/// Owned by the rasterizer: the tonemap pass draws on the 3D engine, and the network's passes do
/// not all run on the same compute engine.
struct BypassState {
    std::mutex mutex;
    /// Render target 0 of the latest tonemap draw (address 0 until one is seen), under mutex.
    Tegra::Engines::Maxwell3D::Regs::RenderTargetConfig tonemapped{};
    /// What that draw could reach of it (DrawArea), under mutex.
    Area tonemapped_area{};
    std::atomic<bool> output_replaced{}; ///< the latest output pass was replaced
    /// EDEN_NX2_OOT_BYPASS_STEP_GUARD: the tonemapped image's address and drawn area at the last
    /// successful replacement, under mutex (written only with the switch).
    u64 replaced_address{};
    Area replaced_area{};
};

/// EDEN_NX2_OOT_BYPASS_STEP_GUARD: the prepass and the network may be skipped only while the
/// current tonemap is the one whose output pass was last replaced, same image and same drawn
/// area. The first frame at a new resolution step runs the whole network.
[[nodiscard]] inline bool SkipAllowed(bool output_replaced, u64 replaced_address,
                                      const Area& replaced_area, u64 tonemapped_address,
                                      const Area& tonemapped_area) {
    return output_replaced && replaced_address == tonemapped_address &&
           replaced_area.x0 == tonemapped_area.x0 && replaced_area.y0 == tonemapped_area.y0 &&
           replaced_area.x1 == tonemapped_area.x1 && replaced_area.y1 == tonemapped_area.y1;
}

/// The images of a compute pass that reads one image and stores one.
struct OutputShapeInput {
    bool scene_e5b9g9r9;   ///< the read image is E5B9G9R9
    bool output_b10g11r11; ///< the stored image is B10G11R11
    u64 scene_address;
    u64 output_address;
    u32 scene_w;
    u32 scene_h;
    u32 output_w;
    u32 output_h;
};

/// Whether such a pass has the shape of the game's output pass at any resolution step: it reads
/// the network's E5B9G9R9 image and stores a B10G11R11 image elsewhere, no smaller in either axis.
[[nodiscard]] inline bool IsOutputShape(const OutputShapeInput& input) {
    return input.scene_e5b9g9r9 && input.output_b10g11r11 && input.output_address != 0 &&
           input.output_address != input.scene_address && input.output_w >= input.scene_w &&
           input.output_h >= input.scene_h;
}

/// Whether the image an output pass reads is the size of what the tonemap drew, within 1/32 in
/// each axis (the game's network image can be a few texels narrower than the drawn area).
[[nodiscard]] inline bool MatchesDrawnArea(u32 scene_w, u32 scene_h, const Area& drawn) {
    if (drawn.x1 <= drawn.x0 || drawn.y1 <= drawn.y0) {
        return false;
    }
    const auto close_to = [](u32 size, u32 drawn_size) {
        const u64 difference = size > drawn_size ? size - drawn_size : drawn_size - size;
        return difference * 32 <= drawn_size;
    };
    return close_to(scene_w, drawn.x1 - drawn.x0) && close_to(scene_h, drawn.y1 - drawn.y0);
}

inline bool BypassEnabled(u64 program_id) {
    return program_id == ProgramId &&
           Settings::values.oot_anti_aliasing.GetValue() == Settings::OotAntiAliasing::NoSharpening;
}

} // namespace VideoCommon::OotNetwork
