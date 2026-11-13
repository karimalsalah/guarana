#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""An in-memory GitHub REST API for the tests: issues, comments, labels and pull requests."""

import copy
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

import github_api  # noqa: E402

REPO = "karimalsalah/guarana"
BOT = {"login": "github-actions[bot]", "type": "Bot"}


def make_issue(number, body, login="reporter", labels=("device-report", "needs-triage"), state="open",
               state_reason=None, created="2026-11-20T10:00:00Z", title="[Device] report", user_type="User"):
    return {"number": number, "title": title, "body": body, "user": {"login": login, "type": user_type},
            "labels": [{"name": name} for name in labels], "state": state, "state_reason": state_reason,
            "created_at": created}


class FakeGitHub(github_api.GitHub):
    """Answers the endpoints ingest.py and triage.py use, and records every call. Editing an
    issue itself (its state, title or body) is refused: neither script may."""

    def __init__(self, issues=(), repo=REPO, default_branch="main"):
        super().__init__("test-token", api="https://api.github.test")
        self.repo, self.default_branch = repo, default_branch
        self.issues = {issue["number"]: copy.deepcopy(issue) for issue in issues}
        self.comments = {}
        self.pulls = {}
        self.calls = []
        self.forbid_pull_create = False
        self.next_id = 9000

    def writes(self):
        return [(method, path) for method, path, _ in self.calls if method != "GET"]

    def request(self, method, path, body=None, params=None, accept=None):
        self.calls.append((method, path, copy.deepcopy(body)))
        prefix = f"/repos/{self.repo}"
        if not path.startswith(prefix):
            raise AssertionError(f"unexpected path {path}")
        rest, params = path[len(prefix):], params or {}
        if method == "GET" and rest == "":
            return {"default_branch": self.default_branch}, ""
        if method == "GET" and rest == "/issues":
            label = params.get("labels")
            found = [issue for number, issue in sorted(self.issues.items())
                     if not label or any(item["name"] == label for item in issue["labels"])]
            # The issues endpoint lists pull requests too.
            found.append({"number": 999, "pull_request": {"url": "x"}, "labels": [{"name": "device-report"}],
                          "body": "", "user": {"login": "someone", "type": "User"}})
            return copy.deepcopy(found), ""
        match = re.fullmatch(r"/issues/(\d+)", rest)
        if match:
            if method != "GET":
                raise AssertionError(f"{method} {path}: the scripts never edit an issue")
            return copy.deepcopy(self.issues[int(match.group(1))]), ""
        match = re.fullmatch(r"/issues/(\d+)/comments", rest)
        if match:
            number = int(match.group(1))
            if method == "GET":
                return copy.deepcopy(self.comments.get(number, [])), ""
            self.next_id += 1
            comment = {"id": self.next_id, "body": body["body"], "user": BOT}
            self.comments.setdefault(number, []).append(comment)
            return copy.deepcopy(comment), ""
        match = re.fullmatch(r"/issues/comments/(\d+)", rest)
        if match and method == "PATCH":
            for comments in self.comments.values():
                for comment in comments:
                    if comment["id"] == int(match.group(1)):
                        comment["body"] = body["body"]
                        return copy.deepcopy(comment), ""
            raise AssertionError("no such comment")
        match = re.fullmatch(r"/issues/(\d+)/labels", rest)
        if match and method == "POST":
            issue = self.issues[int(match.group(1))]
            for name in body["labels"]:
                if all(item["name"] != name for item in issue["labels"]):
                    issue["labels"].append({"name": name})
            return copy.deepcopy(issue["labels"]), ""
        if rest == "/pulls" and method == "GET":
            owner, _, branch = params.get("head", ":").partition(":")
            found = [pull for _, pull in sorted(self.pulls.items())
                     if pull["state"] == params.get("state", "open") and pull["head"]["ref"] == branch
                     and owner == self.repo.split("/")[0] and pull["base"]["ref"] == params.get("base", pull["base"]["ref"])]
            return copy.deepcopy(found), ""
        if rest == "/pulls" and method == "POST":
            if self.forbid_pull_create:
                raise github_api.GitHubError(403, "GitHub Actions is not permitted to create or approve pull requests.",
                                             path)
            if any(pull["state"] == "open" and pull["head"]["ref"] == body["head"] for pull in self.pulls.values()):
                raise github_api.GitHubError(422, "A pull request already exists for this branch.", path)
            self.next_id += 1
            number = 500 + len(self.pulls)
            self.pulls[number] = {"number": number, "title": body["title"], "body": body["body"], "state": "open",
                                  "head": {"ref": body["head"]}, "base": {"ref": body["base"]}}
            return copy.deepcopy(self.pulls[number]), ""
        match = re.fullmatch(r"/pulls/(\d+)", rest)
        if match and method == "PATCH":
            pull = self.pulls[int(match.group(1))]
            pull.update({key: value for key, value in body.items() if key in ("title", "body", "state")})
            return copy.deepcopy(pull), ""
        raise AssertionError(f"unexpected call {method} {path}")


def git(*args, cwd=None, data=None):
    env = dict(os.environ, GIT_AUTHOR_NAME="Test", GIT_AUTHOR_EMAIL="test@example.invalid",
               GIT_COMMITTER_NAME="Test", GIT_COMMITTER_EMAIL="test@example.invalid", GIT_TERMINAL_PROMPT="0")
    result = subprocess.run(["git", *args], cwd=cwd, input=data, capture_output=True, env=env)
    if result.returncode:
        raise AssertionError(f"git {' '.join(args)}: {result.stderr.decode('utf-8', 'replace')}")
    return result.stdout.decode("utf-8").strip()
