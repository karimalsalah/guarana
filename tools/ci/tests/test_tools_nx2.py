#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for .github/workflows/tools-nx2.yml: python -I tools/ci/tests/test_tools_nx2.py

Every tools/nx2 suite it runs exists, and scripts/run-tests.ps1 and scripts/run-tests.sh run it too.
"""

import os
import re
import sys
import unittest
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = Path(HERE).parent.parent.parent
sys.path.insert(0, str(ROOT / "tools" / "ingest" / "tests"))  # -I leaves the script's directory off sys.path.

import yaml_subset  # noqa: E402


def read(*parts):
    return ROOT.joinpath(*parts).read_text(encoding="utf-8")


class ToolsNx2WorkflowTests(unittest.TestCase):
    def setUp(self):
        self.doc = yaml_subset.load(read(".github", "workflows", "tools-nx2.yml"))
        self.run = "\n".join(step.get("run", "") for step in self.doc["jobs"]["test"]["steps"])
        loop = re.search(r"for test in (.*?); do", self.run, re.S)
        self.assertIsNotNone(loop)
        self.suites = loop.group(1).replace("\\", " ").split()

    def test_it_runs_on_pull_requests_with_a_read_only_token(self):
        self.assertEqual(list(self.doc["on"]), ["pull_request"])
        self.assertIn("tools/nx2/**", self.doc["on"]["pull_request"]["paths"])
        self.assertEqual(self.doc["permissions"], {"contents": "read"})
        self.assertEqual(self.doc["jobs"]["test"]["timeout-minutes"], "10")

    def test_every_suite_exists(self):
        self.assertGreaterEqual(len(self.suites), 8)
        for suite in self.suites:
            self.assertTrue((ROOT / "tools" / "nx2" / f"{suite}.py").is_file(), suite)

    def test_the_local_scripts_run_every_suite(self):
        powershell = read("scripts", "run-tests.ps1")
        bash = read("scripts", "run-tests.sh")
        for suite in self.suites:
            self.assertIn(f"'tools\\nx2\\{suite.replace('/', chr(92))}.py'", powershell, suite)
            listed = f"tools/nx2/{suite}.py" in bash or re.search(rf"\s{re.escape(suite)}[\s;]", bash)
            self.assertTrue(listed, suite)


if __name__ == "__main__":
    unittest.main()
