// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "frontend_common/problem_report.h"

using namespace FrontendCommon::ProblemReport;

TEST_CASE("Problem reports remove the home folder, user and computer names",
          "[frontend_common][problem_report]") {
    const std::vector<std::string> users{"Yousof"};
    const std::vector<std::string> hosts{"DESKTOP-4UD89UT"};
    const auto redactions = MakeRedactions("C:/Users/Yousof/", users, hosts);

    CHECK(Scrub("C:\\Users\\Yousof\\AppData\\Roaming\\lichii\\log", redactions) ==
          "<home>\\AppData\\Roaming\\lichii\\log");
    CHECK(Scrub("dir c:/users/yousof/x", redactions) == "dir <home>/x");
    CHECK(Scrub("\"C:\\\\Users\\\\Yousof\\\\x\"", redactions) == "\"<home>\\\\x\"");
    CHECK(Scrub("Yousof on DESKTOP-4UD89UT.", redactions) == "<user> on <host>.");
    // Only where the name stands alone.
    CHECK(Scrub("Yousofs, xYousof and C:\\Users\\Yousof2", redactions) ==
          "Yousofs, xYousof and C:\\Users\\Yousof2");
    CHECK(Scrub("Users", redactions) == "Users");

    const std::vector<std::string> generic{"user", "ab"};
    CHECK(MakeRedactions("", generic, {}).empty());
    CHECK(MakeRedactions("C:", {}, {}).empty());
}

TEST_CASE("Problem report links are percent-encoded", "[frontend_common][problem_report]") {
    CHECK(PercentEncode("a b&c=d\n#+\xC3\xA9-._~") == "a%20b%26c%3Dd%0A%23%2B%C3%A9-._~");

    const std::string base = "https://example.com/new?template=game_bug.yml";
    CHECK(IssueUrl(base, {{"title", "Hi there", ""}}, 7000) == base + "&title=Hi%20there");
}

TEST_CASE("Problem report links are cut to fit", "[frontend_common][problem_report]") {
    const std::string base = "https://example.com/new?template=game_bug.yml";
    std::string logs;
    for (int i = 0; i < 400; ++i) {
        logs += fmt::format("line {}\n", i);
    }
    const std::string url =
        IssueUrl(base, {{"title", "T", ""}, {"logs", logs, "(more in the zip)"}}, 1000);
    CHECK(url.size() <= 1000);
    CHECK(url.starts_with(base + "&title=T&logs=line%200%0Aline%201%0A"));
    // Cut at a line end, then the note.
    CHECK(url.ends_with("%0A%28more%20in%20the%20zip%29"));
    CHECK(url.find("%0A%0A") == std::string::npos);

    // A cut never splits a UTF-8 sequence.
    std::string accents;
    for (int i = 0; i < 300; ++i) {
        accents += "\xC3\xA9";
    }
    const std::string cut = IssueUrl(base, {{"steps", accents, "(cut)"}}, 400);
    CHECK(cut.size() <= 400);
    for (auto at = cut.find("%C3"); at != std::string::npos; at = cut.find("%C3", at + 1)) {
        CHECK(cut.substr(at + 3, 3) == "%A9");
    }
    CHECK(cut.ends_with("%0A%28cut%29"));
}

TEST_CASE("Problem reports keep the ends of long logs", "[frontend_common][problem_report]") {
    const std::string text = "aaaa\nbbbb\ncccc\ndddd\n";
    CHECK(KeepEnds(text, 100, 100) == text);
    CHECK(KeepEnds(text, 7, 7) ==
          "aaaa\n[... 10 bytes of this log are left out of the report ...]\ndddd\n");
}

TEST_CASE("Problem reports find the last game a log booted", "[frontend_common][problem_report]") {
    const std::string log =
        "[   0.1] Frontend <Info> qt_common/qt_common.cpp:273:Init: Host OS: Windows 11\r\n"
        "[   9.1] Frontend <Info> lichii/main_window.cpp:2462:BootGame: Booting game: "
        "0400486024DEC000 | The Legend of Zelda Ocarina of Time (64-bit) | \n"
        "[   9.2] Frontend <Info> x\n"
        "[  20.0] Frontend <Info> lichii/main_window.cpp:2462:BootGame: Booting game: "
        "0400C3F00006E000 | Mario Kart World (64-bit) | 1.4.0\n";
    CHECK(LastValue(log, "Init: Host OS: ") == "Windows 11");
    CHECK_FALSE(LastValue(log, "Report: Device: "));

    const auto game = LastBootedGame(log);
    REQUIRE(game);
    CHECK(game->program_id == 0x0400C3F00006E000);
    CHECK(game->title == "Mario Kart World (64-bit)");
    CHECK(game->version == "1.4.0");

    const auto first = LastBootedGame(log.substr(0, log.find("[   9.2]")));
    REQUIRE(first);
    CHECK(first->program_id == 0x0400486024DEC000);
    CHECK(first->title == "The Legend of Zelda Ocarina of Time (64-bit)");
    CHECK(first->version.empty());

    CHECK_FALSE(LastBootedGame("no game here"));
}
