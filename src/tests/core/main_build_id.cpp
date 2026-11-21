// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/common_funcs.h"
#include "common/hex_util.h"
#include "core/core.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/loader/deconstructed_rom_directory.h"
#include "core/loader/nso.h"
#include "tests/core/system_test_context.h"

namespace {

using BuildId = std::array<u8, 0x20>;

constexpr BuildId MainBuildId{0xE0, 0xF3, 0xCC, 0xFA, 0xF5, 0x00, 0x65, 0xAE, 0x4E, 0xA0,
                              0x1E, 0xB5, 0xAE, 0xB1, 0x3B, 0x1A, 0x45, 0xDA, 0x33, 0x48};
constexpr BuildId SdkBuildId{0x5D, 0x4B};

std::vector<u8> NsoHeader(const BuildId& build_id) {
    Loader::NSOHeader header{};
    header.magic = Common::MakeMagic('N', 'S', 'O', '0');
    header.build_id = build_id;
    std::vector<u8> bytes(sizeof(header));
    std::memcpy(bytes.data(), &header, sizeof(header));
    return bytes;
}

FileSys::VirtualFile File(std::vector<u8> bytes, std::string name) {
    return std::make_shared<FileSys::VectorVfsFile>(std::move(bytes), std::move(name));
}

FileSys::VirtualDir ExeFS(std::vector<FileSys::VirtualFile> files) {
    return std::make_shared<FileSys::VectorVfsDirectory>(std::move(files),
                                                         std::vector<FileSys::VirtualDir>{},
                                                         "exefs");
}

/// A loader with nothing but what every loader must have.
class BareLoader final : public Loader::AppLoader {
public:
    BareLoader() : AppLoader(nullptr) {}

    Loader::FileType GetFileType() const override {
        return Loader::FileType::Unknown;
    }

    LoadResult Load(Kernel::KProcess&, Core::System&) override {
        return {Loader::ResultStatus::ErrorNotImplemented, {}};
    }
};

} // Anonymous namespace

TEST_CASE("Loaders read the build id of main from its header alone", "[core][loader]") {
    ScopedSystemTestContext environment;
    Core::System system;
    BuildId build_id{};

    SECTION("An NSO") {
        auto bytes = NsoHeader(MainBuildId);
        // Segments that are never read.
        bytes.resize(bytes.size() + 0x1000, 0xCC);
        Loader::AppLoader_NSO loader{File(std::move(bytes), "main")};
        REQUIRE(loader.ReadMainBuildId(system, build_id) == Loader::ResultStatus::Success);
        CHECK(build_id == MainBuildId);
        CHECK(loader.ReadMainBuildIdHex(system) == Common::HexToString(MainBuildId));
    }

    SECTION("A file shorter than the header, or not an NSO") {
        auto truncated = NsoHeader(MainBuildId);
        truncated.resize(sizeof(Loader::NSOHeader) - 1);
        auto other_magic = NsoHeader(MainBuildId);
        other_magic[3] = 'X';
        for (auto& bytes : {truncated, other_magic}) {
            Loader::AppLoader_NSO loader{File(bytes, "main")};
            CHECK(loader.ReadMainBuildId(system, build_id) ==
                  Loader::ResultStatus::ErrorLoadingNSO);
            CHECK_FALSE(loader.ReadMainBuildIdHex(system).has_value());
        }
    }

    SECTION("A loose ExeFS, which loads without updates or mods") {
        Loader::AppLoader_DeconstructedRomDirectory loader{
            ExeFS({File(NsoHeader(SdkBuildId), "sdk"), File(NsoHeader(MainBuildId), "main")})};
        REQUIRE(loader.ReadMainBuildId(system, build_id) == Loader::ResultStatus::Success);
        CHECK(build_id == MainBuildId);

        Loader::AppLoader_DeconstructedRomDirectory without_main{
            ExeFS({File(NsoHeader(SdkBuildId), "sdk")})};
        CHECK(without_main.ReadMainBuildId(system, build_id) == Loader::ResultStatus::ErrorNoExeFS);
    }

    SECTION("A loader that cannot tell says so") {
        BareLoader loader;
        CHECK(loader.ReadMainBuildId(system, build_id) == Loader::ResultStatus::ErrorNotImplemented);
        CHECK_FALSE(loader.ReadMainBuildIdHex(system).has_value());
    }
}
