#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for .github/workflows/upstream-sync.yml: python -I tools/ci/tests/test_upstream_sync.py

A workflow_dispatch run cannot reach the blocked-sync issue step, which only a failed scheduled run
starts, so its wiring is read here.
"""

import os
import sys
import unittest
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = Path(HERE).parent.parent.parent
sys.path.insert(0, str(ROOT / "tools" / "ingest" / "tests"))  # -I leaves the script's directory off sys.path.

import yaml_subset  # noqa: E402

TITLE = "upstream-sync is blocked"


class BlockedSyncIssueTests(unittest.TestCase):
    def setUp(self):
        self.text = (ROOT / ".github" / "workflows" / "upstream-sync.yml").read_text(encoding="utf-8")
        self.doc = yaml_subset.load(self.text)
        self.steps = self.doc["jobs"]["sync"]["steps"]

    def gated(self, word):
        return [step for step in self.steps if word in (step.get("if") or "")]

    def test_the_token_may_write_issues(self):
        self.assertEqual(self.doc["permissions"].get("issues"), "write")

    def test_one_step_reports_a_failed_scheduled_run(self):
        steps = self.gated("failure()")
        self.assertEqual(len(steps), 1)
        step = steps[0]
        self.assertIs(step, self.steps[-1])
        self.assertIn("github.event_name == 'schedule'", step["if"])
        self.assertEqual(step["env"]["GH_TOKEN"], "${{ github.token }}")
        self.assertIn(TITLE, step["run"])
        self.assertIn("UPSTREAM_SYNC_TOKEN", step["run"])
        self.assertIn("docs/LANES.md", step["run"])
        self.assertIn("gh issue create", step["run"])
        self.assertIn("gh issue comment", step["run"])

    def test_a_good_run_closes_the_issue(self):
        steps = self.gated("success()")
        self.assertEqual(len(steps), 1)
        self.assertEqual(steps[0]["env"]["GH_TOKEN"], "${{ github.token }}")
        self.assertIn(TITLE, steps[0]["run"])
        self.assertIn("gh issue close", steps[0]["run"])

    def test_the_issue_steps_never_see_the_sync_token(self):
        for step in self.gated("failure()") + self.gated("success()"):
            self.assertNotIn("secrets.", str(step))

    def test_the_fix_it_names_is_documented(self):
        lanes = (ROOT / "docs" / "LANES.md").read_text(encoding="utf-8")
        self.assertIn("### When the sync stops", lanes)
        self.assertIn("UPSTREAM_SYNC_TOKEN", lanes)


if __name__ == "__main__":
    unittest.main()
