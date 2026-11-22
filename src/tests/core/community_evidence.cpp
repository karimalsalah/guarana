// SPDX-FileCopyrightText: Copyright 2026 Guarana contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/community_evidence.h"

namespace {

using namespace Core::CommunityEvidence;
using Common::GpuFamily;

constexpr u64 OcarinaTitleId = 0x0400486024DEC000;

// The shape tools/ingest/evidence.py writes, with entries the parser must skip or survive.
constexpr std::string_view Document = R"json({
  "schema_version": 1,
  "effects": [
    {"family": "IntelArrowLakeH", "game": "The Legend of Zelda: Ocarina of Time",
     "title_id": "0400486024DEC000", "scene": "oot-hyrule-field",
     "change": "EDEN_NX2_GM=11 against EDEN_NX2_GM=0", "set": {"EDEN_NX2_GM": "11"},
     "against": {"EDEN_NX2_GM": "0"}, "context": [], "build": "v20.5.0", "power": "ac",
     "pairs": 4, "issues": [12, 40, 52], "devices": 3, "per_device": [],
     "fps_change_pct": {"min": 31.0, "median": 48.0, "max": 55.0},
     "p99_change_pct": {"min": -45.0, "median": -40.0, "max": -30.0},
     "within_2ms_change_pt": null},
    {"family": "IntelArrowLakeH", "title_id": "0400486024DEC000", "scene": "oot-hyrule-field",
     "set": {"EDEN_NX2_GM": "11"}, "against": {"EDEN_NX2_GM": "0"}, "context": [],
     "build": "v20.5.0", "power": "battery", "pairs": 1, "issues": [60], "devices": 1,
     "fps_change_pct": {"min": 20.0, "median": 20.0, "max": 20.0}, "p99_change_pct": null},
    {"family": "IntelArcLunarLake", "title_id": "0400486024DEC000", "scene": "oot-hyrule-field",
     "set": {"EDEN_NX2_GM": "11"}, "against": {"EDEN_NX2_GM": "0"}, "context": [],
     "build": "v20.5.0", "power": "ac", "pairs": 1, "issues": [61], "devices": 1,
     "fps_change_pct": {"min": -3.0, "median": -3.0, "max": -3.0}, "p99_change_pct": null},
    {"family": "IntelArrowLakeH", "title_id": "0400486024DEC000", "scene": "oot-hyrule-field",
     "set": {"setting:Renderer/vram_usage_mode": "1"},
     "against": {"setting:Renderer/vram_usage_mode": "0"}, "context": ["EDEN_NX2_FLUID=2"],
     "build": "v20.5.0", "power": "ac", "pairs": 2, "issues": [70], "devices": 2,
     "fps_change_pct": {"min": 1.0, "median": 2.5, "max": 4.0}, "p99_change_pct": null},
    {"family": "IntelArrowLakeH", "title_id": "0400486024DEC000", "scene": "oot-hyrule-field",
     "set": {"EDEN_NX2_FLUID": "2", "EDEN_NX2_GM": "11"},
     "against": {"EDEN_NX2_FLUID": "0", "EDEN_NX2_GM": "0"}, "devices": 9, "pairs": 9},
    {"family": "NotAFamily", "title_id": "0400486024DEC000", "set": {"EDEN_NX2_GM": "11"},
     "devices": 20},
    "not an effect",
    {"family": "IntelArrowLakeH", "title_id": "not hex", "set": {"EDEN_NX2_GM": "11"},
     "devices": "three", "issues": [-1, "x", 7], "fps_change_pct": {"median": "a lot"}}
  ]
})json";

} // Anonymous namespace

TEST_CASE("Community evidence reads the effects of known GPU families", "[core][community_evidence]") {
    const std::vector<Effect> effects = Parse(Document);
    REQUIRE(effects.size() == 6);
    const Effect& first = effects.front();
    CHECK(first.family == GpuFamily::IntelArrowLakeH);
    CHECK(first.title_id == OcarinaTitleId);
    CHECK(first.scene == "oot-hyrule-field");
    REQUIRE(first.set.size() == 1);
    CHECK(first.set.front().first == "EDEN_NX2_GM");
    CHECK(first.set.front().second == "11");
    CHECK(first.against.front().second == "0");
    CHECK((first.build == "v20.5.0" && first.power == "ac"));
    CHECK((first.devices == 3 && first.pairs == 4));
    CHECK(first.fps_change_median == 48.0);
    CHECK(first.fps_change_min == 31.0);
    CHECK(first.fps_change_max == 55.0);
    CHECK(first.p99_change_median == -40.0);
    CHECK((first.issues == std::vector<u32>{12, 40, 52}));
    CHECK(effects[3].context == std::vector<std::string>{"EDEN_NX2_FLUID=2"});
    // Wrong types are left out, not trusted.
    const Effect& odd = effects.back();
    CHECK(odd.title_id == 0);
    CHECK(odd.devices == 0);
    CHECK(odd.issues == std::vector<u32>{7});
    CHECK(!odd.fps_change_median.has_value());
}

