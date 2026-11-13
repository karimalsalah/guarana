#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The first contribution: a /claim that Git Bash rewrote into a Windows path gets an answer
(agent-claims.yml), a first merged pull request or first device report gets one thank-you
(welcome.yml), and the docs teach the forms of the claim command that work in every shell.

The structural checks read the files. Where Node.js is on the PATH (it is on GitHub's Ubuntu
runners), the workflows' scripts also run against a fake API, as actions/github-script runs them.

  python -I tools/ci/tests/test_contributor_flow.py
"""

import json
import os
import re
import shutil
import subprocess
import sys
import unittest
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = Path(HERE).parent.parent.parent
sys.path.insert(0, str(ROOT / "tools" / "ingest" / "tests"))  # -I leaves the script's directory off sys.path.

import yaml_subset  # noqa: E402

NODE = shutil.which("node")
REPO = {"owner": "karimalsalah", "repo": "guarana"}

# Runs one github-script body with a fake `github` that records every call, and prints the calls.
HARNESS = r"""
const input = JSON.parse(require("fs").readFileSync(0, "utf8"));
const calls = [];
const replies = { addAssignees: (args) => ({ assignees: args.assignees.map((login) => ({ login })) }) };
const call = (name) => async (args) => {
  calls.push({ name, args });
  return { data: (replies[name] || (() => ({})))(args) };
};
const listing = (name) => Object.assign(call(name), { listing: name });
const github = {
  rest: {
    issues: {
      get: async () => ({ data: input.issue }),
      listComments: listing("listComments"),
      listForRepo: listing("listForRepo"),
      createComment: call("createComment"),
      addLabels: call("addLabels"),
      addAssignees: call("addAssignees"),
      removeLabel: call("removeLabel"),
      removeAssignees: call("removeAssignees"),
    },
  },
  paginate: async (method, args) => {
    calls.push({ name: method.listing, args });
    return input.lists[method.listing] || [];
  },
};
const context = { repo: input.repo, payload: input.payload };
const core = { info() {}, warning() {} };
const AsyncFunction = (async () => {}).constructor;
new AsyncFunction("github", "context", "core", input.script)(github, context, core).then(
  () => process.stdout.write(JSON.stringify(calls)),
  (error) => { process.stderr.write(String(error && error.stack || error)); process.exit(1); },
);
"""


def read(*parts):
    return ROOT.joinpath(*parts).read_text(encoding="utf-8").replace("\r\n", "\n")


def workflow(name):
    text = read(".github", "workflows", name)
    return text, yaml_subset.load(text)


def run(script, payload, issue=None, **lists):
    feed = {"script": script, "payload": payload, "issue": issue, "repo": REPO, "lists": lists}
    done = subprocess.run([NODE, "-e", HARNESS], input=json.dumps(feed), capture_output=True,
                          text=True, encoding="utf-8", timeout=60)
    if done.returncode:
        raise AssertionError(done.stderr)
    return json.loads(done.stdout)


def named(calls, name):
    return [call["args"] for call in calls if call["name"] == name]


def user(login="newcomer"):
    return {"login": login, "type": "User"}


class MangledClaimTests(unittest.TestCase):
    """Git Bash turns `gh issue comment 36 --body "/claim"` into the comment "C:/Program Files/Git/claim"."""

    def setUp(self):
        self.text, doc = workflow("agent-claims.yml")
        self.job = doc["jobs"]["command"]
        self.script = self.job["steps"][0]["with"]["script"]
        source, flags = re.search(r"const MANGLED = /(.+)/([a-z]*);", self.script).groups()
        self.mangled = re.compile(source, re.I if "i" in flags else 0)

    def test_the_pattern_knows_a_rewritten_command_from_a_sentence(self):
        for body in ("C:/Program Files/Git/claim", "C:/Program Files/Git/unclaim", "D:/Tools/Git/claim",
                     "c:/program files/git/CLAIM", "C:/Program Files/Git/claim please", "C:/claim"):
            self.assertTrue(self.mangled.match(body), body)
        for body in ("/claim", "/unclaim", "I will claim this tomorrow", "C:/Program Files/Git/claimed",
                     "See https://example.com/claim", "Logs are in C:/temp, I will /claim later",
                     "Path C:/x/claim"):
            self.assertFalse(self.mangled.match(body), body)

    def test_the_job_starts_for_a_rewritten_command(self):
        condition = self.job["if"]
        self.assertIn("contains(github.event.comment.body, ':/')", condition)
        self.assertIn("contains(github.event.comment.body, '/claim')", condition)
        self.assertIn("contains(github.event.comment.body, '/unclaim')", condition)
        self.assertIn("!github.event.issue.pull_request", condition)
        self.assertIn("github.event.comment.user.type != 'Bot'", condition)

    @unittest.skipUnless(NODE, "Node.js is not on the PATH")
    def test_a_rewritten_claim_gets_one_line_and_claims_nothing(self):
        for word in ("claim", "unclaim"):
            payload = {"issue": {"number": 36}, "comment": {"body": f"C:/Program Files/Git/{word}",
                                                            "user": user(), "author_association": "NONE"}}
            calls = run(self.script, payload, issue={"state": "open", "labels": [], "assignees": []})
            self.assertEqual([call["name"] for call in calls], ["createComment"], word)
            body = calls[0]["args"]["body"]
            self.assertNotIn("\n", body)
            self.assertIn("@newcomer, nothing changed", body)
            self.assertIn(f"`MSYS_NO_PATHCONV=1 gh issue comment 36 -R karimalsalah/guarana --body /{word}`", body)
            self.assertIn("You can delete the comment above", body)
            self.assertIn("docs/agents/README.md#2-claim-it", body)

    @unittest.skipUnless(NODE, "Node.js is not on the PATH")
    def test_a_real_claim_still_claims(self):
        payload = {"issue": {"number": 36}, "comment": {"body": "/claim", "user": user(), "author_association": "NONE"}}
        calls = run(self.script, payload, issue={"state": "open", "labels": [], "assignees": []})
        self.assertEqual(named(calls, "addLabels"), [{**REPO, "issue_number": 36, "labels": ["claimed"]}])
        self.assertEqual(len(named(calls, "addAssignees")), 1)
        self.assertIn("**Claimed by @newcomer**", named(calls, "createComment")[0]["body"])

    @unittest.skipUnless(NODE, "Node.js is not on the PATH")
    def test_other_comments_get_no_answer(self):
        for body in ("I can take this, /claim soon", "Path C:/x/claim"):
            payload = {"issue": {"number": 36}, "comment": {"body": body, "user": user(), "author_association": "NONE"}}
            self.assertEqual(run(self.script, payload, issue={"state": "open", "labels": [], "assignees": []}), [], body)


class WelcomeWorkflowTests(unittest.TestCase):
    def setUp(self):
        self.text, self.doc = workflow("welcome.yml")
        self.jobs = self.doc["jobs"]
        self.scripts = {name: job["steps"][0]["with"]["script"] for name, job in self.jobs.items()}

    def test_triggers(self):
        self.assertEqual(self.doc["on"], {"pull_request_target": {"types": ["closed"]}, "issues": {"types": ["opened"]}})

    def test_least_permissions_and_only_the_repositorys_token(self):
        self.assertEqual(self.doc["permissions"], {})
        self.assertEqual(self.jobs["first-merge"]["permissions"], {"issues": "write", "pull-requests": "read"})
        self.assertEqual(self.jobs["first-report"]["permissions"], {"issues": "write"})
        self.assertNotIn("secrets.", self.text)

    def test_no_code_from_the_pull_request_runs(self):
        # pull_request_target hands a token that can write to the base repository's workflow.
        self.assertNotIn("checkout", self.text)
        for job in self.jobs.values():
            self.assertEqual(len(job["steps"]), 1)
            step = job["steps"][0]
            self.assertEqual(step["uses"], "actions/github-script@v9", "pinned to a major, as agent-claims.yml")
            self.assertNotIn("run", step)
            self.assertIn("name", step)
            self.assertNotIn("${{", step["with"]["script"])

    def test_conditions(self):
        merge, report = self.jobs["first-merge"]["if"], self.jobs["first-report"]["if"]
        self.assertIn("github.event_name == 'pull_request_target'", merge)
        self.assertIn("github.event.pull_request.merged", merge)
        self.assertIn("github.event.pull_request.user.type == 'User'", merge)
        self.assertIn("github.event.pull_request.author_association != 'OWNER'", merge)
        self.assertIn("github.event_name == 'issues'", report)
        self.assertIn("contains(github.event.issue.labels.*.name, 'device-report')", report)
        self.assertIn("github.event.issue.user.type == 'User'", report)
        self.assertIn("github.event.issue.author_association != 'OWNER'", report)
        for condition in (merge, report):
            self.assertIn("github.repository_owner == 'karimalsalah'", condition)

    def test_the_tools_ci_tests_run_when_it_changes(self):
        self.assertIn('- ".github/workflows/welcome.yml"', read(".github", "workflows", "legacy.yml"))

    def merge(self, *earlier):
        payload = {"pull_request": {"number": 60, "user": user(), "merged": True}}
        return run(self.scripts["first-merge"], payload, listForRepo=list(earlier))

    @unittest.skipUnless(NODE, "Node.js is not on the PATH")
    def test_a_first_merge_is_thanked_once_with_a_next_step(self):
        for earlier in ((), ({"number": 60, "pull_request": {"merged_at": "2026-11-14T00:00:00Z"}},),
                        ({"number": 53, "pull_request": {"merged_at": None}}, {"number": 41})):
            calls = self.merge(*earlier)
            self.assertEqual(named(calls, "listForRepo")[0]["creator"], "newcomer")
            comments = named(calls, "createComment")
            self.assertEqual(len(comments), 1, earlier)
            self.assertEqual(comments[0]["issue_number"], 60)
            body = comments[0]["body"]
            for needed in ("@newcomer", "first merged pull request", "legacy.html", "good%20first%20issue",
                           "`/claim`", "template=device-report.yml", "#55"):
                self.assertIn(needed, body)

    @unittest.skipUnless(NODE, "Node.js is not on the PATH")
    def test_a_second_merge_gets_nothing(self):
        calls = self.merge({"number": 54, "pull_request": {"merged_at": "2026-11-13T00:00:00Z"}})
        self.assertEqual(named(calls, "createComment"), [])

    @unittest.skipUnless(NODE, "Node.js is not on the PATH")
    def test_a_first_device_report_is_thanked_once(self):
        payload = {"issue": {"number": 90, "user": user()}}
        calls = run(self.scripts["first-report"], payload, listForRepo=[{"number": 90}])
        self.assertEqual(named(calls, "listForRepo")[0]["labels"], "device-report")
        comments = named(calls, "createComment")
        self.assertEqual(len(comments), 1)
        for needed in ("@newcomer", "first device report", "good%20first%20issue", "#55"):
            self.assertIn(needed, comments[0]["body"])
        calls = run(self.scripts["first-report"], payload, listForRepo=[{"number": 90}, {"number": 70}])
        self.assertEqual(named(calls, "createComment"), [])


class ClaimDocsTests(unittest.TestCase):
    def test_the_docs_that_teach_claim_give_the_forms_that_work_in_every_shell(self):
        for doc in (("docs", "agents", "README.md"), ("CONTRIBUTING.md",), ("docs", "AI-QUICKSTART.md")):
            text = read(*doc)
            self.assertIn("echo /claim | gh issue comment", text, doc)
            self.assertIn("--body-file -", text, doc)
            self.assertIn("MSYS_NO_PATHCONV=1", text, doc)


if __name__ == "__main__":
    unittest.main()
