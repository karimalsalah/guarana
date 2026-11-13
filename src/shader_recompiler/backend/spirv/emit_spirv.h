// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>

#include "common/common_types.h"
#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"

namespace Shader::Backend::SPIRV {

constexpr u32 NUM_TEXTURE_SCALING_WORDS = 4;
constexpr u32 NUM_IMAGE_SCALING_WORDS = 2;
constexpr u32 NUM_TEXTURE_AND_IMAGE_SCALING_WORDS =
    NUM_TEXTURE_SCALING_WORDS + NUM_IMAGE_SCALING_WORDS;

struct RescalingLayout {
    alignas(16) std::array<u32, NUM_TEXTURE_SCALING_WORDS> rescaling_textures;
    alignas(16) std::array<u32, NUM_IMAGE_SCALING_WORDS> rescaling_images;
    u32 down_factor;
};
struct RenderAreaLayout {
    std::array<f32, 4> render_area;
};
constexpr u32 RESCALING_LAYOUT_WORDS_OFFSET = offsetof(RescalingLayout, rescaling_textures);
constexpr u32 RESCALING_LAYOUT_DOWN_FACTOR_OFFSET = offsetof(RescalingLayout, down_factor);
constexpr u32 RENDERAREA_LAYOUT_OFFSET = offsetof(RenderAreaLayout, render_area);

/// Culling the epilogue of a native mesh program can add (Profile and RuntimeInfo mesh_cull).
constexpr u32 MESH_CULL_FRUSTUM = 1; ///< triangles outside one of the x, y or w clip planes
constexpr u32 MESH_CULL_SMALL = 2;   ///< triangles whose bounding box holds no pixel centre
constexpr u32 MESH_CULL_EMPTY = 4;   ///< no outputs for a workgroup with no triangle left
/// Push constant of the small triangle test: viewport 0 (x, y, width, height) in host pixels.
/// It takes the last sizeof(RenderAreaLayout) bytes of the pipeline layout's range, which the
/// render area (at offset 0) leaves unused.
constexpr u32 MESH_CULL_VIEWPORT_OFFSET = sizeof(RescalingLayout);

/// Cheaper outputs of a native mesh program (Profile and RuntimeInfo mesh_out).
constexpr u32 MESH_OUT_DEAD = 1;   ///< no output for generic components the fragment stage never reads
constexpr u32 MESH_OUT_RATE = 2;   ///< 2x2 shading rate for the triangles MESH_CULL_SMALL finds tiny
constexpr u32 MESH_OUT_VECTOR = 8; ///< one store per output vector instead of one per component
constexpr u32 MESH_OUT_COUNT = 16; ///< only as many vertices as the primitives use, not max_vertices

/// Cheaper run-time routing of global memory accesses (Profile::global_memory_opt).
constexpr u32 GM_HOIST = 1;  ///< buffer base addresses are computed once, at the start of main
constexpr u32 GM_NARROW = 2; ///< a load searches only the buffers the storage buffer pass named
/// 4 is reserved for rebasing single-pointer accesses at compile time (not implemented).
constexpr u32 GM_NARROW_NO_FALLBACK = 8; ///< with GM_NARROW: zero, not the full search, below them

[[nodiscard]] std::vector<u32> EmitSPIRV(const Profile& profile, const RuntimeInfo& runtime_info, IR::Program& program, Bindings& bindings);
[[nodiscard]] inline std::vector<u32> EmitSPIRV(const Profile& profile, IR::Program& program) {
    Bindings binding;
    return EmitSPIRV(profile, {}, program, binding);
}

/// Whether EmitSPIRV gathers the program's software HMMA operands from subgroup scratch in shared
/// memory (Profile::support_software_matrix_shared_memory). Its pipeline needs full subgroups of
/// the guest's 32 lanes; this answers before the SPIR-V exists.
[[nodiscard]] bool UsesSoftwareMatrixSharedMemory(const Profile& profile,
                                                  const IR::Program& program);

/// Words per lane in the buffer of EmitCooperativeHmmaTest: the guest registers a0 a1 a2 a3 b0 b1
/// c0 c1 as input, then d0 d1 as output.
constexpr u32 COOPERATIVE_HMMA_TEST_LANE_WORDS = 10;

/// A compute shader of one warp running the HMMA.16816.F16 lowering on the registers in its storage
/// buffer (set 0, binding 0), to check at startup that the driver's matrix layout is the expected.
[[nodiscard]] std::vector<u32> EmitCooperativeHmmaTest(u32 spirv_version);

/// Mesh emulation without VK_EXT_mesh_shader. The mesh program runs as a compute shader
/// (RuntimeInfo::mesh_as_compute) that writes one block per workgroup to a u32 storage buffer:
/// max_vertices * vertex_lines words (vertex v, line l at v * vertex_lines + l), then
/// max_primitives * 3 vertex indices (0 0 0 past the primitive count, a degenerate triangle).
[[nodiscard]] inline u32 MeshEmulationBlockWords(const MeshInfo& mesh) {
    return mesh.max_vertices * static_cast<u32>(mesh.vertex_lines.size()) +
           mesh.max_primitives * 3;
}

/// Vertex shader drawing the emulation's output as a triangle list of
/// groups * max_primitives * 3 vertices, with the mesh program's vertex outputs. With a task
/// program (args_binding != ~0) the blocks start at the arguments' MESH_EMULATION_ARGS_BASE.
[[nodiscard]] std::vector<u32> EmitMeshEmulationVertex(const Profile& profile,
                                                       const MeshInfo& mesh, u32 binding,
                                                       u32 args_binding, bool convert_depth);

/// Mesh emulation with a task program: push constants of the distribution pass.
struct MeshEmulationDistributionConstants {
    u32 tasks;              ///< task workgroups of the draw
    u32 payload_words;      ///< words per task in the payload buffer (count at word 1)
    u32 capacity_words;     ///< words of the output arena
    u32 block_words;        ///< output words per mesh workgroup
    u32 vertices_per_group; ///< draw vertices per mesh workgroup
    u32 max_groups;         ///< the device's maxComputeWorkGroupCount[0]
};
/// Words of the distribution's argument buffer: vkCmdDispatchIndirect at 0, the mesh workgroups
/// the tasks asked for at 3, vkCmdDrawIndirect at MESH_EMULATION_DRAW_ARGS_OFFSET, then the
/// draw's first output word in the arena and its task count.
constexpr u32 MESH_EMULATION_ARGS_REQUESTED = 3;
constexpr u32 MESH_EMULATION_DRAW_ARGS_OFFSET = 16;
constexpr u32 MESH_EMULATION_ARGS_BASE = 8;
constexpr u32 MESH_EMULATION_ARGS_TASKS = 9;
constexpr u32 MESH_EMULATION_ARGS_WORDS = 10;

/// Distribution pass (one invocation) between the task and mesh dispatches, set 0: payload
/// buffer (binding 0), offsets (1: first mesh workgroup of each task), arguments (2) and the
/// arena counter (3: output words allocated so far, kept counting past the arena's end). The
/// draw's output is allocated from the arena every draw of the submission shares; mesh
/// workgroups that do not fit are not dispatched.
[[nodiscard]] std::vector<u32> EmitMeshEmulationDistribution(const Profile& profile);

} // namespace Shader::Backend::SPIRV