TEST_CASE("Community evidence that cannot be read is empty", "[core][community_evidence]") {
    for (const std::string_view text :
         {std::string_view{}, std::string_view{"not json"}, std::string_view{"{}"},
          std::string_view{"[]"}, std::string_view{R"({"effects": {}})"},
          std::string_view{R"({"effects": [)"}}) {
        CHECK(Parse(text).empty());
    }
}

TEST_CASE("Community evidence finds the effect of one switch or setting", "[core][community_evidence]") {
    const std::vector<Effect> effects = Parse(Document);
    REQUIRE(effects.size() == 6);
    // Of the two Arrow Lake-H effects of EDEN_NX2_GM=11 alone, the one with more devices; the effect
    // with nine devices also sets EDEN_NX2_FLUID, so it measures something else.
    CHECK(Find(effects, GpuFamily::IntelArrowLakeH, OcarinaTitleId, "EDEN_NX2_GM", "11") ==
          &effects[0]);
    CHECK(Find(effects, GpuFamily::IntelArcLunarLake, OcarinaTitleId, "EDEN_NX2_GM", "11") ==
          &effects[2]);
    // A setting is found by its label.
    CHECK(Find(effects, GpuFamily::IntelArrowLakeH, OcarinaTitleId, "vram_usage_mode", "1") ==
          &effects[3]);
    // The value against which the other was measured is cited too: EDEN_NX2_GM=0 was measured.
    CHECK(Find(effects, GpuFamily::IntelArrowLakeH, OcarinaTitleId, "vram_usage_mode", "0") ==
          &effects[3]);
    CHECK(Find(effects, GpuFamily::IntelArrowLakeH, OcarinaTitleId, "EDEN_NX2_GM", "0") ==
          &effects[0]);
    CHECK(Find(effects, GpuFamily::IntelArrowLakeH, OcarinaTitleId, "vram_usage_mode", "2") ==
          nullptr);
    CHECK(Find(effects, GpuFamily::IntelArrowLakeH, OcarinaTitleId, "EDEN_NX2_GM", "3") == nullptr);
    CHECK(Find(effects, GpuFamily::IntelArrowLakeH, 0x0100152000022000, "EDEN_NX2_GM", "11") ==
          nullptr);
    CHECK(Find(effects, GpuFamily::None, OcarinaTitleId, "EDEN_NX2_GM", "11") == nullptr);
    CHECK(Find(effects, GpuFamily::AmdRadeon780M, OcarinaTitleId, "EDEN_NX2_GM", "11") == nullptr);
    CHECK(Find({}, GpuFamily::IntelArrowLakeH, OcarinaTitleId, "EDEN_NX2_GM", "11") == nullptr);
}

TEST_CASE("Community evidence is described in one line", "[core][community_evidence]") {
    const std::vector<Effect> effects = Parse(Document);
    REQUIRE(effects.size() == 6);
    CHECK(Describe(effects[0]) ==
          "EDEN_NX2_GM=11 against EDEN_NX2_GM=0 in oot-hyrule-field: game fps +48.0% median "
          "(+31.0% to +55.0%), p99 -40.0%, 3 devices, 4 pairs, v20.5.0, mains, community reports "
          "#12 #40 #52");
    CHECK(Describe(effects[1]) ==
          "EDEN_NX2_GM=11 against EDEN_NX2_GM=0 in oot-hyrule-field: game fps +20.0% median, "
          "1 device, 1 pair, v20.5.0, battery, community reports #60");
    CHECK(Describe(effects[3]) ==
          "setting:Renderer/vram_usage_mode=1 against setting:Renderer/vram_usage_mode=0 in "
          "oot-hyrule-field: game fps +2.5% median (+1.0% to +4.0%), 2 devices, 2 pairs, with "
          "EDEN_NX2_FLUID=2, v20.5.0, mains, community reports #70");
}

TEST_CASE("The build's community evidence is read once", "[core][community_evidence]") {
    const std::span<const Effect> first = Embedded();
    const std::span<const Effect> again = Embedded();
    CHECK(first.data() == again.data());
    CHECK(first.size() == again.size());
    for (const Effect& effect : first) {
        CHECK(effect.family != GpuFamily::None);
    }
}
