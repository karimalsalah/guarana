// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <string>
#include <catch2/catch_test_macros.hpp>
#ifndef SI_NO_CONVERSION
#define SI_NO_CONVERSION 1
#endif
#include <SimpleIni.h>
#include "common/fs/path_util.h"
#include "common/fs/profile_migration.h"
#include "frontend_common/profile_import.h"

namespace fs = std::filesystem;
namespace Migration = Common::FS::ProfileMigration;
using namespace FrontendCommon;

namespace {
struct Fixture {
    fs::path root = fs::temp_directory_path() /
                    ("lichii-import-config-" + std::to_string(std::random_device{}()));
    Fixture() {
        fs::create_directories(root);
    }
    ~Fixture() {
        std::error_code error;
        fs::remove_all(root, error);
    }
};
} // namespace

TEST_CASE("Imported paths are remapped by path component", "[migration]") {
    const auto old_root = fs::absolute("eden");
    const auto new_root = fs::absolute("lichii");
    CHECK(RemapImportedPath(old_root / "custom-nand", old_root, new_root) ==
          new_root / "custom-nand");
    CHECK(RemapImportedPath(old_root, old_root, new_root) == new_root);
    const auto external = old_root.parent_path() / "eden-other" / "save";
    CHECK(RemapImportedPath(external, old_root, new_root) == external);
    const auto escape = old_root / ".." / "external";
    CHECK(RemapImportedPath(escape, old_root, new_root) == escape);
    CHECK(RemapImportedPath({}, old_root, new_root).empty());
    CHECK(RemapImportedPath("relative", old_root, new_root) == "relative");
}

TEST_CASE("Move remaps every raw imported storage path before the source disappears",
          "[migration]") {
    Fixture fixture;
    const auto source = fixture.root / "eden";
    const auto destination = fixture.root / "lichii";
    const auto config = source / "config";
    fs::create_directories(config / "custom");
    constexpr std::array keys{"nand_directory", "save_directory", "sdmc_directory",
                              "load_directory", "tas_directory",  "dump_directory"};
    for (const auto name : {"qt-config.ini", "sdl2-config.ini", "custom/game.ini"}) {
        std::ofstream file{config / name};
        file << "[Data%20Storage]\n";
        for (const auto* key : keys) {
            fs::create_directories(source / key);
            file << key << "=\"" << (source / key).generic_string() << "\"\n";
        }
        fs::create_directories(source / "custom-screenshots");
        file << "[Screenshots]\nscreenshot_path=\""
             << (source / "custom-screenshots").generic_string() << "\"\n";
    }
    const auto state = fixture.root / "lichii.migration";
    Migration::RunImport(
        {{{source, destination}}, Migration::Strategy::Move, state, {}}, [&](const auto& staged) {
            RemapImportedConfig(staged.at(0).destination / "config", source, destination);
        });
    CHECK_FALSE(fs::exists(source));
    for (const auto name : {"qt-config.ini", "sdl2-config.ini", "custom/game.ini"}) {
        std::ifstream file{destination / "config" / name};
        const std::string contents{std::istreambuf_iterator<char>{file}, {}};
        CSimpleIniA ini;
        REQUIRE(ini.LoadData(contents) >= 0);
        for (const auto* key : keys) {
            const fs::path selected{ini.GetValue("Data%20Storage", key, "")};
            CHECK(selected == destination / key);
            CHECK(fs::is_directory(selected));
        }
        const fs::path screenshot{ini.GetValue("Screenshots", "screenshot_path", "")};
        CHECK(screenshot == destination / "custom-screenshots");
        CHECK(fs::is_directory(screenshot));
    }
    CHECK(Migration::GetState(state) == Migration::State::Complete);
}

