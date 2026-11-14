#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for tools/ci/thanks.py with fake GitHub data: python -I tools/ci/tests/test_thanks.py"""

import contextlib
import io
import json
import os
import sys
import unittest
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
CI = os.path.dirname(HERE)
ROOT = Path(CI).parent.parent
sys.path.insert(0, CI)  # -I leaves the script's directory off sys.path.
sys.path.insert(0, HERE)
sys.path.insert(0, str(ROOT / "tools" / "ingest" / "tests"))

import thanks  # noqa: E402
import github_api  # noqa: E402
from fakes import REPO, FakeGitHub  # noqa: E402
from test_legacy import CAPY, CLAUDE, commit  # noqa: E402


class FakeRange(FakeGitHub):
    """Tags, a compare range and the commits list; records the ranges asked for."""

    def __init__(self, tags, commits, **kwargs):
        super().__init__(**kwargs)
        self.tags, self.commits, self.asked = tags, commits, []

    def request(self, method, path, body=None, params=None, accept=None):
        params = params or {}
        prefix = f"/repos/{self.repo}"
        if method == "GET" and path == prefix + "/tags":
            return [{"name": name} for name in self.tags], ""
        if method == "GET" and path.startswith(prefix + "/compare/"):
            self.asked.append(path[len(prefix + "/compare/"):])
            page, size = params.get("page", 1), params.get("per_page", 100)
            return {"commits": json.loads(json.dumps(self.commits[(page - 1) * size:page * size]))}, ""
        if method == "GET" and path == prefix + "/commits":
            self.asked.append(("commits", params.get("sha"), params.get("since")))
            return json.loads(json.dumps(self.commits)), ""
        raise github_api.GitHubError(404, "Not Found", path)


COMMITS = [
    commit("a1", "alice", "2026-12-01",
           trailers=[f"Co-authored-by: {CLAUDE}", "Co-authored-by: Bob B <77+bob@users.noreply.github.com>"]),
    commit("c1", "carol", "2026-12-02",
           trailers=[f"Co-authored-by: {CAPY}", "Co-authored-by: Dan Nolink <dan@nowhere.example>"]),
    commit("bot", "github-actions[bot]", "2026-12-03", user_type="Bot"),
    commit("m1", "mallory", "2026-12-04", message="Merge pull request #9", parents=2),
    commit("a2", "alice", "2026-12-05"),
]


class LineTests(unittest.TestCase):
    def test_people_only_handles_first_then_plain_names(self):
        self.assertEqual(thanks.thanks_line(COMMITS), "Thanks to @alice, @bob, @carol and Dan Nolink.")

    def test_tools_bots_and_merges_are_left_out(self):
        line = thanks.thanks_line(COMMITS)
        for word in ("Claude", "Capy", "github-actions", "mallory", "example", "users.noreply"):
            self.assertNotIn(word, line)

    def test_one_person_and_nobody(self):
        self.assertEqual(thanks.thanks_line([commit("x", "zed", "2026-12-01")]), "Thanks to @zed.")
        self.assertEqual(thanks.thanks_line([commit("b", "github-actions[bot]", "2026-12-01", user_type="Bot")]), "")
        self.assertEqual(thanks.thanks_line([]), "")

    def test_two_people(self):
        two = [commit("x", "zed", "2026-12-01"), commit("y", "amy", "2026-12-01")]
        self.assertEqual(thanks.thanks_line(two), "Thanks to @amy and @zed.")


class PreviousTagTests(unittest.TestCase):
    TAGS = ["nightly", "v0.9.0", "v1.0.0-rc1", "v1.0.0-rc2", "v1.0.0", "v1.1.0-rc1", "v1.1.0", "junk", "v1.2.0-rc1"]

    def test_a_final_release_skips_its_own_candidates(self):
        self.assertEqual(thanks.previous_tag(self.TAGS, "v1.1.0"), "v1.0.0")
        self.assertEqual(thanks.previous_tag(self.TAGS, "v1.0.0"), "v0.9.0")

    def test_a_candidate_follows_the_one_before_it(self):
        self.assertEqual(thanks.previous_tag(self.TAGS, "v1.0.0-rc2"), "v1.0.0-rc1")
        self.assertEqual(thanks.previous_tag(self.TAGS, "v1.1.0-rc1"), "v1.0.0")
        self.assertEqual(thanks.previous_tag(self.TAGS, "v1.2.0-rc1"), "v1.1.0")

    def test_the_first_release_and_odd_tags_have_none(self):
        self.assertIsNone(thanks.previous_tag(self.TAGS, "v0.9.0"))
        self.assertIsNone(thanks.previous_tag(self.TAGS, "nightly"))
        self.assertIsNone(thanks.previous_tag([], "v1.0.0"))

    def test_versions_sort_by_number_not_text(self):
        self.assertEqual(thanks.previous_tag(["v1.2.0", "v1.10.0"], "v1.11.0"), "v1.10.0")


class RangeTests(unittest.TestCase):
    def test_a_tag_compares_with_the_tag_before_it(self):
        github = FakeRange(["v1.0.0", "v1.1.0"], COMMITS)
        commits = thanks.range_commits(github, REPO, "deadbeef", tag="v1.1.0")
        self.assertEqual(github.asked, ["v1.0.0...deadbeef"])
        self.assertEqual(len(commits), len(COMMITS))

    def test_an_explicit_base_wins(self):
        github = FakeRange(["v1.0.0"], COMMITS)
        thanks.range_commits(github, REPO, "head", base="abc123", tag="v1.1.0")
        self.assertEqual(github.asked, ["abc123...head"])

    def test_the_first_release_counts_from_the_first_day(self):
        github = FakeRange([], COMMITS)
        thanks.range_commits(github, REPO, "head", tag="v0.1.0")
        self.assertEqual(github.asked, [("commits", "head", thanks.legacy.SINCE)])

    def test_long_ranges_are_read_page_by_page(self):
        many = [commit(f"s{i}", f"user{i:03d}", "2026-12-01") for i in range(230)]
        github = FakeRange([], many)
        self.assertEqual(len(thanks.compare_commits(github, REPO, "a", "b")), 230)
        self.assertEqual(len(github.asked), 3)


class MainTests(unittest.TestCase):
    def run_main(self, github, argv):
        out, err = io.StringIO(), io.StringIO()
        original = github_api.from_environment
        github_api.from_environment = lambda: (github, REPO)
        try:
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                code = thanks.main(argv)
        finally:
            github_api.from_environment = original
        return code, out.getvalue(), err.getvalue()

    def test_prints_the_line(self):
        github = FakeRange(["v1.0.0", "v1.1.0"], COMMITS)
        code, out, _ = self.run_main(github, ["--repo", REPO, "--head", "h", "--tag", "v1.1.0"])
        self.assertEqual((code, out), (0, "Thanks to @alice, @bob, @carol and Dan Nolink.\n"))

    def test_a_github_error_never_fails_the_release(self):
        class Broken(FakeRange):
            def request(self, *args, **kwargs):
                raise github_api.GitHubError(500, "boom", "/x")

        code, out, err = self.run_main(Broken([], []), ["--repo", REPO, "--head", "h", "--base", "v1"])
        self.assertEqual((code, out), (0, ""))
        self.assertIn("no thanks line", err)

    def test_nobody_prints_nothing(self):
        github = FakeRange([], [commit("b", "github-actions[bot]", "2026-12-01", user_type="Bot")])
        self.assertEqual(self.run_main(github, ["--repo", REPO, "--head", "h", "--base", "v1"])[1], "")


if __name__ == "__main__":
    unittest.main()
