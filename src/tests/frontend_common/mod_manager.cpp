// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "common/fs/path_util.h"
#include "frontend_common/mod_manager.h"

namespace {

namespace fs = std::filesystem;

constexpr u64 ProgramId = 0x0100000000010000;

// A load folder of its own, and a folder for the mods a player would import.
class ModTestDirs {
public:
    ModTestDirs() : previous{Common::FS::GetEdenPath(Common::FS::EdenPath::LoadDir)} {
        fs::create_directories(root / "load");
        fs::create_directories(root / "downloads");
        Common::FS::SetEdenPath(Common::FS::EdenPath::LoadDir, root / "load");
    }

    ~ModTestDirs() {
        Common::FS::SetEdenPath(Common::FS::EdenPath::LoadDir, previous);
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    ModTestDirs(const ModTestDirs&) = delete;
    ModTestDirs& operator=(const ModTestDirs&) = delete;

    // Writes one file of a mod to import and returns the mod's folder.
    fs::path Source(const std::string& mod, const std::string& file,
                    const std::string& text) const {
        Write(root / "downloads" / mod / file, text);
        return root / "downloads" / mod;
    }

    static fs::path Installed(const std::string& mod) {
        return FrontendCommon::GetModsDir(ProgramId) / mod;
    }

    static void Write(const fs::path& path, const std::string& text) {
        fs::create_directories(path.parent_path());
        std::ofstream file{path, std::ios::binary};
        file << text;
    }

    static std::string Read(const fs::path& path) {
        std::ifstream file{path, std::ios::binary};
        return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    }

    const fs::path root =
        fs::temp_directory_path() /
        ("mod-manager-test-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

private:
    const fs::path previous;
};

} // Anonymous namespace

TEST_CASE("A mod is installed with a mod.json that says where it came from", "[mod_manager]") {
    ModTestDirs dirs;
    const fs::path source = dirs.Source("Cool Mod", "romfs/a.txt", "new");

    CHECK(FrontendCommon::GetModsDir(ProgramId) == dirs.root / "load" / "0100000000010000");
    REQUIRE_FALSE(FrontendCommon::IsModInstalled(ProgramId, "Cool Mod"));
    REQUIRE(FrontendCommon::InstallMod(source, ProgramId, {.source = "cool-mod-v2.zip"}) ==
            FrontendCommon::Success);

    CHECK(FrontendCommon::IsModInstalled(ProgramId, "Cool Mod"));
    CHECK(ModTestDirs::Read(ModTestDirs::Installed("Cool Mod") / "romfs" / "a.txt") == "new");
    // Copied, not moved.
    CHECK(ModTestDirs::Read(source / "romfs" / "a.txt") == "new");
    // Nothing is left of the folder the copy was staged in.
    CHECK_FALSE(fs::exists(dirs.root / "load" / ".install-staging"));

    const auto info = FrontendCommon::ReadModInfo(ModTestDirs::Installed("Cool Mod"));
    REQUIRE(info);
    CHECK(info->name == "Cool Mod");
    CHECK(info->source == "cool-mod-v2.zip");
    // 2026-11-16T21:04:05Z
    REQUIRE(info->date.size() == 20);
    CHECK(info->date[4] == '-');
    CHECK(info->date[10] == 'T');
    CHECK(info->date.back() == 'Z');
    // The mod to import got no mod.json: only the installed copy does.
    CHECK_FALSE(fs::exists(source / "mod.json"));
}

TEST_CASE("A mod of the same name is replaced only when told to", "[mod_manager]") {
    ModTestDirs dirs;
    const fs::path installed = ModTestDirs::Installed("Cool Mod");
    REQUIRE(FrontendCommon::InstallMod(dirs.Source("Cool Mod", "romfs/a.txt", "old"),
                                       ProgramId) == FrontendCommon::Success);
    // A newer download of the mod, with another file.
    fs::remove_all(dirs.root / "downloads");
    const fs::path newer = dirs.Source("Cool Mod", "romfs/b.txt", "new");

    SECTION("Without a yes the installed mod stays as it is") {
        CHECK(FrontendCommon::InstallMod(newer, ProgramId) == FrontendCommon::Cancelled);
        CHECK(ModTestDirs::Read(installed / "romfs" / "a.txt") == "old");
        CHECK_FALSE(fs::exists(installed / "romfs" / "b.txt"));
    }

    SECTION("With a yes it is replaced as a whole, not merged") {
        CHECK(FrontendCommon::InstallMod(newer, ProgramId, {.replace_existing = true}) ==
              FrontendCommon::Success);
        CHECK_FALSE(fs::exists(installed / "romfs" / "a.txt"));
        CHECK(ModTestDirs::Read(installed / "romfs" / "b.txt") == "new");
        CHECK_FALSE(fs::exists(dirs.root / "load" / ".install-staging"));
    }

    SECTION("A copy that fails leaves the installed mod alone") {
        // The folder to import is gone by the time it is copied.
        const fs::path gone = dirs.root / "downloads" / "gone" / "Cool Mod";
        CHECK(FrontendCommon::InstallMod(gone, ProgramId, {.replace_existing = true}) ==
              FrontendCommon::Failed);
        CHECK(ModTestDirs::Read(installed / "romfs" / "a.txt") == "old");
        CHECK(FrontendCommon::ReadModInfo(installed).has_value());
        CHECK_FALSE(fs::exists(dirs.root / "load" / ".install-staging"));
    }

    SECTION("Installing a mod from its own folder changes nothing") {
        CHECK(FrontendCommon::InstallMod(installed, ProgramId, {.copy = false}) ==
              FrontendCommon::Success);
        CHECK(ModTestDirs::Read(installed / "romfs" / "a.txt") == "old");
    }
}

TEST_CASE("An install clears only what it may", "[mod_manager]") {
    ModTestDirs dirs;
    const fs::path load = dirs.root / "load";
    const fs::path staging = load / ".install-staging";

    SECTION("Files an earlier install left in the staging folder are not installed") {
        ModTestDirs::Write(staging / "new" / "romfs" / "stale.txt", "stale");
        REQUIRE(FrontendCommon::InstallMod(dirs.Source("Cool Mod", "romfs/a.txt", "new"),
                                           ProgramId) == FrontendCommon::Success);
        CHECK(ModTestDirs::Read(ModTestDirs::Installed("Cool Mod") / "romfs" / "a.txt") == "new");
        CHECK_FALSE(fs::exists(ModTestDirs::Installed("Cool Mod") / "romfs" / "stale.txt"));
        CHECK_FALSE(fs::exists(staging));
    }

    SECTION("A mod an interrupted install moved aside is kept") {
        // Cut off between moving the installed mod aside and putting the new one in its place,
        // an install leaves the only copy of the installed mod in the staging folder.
        ModTestDirs::Write(staging / "replaced" / "romfs" / "a.txt", "old");
        REQUIRE(FrontendCommon::InstallMod(dirs.Source("Cool Mod", "romfs/b.txt", "new"),
                                           ProgramId) == FrontendCommon::Success);
        CHECK(ModTestDirs::Read(ModTestDirs::Installed("Cool Mod") / "romfs" / "b.txt") == "new");
        CHECK_FALSE(fs::exists(staging));

        int kept = 0;
        for (const auto& entry : fs::directory_iterator(load)) {
            if (entry.path().filename().string().starts_with(".install-kept-")) {
                ++kept;
                CHECK(ModTestDirs::Read(entry.path() / "replaced" / "romfs" / "a.txt") == "old");
            }
        }
        CHECK(kept == 1);
    }
}

TEST_CASE("A mod's name names one folder in the folder it goes in", "[mod_manager]") {
    for (const char* name : {"Cool Mod", ".hidden", "Mod v1.2"}) {
        CAPTURE(name);
        CHECK(FrontendCommon::IsModName(name));
    }
    for (const char* name : {"", ".", "..", "a/b", "a\\b", "/", "\\"}) {
        CAPTURE(name);
        CHECK_FALSE(FrontendCommon::IsModName(name));
    }
#ifdef _WIN32
    // Joined to a folder on drive C:, "D:" and "D:." are drive D's current folder, and "C:" is the
    // folder itself. Windows drops the periods and spaces a name ends with, so "...", " " and ". ."
    // name the folder itself too, and "Mod." names "Mod".
    for (const char* name : {"D:", "D:.", "C:", "a:b", "...", " ", ". .", "Mod.", "Mod "}) {
        CAPTURE(name);
        CHECK_FALSE(FrontendCommon::IsModName(name));
    }
#else
    // Elsewhere these are names like any other.
    for (const char* name : {"D:", "a:b", "Mod.", "Mod "}) {
        CAPTURE(name);
        CHECK(FrontendCommon::IsModName(name));
    }
#endif
}

TEST_CASE("A mod is installed under the name it is given", "[mod_manager]") {
    ModTestDirs dirs;
    const fs::path source = dirs.Source("folder", "exefs/x.pchtxt", "patch");

    CHECK(FrontendCommon::InstalledModName(fs::path{"a"} / "b" / "Cool Mod") == "Cool Mod");
    CHECK(FrontendCommon::InstalledModName(fs::path{"a"} / "b" / "Cool Mod" / "") == "Cool Mod");
    CHECK(FrontendCommon::InstalledModName(source, {.name = "Better"}) == "Better");

    SECTION("A given name wins over the folder's") {
        REQUIRE(FrontendCommon::InstallMod(source, ProgramId, {.name = "Better Name"}) ==
                FrontendCommon::Success);
        CHECK(ModTestDirs::Read(ModTestDirs::Installed("Better Name") / "exefs" / "x.pchtxt") ==
              "patch");
        CHECK_FALSE(fs::exists(ModTestDirs::Installed("folder")));
        const auto info = FrontendCommon::ReadModInfo(ModTestDirs::Installed("Better Name"));
        REQUIRE(info);
        CHECK(info->name == "Better Name");
        // Without a source given, the folder it was imported from.
        CHECK(info->source == "folder");
    }

    SECTION("A name that is not the name of one folder is refused") {
        for (const char* name : {"..", ".", "a/b", "a\\b"}) {
            CAPTURE(name);
            CHECK(FrontendCommon::InstallMod(source, ProgramId, {.name = name}) ==
                  FrontendCommon::Failed);
            CHECK_FALSE(FrontendCommon::IsModInstalled(ProgramId, name));
        }
        // Nothing was written for the game, or beside its folder.
        CHECK_FALSE(fs::exists(FrontendCommon::GetModsDir(ProgramId)));
        CHECK_FALSE(fs::exists(dirs.root / "load" / "b"));
    }

    SECTION("A move removes the folder it was imported from") {
        REQUIRE(FrontendCommon::InstallMod(source, ProgramId, {.copy = false}) ==
                FrontendCommon::Success);
        CHECK_FALSE(fs::exists(source));
        CHECK(fs::exists(ModTestDirs::Installed("folder") / "exefs" / "x.pchtxt"));
    }
}

TEST_CASE("mod.json: ours is rewritten, a mod's own is kept", "[mod_manager]") {
    ModTestDirs dirs;
    const fs::path dir = dirs.root / "mod";
    fs::create_directories(dir);

    // Quotes, a backslash and a letter outside ASCII (UTF-8).
    const FrontendCommon::ModInfo info{.name = "N \"quoted\" \\ \xC3\xA9",
                                       .source = "s.zip",
                                       .date = "2026-11-16T21:04:05Z"};
    REQUIRE(FrontendCommon::WriteModInfo(dir, info));
    CHECK(FrontendCommon::ReadModInfo(dir) == info);

    const FrontendCommon::ModInfo newer{.name = "N", .source = "t.zip", .date = "later"};
    REQUIRE(FrontendCommon::WriteModInfo(dir, newer));
    CHECK(FrontendCommon::ReadModInfo(dir) == newer);

    // A file with a field we do not write belongs to the mod.
    const std::string theirs = R"({"name": "Theirs", "version": 3})";
    ModTestDirs::Write(dir / "mod.json", theirs);
    CHECK_FALSE(FrontendCommon::WriteModInfo(dir, info));
    CHECK(ModTestDirs::Read(dir / "mod.json") == theirs);
    const auto read = FrontendCommon::ReadModInfo(dir);
    REQUIRE(read);
    CHECK(read->name == "Theirs");
    CHECK(read->source.empty());
    CHECK(read->date.empty());

    // So does one that is not JSON, and it reads as nothing.
    ModTestDirs::Write(dir / "mod.json", "not json");
    CHECK_FALSE(FrontendCommon::WriteModInfo(dir, info));
    CHECK(ModTestDirs::Read(dir / "mod.json") == "not json");
    CHECK_FALSE(FrontendCommon::ReadModInfo(dir));

    CHECK_FALSE(FrontendCommon::ReadModInfo(dirs.root / "nowhere"));
}

TEST_CASE("The date in mod.json is UTC, to the second", "[mod_manager]") {
    using std::chrono::system_clock;
    CHECK(FrontendCommon::FormatModDate(system_clock::time_point{}) == "1970-01-01T00:00:00Z");
    CHECK(FrontendCommon::FormatModDate(
              system_clock::time_point{std::chrono::seconds{1794863045}}) ==
          "2026-11-16T21:04:05Z");
    // A leap day, and the last second of a day.
    CHECK(FrontendCommon::FormatModDate(
              system_clock::time_point{std::chrono::seconds{951868799}}) ==
          "2000-02-29T23:59:59Z");
    // Parts of a second are dropped, also before 1970.
    CHECK(FrontendCommon::FormatModDate(
              system_clock::time_point{std::chrono::milliseconds{1794863045999}}) ==
          "2026-11-16T21:04:05Z");
    CHECK(FrontendCommon::FormatModDate(system_clock::time_point{std::chrono::milliseconds{-1}}) ==
          "1969-12-31T23:59:59Z");
}

TEST_CASE("Files that more than one mod replaces are found, in load order", "[mod_manager]") {
    using Conflict = FrontendCommon::ModFileConflict;
    ModTestDirs dirs;
    const fs::path mods = FrontendCommon::GetModsDir(ProgramId);
    const auto write = [&mods](const char* mod, const char* file) {
        ModTestDirs::Write(mods / mod / file, mod);
    };
    write("B mod", "romfs/Model/Link.bfres");
    write("A mod", "romfs/Model/Link.bfres");
    write("A mod", "romfs/only-a.txt");
    // In both of a mod's folders: the mod counts once.
    write("C mod", "romfslite/Model/Link.bfres");
    write("C mod", "romfs/Model/Link.bfres");
    // ExeFS files are not files of the RomFS.
    write("B mod", "exefs/main");
    write("C mod", "exefs/main");
    write("D mod", "romfs/UI/icon.png");
    write("B mod", "romfs/UI/icon.png");
    // The loader takes the folder whatever its case.
    write("E mod", "RomFS/UI/icon.png");
    // A file in the game's mod folder is not a mod.
    ModTestDirs::Write(mods / "cheat_x.txt", "cheat");

    CHECK(FrontendCommon::FindModFileConflicts(mods) ==
          std::vector<Conflict>{
              {.path = "Model/Link.bfres", .mods = {"A mod", "B mod", "C mod"}},
              {.path = "UI/icon.png", .mods = {"B mod", "D mod", "E mod"}},
          });

    // A mod that is turned off takes no part.
    const std::vector<std::string> without_b{"B mod"};
    CHECK(FrontendCommon::FindModFileConflicts(mods, without_b) ==
          std::vector<Conflict>{
              {.path = "Model/Link.bfres", .mods = {"A mod", "C mod"}},
              {.path = "UI/icon.png", .mods = {"D mod", "E mod"}},
          });
    const std::vector<std::string> only_c_and_d{"A mod", "B mod", "E mod"};
    CHECK(FrontendCommon::FindModFileConflicts(mods, only_c_and_d).empty());

    CHECK(FrontendCommon::FindModFileConflicts(dirs.root / "nowhere").empty());
}
