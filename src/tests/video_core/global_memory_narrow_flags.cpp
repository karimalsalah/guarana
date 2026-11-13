// SPDX-License-Identifier: GPL-3.0-or-later

// EDEN_NX2_GM bit 2 (HostTranslateInfo::narrow_global_candidates) names the buffers a load that
// stayed global can only be in. It used to store that list in the load's flags, which the same
// load also carries the LDG.CG "coherent" bit in (IR::GlobalLoadFlags). These tests keep the two
// apart: a narrowed load is not coherent, a coherent load keeps its bit whether it is narrowed or
// not, and an unnarrowed coherent load is not routed to buffer 0 by the SPIR-V backend.

#include <algorithm>
#include <array>
#include <optional>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/modifiers.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/ir_opt/passes.h"

namespace {
using namespace Shader;

class TestEnvironment final : public Environment {
public:
    TestEnvironment() {
        stage = Stage::Compute;
    }
    bool IsNativeProgram() const override { return true; }
    bool HasNativeInstructions() const override { return true; }
    u64 ReadInstruction(u32) override { return 0; }
    u32 ReadCbufValue(u32, u32) override { return 0; }
    TextureType ReadTextureType(u32) override { return TextureType::Color2D; }
    TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return TexturePixelFormat::A8B8G8R8_UNORM;
    }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    u32 ReadViewportTransformState() override { return 1; }
    u32 TextureBoundBuffer() const override { return 0; }
    u32 LocalMemorySize() const override { return 0; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {32, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<ReplaceConstant> GetReplaceConstBuffer(u32, u32) override { return std::nullopt; }
    void Dump(u64, u64) override {}
};

struct Fixture {
    ObjectPool<IR::Inst> pool{256};
    IR::Block block{pool};
    IR::Program program;
    TestEnvironment env;
    Profile profile{};

    Fixture() {
        program.stage = Stage::Compute;
        program.workgroup_size = {32, 1, 1};
        program.blocks = {&block};
        program.post_order_blocks = {&block};
        program.syntax_list = {{{.block = &block}, IR::AbstractSyntaxNode::Type::Block},
                               {{}, IR::AbstractSyntaxNode::Type::Return}};
        profile.supported_spirv = 0x00010300;
        profile.min_ssbo_alignment = 16;
    }
};

IR::U32 Word(IR::IREmitter& ir, u32 offset) {
    return ir.GetCbuf(ir.Imm32(0), ir.Imm32(offset));
}

/// Ocarina of Time's foliage mesh shape: a pointer loaded from a table entry at *c0[0x30] + 0x20,
/// and a 128-bit record at that pointer plus a lane offset, which bit 2 narrows to the pointer's
/// buffer. `coherent` is the record's LDG.CG bit.
struct NarrowedRecord {
    Fixture fixture;
    IR::IREmitter ir{fixture.block};
    IR::Value pointer;

    explicit NarrowedRecord(bool coherent, bool store_to_pointer_buffer = false) {
        const IR::U64 table{ir.PackUint2x32(ir.CompositeConstruct(Word(ir, 0x30), Word(ir, 0x34)))};
        pointer = ir.LoadGlobal64(ir.IAdd(table, ir.Imm64(u64{0x20})));
        const IR::U32 pointer_low{ir.CompositeExtract(pointer, 0)};
        const IR::U32 pointer_high{ir.CompositeExtract(pointer, 1)};
        const auto address = [&] {
            const IR::U32 low{ir.IAdd(
                pointer_low,
                ir.ShiftLeftLogical(ir.LocalInvocationIdX(), ir.Imm32(4)))};
            const IR::U32 carry{ir.Select(ir.GetCarryFromOp(low), ir.Imm32(1), ir.Imm32(0))};
            return ir.PackUint2x32(ir.CompositeConstruct(low, ir.IAdd(pointer_high, carry)));
        };
        if (store_to_pointer_buffer) {
            // The buffer is written, so the load cannot be narrowed and searches every buffer
            const IR::U32 store_low{ir.IAdd(pointer_low, ir.Imm32(0x100))};
            ir.WriteGlobal32(ir.PackUint2x32(ir.CompositeConstruct(store_low, pointer_high)),
                             ir.Imm32(1));
        }
        static_cast<void>(ir.LoadGlobal128(address(), coherent));
    }

    void RunStorageBufferPass() {
        HostTranslateInfo host{};
        host.min_ssbo_alignment = 16;
        host.support_int64 = true;
        host.narrow_global_candidates = true;
        Optimization::GlobalMemoryToStorageBufferPass(fixture.env, fixture.program, host);
    }

    const IR::Inst& Record() const {
        const auto& insts{fixture.block.Instructions()};
        const auto it{std::ranges::find_if(insts, [](const IR::Inst& inst) {
            return inst.GetOpcode() == IR::Opcode::LoadGlobal128;
        })};
        REQUIRE(it != insts.end());
        return *it;
    }
};

size_t CountFunctions(const std::vector<u32>& code) {
    constexpr u32 OP_FUNCTION{54};
    size_t count{};
    for (size_t i = 5; i < code.size();) {
        const u32 words{code[i] >> 16};
        REQUIRE(words > 0);
        count += (code[i] & 0xffff) == OP_FUNCTION ? 1 : 0;
        i += words;
    }
    return count;
}
} // Anonymous namespace

TEST_CASE("A narrowed global load is not coherent", "[shader][storage_buffer][coherence]") {
    NarrowedRecord program{false};
    program.RunStorageBufferPass();
    const IR::Inst& record{program.Record()};
    // The narrowing happened: the record names buffer 0 (its pointer's), and is not coherent
    REQUIRE(IR::GlobalLoadFlags::NarrowList(record.Flags<u32>()) != 0);
    CHECK_FALSE(IR::GlobalLoadFlags::IsCoherent(record.Flags<u32>()));

    Optimization::CollectShaderInfoPass(program.fixture.env, program.fixture.program);
    CHECK_FALSE(program.fixture.program.info.requires_coherent_global_memory);
}

TEST_CASE("A narrowed LDG.CG load keeps its coherent bit and its buffer list",
          "[shader][storage_buffer][coherence]") {
    NarrowedRecord program{true};
    program.RunStorageBufferPass();
    const IR::Inst& record{program.Record()};
    const u32 flags{record.Flags<u32>()};
    CHECK(IR::GlobalLoadFlags::IsCoherent(flags));
    CHECK(IR::GlobalLoadFlags::NarrowList(flags) == 1); // [binding 0]

    Optimization::CollectShaderInfoPass(program.fixture.env, program.fixture.program);
    CHECK(program.fixture.program.info.requires_coherent_global_memory);
}

TEST_CASE("An LDG.CG load that could not be narrowed keeps its coherent bit",
          "[shader][storage_buffer][coherence]") {
    NarrowedRecord program{true, true};
    program.RunStorageBufferPass();
    const IR::Inst& record{program.Record()};
    CHECK(IR::GlobalLoadFlags::NarrowList(record.Flags<u32>()) == 0);
    CHECK(IR::GlobalLoadFlags::IsCoherent(record.Flags<u32>()));

    Optimization::CollectShaderInfoPass(program.fixture.env, program.fixture.program);
    CHECK(program.fixture.program.info.requires_coherent_global_memory);
}

TEST_CASE("Narrowed buffer lists round trip beside the coherent bit", "[shader][coherence]") {
    using namespace IR::GlobalLoadFlags;
    for (const bool coherent : {false, true}) {
        u32 list{};
        const std::array<u32, 4> indices{0, 5, 63, 126};
        for (u32 count = 0; count < indices.size(); ++count) {
            REQUIRE(AppendNarrowBuffer(list, count, indices[count]));
            const u32 flags{WithNarrowList(coherent ? COHERENT_BIT : 0, list)};
            CHECK(IsCoherent(flags) == coherent);
            CHECK(NarrowList(flags) == list);
            std::vector<u32> decoded;
            CHECK(ForEachNarrowBuffer(NarrowList(flags), [&](u32 index) {
                decoded.push_back(index);
                return true;
            }));
            CHECK(decoded == std::vector<u32>(indices.begin(), indices.begin() + count + 1));
        }
        CHECK_FALSE(AppendNarrowBuffer(list, 4, 0));
        CHECK_FALSE(AppendNarrowBuffer(list, 0, NARROW_MAX_INDEX + 1));
        // Clearing the list leaves the coherent bit as it was
        const u32 base{coherent ? COHERENT_BIT : 0};
        CHECK(WithNarrowList(base | (list << NARROW_SHIFT), 0) == base);
    }
    CHECK(NarrowList(COHERENT_BIT) == 0);
}

TEST_CASE("The SPIR-V backend does not route an unnarrowed LDG.CG load to buffer 0",
          "[shader][spirv][coherence]") {
    // GM_NARROW | GM_NARROW_NO_FALLBACK with native bounds: a load with a buffer list gets a
    // function of its own that searches only those buffers. A load without one must call the
    // function that searches all of them, whether or not it is coherent.
    const auto emitted_functions = [](bool coherent) {
        Fixture fixture;
        auto& info{fixture.program.info};
        info.used_storage_buffer_types = IR::Type::U32 | IR::Type::U32x2 | IR::Type::U32x4;
        info.storage_buffers_descriptors = {{0, 0x168, 1, false}, {0, 0x178, 1, false}};
        info.uses_global_memory = true;
        info.native_global_bounds = true;
        info.uses_int64 = true;
        info.used_constant_buffer_types = IR::Type::U32 | IR::Type::U32x2;
        info.constant_buffer_descriptors = {{0, 1}};
        info.uses_local_invocation_id = true;
        fixture.profile.support_int64 = true;
        fixture.profile.support_descriptor_aliasing = true;
        fixture.profile.unified_descriptor_binding = true;
        fixture.profile.global_memory_opt =
            Backend::SPIRV::GM_NARROW | Backend::SPIRV::GM_NARROW_NO_FALLBACK;
        IR::IREmitter ir{fixture.block};
        const IR::U64 address{
            ir.PackUint2x32(ir.CompositeConstruct(ir.LocalInvocationIdX(), ir.Imm32(0)))};
        static_cast<void>(ir.LoadGlobal32(address, coherent));
        static_cast<void>(ir.LoadGlobal64(address, coherent));
        static_cast<void>(ir.LoadGlobal128(address, coherent));
        return CountFunctions(Backend::SPIRV::EmitSPIRV(fixture.profile, fixture.program));
    };
    CHECK(emitted_functions(true) == emitted_functions(false));
}
