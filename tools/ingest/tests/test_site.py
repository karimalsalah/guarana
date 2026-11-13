#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Checks of the site and docs files of the feedback loop: site/data/community.json,
site/agents.json, the leaderboard in site/benchmarks.html, site/agents.html, site/llms.txt,
docs/AGENTS-INVITE.md and docs/FEEDBACK.md.

  python -I tools/ingest/tests/test_site.py
"""

import html.parser
import json
import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)  # -I leaves the script's directory off sys.path.
sys.path.insert(0, os.path.dirname(HERE))

import ingest  # noqa: E402
import yaml_subset  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
SITE = os.path.join(ROOT, "site")
VOID = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "source", "track", "wbr"}


def read(*parts):
    with open(os.path.join(ROOT, *parts), encoding="utf-8") as handle:
        return handle.read()


def label_names():
    return {label["name"] for label in yaml_subset.load(read(".github", "labels.yml"))}


class Page(html.parser.HTMLParser):
    """Tag balance, ids, links, inline scripts, and the ids each element sits inside."""

    def __init__(self, text):
        super().__init__(convert_charrefs=True)
        self.stack, self.errors, self.ids, self.hrefs, self.scripts = [], [], [], [], []
        self.ancestors = {}
        self._script = None
        self.feed(text)
        self.close()
        if self.stack:
            self.errors.append(f"unclosed: {[tag for tag, _ in self.stack]}")

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if "id" in attrs:
            self.ids.append(attrs["id"])
            self.ancestors[attrs["id"]] = [element_id for _, element_id in self.stack if element_id]
        for key in ("href", "src"):
            if attrs.get(key):
                self.hrefs.append(attrs[key])
        if tag == "script" and "src" not in attrs and attrs.get("type") != "application/ld+json":
            self._script = []
        if tag not in VOID:
            self.stack.append((tag, attrs.get("id")))

    def handle_startendtag(self, tag, attrs):
        attrs = dict(attrs)
        if "id" in attrs:
            self.ids.append(attrs["id"])

    def handle_endtag(self, tag):
        if tag in VOID:
            return
        if not self.stack or self.stack[-1][0] != tag:
            self.errors.append(f"line {self.getpos()[0]}: </{tag}> does not close {self.stack[-1:]}")
            return
        self.stack.pop()
        if tag == "script" and self._script is not None:
            self.scripts.append("".join(self._script))
            self._script = None

    def handle_data(self, data):
        if self._script is not None:
            self._script.append(data)


class DataFileTests(unittest.TestCase):
    def test_community_json_is_what_the_leaderboard_reads(self):
        text = read("site", "data", "community.json")
        doc = json.loads(text)
        self.assertEqual(set(doc), set(ingest.make_document({}, "karimalsalah/guarana")))
        self.assertEqual((doc["schema_version"], doc["count"]), (ingest.SCHEMA_VERSION, len(doc["reports"])))
        self.assertEqual(doc["issue_url"], "https://github.com/karimalsalah/guarana/issues/{issue}")
        self.assertNotIn("\r", text)
        keys = list(doc["reports"])
        self.assertEqual(keys, sorted(keys, key=int))
        for key, record in doc["reports"].items():
            self.assertEqual(record["issue"], int(key))
            self.assertIn(record["scene_ok"], (True, None))
            self.assertIn(record["gpu_vendor"], ("intel", "amd", "nvidia", "other"))
            self.assertTrue(record["fps"] is None or isinstance(record["fps"], (int, float)))


class AgentsJsonTests(unittest.TestCase):
    def setUp(self):
        self.doc = json.loads(read("site", "agents.json"))
        self.strings = []

        def walk(value):
            if isinstance(value, dict):
                for item in value.values():
                    walk(item)
            elif isinstance(value, list):
                for item in value:
                    walk(item)
            elif isinstance(value, str):
                self.strings.append(value)
        walk(self.doc)

    def test_every_repository_path_exists(self):
        paths = [text for text in self.strings
                 if re.fullmatch(r"(?:\.?[\w-]+/)*\.?[\w.-]+\.(?:md|yml|json|mdc|py|ps1|sh)", text)]
        self.assertGreater(len(paths), 10)
        for path in paths:
            self.assertTrue(os.path.exists(os.path.join(ROOT, path)), path)

    def test_every_label_exists(self):
        names = label_names()
        queue = self.doc["work_queue"]
        used = set(self.doc["labels"]) | {queue["label"]} | set(queue["skip_if_labeled"])
        self.assertEqual(used - names, set())

    def test_links_and_contact(self):
        for text in self.strings:
            if "://" in text:
                self.assertTrue(text.startswith("https://"), text)
        config = yaml_subset.load(read(".github", "ISSUE_TEMPLATE", "config.yml"))
        urls = {link["url"] for link in config["contact_links"]}
        self.assertIn(self.doc["contact"]["questions"], urls)
        self.assertIn(self.doc["contact"]["security"], urls)
        self.assertTrue(self.doc["agents_welcome"])

    def test_the_mcp_command_is_the_one_mcp_json_registers(self):
        mcp = json.loads(read(".mcp.json"))["mcpServers"]["emulator-debug"]
        server = self.doc["entry_points"]["debug_mcp_server"]
        self.assertEqual((server["command"], server["args"]), (mcp["command"], mcp["args"]))


class BenchmarksPageTests(unittest.TestCase):
    def setUp(self):
        self.text = read("site", "benchmarks.html")
        self.page = Page(self.text)

    def test_well_formed_with_unique_ids(self):
        self.assertEqual(self.page.errors, [])
        self.assertEqual(len(self.page.ids), len(set(self.page.ids)))

    def test_the_leaderboard_sits_outside_bench_app(self):
        # bench.js replaces everything inside #bench-app.
        self.assertIn("community-app", self.page.ancestors)
        self.assertNotIn("bench-app", self.page.ancestors["community-app"])
        self.assertIn("community", self.page.ancestors["community-app"])
        self.assertIn('data-src="data/community.json"', self.text)
        for row in ("arc140t-oot-hyrule-field-gm11", "arc140t-oot-play-previous", "arc140t-oot-play-v20"):
            self.assertIn(row, self.page.ids)

    def test_the_script_puts_data_in_as_text_only(self):
        scripts = [script for script in self.page.scripts if "community-app" in script]
        self.assertEqual(len(scripts), 1)
        for forbidden in ("innerHTML", "outerHTML", "insertAdjacentHTML", "document.write", "eval(", "new Function"):
            self.assertNotIn(forbidden, scripts[0])
        self.assertIn("r.scene_ok === true", scripts[0])
        self.assertNotRegex(scripts[0], r"=>|`|\blet\b|\bconst\b")  # ES5, like assets/bench.js

    def test_no_external_libraries(self):
        for url in self.page.hrefs:
            if re.match(r"(?:https?:)?//", url) and url.endswith((".js", ".css")):
                self.fail(f"external library {url}")

    def test_json_ld_parses(self):
        for block in re.findall(r'<script type="application/ld\+json">(.*?)</script>', self.text, re.S):
            json.loads(block)


class AgentsPageTests(unittest.TestCase):
    def test_well_formed_and_linked(self):
        text = read("site", "agents.html")
        page = Page(text)
        self.assertEqual(page.errors, [])
        self.assertEqual(len(page.ids), len(set(page.ids)))
        self.assertIn("<title>Autonomous AI Agents Welcome | Guarana</title>", text)
        self.assertIn("Autonomous AI agents welcome", text)
        for href in page.hrefs:
            if not re.match(r"[a-z]+:|#|//", href):
                self.assertTrue(os.path.exists(os.path.join(SITE, href.split("#")[0] or "index.html")), href)
        self.assertIn("agents.json", page.hrefs)
        for block in re.findall(r'<script type="application/ld\+json">(.*?)</script>', text, re.S):
            json.loads(block)
        self.assertEqual(page.scripts, [])


class LlmsTxtTests(unittest.TestCase):
    def test_llms_txt_links_the_invitation(self):
        lines = [line for line in read("site", "llms.txt").split("\n")
                 if "https://karimalsalah.github.io/guarana/agents.json" in line]
        self.assertEqual(len(lines), 1)
        self.assertIn("https://karimalsalah.github.io/guarana/agents.html", lines[0])


class DocsTests(unittest.TestCase):
    def check_links(self, name):
        text = read("docs", name)
        for target in re.findall(r"\]\(([^)\s]+)\)", text):
            if re.match(r"[a-z]+:|#", target):
                continue
            path = os.path.normpath(os.path.join(ROOT, "docs", target.split("#")[0]))
            self.assertTrue(os.path.exists(path), f"{name}: {target}")
        return text

    def test_agents_invite(self):
        text = self.check_links("AGENTS-INVITE.md")
        for needed in ("agent-ready", "AGENTS.md", "Assisted-by:", "AI_POLICY.md", "tools/bench", "debug_mcp.py",
                       "Claude Code", "Codex", "Copilot", "Cursor", "Devin", "OpenHands", "Aider", "needs-triage"):
            self.assertIn(needed, text)

    def test_feedback_diagram(self):
        text = self.check_links("FEEDBACK.md")
        diagram = re.search(r"```mermaid\n(.*?)\n```", text, re.S).group(1)
        for stage in ("launcher", "Issue form", "triage.yml", "ingest-reports.yml", "leaderboard", "agent-ready",
                      "Pull request", "test build", "tools/bench", "Merge", "nightly", "release"):
            self.assertIn(stage, diagram)
        for line in diagram.split("\n"):
            self.assertEqual(line.count('"') % 2, 0, line)
            self.assertNotIn("#", line)  # mermaid reads # as the start of an entity


if __name__ == "__main__":
    unittest.main()
