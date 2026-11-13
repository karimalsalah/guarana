#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for tools/ci/legacy.py with fake GitHub data: python -I tools/ci/tests/test_legacy.py

The publish tests run git against a bare repository in a temporary directory.
"""

import contextlib
import io
import json
import os
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
CI = os.path.dirname(HERE)
ROOT = Path(CI).parent.parent
sys.path.insert(0, CI)  # -I leaves the script's directory off sys.path.
sys.path.insert(0, str(ROOT / "tools" / "ingest" / "tests"))

import legacy  # noqa: E402
import yaml_subset  # noqa: E402
from fakes import REPO, FakeGitHub, git  # noqa: E402

CLAUDE = "Claude Opus 5.5 <noreply@anthropic.com>"
CAPY = "capy-ai[bot] <230910855+capy-ai[bot]@users.noreply.github.com>"


def commit(sha, login, date, name=None, email=None, message="area: change", parents=1, user_type="User", trailers=()):
    body = message + ("\n\n" + "\n".join(trailers) if trailers else "")
    return {
        "sha": sha,
        "commit": {"message": body, "author": {"name": name or (login or "Someone"),
                                                "email": email or f"{login}@example.invalid", "date": date + "T10:00:00Z"},
                   "committer": {"name": "x", "email": "x@example.invalid", "date": date + "T10:00:00Z"}},
        "author": {"login": login, "type": user_type} if login else None,
        "parents": [{"sha": "p"}] * parents,
    }


HISTORY = [
    commit("a1", "alice", "2026-11-14", email="alice@real.example", trailers=[f"Co-authored-by: {CLAUDE}"]),
    commit("a2", "alice", "2026-11-15", trailers=[f"Co-authored-by: {CLAUDE}", "Co-authored-by: Claude Sonnet 5.5 <noreply@anthropic.com>"]),
    commit("b1", "bob", "2026-11-13", trailers=[f"Co-authored-by: {CAPY}", "Co-authored-by: Carol Danvers <12345+carol@users.noreply.github.com>"]),
    commit("c1", "carol", "2026-11-20", message="vulkan: fix"),
    commit("m1", "alice", "2026-11-21", message="Merge pull request #3", parents=2,
           trailers=["Co-authored-by: Ghost <ghost@real.example>"]),
    commit("d1", "dave", "2026-11-22", trailers=["Co-authored-by: Alice A <alice@real.example>",
                                                 "Co-authored-by: Erin Unlinked <erin@nowhere.example>",
                                                 "Assisted-by: Codex", "Assisted-by: Claude Code"]),
    commit("bot1", "github-actions[bot]", "2026-11-23", user_type="Bot"),
    commit("anon", None, "2026-11-24", name="Frank NoAccount", email="frank@nowhere.example"),
    commit("self", "dave", "2026-11-25", trailers=["Co-authored-by: dave <dave@example.invalid>"]),
]


class FakeCommits(FakeGitHub):
    def __init__(self, commits, **kwargs):
        super().__init__(**kwargs)
        self.commits = commits

    def request(self, method, path, body=None, params=None, accept=None):
        if method == "GET" and path == f"/repos/{self.repo}/commits":
            self.calls.append((method, path, None))
            self.since = (params or {}).get("since")
            return json.loads(json.dumps(self.commits)), ""
        return super().request(method, path, body, params, accept)


def people(doc):
    return {entry["login"] or entry["name"]: entry for entry in doc["people"]}


class LedgerTests(unittest.TestCase):
    def setUp(self):
        self.doc = legacy.document(legacy.build_ledger(HISTORY), REPO, legacy.SINCE)
        self.people = people(self.doc)

    def test_authors_are_counted_by_login(self):
        self.assertEqual(self.people["alice"]["commits"], 2)
        self.assertEqual(self.people["bob"]["commits"], 1)
        self.assertEqual(self.people["carol"]["commits"], 1)
        self.assertEqual(self.people["dave"]["commits"], 2)

    def test_merge_commits_count_for_nothing(self):
        self.assertEqual(self.doc["commits"], len(HISTORY) - 1)
        self.assertNotIn("Ghost", self.people)
        self.assertEqual(self.people["alice"]["coauthored"], 1)  # only dave's trailer

    def test_co_authors_are_found_by_noreply_address_and_by_a_known_address(self):
        self.assertEqual(self.people["carol"]["coauthored"], 1)  # 12345+carol@users.noreply.github.com
        self.assertEqual(self.people["alice"]["coauthored"], 1)  # alice@real.example is alice's own commit address

    def test_a_human_without_a_login_is_listed_by_name_only(self):
        self.assertEqual(self.people["Erin Unlinked"]["login"], None)
        self.assertEqual(self.people["Erin Unlinked"]["coauthored"], 1)
        self.assertEqual(self.people["Frank NoAccount"]["commits"], 1)

    def test_naming_yourself_is_not_a_co_author(self):
        self.assertEqual(self.people["dave"]["coauthored"], 0)

    def test_ai_and_bots_are_tools_not_people(self):
        tools = {tool["name"]: tool["commits"] for tool in self.doc["tools"]}
        self.assertEqual(tools["Claude"], 3)  # a1, a2 (two lines, once), d1's Assisted-by
        self.assertEqual(tools["Capy"], 1)
        self.assertEqual(tools["OpenAI Codex / ChatGPT"], 1)
        self.assertEqual(tools["GitHub Actions"], 1)
        for name in ("Claude Opus 5.5", "Claude Sonnet 5.5", "capy-ai[bot]", "github-actions[bot]", "Codex"):
            self.assertNotIn(name, self.people)
        self.assertNotIn("github-actions[bot]", self.people)

    def test_people_come_in_the_order_they_first_appeared(self):
        order = [entry["login"] or entry["name"] for entry in self.doc["people"]]
        self.assertEqual(order[:3], ["bob", "carol", "alice"])
        self.assertEqual(self.doc["as_of"], "2026-11-25")

    def test_an_unsafe_login_never_becomes_a_link(self):
        bad = [commit("x1", 'evil"><script>', "2026-11-14", name="Evil Person")]
        doc = legacy.document(legacy.build_ledger(bad), REPO, legacy.SINCE)
        html = legacy.readme_section(doc) + legacy.contributors_md(doc)
        self.assertNotIn("<script>", html)
        self.assertEqual([entry["login"] for entry in doc["people"]], [None])

    def test_no_address_is_written_anywhere(self):
        files = legacy.outputs(self.doc, "intro\n" + legacy.START + "\n" + legacy.END + "\n")
        text = "\n".join(files.values())
        for fragment in ("@", "example.invalid", "real.example", "nowhere.example", "anthropic.com"):
            self.assertNotIn(fragment, text.replace("@users", "") if fragment == "@" else text, fragment)

    def test_output_is_stable(self):
        again = legacy.document(legacy.build_ledger(list(reversed(HISTORY))), REPO, legacy.SINCE)
        self.assertEqual(legacy.dump(again), legacy.dump(self.doc))
        self.assertTrue(legacy.dump(self.doc).endswith("}\n"))


class RenderTests(unittest.TestCase):
    def setUp(self):
        self.doc = legacy.document(legacy.build_ledger(HISTORY), REPO, legacy.SINCE)

    def test_contributors_md_lists_people_and_tools_apart(self):
        text = legacy.contributors_md(self.doc)
        people, tools = text.split("## People")[1].split("## Tools")
        self.assertIn("[alice](https://github.com/alice)", people)
        self.assertIn("Erin Unlinked", people)
        self.assertNotIn("Claude", people)
        self.assertIn("| Claude | 3 |", tools)
        self.assertIn("since 2026-11-13", text)
        self.assertIn("CREDITS.md", text)

    def test_names_cannot_break_the_table(self):
        doc = {"repo": REPO, "since": "2026-11-13", "commits": 1, "tools": [],
               "people": [{"login": None, "name": "Pipe | <b>x</b>\nnext", "commits": 1, "coauthored": 0, "first": "2026-11-14"}]}
        text = legacy.contributors_md(doc)
        row = [line for line in text.split("\n") if "Pipe" in line][0]
        self.assertNotIn("<b>", row)
        self.assertEqual(row.count("|"), 5)

    def test_readme_section_has_avatars_a_count_and_the_links(self):
        section = legacy.readme_section(self.doc)
        self.assertTrue(section.startswith(legacy.START) and section.endswith(legacy.END))
        self.assertIn('src="https://github.com/alice.png?size=96"', section)
        self.assertIn("CONTRIBUTORS.md", section)
        self.assertIn("karimalsalah.github.io/guarana/legacy.html", section)
        self.assertNotIn("Erin", section)  # no login, no avatar
        self.assertIn("6 people credited for good", section)

    def test_readme_avatars_are_limited(self):
        many = [commit(f"s{i}", f"user{i}", "2026-11-14") for i in range(100)]
        doc = legacy.document(legacy.build_ledger(many), REPO, legacy.SINCE)
        section = legacy.readme_section(doc)
        self.assertEqual(section.count("<img "), legacy.README_AVATARS)
        self.assertIn("40 of them not shown here", section)

    def test_an_empty_wall_still_invites(self):
        doc = legacy.document(legacy.build_ledger([]), REPO, legacy.SINCE)
        self.assertIn("first name on it can be yours", legacy.readme_section(doc))
        self.assertIn("Nobody yet", legacy.contributors_md(doc))
        self.assertIn("No tool has been named yet", legacy.contributors_md(doc))

    def test_apply_readme_replaces_only_between_the_markers_and_keeps_line_endings(self):
        text = f"top\r\n{legacy.START}\r\nold\r\n{legacy.END}\r\nbottom\r\n"
        out = legacy.apply_readme(text, legacy.START + "\nnew\n" + legacy.END)
        self.assertEqual(out, f"top\r\n{legacy.START}\r\nnew\r\n{legacy.END}\r\nbottom\r\n")
        self.assertIsNone(legacy.apply_readme("no markers", "x"))
        self.assertIsNone(legacy.apply_readme(f"{legacy.END} {legacy.START}", "x"))
        again = legacy.apply_readme(out, legacy.START + "\nnew\n" + legacy.END)
        self.assertEqual(again, out)

    def test_outputs_leave_readme_out_without_markers(self):
        self.assertEqual(set(legacy.outputs(self.doc, "no markers")), {legacy.DATA_PATH, legacy.CONTRIBUTORS_PATH})
        self.assertEqual(set(legacy.outputs(self.doc, legacy.START + legacy.END)),
                         {legacy.DATA_PATH, legacy.CONTRIBUTORS_PATH, legacy.README_PATH})

    def test_the_json_has_what_the_page_reads(self):
        data = json.loads(legacy.dump(self.doc))
        self.assertEqual(data["schema_version"], 1)
        for key in ("repo", "since", "as_of", "commits", "people", "tools"):
            self.assertIn(key, data)
        self.assertEqual(set(data["people"][0]), {"login", "name", "commits", "coauthored", "first"})


class BuildTests(unittest.TestCase):
    def test_build_writes_the_three_files_and_only_when_they_change(self):
        with tempfile.TemporaryDirectory() as temp:
            Path(temp, "README.md").write_text(f"# x\r\n{legacy.START}\r\n{legacy.END}\r\n", encoding="utf-8", newline="")
            github = FakeCommits(HISTORY)
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                from unittest import mock
                with mock.patch.object(legacy.github_api, "GitHub", return_value=github):
                    self.assertEqual(legacy.main(["build", "--repo", REPO, "--root", temp]), 0)
                    first = json.loads(out.getvalue().splitlines()[-1])
                    out.truncate(0), out.seek(0)
                    self.assertEqual(legacy.main(["build", "--repo", REPO, "--root", temp]), 0)
                    second = json.loads(out.getvalue().splitlines()[-1])
            self.assertEqual(set(first["written"]), {"README.md", "CONTRIBUTORS.md", "site/data/legacy.json"})
            self.assertEqual(second["written"], [])
            self.assertEqual(github.since, legacy.SINCE)
            readme = Path(temp, "README.md").read_bytes().decode("utf-8")
            self.assertIn("alice.png", readme)
            self.assertNotIn("\r\n\n", readme.replace("\r\n", "\r\n"))
            self.assertEqual(readme.count("\n"), readme.count("\r\n"))


class Origin:
    """A bare repository whose main holds README.md with the markers, and a clone of it."""

    def __init__(self, root, readme):
        self.bare, self.work = os.path.join(root, "origin.git"), os.path.join(root, "work")
        git("init", "-q", "--bare", "-b", "main", self.bare)
        git("init", "-q", "-b", "main", self.work)
        git("remote", "add", "origin", self.bare, cwd=self.work)
        Path(self.work, "README.md").write_text(readme, encoding="utf-8", newline="")
        git("add", "README.md", cwd=self.work)
        git("commit", "-q", "-m", "seed", cwd=self.work)
        git("push", "-q", "origin", "main", cwd=self.work)

    def head(self, branch):
        out = git("ls-remote", self.bare, f"refs/heads/{branch}")
        return out.split()[0] if out else None

    def show(self, ref, path):
        return git("show", f"{ref}:{path}", cwd=self.bare)

    def advance(self, files):
        """What merging the pull request does: main gets the files."""
        git("pull", "-q", "origin", "main", cwd=self.work)
        for path, content in files.items():
            target = Path(self.work, path)
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(content, encoding="utf-8", newline="")
            git("add", path, cwd=self.work)
        git("commit", "-q", "-m", "merge the Hall of fame", cwd=self.work)
        git("push", "-q", "origin", "main", cwd=self.work)


class PublishTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.origin = Origin(self.temp.name, f"# Guarana\n{legacy.START}\nold\n{legacy.END}\nmore\n")
        self.github = FakeCommits(HISTORY)
        self.git = __import__("ingest").Git(self.work)
        self.log = []

    @property
    def work(self):
        return self.origin.work

    def publish(self, **kwargs):
        return legacy.publish(self.github, self.git, REPO, "main", log=self.log.append, **kwargs)

    def test_opens_a_pull_request_with_all_three_files_in_one_commit(self):
        summary = self.publish()
        self.assertTrue(summary["pushed"])
        self.assertEqual(set(summary["changed"]), {"README.md", "CONTRIBUTORS.md", "site/data/legacy.json"})
        head = self.origin.head(legacy.BRANCH)
        self.assertTrue(head)
        self.assertIn("alice", self.origin.show(head, "CONTRIBUTORS.md"))
        self.assertIn("alice.png", self.origin.show(head, "README.md"))
        self.assertIn("more", self.origin.show(head, "README.md"))  # the rest of the README is main's
        self.assertEqual(json.loads(self.origin.show(head, "site/data/legacy.json"))["repo"], REPO)
        self.assertEqual(self.git.parents(head), [self.origin.head("main")])
        pulls = list(self.github.pulls.values())
        self.assertEqual(len(pulls), 1)
        self.assertEqual(pulls[0]["title"], legacy.PULL_TITLE)
        self.assertTrue(pulls[0]["body"].startswith(legacy.PR_MARKER))
        self.assertEqual(pulls[0]["head"]["ref"], legacy.BRANCH)
        self.assertNotRegex(pulls[0]["body"], r"(?<![`\w])@\w")  # nobody is pinged

    def test_a_second_run_changes_nothing(self):
        self.publish()
        head = self.origin.head(legacy.BRANCH)
        writes = len(self.github.writes())
        summary = self.publish()
        self.assertFalse(summary["pushed"])
        self.assertEqual(self.origin.head(legacy.BRANCH), head)
        self.assertEqual(len(self.github.pulls), 1)
        self.assertEqual(len(self.github.writes()), writes)

    def test_new_history_updates_the_same_pull_request(self):
        self.publish()
        self.github.commits = HISTORY + [commit("n1", "newcomer", "2026-12-01")]
        summary = self.publish()
        self.assertTrue(summary["pushed"])
        self.assertEqual(len(self.github.pulls), 1)
        self.assertIn("newcomer", self.origin.show(self.origin.head(legacy.BRANCH), "CONTRIBUTORS.md"))

    def test_when_main_has_the_output_the_pull_request_and_branch_go(self):
        self.publish()
        head = self.origin.head(legacy.BRANCH)
        files = {path: self.git.show(head, path) for path in ("README.md", "CONTRIBUTORS.md", "site/data/legacy.json")}
        self.origin.advance(files)
        summary = self.publish()
        self.assertEqual(summary["changed"], [])
        self.assertEqual(summary.get("closed"), list(self.github.pulls)[0])
        self.assertEqual(list(self.github.pulls.values())[0]["state"], "closed")
        self.assertIsNone(self.origin.head(legacy.BRANCH))

    def test_dry_run_touches_nothing(self):
        summary = self.publish(dry_run=True)
        self.assertFalse(summary["pushed"])
        self.assertIsNone(self.origin.head(legacy.BRANCH))
        self.assertEqual(self.github.writes(), [])
        self.assertTrue(any("[dry run]" in line for line in self.log))

    def test_a_readme_without_markers_still_gets_the_other_files(self):
        origin = Origin(os.path.join(self.temp.name, "second") if os.makedirs(os.path.join(self.temp.name, "second")) is None else "",
                        "# Guarana\nno markers\n")
        git_ = __import__("ingest").Git(origin.work)
        summary = legacy.publish(self.github, git_, REPO, "main", log=self.log.append)
        self.assertEqual(set(summary["changed"]), {"CONTRIBUTORS.md", "site/data/legacy.json"})
        self.assertTrue(any("markers" in line for line in self.log))

    def test_a_forbidden_pull_request_says_how_to_allow_it(self):
        self.github.forbid_pull_create = True
        with self.assertRaises(legacy.PublishError) as raised:
            self.publish()
        self.assertIn("Allow GitHub Actions to create pull requests", str(raised.exception))
        self.assertTrue(self.origin.head(legacy.BRANCH))  # the branch has the files


def read(*parts):
    return (ROOT.joinpath(*parts)).read_text(encoding="utf-8")


class WorkflowTests(unittest.TestCase):
    def setUp(self):
        self.text = read(".github", "workflows", "legacy.yml")
        self.doc = yaml_subset.load(self.text)

    def test_triggers(self):
        on = self.doc["on"]
        self.assertEqual(on["schedule"], [{"cron": "17 4 * * *"}])
        self.assertEqual(on["workflow_dispatch"]["inputs"]["dry_run"]["type"], "boolean")
        self.assertIn("tools/ci/legacy.py", on["pull_request"]["paths"])

    def test_permissions_are_per_job(self):
        self.assertEqual(self.doc["permissions"], {})
        jobs = self.doc["jobs"]
        self.assertEqual(jobs["test"]["permissions"], {"contents": "read"})
        self.assertEqual(jobs["publish"]["permissions"], {"contents": "write", "pull-requests": "write"})

    def test_one_publish_at_a_time(self):
        self.assertEqual(self.doc["jobs"]["publish"]["concurrency"],
                         {"group": "legacy-publish", "cancel-in-progress": "false"})

    def test_steps_use_only_checkout_and_python_and_the_repositorys_token(self):
        self.assertNotIn("secrets.", self.text)
        for job in self.doc["jobs"].values():
            self.assertEqual(job["runs-on"], "ubuntu-24.04")
            for step in job["steps"]:
                self.assertEqual(len({"uses", "run"} & set(step)), 1)
                if "uses" in step:
                    self.assertEqual(step["uses"], "actions/checkout@v5")
                else:
                    self.assertTrue(step["run"].startswith("python3 -I "), step["run"])
                    self.assertNotIn("${{", step["run"])
                for key, value in (step.get("env") or {}).items():
                    if "TOKEN" in key:
                        self.assertEqual(value, "${{ github.token }}")

    def test_the_scripts_and_tests_it_runs_exist(self):
        for job in self.doc["jobs"].values():
            for step in job["steps"]:
                for path in re.findall(r"tools/ci/[\w./-]+\.py", step.get("run", "")):
                    self.assertTrue((ROOT / path).exists(), path)

    def test_the_publish_job_tests_before_it_publishes(self):
        runs = [step["run"] for step in self.doc["jobs"]["publish"]["steps"] if "run" in step]
        self.assertEqual(runs, ["python3 -I tools/ci/tests/test_legacy.py", "python3 -I tools/ci/legacy.py publish"])


class RepositoryTests(unittest.TestCase):
    def test_the_readme_has_the_markers_exactly_once(self):
        text = read("README.md")
        self.assertEqual(text.count(legacy.START), 1)
        self.assertEqual(text.count(legacy.END), 1)
        self.assertIsNotNone(legacy.apply_readme(text, legacy.START + "\nx\n" + legacy.END))

    def test_the_page_reads_the_file_the_script_writes(self):
        page = read("site", "legacy.html")
        self.assertIn('data-src="data/legacy.json"', page)
        self.assertTrue((ROOT / legacy.DATA_PATH).exists())
        data = json.loads(read(legacy.DATA_PATH))
        self.assertEqual(data["schema_version"], legacy.SCHEMA_VERSION)
        for forbidden in ("innerHTML", "outerHTML", "insertAdjacentHTML", "document.write", "eval(", "new Function"):
            self.assertNotIn(forbidden, page)
        self.assertTrue((ROOT / legacy.CONTRIBUTORS_PATH).exists())

    def test_the_committed_files_match_what_the_script_would_write_from_its_own_data(self):
        data = json.loads(read(legacy.DATA_PATH))
        self.assertEqual(read(legacy.CONTRIBUTORS_PATH).replace("\r\n", "\n"), legacy.contributors_md(data))
        self.assertEqual(read(legacy.DATA_PATH).replace("\r\n", "\n"), legacy.dump(data))
        readme = read("README.md").replace("\r\n", "\n")
        self.assertEqual(legacy.apply_readme(readme, legacy.readme_section(data)), readme)


if __name__ == "__main__":
    unittest.main()
