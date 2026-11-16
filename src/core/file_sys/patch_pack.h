// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "common/common_types.h"
#include "core/file_sys/patch_manager.h"

// Built-in patches as data: patch packs, JSON files shipped next to the program
// (dist/patch-packs, docs/patch-packs.md). PatchManager applies and lists their patches as it does
// the built-ins compiled into patch_manager.cpp, after them.
namespace FileSys {

// A run of bytes a pack patch writes over its module.
struct PatchPackRecord {
    size_t offset{}; // From the module's start, as main+0x... in the log
    std::vector<u8> bytes;
    // In the executable padding after the module's text, which must be zero. Otherwise the record
    // writes over the game's code, whose original_size bytes must hash to original_sha256.
    bool cave{};
    std::array<u8, 32> original_sha256{};
    size_t original_size{};

    bool operator==(const PatchPackRecord&) const = default;
};

struct PatchPackPatch {
    std::string name; // Also its [DisabledAddOns] key: never rename a patch that has shipped
    std::string description;
    BuiltinPatchLevel level{BuiltinPatchLevel::Standard};
    std::string group;
    std::string replaces;
    bool off_by_default{};
    size_t state_offset{};
    size_t state_size{};
    std::vector<PatchPackRecord> records;

    bool operator==(const PatchPackPatch&) const = default;
};

struct PatchPack {
    // The file name: the pack's id, and how its messages start. A file in the folder replaces the
    // compiled-in copy of the same name.
    std::string source;
    bool compiled_in{}; // Read from the copy compiled into the program, not from the folder
    u64 title_id{};
    std::string module;
    std::string build_id; // Upper case without trailing zeros, as PatchNSO compares it
    std::vector<PatchPackPatch> patches;
};

// Reads and checks one pack. When anything in it is wrong, returns nothing and adds one line per
// problem to `errors`: "<source>: <field>: <problem>".
[[nodiscard]] std::optional<PatchPack> ParsePatchPack(std::string_view text,
                                                      std::string_view source,
                                                      std::vector<std::string>& errors);

struct PatchPackLoad {
    std::vector<PatchPack> packs;
    std::vector<std::string> errors;
};

// A pack compiled into the program: a file of dist/patch-packs as it was when the program was
// built (CMakeModules/EmbedPatchPacks.cmake writes the list).
struct EmbeddedPatchPack {
    std::string_view file_name;
    std::span<const u8> text;
};

// The packs compiled into the program, in file name order. A program with no folder of packs
// beside it (Android, guarana-cli inside a macOS bundle) still has these.
[[nodiscard]] std::span<const EmbeddedPatchPack> EmbeddedPatchPacks();

// The *.json packs in `directory`, and each of `compiled_in` whose file name no file there has:
// a file in the folder replaces the compiled-in copy of its name. When the folder's copy cannot be
// used, its problems are reported and the compiled-in copy is used instead. The packs come in file
// name order, whichever copy each is read from. A pack with any problem is left out whole. A
// folder that is not there holds no packs, which is not a problem.
[[nodiscard]] PatchPackLoad LoadPatchPacks(const std::filesystem::path& directory,
                                           std::span<const EmbeddedPatchPack> compiled_in = {});

// Where the folder of packs shipped with the program is: patch-packs next to the executable, or
// in Contents/Resources of a macOS bundle. Empty where the executable cannot be found.
[[nodiscard]] std::filesystem::path ShippedPatchPackDirectory();

[[nodiscard]] bool PatchPackSha256(std::span<const u8> data, std::array<u8, 32>& digest);

// Implemented in patch_manager.cpp, which merges the packs with the built-ins compiled in.

// Why a pack PatchManager uses was left out: its own problems, a missing folder, and a patch name
// another built-in of the same title already has. Empty when every pack is in use.
[[nodiscard]] std::vector<std::string> PatchPackErrors();

// For tests: PatchManager uses `packs` instead of the shipped ones, until this is called with
// nothing.
void UsePatchPacksForTesting(std::optional<std::vector<PatchPack>> packs);

} // namespace FileSys
