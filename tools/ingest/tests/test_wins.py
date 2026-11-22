#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Checks of the Open wins page: site/wins.html, site/data/wins.json, its place in the nav of every page,
the README line, the sitemap and llms.txt. No network.

  python -I tools/ingest/tests/test_wins.py
"""

import json
import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)  # -I leaves the script's directory off sys.path.
sys.path.insert(0, os.path.dirname(HERE))

import yaml_subset  # noqa: E402
from test_site import ROOT, SITE, Page, label_names, read  # noqa: E402

REWARD = "What you get: your name in the release notes, the hall of fame and the app's credits."
EXPECTED = {"readback-stalls", "first-visit-shader-stutter", "boot-time", "hyrule-field-fps", "amd-first-reports",
            "controller-gyro-rumble"}


def wins():
    return json.loads(read("site", "data", "wins.json"))


class WinsDataTests(unittest.TestCase):
    def setUp(self):
        self.doc = wins()

    def test_the_six_named_problems_are_there_with_unique_ids(self):
        ids = [win["id"] for win in self.doc["wins"]]
        self.assertEqual(len(ids), len(set(ids)))
        self.assertTrue(EXPECTED <= set(ids), EXPECTED - set(ids))

    def test_every_win_has_scene_numbers_area_and_issue_link(self):
        self.assertEqual(self.doc["schema_version"], 1)
        for win in self.doc["wins"]:
            for key in ("title", "scene", "metric", "next_step", "measure"):
                self.assertTrue(win[key].strip(), (win["id"], key))
            for key in ("current", "target"):
                self.assertTrue(win[key]["text"].strip(), (win["id"], key))
                self.assertIn("value", win[key])
            self.assertTrue(win["files"], win["id"])
            self.assertIn(win["status"], ("open", "won"))
            self.assertRegex(win["issue_url"], r"^https://github\.com/karimalsalah/guarana/issues(?:/\d+|\?q=[A-Za-z0-9%._+:-]+)$")
            for number in win["related_issues"]:
                self.assertIsInstance(number, int)

    def test_the_issue_links_use_labels_that_exist(self):
        known = label_names()
        for win in self.doc["wins"]:
            for label in win["labels"]:
                self.assertIn(label, known, (win["id"], label))
            self.assertIn(win["issue_label"], known)

    def test_the_figures_the_maintainers_measured(self):
        by_id = {win["id"]: win for win in self.doc["wins"]}
        readback = by_id["readback-stalls"]
        self.assertEqual((readback["current"]["value"], readback["target"]["value"]), (133, 33.6))
        self.assertEqual(readback["switch"], "EDEN_NX2_EARLY_AFTER")
        self.assertIn("133 ms", readback["current"]["text"])
        self.assertIn("33.6 ms", readback["target"]["text"])
        self.assertIn("graphics pipeline library", by_id["first-visit-shader-stutter"]["next_step"].lower())
        self.assertIn("3.6 to 4.8 s", by_id["boot-time"]["current"]["text"])
        self.assertEqual((by_id["hyrule-field-fps"]["current"]["value"], by_id["hyrule-field-fps"]["target"]["value"]), (25, 30))
        self.assertIn("780M", by_id["amd-first-reports"]["title"])
        self.assertIn("890M", by_id["amd-first-reports"]["title"])
        self.assertIn("gyro", by_id["controller-gyro-rumble"]["title"].lower())

    def test_the_repository_paths_it_names_exist(self):
        for win in self.doc["wins"]:
            for entry in win["files"]:
                path = re.split(r"[ (]", entry)[0]
                self.assertTrue(os.path.exists(os.path.join(ROOT, path)), (win["id"], path))

    def test_no_personal_data(self):
        text = read("site", "data", "wins.json")
        self.assertNotRegex(text, r"[A-Za-z]:\\\\Users|@[A-Za-z0-9-]+\.[a-z]{2,}")


class WinsPageTests(unittest.TestCase):
    def setUp(self):
        self.text = read("site", "wins.html")
        self.page = Page(self.text)

    def test_well_formed_with_unique_ids(self):
        self.assertEqual(self.page.errors, [])
        self.assertEqual(len(self.page.ids), len(set(self.page.ids)))
        self.assertIn("wins-list", self.page.ids)

    def test_every_card_says_what_you_get(self):
        self.assertIn(REWARD, self.text)

    def test_the_script_draws_the_data_as_text_only(self):
        scripts = [script for script in self.page.scripts if "wins-app" in script]
        self.assertEqual(len(scripts), 1)
        for forbidden in ("innerHTML", "outerHTML", "insertAdjacentHTML", "document.write", "eval(", "new Function"):
            self.assertNotIn(forbidden, scripts[0])
        self.assertNotRegex(scripts[0], r"=>|`|\blet\b|\bconst\b")
        self.assertIn("github\\.com", scripts[0])  # links must stay on GitHub
        self.assertIn('data-src="data/wins.json"', self.text)

    def test_the_page_is_in_its_own_nav_and_in_the_sitemap_and_llms_txt(self):
        self.assertIn('<li><a href="wins.html" aria-current="page">Open wins</a></li>', self.text)
        self.assertIn("https://karimalsalah.github.io/guarana/wins.html", read("site", "sitemap.xml"))
        self.assertIn("https://karimalsalah.github.io/guarana/wins.html", read("site", "llms.txt"))
        self.assertIn('rel="canonical" href="https://karimalsalah.github.io/guarana/wins.html"', self.text)

    def test_json_ld_parses(self):
        for block in re.findall(r'<script type="application/ld\+json">(.*?)</script>', self.text, re.S):
            json.loads(block)

    def test_no_external_libraries(self):
        for url in self.page.hrefs:
            if re.match(r"(?:https?:)?//", url) and url.endswith((".js", ".css")):
                self.fail(f"external library {url}")


class NavTests(unittest.TestCase):
    def test_every_page_with_the_agents_link_has_the_wins_link_next_to_it(self):
        checked = 0
        for folder, _, names in os.walk(SITE):
            for name in names:
                if not name.endswith(".html"):
                    continue
                path = os.path.join(folder, name)
                with open(path, encoding="utf-8") as handle:
                    text = handle.read()
                for match in re.finditer(r'<li><a href="((?:\.\./)?)agents\.html"[^>]*>Agents</a></li>', text):
                    checked += 1
                    expected = f'<li><a href="{match.group(1)}wins.html"'
                    self.assertTrue(text[match.end():].lstrip().startswith(expected), (path, match.group(0)))
        self.assertGreater(checked, 40)

    def test_the_not_found_page_links_the_absolute_url(self):
        self.assertIn('<a href="https://karimalsalah.github.io/guarana/wins.html">Open wins</a>', read("site", "404.html"))


class ReadmeTests(unittest.TestCase):
    def test_why_contribute_now_links_the_page_in_one_line(self):
        section = read("README.md").split("## Why contribute now")[1].split("\n## ")[0]
        lines = [line for line in section.split("\n") if "wins.html" in line]
        self.assertEqual(len(lines), 1)
        self.assertTrue(lines[0].startswith("- "))
        self.assertIn("https://karimalsalah.github.io/guarana/wins.html", lines[0])


class LabelsFileTests(unittest.TestCase):
    def test_labels_file_loads(self):
        self.assertTrue(yaml_subset.load(read(".github", "labels.yml")))


if __name__ == "__main__":
    unittest.main()