#if !defined(_WIN32) && !defined(__ANDROID__)
namespace {
constexpr std::array<const char*, 3> xdg_names{"XDG_DATA_HOME", "XDG_CONFIG_HOME",
                                               "XDG_CACHE_HOME"};

// Points the XDG folders and the working directory into a fixture, and puts them back afterwards.
struct XdgHome {
    fs::path cwd = fs::current_path();
    std::array<std::optional<std::string>, 3> env;
    explicit XdgHome(const fs::path& root) {
        for (std::size_t i = 0; i < xdg_names.size(); ++i) {
            const auto* value = std::getenv(xdg_names[i]);
            env[i] = value ? std::optional<std::string>{value} : std::nullopt;
        }
        fs::current_path(root);
        setenv("XDG_DATA_HOME", (root / "data").c_str(), 1);
        setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
        setenv("XDG_CACHE_HOME", (root / "cache").c_str(), 1);
    }
    ~XdgHome() {
        fs::current_path(cwd);
        for (std::size_t i = 0; i < xdg_names.size(); ++i) {
            if (env[i]) {
                setenv(xdg_names[i], env[i]->c_str(), 1);
            } else {
                unsetenv(xdg_names[i]);
            }
        }
        Common::FS::SetAppDirectory("");
    }
};
} // namespace

TEST_CASE("CLI-first configuration is blocked and portable user stays untouched", "[migration]") {
    Fixture fixture;
    const XdgHome home{fixture.root};
    fs::create_directories(fixture.root / "data/eden");
    fs::create_directories(fixture.root / "config/lichii");
    Common::FS::SetAppDirectory("");
    CHECK(NeedsLegacyProfileImport());
    CHECK_THROWS(CheckLegacyProfileBeforeConfig());
    CHECK(fs::is_empty(fixture.root / "config/lichii"));
    Migration::MarkFresh(ProfileImportStateDirectory());
    CHECK_FALSE(NeedsLegacyProfileImport());
    CHECK_NOTHROW(CheckLegacyProfileBeforeConfig());

    fs::create_directories(fixture.root / "user");
    Common::FS::SetAppDirectory("");
    CHECK(Common::FS::IsPortableMode());
    CHECK(Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) == fixture.root / "user");
    CHECK_FALSE(NeedsLegacyProfileImport());
    CHECK_NOTHROW(CheckLegacyProfileBeforeConfig());
    CHECK(fs::is_empty(fixture.root / "user"));
}

TEST_CASE("An import settled before the rename is not asked again", "[migration]") {
    Fixture fixture;
    const XdgHome home{fixture.root};
    // A Lichii user after the rename copy: the old data next to the new folder, and the answer to
    // the import prompt still in the pre-rename state folder.
    fs::create_directories(fixture.root / "data/eden");
    fs::create_directories(fixture.root / "data/guarana");
    const auto old_state = fixture.root / "data/lichii.migration";
    fs::create_directories(old_state);
    const auto touch = [](const fs::path& path) { std::ofstream file{path}; };
    Common::FS::SetAppDirectory("");
    REQUIRE(Migration::GetState(ProfileImportStateDirectory()) == Migration::State::Unresolved);
    // Nothing settled yet anywhere: asked, as before.
    CHECK(NeedsLegacyProfileImport());

    SECTION("started fresh in Lichii") {
        touch(old_state / "fresh");
        CHECK_FALSE(NeedsLegacyProfileImport());
        CHECK_NOTHROW(CheckLegacyProfileBeforeConfig());
    }
    SECTION("imported in Lichii") {
        touch(old_state / "complete");
        CHECK_FALSE(NeedsLegacyProfileImport());
        CHECK_NOTHROW(CheckLegacyProfileBeforeConfig());
    }
    SECTION("an import left unfinished in Lichii still asks") {
        touch(old_state / "pending");
        CHECK(NeedsLegacyProfileImport());
    }
    // The old folder is only read, never written or removed.
    CHECK(fs::is_directory(old_state));
    CHECK_FALSE(fs::exists(ProfileImportStateDirectory()));
}
#endif
