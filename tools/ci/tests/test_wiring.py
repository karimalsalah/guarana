#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Checks that the credits reach the products: the Qt About dialog's list is generated from CONTRIBUTORS.md
at configure time, the launcher embeds site/data/legacy.json, and the release notes end with the thanks.
Source checks only (nothing is built): python -I tools/ci/tests/test_wiring.py
"""

import json
import os
import re
import unittest
from pathlib import Path

ROOT = Path(os.path.dirname(os.path.abspath(__file__))).parent.parent.parent


def read(*parts):
    return ROOT.joinpath(*parts).read_text(encoding="utf-8")


class AboutDialogTests(unittest.TestCase):
    def test_the_list_is_generated_at_configure_time_from_contributors_md(self):
        cmake = read("src", "lichii", "CMakeLists.txt")
        self.assertIn("include(GuaranaContributors)", cmake)
        self.assertIn("CONTRIBUTORS.md", cmake)
        self.assertIn("guarana_contributors.h", cmake)
        module = read("CMakeModules", "GuaranaContributors.cmake")
        self.assertIn("file(READ", module)
        self.assertIn("configure_file(", module)
        self.assertIn("CMAKE_CONFIGURE_DEPENDS", module)  # a changed CONTRIBUTORS.md re-runs configure
        self.assertTrue((ROOT / "src" / "lichii" / "guarana_contributors.h.in").is_file())

    def test_names_are_sanitized_before_they_become_a_literal(self):
        module = read("CMakeModules", "GuaranaContributors.cmake")
        self.assertIn('R\\"GUARANA(', module)
        self.assertIn("string(REGEX REPLACE", module)

    def test_the_dialog_escapes_every_name_and_stays_small(self):
        source = read("src", "lichii", "about_dialog.cpp")
        self.assertIn("toHtmlEscaped()", source)
        self.assertIn('#include "guarana_contributors.h"', source)
        self.assertIn("max_shown", source)
        self.assertIn("Your name could be here: contribute", source)
        self.assertIn("CONTRIBUTORS.md", source)

    def test_the_table_the_cmake_regex_reads_is_the_one_legacy_py_writes(self):
        text = read("CONTRIBUTORS.md")
        people = text.split("## People")[1].split("\n## Tools")[0]
        rows = re.findall(r"\n\|[ ]*([^|\n]+)\|[^\n]*", people)
        names = [row.strip() for row in rows]
        self.assertEqual(names[0], "Contributor")
        self.assertRegex(names[1], r"^-+$")
        self.assertGreater(len(names), 2)


class LauncherTests(unittest.TestCase):
    def test_the_launcher_embeds_the_hall_of_fame_at_build_time(self):
        build = read("launcher", "src-tauri", "build.rs")
        self.assertIn("legacy.json", build)
        self.assertIn("OUT_DIR", build)
        credits = read("launcher", "src-tauri", "src", "credits.rs")
        self.assertIn("legacy_builtin.json", credits)
        self.assertIn("https://karimalsalah.github.io/guarana/data/legacy.json", credits)
        self.assertIn("If-None-Match", credits)
        self.assertIn("get_credits", read("launcher", "src-tauri", "src", "main.rs"))
        self.assertTrue(json.loads(read("site", "data", "legacy.json"))["people"])

    def test_the_screen_links_the_two_contribution_docs(self):
        screen = read("launcher", "ui", "src", "screens", "about.ts")
        self.assertIn("Your name could be here: contribute", screen)
        self.assertIn("AI_QUICKSTART_URL", screen)
        self.assertIn("CONTRIBUTING.md", screen)
        self.assertIn("get_credits", read("launcher", "CONTRACT.md"))


class ReleaseNotesTests(unittest.TestCase):
    def test_both_channels_end_their_notes_with_the_thanks_line(self):
        workflow = read(".github", "workflows", "release-binaries.yml")
        self.assertEqual(workflow.count("python3 -I tools/ci/thanks.py"), 2)
        self.assertIn('--tag "$TAG"', workflow)
        self.assertIn('--base "$previous"', workflow)
        # thanks.py imports tools/ingest/github_api.py, so the sparse checkout must carry it.
        self.assertRegex(workflow, r"sparse-checkout: \|\s+tools/ci\s+tools/ingest")


if __name__ == "__main__":
    unittest.main()
