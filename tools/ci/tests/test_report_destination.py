#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Checks where the emulator's game-list report goes (issue #107): to Guarana's own GitHub forms
through Help > Report a problem, never to upstream Lichii's compatibility site.
Source checks only (nothing is built): python -I tools/ci/tests/test_report_destination.py
"""

import os
import re
import unittest
from pathlib import Path

ROOT = Path(os.path.dirname(os.path.abspath(__file__))).parent.parent.parent


def read(*parts):
    return ROOT.joinpath(*parts).read_text(encoding="utf-8")


def handler(source, signal):
    """The body of the lambda connected to `signal` in MainWindow::ConnectWidgetEvents."""
    start = source.index(f"&GameList::{signal}")
    end = source.index("});", start)
    return source[start:end]


class GameListReportTests(unittest.TestCase):
    def test_the_entry_names_the_fork_flow(self):
        game_list = read("src", "lichii", "game", "game_list.cpp")
        self.assertIn('tr("Report a problem with this game…")', game_list)
        self.assertNotIn("Submit compatibility report", game_list)

    def test_the_entry_opens_report_a_problem_not_upstreams_dialog(self):
        body = handler(read("src", "lichii", "main_window.cpp"), "ReportCompatibilityRequested")
        self.assertIn("OnReportProblem();", body)
        self.assertNotIn("CompatibilityReporting::Dialog", body)
        # Only the game whose session it is, and not while a game runs.
        self.assertIn("report_session->program_id != program_id", body)
        self.assertIn("emulation_running", body)

    def test_nothing_else_opens_upstreams_dialog(self):
        main_window = read("src", "lichii", "main_window.cpp")
        self.assertNotIn("CompatibilityReporting::Dialog", main_window)

    def test_report_a_problem_goes_to_this_repository(self):
        source = read("src", "lichii", "problem_report.cpp")
        forms = re.search(r'IssueForms\s*=\s*"([^"]+)"', source)
        self.assertIsNotNone(forms)
        self.assertEqual(forms.group(1), "https://github.com/karimalsalah/guarana/issues/new?template=")
        self.assertIn('"game_bug.yml"', source)
        self.assertTrue((ROOT / ".github" / "ISSUE_TEMPLATE" / "game_bug.yml").is_file())


if __name__ == "__main__":
    unittest.main()
