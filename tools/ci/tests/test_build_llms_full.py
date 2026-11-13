#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for tools/ci/build_llms_full.py: python -I tools/ci/tests/test_build_llms_full.py"""

import os
import re
import sys
import tempfile
import unittest
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
CI = os.path.dirname(HERE)
sys.path.insert(0, CI)  # -I leaves the script's directory off sys.path.

import build_llms_full as llms  # noqa: E402

ROOT = Path(CI).parent.parent

AGENTS = """# AGENTS

## Guarana: read this first

Guarana is a fork. See [switches](docs/switches.md) and [the map](#repo-map).

### Rules

1. **Measure.**

### Repo map

| Path | Contents |
|---|---|
| `src/core` | HLE |

### Build

```sh
# a comment that is not a heading
cmake -S . -B build
```

---

## Upstream Lichii guidance

Upstream only text that must not appear.
"""

SWITCHES = """# Switch registry

Every `EDEN_NX2_*` switch that Guarana adds. See [AGENTS.md](../AGENTS.md).

## Setting a switch

Not a table.

## Proven

| Switch | Values | What it does | Measured | Code |
|---|---|---|---|---|
| `EDEN_NX2_GM` | `11` | Cheaper routing. Second sentence is cut. | 14.75 to 22.83 | `a.cpp` |

## Diagnostic

| Switch | Values | What it does | Code |
|---|---|---|---|
| `EDEN_NX2_A`, `EDEN_NX2_B` | `1`; a pipe `|` in code | Logs a thing. | `b.cpp` |
"""

CONTRIBUTING = "# Contributing to Guarana\n\nIntro line.\n\n## Five-minute start\n\nSee [AGENTS](AGENTS.md).\n"
LEGAL = "# Legal\n\n## Not affiliated\n\nIndependent project. More words.\n\n## No piracy\n\nNo keys. Never.\n"
FACTS = ("# Guarana: full reference\n\n> intro\n\n## What it is\n\n- A fork.\n\n"
         "## Contributing\n\n- duplicate that must be skipped\n\n## Devices\n\n### Arc 140T (Tested)\n\n- fact\n")


def make_tree(directory, facts=FACTS):
    root = Path(directory)
    for name, text in (("AGENTS.md", AGENTS), ("docs/switches.md", SWITCHES), ("CONTRIBUTING.md", CONTRIBUTING),
                       ("LEGAL.md", LEGAL), ("tools/ci/llms-site-facts.txt", facts)):
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
    return root


class FixtureTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = make_tree(self.temp.name)
        self.text = llms.build(self.root)

    def test_every_source_is_in_it(self):
        for needed in ("## Rules for agents and contributors (AGENTS.md)", "### Repo map", "### Build",
                       "cmake -S . -B build", "## Switches overview", "### Proven (1)", "### Diagnostic (1)",
                       "## CONTRIBUTING.md", "### Five-minute start", "## Legal in short", "## Site reference",
                       "#### Arc 140T (Tested)", "https://discord.gg/urz7vMdRM"):
            self.assertIn(needed, self.text)

    def test_the_upstream_half_of_agents_md_is_left_out(self):
        self.assertNotIn("must not appear", self.text)
        self.assertNotIn("\n---\n", self.text)

    def test_switch_rows_keep_name_values_and_first_sentence(self):
        self.assertIn("- `EDEN_NX2_GM` (values: `11`): Cheaper routing.", self.text)
        self.assertNotIn("Second sentence", self.text)
        self.assertIn("- `EDEN_NX2_A`, `EDEN_NX2_B` (values: `1`; a pipe `|` in code): Logs a thing.", self.text)
        self.assertNotIn("Not a table", self.text.split("## CONTRIBUTING.md")[0].split("### Proven")[1])

    def test_relative_links_become_absolute(self):
        self.assertIn("](https://github.com/karimalsalah/guarana/blob/HEAD/docs/switches.md)", self.text)
        self.assertIn("](https://github.com/karimalsalah/guarana/blob/HEAD/AGENTS.md)", self.text)  # ../AGENTS.md
        self.assertIn("](#repo-map)", self.text)  # anchors stay
        self.assertNotRegex(self.text, r"\]\((?!https?://|#)")

    def test_a_hash_in_a_code_fence_is_not_a_heading(self):
        self.assertIn("\n# a comment that is not a heading\n", self.text)

    def test_the_duplicate_facts_section_is_skipped(self):
        self.assertNotIn("duplicate that must be skipped", self.text)
        self.assertIn("- A fork.", self.text)

    def test_legal_summary_has_each_first_sentence(self):
        self.assertIn("- Not affiliated: Independent project.", self.text)
        self.assertIn("- No piracy: No keys.", self.text)

    def test_it_is_deterministic_and_ends_with_one_newline(self):
        self.assertEqual(self.text, llms.build(self.root))
        self.assertTrue(self.text.endswith("\n") and not self.text.endswith("\n\n"))
        self.assertNotIn("\r", self.text)

    def test_without_the_facts_file_it_still_builds(self):
        (self.root / "tools/ci/llms-site-facts.txt").unlink()
        self.assertNotIn("## Site reference", llms.build(self.root))

    def test_main_writes_the_file_and_check_writes_nothing(self):
        out = Path(self.temp.name) / "out" / "llms-full.txt"
        self.assertEqual(llms.main(["--root", str(self.root), "--out", str(out), "--check"]), 0)
        self.assertFalse(out.exists())
        self.assertEqual(llms.main(["--root", str(self.root), "--out", str(out)]), 0)
        self.assertEqual(out.read_text(encoding="utf-8"), self.text)

    def test_a_missing_agents_heading_is_an_error(self):
        (self.root / "AGENTS.md").write_text("# nothing here\n", encoding="utf-8")
        with self.assertRaises(SystemExit):
            llms.build(self.root)


class RepositoryTests(unittest.TestCase):
    """The real tree: structure only, because other work changes the numbers."""

    def setUp(self):
        self.text = llms.build(ROOT)

    def test_it_builds_from_the_repository(self):
        for needed in ("### Repo map", "### Build", "### Never commit", "## Switches overview",
                       "### Proven", "EDEN_NX2_GM", "## CONTRIBUTING.md", "## Legal in short", "## Site reference"):
            self.assertIn(needed, self.text)
        self.assertLess(len(self.text), llms.MAX_CHARS)

    def test_no_relative_link_is_left(self):
        self.assertNotRegex(self.text, r"\]\((?!https?://|#)")

    def test_the_pages_workflow_builds_it_before_the_upload(self):
        workflow = (ROOT / ".github" / "workflows" / "pages.yml").read_text(encoding="utf-8")
        build = workflow.index("tools/ci/build_llms_full.py")
        self.assertLess(build, workflow.index("actions/upload-pages-artifact"))
        for needed in ("AGENTS.md", "CONTRIBUTING.md", "LEGAL.md", "docs/switches.md", "tools/ci"):
            self.assertIn(needed, workflow)

    def test_the_facts_file_has_no_secret_looking_text(self):
        facts = (ROOT / llms.FACTS).read_text(encoding="utf-8")
        self.assertFalse(re.search(r"(?i)prod\.keys\s*[:=]|api[_-]?key\s*[:=]", facts))


if __name__ == "__main__":
    unittest.main()
