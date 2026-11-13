// SPDX-FileCopyrightText: Copyright 2026 Guarana contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/fs/data_dir_migration.h"

namespace {
namespace fs = std::filesystem;
namespace M = Common::FS::DataDirMigration;

struct Fixture {
    fs::path base = fs::temp_directory_path() /
                    ("guarana-dir-migration-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::path legacy = base / "lichii";
    fs::path current = base / "guarana";

    Fixture() {
        fs::create_directories(base);
    }
    ~Fixture() {
        std::error_code ec;
        fs::remove_all(base, ec);
    }

    void MakeLegacy() const {
        fs::create_directories(legacy / "keys");
        fs::create_directories(legacy / "nand" / "user");
        Write(legacy / "keys" / "prod.keys", "keys");
        Write(legacy / "nand" / "user" / "save.bin", std::string(5000, 's'));
        Write(legacy / "nx2_switches.txt", "EDEN_NX2_X=1\n");
        Write(legacy / "config" / "qt-config.ini", "[a]\n");
    }

    static void Write(const fs::path& path, const std::string& text) {
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << text;
    }
    static std::string Read(const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }
    bool NoStagingLeft() const {
        for (const auto& entry : fs::directory_iterator(base)) {
            if (entry.path().filename().string().find(".migrating-") != std::string::npos) {
                return false;
            }
        }
        return true;
    }
};
} // namespace

TEST_CASE("Data folder decision depends only on which folders exist", "[fs][guarana]") {
    using D = M::Decision;
    // (new exists, old exists)
    CHECK(M::Decide(true, true) == D::UseCurrent); // never copy over an existing guarana folder
    CHECK(M::Decide(true, false) == D::UseCurrent);
    CHECK(M::Decide(false, true) == D::CopyFromLegacy);
    CHECK(M::Decide(false, false) == D::StartFresh);
}

TEST_CASE("A failed copy keeps this session on the old folder", "[fs][guarana]") {
    CHECK(M::OutcomeAfterCopy(true) == M::Outcome::UseCurrent);
    CHECK(M::OutcomeAfterCopy(false) == M::Outcome::UseLegacy);
}

TEST_CASE("Old data is copied once and left untouched", "[fs][guarana]") {
    Fixture f;
    f.MakeLegacy();

    const auto result = M::Resolve({{f.legacy, f.current}});
    CHECK(result.decision == M::Decision::CopyFromLegacy);
    CHECK(result.outcome == M::Outcome::UseCurrent);
    CHECK_FALSE(result.warning);
    CHECK_FALSE(result.message.empty());
    CHECK(result.message.find("4 files") != std::string::npos);

    CHECK(Fixture::Read(f.current / "keys" / "prod.keys") == "keys");
    CHECK(Fixture::Read(f.current / "nand" / "user" / "save.bin").size() == 5000);
    CHECK(Fixture::Read(f.current / "nx2_switches.txt") == "EDEN_NX2_X=1\n");
    CHECK(Fixture::Read(f.current / "config" / "qt-config.ini") == "[a]\n");
    // Old folder is complete and unchanged
    CHECK(Fixture::Read(f.legacy / "keys" / "prod.keys") == "keys");
    CHECK(Fixture::Read(f.legacy / "nand" / "user" / "save.bin").size() == 5000);
    CHECK(f.NoStagingLeft());

    SECTION("a second start does nothing") {
        Fixture::Write(f.current / "keys" / "prod.keys", "edited in guarana");
        const auto again = M::Resolve({{f.legacy, f.current}});
        CHECK(again.decision == M::Decision::UseCurrent);
        CHECK(again.outcome == M::Outcome::UseCurrent);
        CHECK(again.message.empty());
        CHECK(Fixture::Read(f.current / "keys" / "prod.keys") == "edited in guarana");
        CHECK(Fixture::Read(f.legacy / "keys" / "prod.keys") == "keys");
    }
}

TEST_CASE("An existing guarana folder is never overwritten", "[fs][guarana]") {
    Fixture f;
    f.MakeLegacy();
    Fixture::Write(f.current / "marker.txt", "mine");

    const auto result = M::Resolve({{f.legacy, f.current}});
    CHECK(result.decision == M::Decision::UseCurrent);
    CHECK(result.message.empty());
    CHECK(fs::exists(f.current / "marker.txt"));
    CHECK_FALSE(fs::exists(f.current / "keys"));
}

TEST_CASE("Neither folder existing starts fresh without creating anything", "[fs][guarana]") {
    Fixture f;
    const auto result = M::Resolve({{f.legacy, f.current}});
    CHECK(result.decision == M::Decision::StartFresh);
    CHECK(result.outcome == M::Outcome::UseCurrent);
    CHECK(result.message.empty());
    CHECK_FALSE(fs::exists(f.current));
    CHECK_FALSE(fs::exists(f.legacy));
}

TEST_CASE("A copy that fails half way leaves no new folder and the old one intact",
          "[fs][guarana]") {
    Fixture f;
    f.MakeLegacy();

    const M::CopyFunction failing = [](const fs::path&, const fs::path& to, M::CopyStats&,
                                       std::string& error) {
        // Leave a partial staging folder behind, as a real copy that ran out of disk would
        Fixture::Write(to / "keys" / "prod.keys", "partial");
        error = "disk full";
        return false;
    };
    const auto result = M::Resolve({{f.legacy, f.current}}, failing);
    CHECK(result.decision == M::Decision::CopyFromLegacy);
    CHECK(result.outcome == M::Outcome::UseLegacy);
    CHECK(result.warning);
    CHECK(result.message.find("disk full") != std::string::npos);
    CHECK_FALSE(fs::exists(f.current));
    CHECK(f.NoStagingLeft());
    CHECK(Fixture::Read(f.legacy / "keys" / "prod.keys") == "keys");
    CHECK(Fixture::Read(f.legacy / "nand" / "user" / "save.bin").size() == 5000);

    SECTION("the next start tries again and can succeed") {
        const auto retry = M::Resolve({{f.legacy, f.current}});
        CHECK(retry.outcome == M::Outcome::UseCurrent);
        CHECK(Fixture::Read(f.current / "keys" / "prod.keys") == "keys");
    }
}

TEST_CASE("A copy that throws is handled like one that fails", "[fs][guarana]") {
    Fixture f;
    f.MakeLegacy();
    const M::CopyFunction throwing = [](const fs::path&, const fs::path&, M::CopyStats&,
                                        std::string&) -> bool { throw std::runtime_error("boom"); };
    const auto result = M::Resolve({{f.legacy, f.current}}, throwing);
    CHECK(result.outcome == M::Outcome::UseLegacy);
    CHECK_FALSE(fs::exists(f.current));
    CHECK(f.NoStagingLeft());
}

TEST_CASE("Config and cache folders are copied together with the data folder", "[fs][guarana]") {
    Fixture f;
    f.MakeLegacy();
    const auto old_config = f.base / "config-lichii";
    const auto new_config = f.base / "config-guarana";
    const auto old_cache = f.base / "cache-lichii";
    const auto new_cache = f.base / "cache-guarana";
    Fixture::Write(old_config / "qt-config.ini", "cfg");
    Fixture::Write(old_cache / "shader" / "a.bin", "shader");

    SECTION("all succeed") {
        const auto result =
            M::Resolve({{f.legacy, f.current}, {old_config, new_config}, {old_cache, new_cache}});
        CHECK(result.outcome == M::Outcome::UseCurrent);
        CHECK(Fixture::Read(new_config / "qt-config.ini") == "cfg");
        CHECK(Fixture::Read(new_cache / "shader" / "a.bin") == "shader");
        CHECK(f.NoStagingLeft());
    }

    SECTION("a failure in the last folder removes the folders already copied") {
        int calls = 0;
        const M::CopyFunction fail_third = [&](const fs::path& from, const fs::path& to,
                                               M::CopyStats& stats, std::string& error) {
            if (++calls == 3) {
                error = "cache locked";
                return false;
            }
            return M::CopyTree(from, to, stats, error);
        };
        const auto result = M::Resolve(
            {{f.legacy, f.current}, {old_config, new_config}, {old_cache, new_cache}}, fail_third);
        CHECK(result.outcome == M::Outcome::UseLegacy);
        CHECK_FALSE(fs::exists(f.current));
        CHECK_FALSE(fs::exists(new_config));
        CHECK_FALSE(fs::exists(new_cache));
        CHECK(f.NoStagingLeft());
        CHECK(fs::exists(old_config / "qt-config.ini"));
        CHECK(fs::exists(old_cache / "shader" / "a.bin"));
    }

    SECTION("a secondary folder that does not exist is skipped") {
        fs::remove_all(old_cache);
        const auto result =
            M::Resolve({{f.legacy, f.current}, {old_config, new_config}, {old_cache, new_cache}});
        CHECK(result.outcome == M::Outcome::UseCurrent);
        CHECK(fs::exists(new_config));
        CHECK_FALSE(fs::exists(new_cache));
    }
}

TEST_CASE("Copied configs point at the new folder", "[fs][guarana]") {
    const fs::path old_root = fs::path("C:/Users/A/AppData/Roaming/lichii");
    const fs::path new_root = fs::path("C:/Users/A/AppData/Roaming/guarana");
    const std::vector<M::Root> roots{{old_root, new_root}};

    SECTION("data folders inside the old folder move, everything else stays") {
        const std::string ini = "[Data%20Storage]\r\n"
                                "nand_directory=C:/Users/A/AppData/Roaming/lichii/nand\r\n"
                                "sdmc_directory=C:/Users/A/AppData/Roaming/lichii/sdmc/\r\n"
                                "dump_directory=D:/Games/dump\r\n"
                                "load_directory=C:/Users/A/AppData/Roaming/lichii2/load\r\n"
                                "save_directory=\r\n";
        CHECK(M::RewriteIniPaths(ini, roots) ==
              "[Data%20Storage]\r\n"
              "nand_directory=C:/Users/A/AppData/Roaming/guarana/nand\r\n"
              "sdmc_directory=C:/Users/A/AppData/Roaming/guarana/sdmc/\r\n"
              "dump_directory=D:/Games/dump\r\n"
              "load_directory=C:/Users/A/AppData/Roaming/lichii2/load\r\n"
              "save_directory=\r\n");
    }

    SECTION("backslashes, Qt's doubled backslashes and quotes") {
        CHECK(M::RewriteIniPaths("a=C:\\Users\\A\\AppData\\Roaming\\lichii\\nand\n", roots) ==
              "a=C:/Users/A/AppData/Roaming/guarana/nand\n");
        CHECK(M::RewriteIniPaths("a=C:\\\\Users\\\\A\\\\AppData\\\\Roaming\\\\lichii\n", roots) ==
              "a=C:/Users/A/AppData/Roaming/guarana\n");
        CHECK(M::RewriteIniPaths("a=\"C:/Users/A/AppData/Roaming/lichii/x, y\"", roots) ==
              "a=\"C:/Users/A/AppData/Roaming/guarana/x, y\"");
    }

    SECTION("text without old paths is returned byte for byte") {
        const std::string ini =
            "[UI]\nwhats_new_version=1\n\n; nand_directory=C:/Users/A/AppData/Roaming/lichii\n";
        CHECK(M::RewriteIniPaths(ini, roots) == ini);
    }
}

TEST_CASE("A migrated config uses the new folder", "[fs][guarana]") {
    Fixture f;
    f.MakeLegacy();
    const auto slashed = [](const fs::path& p) {
        auto s = p.generic_string();
        return s;
    };
    Fixture::Write(f.legacy / "config" / "qt-config.ini",
                   "[Data%20Storage]\nnand_directory=" + slashed(f.legacy / "nand") + "\n");
    Fixture::Write(f.legacy / "config" / "custom" / "0400486024DEC000.ini",
                   "[Core]\nuse_multi_core=true\n");

    const auto result = M::Resolve({{f.legacy, f.current}});
    REQUIRE(result.outcome == M::Outcome::UseCurrent);
    CHECK(Fixture::Read(f.current / "config" / "qt-config.ini") ==
          "[Data%20Storage]\nnand_directory=" + slashed(f.current / "nand") + "\n");
    CHECK(Fixture::Read(f.current / "config" / "custom" / "0400486024DEC000.ini") ==
          "[Core]\nuse_multi_core=true\n");
    // The old folder keeps its own paths.
    CHECK(Fixture::Read(f.legacy / "config" / "qt-config.ini") ==
          "[Data%20Storage]\nnand_directory=" + slashed(f.legacy / "nand") + "\n");
    CHECK(f.NoStagingLeft());
}
