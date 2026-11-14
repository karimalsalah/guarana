#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for tools/ci/pr_gate.py on canned gh and git answers (no network, no git, no gh):
python -I tools/ci/tests/test_pr_gate.py
"""

import contextlib
import io
import json
import os
import re
import sys
import unittest
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
CI = os.path.dirname(HERE)
ROOT = Path(CI).parent.parent
sys.path.insert(0, CI)  # -I leaves the script's directory off sys.path.
sys.path.insert(0, str(ROOT / "tools" / "ingest" / "tests"))

import pr_gate  # noqa: E402
import yaml_subset  # noqa: E402

LINUX, WINDOWS = pr_gate.BUILD_JOBS
CHANGES = pr_gate.CHANGES_JOB
REGISTRY = "Check the switch registry"
HEAD = "a" * 40
MAIN_TIP = "b" * 40
MAIN_PREV = "c" * 40
MAIN_OLD = "d" * 40
RESOLVE = "e" * 40


def run(name, conclusion="success", status="completed", ident=1):
    return {"id": ident, "name": name, "status": status, "conclusion": conclusion}


def green():
    return [run(CHANGES), run(LINUX, ident=2), run(WINDOWS, ident=3)]


def red():
    return [run(CHANGES), run(LINUX, "failure", ident=2), run(WINDOWS, ident=3)]


def docs_only():
    return [run(CHANGES), run(LINUX, "skipped", ident=2), run(WINDOWS, "skipped", ident=3)]


def building():
    return [run(CHANGES), run(LINUX, None, "in_progress", ident=2), run(WINDOWS, None, "queued", ident=3)]


def pull_request(**fields):
    pr = {"number": 7, "state": "OPEN", "isDraft": False, "headRefOid": HEAD, "baseRefName": "main",
          "mergeable": "MERGEABLE", "labels": []}
    pr.update(fields)
    return pr


class Fake:
    """Answers gh and git like the real ones, from tables; records each command."""

    def __init__(self, pr=..., checks=None, main=None, merge_tree=(1, "", ""), fetch=0):
        self.pr = pull_request() if pr is ... else pr
        self.checks = {HEAD: green()} if checks is None else checks
        # Newest first, as the commits API lists them.
        self.main = [(MAIN_TIP, green())] if main is None else main
        self.merge_tree = merge_tree
        self.fetch = fetch
        self.calls = []

    def __call__(self, argv):
        self.calls.append(list(argv))
        if argv[0] == "git":
            if argv[1] == "fetch":
                return self.fetch, "", "fatal" if self.fetch else ""
            if argv[1] == "merge-tree":
                return self.merge_tree
            raise AssertionError(argv)
        if argv[:3] == ["gh", "pr", "view"]:
            if self.pr is None:
                return 1, "", "no such pull request"
            return 0, json.dumps(self.pr), ""
        if argv[:2] == ["gh", "api"]:
            path = argv[2]
            if path.startswith("repos/o/r/commits?sha="):
                match = re.match(r"repos/o/r/commits\?sha=([^&]+)&per_page=(\d+)$", path)
                self.branch = match.group(1)
                return 0, json.dumps([{"sha": sha} for sha, _ in self.main]), ""
            match = re.match(r"repos/o/r/commits/([0-9a-f]+)/check-runs\?per_page=100$", path)
            if match:
                sha = match.group(1)
                runs = self.checks.get(sha)
                if runs is None:
                    runs = dict(self.main).get(sha)
                if runs is None:
                    return 1, "", "no commit " + sha
                return 0, json.dumps({"total_count": len(runs), "check_runs": runs}), ""
        raise AssertionError(argv)

    def git_calls(self):
        return [call for call in self.calls if call[0] == "git"]


def ask(fake, *extra):
    """Runs the command line against the fake: (exit status, stdout)."""
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        status = pr_gate.main(["7", "--repo", "o/r"] + list(extra), runner=fake)
    return status, out.getvalue()


def codes(text):
    return [line[2:].split(":")[0] for line in text.splitlines() if line.startswith("- ")]


class ChecksTests(unittest.TestCase):
    def test_classify(self):
        self.assertEqual(pr_gate.classify(run("x")), "pass")
        self.assertEqual(pr_gate.classify(run("x", "neutral")), "pass")
        self.assertEqual(pr_gate.classify(run("x", "skipped")), "skipped")
        for conclusion in ("failure", "cancelled", "timed_out", "action_required", "startup_failure", None):
            self.assertEqual(pr_gate.classify(run("x", conclusion)), "fail", conclusion)
        for status in ("queued", "in_progress", "waiting", "pending", None):
            self.assertEqual(pr_gate.classify(run("x", None, status)), "pending", status)

    def test_green(self):
        self.assertEqual(pr_gate.judge_checks(green())["state"], "green")

    def test_a_failing_build_wins_over_a_running_one(self):
        runs = [run(CHANGES), run(LINUX, "failure", ident=2), run(WINDOWS, None, "in_progress", ident=3)]
        verdict = pr_gate.judge_checks(runs)
        self.assertEqual(verdict["state"], "failing")
        self.assertEqual(verdict["failing"], [LINUX + " (failure)"])
        self.assertEqual(verdict["pending"], [WINDOWS])

    def test_running_and_unreported_checks_are_pending(self):
        self.assertEqual(pr_gate.judge_checks(building())["state"], "pending")
        verdict = pr_gate.judge_checks([run(CHANGES), run(LINUX, ident=2)])
        self.assertEqual(verdict["state"], "pending")
        self.assertEqual(verdict["missing"], [WINDOWS])
        self.assertEqual(pr_gate.judge_checks([])["missing"], list(pr_gate.REQUIRED_CHECKS))

    def test_documentation_only_changes_skip_the_builds_and_pass(self):
        self.assertEqual(pr_gate.judge_checks(docs_only())["state"], "skipped")

    def test_skipped_builds_after_a_failed_detection_do_not_pass(self):
        runs = [run(CHANGES, "failure"), run(LINUX, "skipped", ident=2), run(WINDOWS, "skipped", ident=3)]
        verdict = pr_gate.judge_checks(runs)
        self.assertEqual(verdict["state"], "failing")
        self.assertEqual(verdict["failing"], [CHANGES + " (failure)"])

    def test_the_newest_run_of_a_name_counts(self):
        runs = [run(CHANGES), run(LINUX, "failure", ident=2), run(LINUX, "success", ident=9), run(WINDOWS, ident=3)]
        self.assertEqual(pr_gate.judge_checks(runs)["state"], "green")
        runs = [run(CHANGES), run(LINUX, "success", ident=2), run(LINUX, "failure", ident=9), run(WINDOWS, ident=3)]
        self.assertEqual(pr_gate.judge_checks(runs)["state"], "failing")

    def test_the_switch_registry_counts_only_when_it_ran(self):
        self.assertEqual(pr_gate.judge_checks(green())["state"], "green")
        self.assertEqual(pr_gate.judge_checks(green() + [run(REGISTRY, "failure", ident=5)])["state"], "failing")
        self.assertEqual(pr_gate.judge_checks(green() + [run(REGISTRY, None, "queued", ident=5)])["state"], "pending")
        self.assertEqual(pr_gate.judge_checks(green() + [run(REGISTRY, ident=5)])["state"], "green")
        self.assertEqual(pr_gate.judge_checks(green() + [run(REGISTRY, "failure", ident=5)], conditional=())["state"],
                         "green")

    def test_other_checks_do_not_count(self):
        runs = green() + [run("Comment the test build", "failure", ident=8)]
        self.assertEqual(pr_gate.judge_checks(runs)["state"], "green")


class MergeTreeTests(unittest.TestCase):
    def test_parse(self):
        self.assertEqual(pr_gate.parse_merge_tree("0123abc\nCHANGELOG.md\nsrc/a b.cpp\n"), ["CHANGELOG.md", "src/a b.cpp"])
        self.assertEqual(pr_gate.parse_merge_tree("0123abc\nCHANGELOG.md\n\nAuto-merging x\nCONFLICT (content)\n"),
                         ["CHANGELOG.md"])
        self.assertEqual(pr_gate.parse_merge_tree("0123abc\n"), [])
        self.assertEqual(pr_gate.parse_merge_tree(""), [])


class GateTests(unittest.TestCase):
    def test_ready(self):
        status, text = ask(Fake())
        self.assertEqual((status, text), (0, "ready\n"))

    def test_a_clean_pull_request_never_runs_git(self):
        fake = Fake()
        ask(fake)
        self.assertEqual(fake.git_calls(), [])

    def test_the_gate_only_reads(self):
        fake = Fake(pr=pull_request(mergeable="CONFLICTING"), merge_tree=(1, "t\nCHANGELOG.md\n", ""))
        ask(fake)
        for call in fake.calls:
            if call[0] == "gh":
                self.assertIn(call[1], ("pr", "api"))
                if call[1] == "pr":
                    self.assertEqual(call[2], "view")
                for writing in ("-X", "--method", "-f", "-F", "--field", "--raw-field", "--input"):
                    self.assertNotIn(writing, call)
            else:
                self.assertIn(call[1], ("fetch", "merge-tree"))

    def test_draft(self):
        status, text = ask(Fake(pr=pull_request(isDraft=True)))
        self.assertEqual(status, 1)
        self.assertEqual(text.splitlines()[0], "not-ready")
        self.assertEqual(codes(text), ["draft"])

    def test_closed_and_merged(self):
        for state in ("CLOSED", "MERGED"):
            status, text = ask(Fake(pr=pull_request(state=state)))
            self.assertEqual((status, codes(text)), (1, ["not-open"]), state)

    def test_a_closed_pull_request_is_not_asked_about_its_merge(self):
        fake = Fake(pr=pull_request(state="MERGED", mergeable="CONFLICTING"))
        status, text = ask(fake)
        self.assertEqual((status, codes(text)), (1, ["not-open"]))
        self.assertEqual(fake.git_calls(), [])

    def test_checks_pending(self):
        status, text = ask(Fake(checks={HEAD: building()}))
        self.assertEqual((status, codes(text)), (1, ["checks-pending"]))
        self.assertIn(LINUX, text)

    def test_checks_not_reported(self):
        status, text = ask(Fake(checks={HEAD: []}))
        self.assertEqual((status, codes(text)), (1, ["checks-pending"]))
        self.assertIn("not reported", text)

    def test_checks_failing(self):
        status, text = ask(Fake(checks={HEAD: red()}))
        self.assertEqual((status, codes(text)), (1, ["checks-failing"]))
        self.assertIn(LINUX + " (failure)", text)

    def test_documentation_only_is_ready_with_a_note(self):
        status, text = ask(Fake(checks={HEAD: docs_only()}))
        self.assertEqual(status, 0)
        self.assertIn("note: the builds were skipped", text)

    def test_every_reason_is_listed(self):
        fake = Fake(pr=pull_request(isDraft=True), checks={HEAD: red()}, main=[(MAIN_TIP, red())])
        status, text = ask(fake)
        self.assertEqual((status, codes(text)), (1, ["draft", "checks-failing", "base-red"]))

    def test_main_red_blocks_a_pull_request_without_the_label(self):
        status, text = ask(Fake(main=[(MAIN_TIP, red())]))
        self.assertEqual((status, codes(text)), (1, ["base-red"]))
        self.assertIn("lacks the fixes-main label", text)
        self.assertIn(MAIN_TIP[:10], text)

    def test_main_red_lets_a_fixes_main_pull_request_through(self):
        pr = pull_request(labels=[{"name": "ci"}, {"name": "fixes-main"}])
        status, text = ask(Fake(pr=pr, main=[(MAIN_TIP, red())]))
        self.assertEqual(status, 0)
        self.assertIn("note: main is red", text)

    def test_a_fixes_main_label_does_not_excuse_its_own_checks(self):
        pr = pull_request(labels=[{"name": "fixes-main"}])
        status, text = ask(Fake(pr=pr, checks={HEAD: red()}, main=[(MAIN_TIP, red())]))
        self.assertEqual((status, codes(text)), (1, ["checks-failing"]))

    def test_a_documentation_only_tip_does_not_hide_a_red_main(self):
        fake = Fake(main=[(MAIN_TIP, docs_only()), (MAIN_PREV, red()), (MAIN_OLD, green())])
        status, text = ask(fake)
        self.assertEqual((status, codes(text)), (1, ["base-red"]))
        self.assertIn(MAIN_PREV[:10], text)

    def test_a_tip_still_building_does_not_hide_a_red_main(self):
        fake = Fake(main=[(MAIN_TIP, building()), (MAIN_PREV, red())])
        status, text = ask(fake)
        self.assertEqual((status, codes(text)), (1, ["base-red"]))

    def test_a_tip_still_building_over_a_green_main_is_a_note(self):
        fake = Fake(main=[(MAIN_TIP, building()), (MAIN_PREV, green())])
        status, text = ask(fake)
        self.assertEqual(status, 0)
        self.assertIn("still building", text)

    def test_a_red_tip_is_not_hidden_by_a_running_job(self):
        runs = [run(CHANGES), run(LINUX, "failure", ident=2), run(WINDOWS, None, "in_progress", ident=3)]
        status, text = ask(Fake(main=[(MAIN_TIP, runs), (MAIN_PREV, green())]))
        self.assertEqual((status, codes(text)), (1, ["base-red"]))

    def test_main_with_nothing_settled_is_a_note(self):
        status, text = ask(Fake(main=[(MAIN_TIP, docs_only()), (MAIN_PREV, [])]))
        self.assertEqual(status, 0)
        self.assertIn("could not find a settled", text)

    def test_the_base_branch_of_the_pull_request_is_the_one_read(self):
        fake = Fake(pr=pull_request(baseRefName="release/v1.0"))
        status, _ = ask(fake)
        self.assertEqual(status, 0)
        self.assertEqual(fake.branch, "release%2Fv1.0")

    def test_conflicts_in_the_union_files_only(self):
        tree = "tree\nCHANGELOG.md\ndocs/switches.md\ndist/nx2_switches.txt\n"
        status, text = ask(Fake(pr=pull_request(mergeable="CONFLICTING"), merge_tree=(1, tree, "")))
        self.assertEqual(status, 0)
        self.assertIn("note: conflicts only in union files", text)
        self.assertIn("keep both sides", text)

    def test_conflicts_outside_the_union_files(self):
        tree = "tree\nCHANGELOG.md\nsrc/video_core/a.cpp\nREADME.md\n"
        status, text = ask(Fake(pr=pull_request(mergeable="CONFLICTING"), merge_tree=(1, tree, "")))
        self.assertEqual((status, codes(text)), (1, ["conflicts"]))
        self.assertIn("src/video_core/a.cpp, README.md", text)
        self.assertNotIn("outside the union files: CHANGELOG.md", text)

    def test_conflicts_are_read_from_git_after_fetching_the_pull_request(self):
        fake = Fake(pr=pull_request(mergeable="CONFLICTING"), merge_tree=(1, "tree\nCHANGELOG.md\n", ""))
        ask(fake, "--remote", "upstream")
        fetch, merge_tree = fake.git_calls()
        self.assertEqual(fetch[:5], ["git", "fetch", "--quiet", "--no-tags", "upstream"])
        self.assertIn("+refs/pull/7/head:refs/pr-gate/pr-7", fetch)
        self.assertIn("+refs/heads/main:refs/pr-gate/base", fetch)
        self.assertEqual(merge_tree[:5], ["git", "merge-tree", "--write-tree", "--name-only", "--no-messages"])
        self.assertEqual(merge_tree[5:], ["refs/pr-gate/base", "refs/pr-gate/pr-7"])

    def test_conflicts_git_cannot_list_block(self):
        cases = {"fetch fails": Fake(pr=pull_request(mergeable="CONFLICTING"), fetch=128),
                 "old git": Fake(pr=pull_request(mergeable="CONFLICTING"), merge_tree=(129, "", "usage")),
                 "no names": Fake(pr=pull_request(mergeable="CONFLICTING"), merge_tree=(1, "tree\n", "")),
                 "stale GitHub": Fake(pr=pull_request(mergeable="CONFLICTING"), merge_tree=(0, "tree\n", ""))}
        for label, fake in cases.items():
            status, text = ask(fake)
            self.assertEqual((status, codes(text)), (1, ["conflicts"]), label)

    def test_mergeability_not_computed_yet(self):
        status, text = ask(Fake(pr=pull_request(mergeable="UNKNOWN")))
        self.assertEqual((status, codes(text)), (1, ["mergeability-unknown"]))

    def test_the_resolve_commit_checks_are_read_with_sha(self):
        fake = Fake(checks={HEAD: red(), RESOLVE: green()})
        status, _ = ask(fake)
        self.assertEqual(status, 1)
        status, text = ask(fake, "--sha", RESOLVE)
        self.assertEqual((status, text), (0, "ready\n"))
        status, _ = ask(Fake(checks={HEAD: green(), RESOLVE: red()}), "--sha", RESOLVE)
        self.assertEqual(status, 1)

    def test_require_adds_a_job(self):
        status, text = ask(Fake(), "--require", "Extra job")
        self.assertEqual((status, codes(text)), (1, ["checks-pending"]))
        self.assertIn("Extra job", text)
        checks = {HEAD: green() + [run("Extra job", ident=9)]}
        self.assertEqual(ask(Fake(checks=checks, main=[(MAIN_TIP, green() + [run("Extra job", ident=9)])]),
                             "--require", "Extra job")[0], 0)

    def test_a_required_job_is_not_asked_of_main(self):
        checks = {HEAD: green() + [run("Extra job", ident=9)]}
        status, text = ask(Fake(checks=checks, main=[(MAIN_TIP, red())]), "--require", "Extra job")
        self.assertEqual((status, codes(text)), (1, ["base-red"]))
        status, text = ask(Fake(checks=checks, main=[(MAIN_TIP, green())]), "--require", "Extra job")
        self.assertEqual((status, text), (0, "ready\n"))

    def test_a_job_named_twice_is_judged_once(self):
        checks = {HEAD: [run(CHANGES), run(LINUX, ident=2), run(WINDOWS, ident=3), run(REGISTRY, "failure", ident=4)]}
        status, text = ask(Fake(checks=checks), "--require", REGISTRY)
        self.assertEqual((status, codes(text)), (1, ["checks-failing"]))
        self.assertEqual(text.count(REGISTRY), 1)

    def test_gh_failures_are_not_ready_and_say_why(self):
        status, text = ask(Fake(pr=None))
        self.assertEqual(status, 1)
        self.assertEqual(text.splitlines()[0], "not-ready")
        self.assertEqual(codes(text), ["gh-error"])
        self.assertIn("no such pull request", text)
        status, text = ask(Fake(checks={}, main=[]))
        self.assertEqual((status, codes(text)), (1, ["gh-error"]))

    def test_gh_missing(self):
        def missing(argv):
            raise pr_gate.GateError("gh is not installed or not on PATH")

        status, text = ask(missing)
        self.assertEqual((status, codes(text)), (1, ["gh-error"]))

    def test_the_repository_comes_from_the_environment_or_gh(self):
        seen = []

        def runner(argv):
            seen.append(argv)
            if argv[:3] == ["gh", "repo", "view"]:
                return 0, "o/r\n", ""
            return Fake()(argv)

        out = io.StringIO()
        saved = os.environ.pop("GITHUB_REPOSITORY", None)
        try:
            with contextlib.redirect_stdout(out):
                self.assertEqual(pr_gate.main(["7"], runner=runner), 0)
            self.assertEqual(seen[0][:3], ["gh", "repo", "view"])
            os.environ["GITHUB_REPOSITORY"] = "o/r"
            seen.clear()
            with contextlib.redirect_stdout(out):
                self.assertEqual(pr_gate.main(["7"], runner=runner), 0)
            self.assertNotEqual(seen[0][:3], ["gh", "repo", "view"])
        finally:
            os.environ.pop("GITHUB_REPOSITORY", None)
            if saved is not None:
                os.environ["GITHUB_REPOSITORY"] = saved

    def test_json(self):
        pr = pull_request(isDraft=True, mergeable="CONFLICTING", labels=[{"name": "fixes-main"}])
        fake = Fake(pr=pr, checks={HEAD: building()}, main=[(MAIN_TIP, red())],
                    merge_tree=(1, "tree\nCHANGELOG.md\nREADME.md\n", ""))
        status, text = ask(fake, "--json")
        self.assertEqual(status, 1)
        answer = json.loads(text)
        self.assertEqual(answer["pr"], 7)
        self.assertIs(answer["ready"], False)
        self.assertEqual([r["code"] for r in answer["reasons"]], ["draft", "checks-pending", "conflicts"])
        self.assertEqual(answer["head"], HEAD)
        self.assertEqual(answer["base"], "main")
        self.assertEqual(answer["checks"]["pending"], [LINUX, WINDOWS])
        self.assertEqual(answer["base_state"]["state"], "red")
        self.assertEqual(answer["conflicts"], ["CHANGELOG.md", "README.md"])
        status, text = ask(Fake(), "--json")
        answer = json.loads(text)
        self.assertEqual((status, answer["ready"], answer["reasons"], answer["conflicts"]), (0, True, [], None))

    def test_the_arguments_are_checked(self):
        for argv in (["0"], ["x"], ["-3"], ["7", "--sha", "main"], ["7", "--sha", "zz12345"], []):
            with contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as raised:
                    pr_gate.main(argv, runner=Fake())
            self.assertEqual(raised.exception.code, 2, argv)


class WiringTests(unittest.TestCase):
    """The names and rules the gate relies on exist in the files it is about."""

    def read(self, *parts):
        return ROOT.joinpath(*parts).read_text(encoding="utf-8")

    def test_the_required_checks_are_jobs_of_build_check(self):
        workflow = self.read(".github", "workflows", "build-check.yml")
        for name in pr_gate.REQUIRED_CHECKS:
            self.assertRegex(workflow, r"(?m)^    name: %s\s*$" % re.escape(name))

    def test_the_conditional_checks_are_jobs_of_switch_registry(self):
        workflow = self.read(".github", "workflows", "switch-registry.yml")
        for name in pr_gate.CONDITIONAL_CHECKS:
            self.assertRegex(workflow, r"(?m)^    name: %s\s*$" % re.escape(name))

    def test_build_check_runs_in_the_merge_queue_and_still_on_pull_requests_and_pushes(self):
        workflow = self.read(".github", "workflows", "build-check.yml")
        triggers = workflow.split("\npermissions:")[0]
        for needed in ("\n  pull_request:\n", "\n  merge_group:\n", "\n  push:\n", "\n  workflow_dispatch:\n"):
            self.assertIn(needed, triggers.replace("\r\n", "\n"))
        self.assertIn('      - "exp/**"', triggers.replace("\r\n", "\n"))

    def test_the_legacy_workflow_runs_this_suite_when_the_gate_changes(self):
        workflow = self.read(".github", "workflows", "legacy.yml")
        self.assertIn('      - "tools/ci/pr_gate.py"', workflow.replace("\r\n", "\n"))

    def test_the_union_files_are_the_ones_the_docs_name(self):
        guide = self.read("docs", "agents", "README.md")
        for path in pr_gate.UNION_FILES:
            self.assertIn(path, guide)

    def test_the_label_exists(self):
        labels = {label["name"] for label in yaml_subset.load(self.read(".github", "labels.yml"))}
        self.assertIn(pr_gate.FIXES_LABEL, labels)

    def test_the_docs_describe_the_gate(self):
        for parts in (("docs", "LANES.md"), ("docs", "agents", "README.md")):
            text = self.read(*parts)
            self.assertIn("tools/ci/pr_gate.py", text, parts)
            self.assertIn("merge_group", text, parts)
            self.assertIn(pr_gate.FIXES_LABEL, text, parts)


if __name__ == "__main__":
    unittest.main()
