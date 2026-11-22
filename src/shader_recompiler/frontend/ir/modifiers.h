// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/bit_field.h"
#include "common/common_types.h"
#include "shader_recompiler/shader_info.h"

namespace Shader::IR {

enum class FmzMode : u8 {
    DontCare, // Not specified for this instruction
    FTZ,      // Flush denorms to zero, NAN is propagated (D3D11, NVN, GL, VK)
    FMZ,      // Flush denorms to zero, x * 0 == 0 (D3D9)
    None,     // Denorms are not flushed, NAN is propagated (nouveau)
};

enum class FpRounding : u8 {
    DontCare, // Not specified for this instruction
    RN,       // Round to nearest even,
    RM,       // Round towards negative infinity
    RP,       // Round towards positive infinity
    RZ,       // Round towards zero
};

struct FpControl {
    bool no_contraction{false};
    FpRounding rounding{FpRounding::DontCare};
    FmzMode fmz_mode{FmzMode::DontCare};
};
static_assert(sizeof(FpControl) <= sizeof(u32));

/// The flags of the global memory loads (LoadGlobal*, and the LoadStorage* they become), one u32.
/// Bit 0 is the guest's coherent bit (LDG.CG on a native program), which the SPIR-V backend turns
/// into the Coherent decoration of the storage buffers. Bits 1 to 28 are written by the storage
/// buffer pass for EDEN_NX2_GM bit 2 on a load that stayed global: up to four descriptors its
/// address can only be in, 7 bits each, index + 1, zero after the last. Nothing else may be read
/// from these flags as a whole: the coherent bit is `flags & 1`, the buffer list `flags >> 1`.
namespace GlobalLoadFlags {
inline constexpr u32 COHERENT_BIT{1};
inline constexpr u32 NARROW_SHIFT{1};
inline constexpr u32 NARROW_BITS_PER_BUFFER{7};
inline constexpr u32 NARROW_MAX_BUFFERS{4};
/// The largest buffer index a narrowed list can name (a stage has at most Info::MAX_SSBOS)
inline constexpr u32 NARROW_MAX_INDEX{(1U << NARROW_BITS_PER_BUFFER) - 2};
static_assert(Info::MAX_SSBOS - 1 <= NARROW_MAX_INDEX);
static_assert(NARROW_SHIFT + NARROW_MAX_BUFFERS * NARROW_BITS_PER_BUFFER <= 32);

[[nodiscard]] constexpr bool IsCoherent(u32 flags) noexcept {
    return (flags & COHERENT_BIT) != 0;
}

/// The narrowed buffer list alone (zero when the load is not narrowed), the key of the backend's
/// routing function for it. Never carries the coherent bit.
[[nodiscard]] constexpr u32 NarrowList(u32 flags) noexcept {
    return flags >> NARROW_SHIFT;
}

/// `flags` with the narrowed buffer list replaced by `list` (a NarrowList value); the coherent bit
/// is kept
[[nodiscard]] constexpr u32 WithNarrowList(u32 flags, u32 list) noexcept {
    return (flags & COHERENT_BIT) | (list << NARROW_SHIFT);
}

/// Appends buffer `index` at position `count` of `list`. False when it cannot be named.
[[nodiscard]] constexpr bool AppendNarrowBuffer(u32& list, u32 count, u32 index) noexcept {
    if (count >= NARROW_MAX_BUFFERS || index > NARROW_MAX_INDEX) {
        return false;
    }
    list |= (index + 1) << (count * NARROW_BITS_PER_BUFFER);
    return true;
}

/// Calls `visit(index)` for each buffer of `list` in order, or returns false at the first entry
/// `visit` rejects
template <typename Visitor>
constexpr bool ForEachNarrowBuffer(u32 list, Visitor&& visit) {
    constexpr u32 mask{(1U << NARROW_BITS_PER_BUFFER) - 1};
    for (; list != 0; list >>= NARROW_BITS_PER_BUFFER) {
        if (!visit((list & mask) - 1)) {
            return false;
        }
    }
    return true;
}
} // namespace GlobalLoadFlags

union TextureInstInfo {
    u32 raw;
    BitField<0, 16, u32> descriptor_index;
    BitField<16, 3, TextureType> type;
    BitField<19, 1, u32> is_depth;
    BitField<19, 1, u32> image_byte_address;
    BitField<20, 1, u32> has_bias;
    BitField<20, 1, u32> image_coherent;
    BitField<21, 1, u32> has_lod_clamp;
    BitField<22, 1, u32> relaxed_precision;
    BitField<23, 2, u32> gather_component;
    BitField<25, 2, u32> num_derivatives;
    BitField<27, 3, ImageFormat> image_format;
    BitField<30, 1, u32> ndv_is_active;
    /// Only meaningful for the sampled-texture family (ImageSampleImplicitLod, ImageFetch,
    /// ImageGather, etc.); unused/zero for storage-image opcodes, which carry their own
    /// is_integer via ImageDescriptor/ImageBufferDescriptor instead.
    BitField<31, 1, u32> is_integer;
};
static_assert(sizeof(TextureInstInfo) <= sizeof(u32));

} // namespace Shader::IR
