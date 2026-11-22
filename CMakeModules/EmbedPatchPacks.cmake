# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# Writes the source that compiles the patch packs into core: FileSys::EmbeddedPatchPacks
# (src/core/file_sys/patch_pack.h, docs/patch-packs.md). src/core/CMakeLists.txt runs it at build
# time:
#
#   cmake -P EmbedPatchPacks.cmake <list.cmake> <output.cpp>
#
# <list.cmake> sets patch_pack_files to the pack files' paths. Each pack becomes an array of its
# bytes, so any text compiles, whatever its length or characters.

set(list_file "${CMAKE_ARGV3}")
set(output "${CMAKE_ARGV4}")
if (NOT list_file OR NOT output)
    message(FATAL_ERROR "Usage: cmake -P EmbedPatchPacks.cmake <list.cmake> <output.cpp>")
endif()

set(patch_pack_files "")
include("${list_file}")
list(SORT patch_pack_files)

# Sixteen bytes to a line.
string(REPEAT "0x[0-9a-f][0-9a-f]," 16 line_of_bytes)

set(arrays "")
set(entries "")
set(count 0)
foreach(pack IN LISTS patch_pack_files)
    get_filename_component(name "${pack}" NAME)
    # The name goes into the source as it is, and it is the pack's id at run time.
    if (NOT name MATCHES "^[0-9A-Za-z][0-9A-Za-z._-]*\\.json$")
        message(FATAL_ERROR "${pack}: a patch pack's file name may hold only letters, digits, "
                            "'.', '_' and '-', and must end in .json")
    endif()
    file(READ "${pack}" hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
    string(REGEX REPLACE "(${line_of_bytes})" "\\1\n    " bytes "${bytes}")
    # A zero byte ends every array, so an empty file still makes one; the span leaves it out.
    string(APPEND arrays "// ${name}\nconstexpr u8 Pack${count}[] = {\n    ${bytes}0x00};\n\n")
    string(APPEND entries "    EmbeddedPatchPack{\"${name}\", "
                          "std::span<const u8>{Pack${count}, sizeof(Pack${count}) - 1}},\n")
    math(EXPR count "${count} + 1")
endforeach()

if (count EQUAL 0)
    set(packs "constexpr std::array<EmbeddedPatchPack, 0> Packs{};\n")
else()
    set(packs "constexpr std::array<EmbeddedPatchPack, ${count}> Packs{{\n${entries}}};\n")
endif()

file(WRITE "${output}" "// Written by CMakeModules/EmbedPatchPacks.cmake from dist/patch-packs: do not edit.

#include <array>
#include <span>

#include \"core/file_sys/patch_pack.h\"

namespace FileSys {
namespace {

${arrays}${packs}
} // Anonymous namespace

std::span<const EmbeddedPatchPack> EmbeddedPatchPacks() {
    return Packs;
}

} // namespace FileSys
")
