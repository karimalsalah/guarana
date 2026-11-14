#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Is this pull request safe to merge? One question, one answer, for the merge resolver.

  python -I tools/ci/pr_gate.py NUMBER [--json] [--repo OWNER/NAME] [--remote NAME] [--sha SHA] [--require JOB]...

Prints `ready` or `not-ready` on the first line, then one `- code: reason` line per reason it is not
ready, then `note:` lines for things worth knowing that do not block. Exit status 0 is ready, 1 is
not ready (a question GitHub cannot answer counts as not ready), 2 is a usage error. --json prints
the same answer, with the evidence, as one JSON object.

It is not ready when:

- the pull request is closed, or a draft ("not compiled yet" or "not measured yet": marking it
  ready for review is the author's signal);
- a required check is failing, cancelled, still running or has not reported on the head commit.
  Required are the three jobs of build-check (REQUIRED_CHECKS); the switch-registry job also counts
  when it ran, since that workflow only runs on some changes. Pull requests that change only
  documentation skip the two builds, and a skipped build is a pass, but only after "Detect source
  changes" passed (a failed detection skips the builds too). --sha reads the checks of another
  commit, for the resolve/pr-N merge commit the resolver pushes after settling a conflict, whose
  checks are the ones that count; --require adds a job to the required ones;
- the base branch (normally main) is red and the pull request lacks the `fixes-main` label. Red is
  read from the newest commit on the base branch whose required checks have settled: a documentation
  only commit that skipped the builds, or one still building, does not hide an earlier red;
- GitHub reports a merge conflict in a file other than the union files (CHANGELOG.md,
  docs/switches.md, dist/nx2_switches.txt), whose conflicts the resolver settles by keeping both
  sides. The conflicting files come from `git merge-tree` in the current repository, after fetching
  the base branch and refs/pull/N/head from --remote into refs/pr-gate/, and only when GitHub says
  there is a conflict.

It changes nothing on GitHub: it only reads. Standard library only, and it calls `gh` (and `git`
for conflicts). Rules and background: docs/LANES.md, "Merging".
"""

import argparse
import json
import os
import re
import subprocess
import sys
import urllib.parse

UNION_FILES = ("CHANGELOG.md", "docs/switches.md", "dist/nx2_switches.txt")
FIXES_LABEL = "fixes-main"

# Job names of .github/workflows/build-check.yml and switch-registry.yml (a test reads them back).
CHANGES_JOB = "Detect source changes"
BUILD_JOBS = (
    "Linux GCC 13 production build and tests",
    "Windows clang-cl production build and tests",
)
REQUIRED_CHECKS = (CHANGES_JOB,) + BUILD_JOBS
CONDITIONAL_CHECKS = ("Check the switch registry",)

BASE_COMMITS = 8  # How far back the base branch is searched for settled checks.
SHA = re.compile(r"^[0-9a-fA-F]{7,40}$")


class GateError(Exception):
    """GitHub, gh or git could not answer. The gate then says not-ready."""


def run_command(argv):
    """Runs argv and returns (exit status, stdout, stderr). Replaced by a fake in the tests."""
    try:
        proc = subprocess.run(argv, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=180)
    except FileNotFoundError:
        raise GateError("%s is not installed or not on PATH" % argv[0])
    except subprocess.TimeoutExpired:
        raise GateError("%s timed out" % " ".join(argv[:3]))
    return proc.returncode, proc.stdout, proc.stderr


class Client:
    """The few things asked of gh, through one runner so a test can answer them from canned JSON."""

    def __init__(self, runner, repo, remote):
        self.runner = runner
        self.repo = repo
        self.remote = remote

    def text(self, argv):
        code, out, err = self.runner(argv)
        if code != 0:
            raise GateError("%s failed: %s" % (" ".join(argv[:3]), (err or out).strip()[:200]))
        return out

    def json(self, argv):
        out = self.text(argv)
        try:
            return json.loads(out)
        except ValueError:
            raise GateError("%s did not return JSON" % " ".join(argv[:3]))

    def pull_request(self, number):
        fields = "number,state,isDraft,headRefOid,baseRefName,mergeable,labels"
        data = self.json(["gh", "pr", "view", str(number), "-R", self.repo, "--json", fields])
        if not isinstance(data, dict) or not data.get("headRefOid") or not data.get("baseRefName"):
            raise GateError("gh pr view returned no head commit for #%s" % number)
        return data

    def check_runs(self, sha):
        data = self.json(["gh", "api", "repos/%s/commits/%s/check-runs?per_page=100" % (self.repo, sha)])
        if not isinstance(data, dict):
            raise GateError("check-runs of %s is not an object" % sha[:10])
        runs = data.get("check_runs") or []
        return runs, int(data.get("total_count") or len(runs))

    def branch_commits(self, branch, limit):
        path = "repos/%s/commits?sha=%s&per_page=%d" % (self.repo, urllib.parse.quote(branch, safe=""), limit)
        data = self.json(["gh", "api", path])
        if not isinstance(data, list):
            raise GateError("the commits of %s are not a list" % branch)
        return [c["sha"] for c in data if isinstance(c, dict) and c.get("sha")]


def classify(run):
    """pass, skipped, fail or pending, for one check run as the REST API names it."""
    if (run.get("status") or "").lower() != "completed":
        return "pending"
    conclusion = (run.get("conclusion") or "").lower()
    if conclusion in ("success", "neutral"):
        return "pass"
    if conclusion == "skipped":
        return "skipped"
    return "fail"


def latest_by_name(runs):
    """A re-run adds a check run of the same name: the newest (highest id) is the one that counts."""
    latest = {}
    for run in runs:
        name = run.get("name")
        if not name:
            continue
        key = int(run.get("id") or 0)
        if name not in latest or key >= int(latest[name].get("id") or 0):
            latest[name] = run
    return latest


def judge_checks(runs, required=REQUIRED_CHECKS, conditional=CONDITIONAL_CHECKS):
    """The state of one commit's checks: failing, pending, skipped (the builds were skipped, as for a
    documentation-only change) or green. A required check that has not reported is pending; a
    conditional one counts only when it reported."""
    latest = latest_by_name(runs)
    results = {}
    failing = []
    pending = []
    missing = []
    for name in dict.fromkeys(tuple(required) + tuple(conditional)):
        run = latest.get(name)
        if run is None:
            if name in required:
                missing.append(name)
            continue
        verdict = classify(run)
        results[name] = verdict
        if verdict == "fail":
            failing.append("%s (%s)" % (name, (run.get("conclusion") or "no conclusion").lower()))
        elif verdict == "pending":
            pending.append(name)
    if failing:
        state = "failing"
    elif pending or missing:
        state = "pending"
    else:
        builds = [name for name in required if name != CHANGES_JOB]
        state = "skipped" if builds and all(results[name] == "skipped" for name in builds) else "green"
    return {"state": state, "failing": failing, "pending": pending, "missing": missing}


def base_state(client, branch, required, limit=BASE_COMMITS):
    """Is the base branch red? Looks at its newest commit whose required checks have settled: green or
    failing. Commits that skipped the builds, or are still building, are passed over so that they
    cannot hide an earlier red. Returns {"state": green | red | unknown, "sha", "tip_unsettled"}."""
    unsettled = False
    for index, sha in enumerate(client.branch_commits(branch, limit)):
        runs, _ = client.check_runs(sha)
        verdict = judge_checks(runs, required, ())
        if verdict["state"] in ("skipped", "pending"):
            if index == 0:
                unsettled = True
            continue
        return {"state": "red" if verdict["state"] == "failing" else "green", "sha": sha,
                "failing": verdict["failing"], "tip_unsettled": unsettled}
    return {"state": "unknown", "sha": None, "failing": [], "tip_unsettled": unsettled}


def parse_merge_tree(text):
    """The conflicted paths in the output of `git merge-tree --write-tree --name-only --no-messages`:
    the tree id on the first line, then one path per line up to the first blank line."""
    lines = text.splitlines()
    paths = []
    for line in lines[1:]:
        if not line.strip():
            break
        paths.append(line.strip())
    return paths


def conflicting_files(client, base_branch, number):
    """The paths that conflict when the pull request is merged into its base, or None when git could
    not tell (not in a clone, no access, git older than 2.38)."""
    base_ref = "refs/pr-gate/base"
    head_ref = "refs/pr-gate/pr-%d" % number
    runner = client.runner
    code, _, _ = runner(["git", "fetch", "--quiet", "--no-tags", client.remote,
                         "+refs/heads/%s:%s" % (base_branch, base_ref), "+refs/pull/%d/head:%s" % (number, head_ref)])
    if code != 0:
        return None
    code, out, _ = runner(["git", "merge-tree", "--write-tree", "--name-only", "--no-messages", base_ref, head_ref])
    if code == 0:
        return []
    if code != 1:
        return None
    return parse_merge_tree(out) or None


def decide(number, pr, head_sha, head_checks, base, conflicts, truncated=False):
    """The answer, from what was read. `conflicts` is what conflicting_files returned: only read when
    GitHub reports a conflict, and None when git could not list the files."""
    reasons = []
    notes = []

    def reason(code, message):
        reasons.append({"code": code, "message": message})

    if (pr.get("state") or "").upper() != "OPEN":
        reason("not-open", "the pull request is %s" % (pr.get("state") or "not open").lower())
    if pr.get("isDraft"):
        reason("draft", "the pull request is a draft; the author marks it ready for review when it is compiled and measured")
    if head_checks["failing"]:
        reason("checks-failing", "failing on %s: %s" % (head_sha[:10], "; ".join(head_checks["failing"])))
    if head_checks["pending"]:
        reason("checks-pending", "still running on %s: %s" % (head_sha[:10], "; ".join(head_checks["pending"])))
    if head_checks["missing"]:
        reason("checks-pending", "not reported on %s: %s" % (head_sha[:10], "; ".join(head_checks["missing"])))
    if head_checks["state"] == "skipped":
        notes.append("the builds were skipped (documentation-only change), which counts as a pass")
    if truncated:
        notes.append("the commit has more than 100 check runs; only the first 100 were read")

    labels = [label.get("name") for label in pr.get("labels") or [] if isinstance(label, dict)]
    branch = pr.get("baseRefName")
    if base["state"] == "red":
        if FIXES_LABEL in labels:
            notes.append("%s is red (%s) and this pull request has the %s label, so it may merge" % (
                branch, "; ".join(base["failing"]) or "checks failing", FIXES_LABEL))
        else:
            reason("base-red", "%s is red at %s (%s) and this pull request lacks the %s label" % (
                branch, (base["sha"] or "")[:10], "; ".join(base["failing"]) or "checks failing", FIXES_LABEL))
    elif base["state"] == "unknown":
        notes.append("could not find a settled build-check result on the newest commits of %s" % branch)
    if base["tip_unsettled"] and base["state"] != "unknown":
        notes.append("the tip of %s is still building (or skipped its builds); its state is read from %s" % (
            branch, (base["sha"] or "")[:10]))

    mergeable = (pr.get("mergeable") or "UNKNOWN").upper()
    if (pr.get("state") or "").upper() != "OPEN":
        mergeable = "NOT_OPEN"  # A closed pull request has no merge to ask about.
    elif mergeable == "UNKNOWN":
        reason("mergeability-unknown", "GitHub has not computed whether the pull request merges; ask again in a minute")
    elif mergeable == "CONFLICTING":
        if conflicts is None:
            reason("conflicts", "GitHub reports a merge conflict and the files could not be listed "
                                "(run this in a clone with git 2.38 or later)")
        elif not conflicts:
            reason("conflicts", "GitHub reports a merge conflict that git does not find; ask again in a minute")
        else:
            outside = [path for path in conflicts if path not in UNION_FILES]
            if outside:
                reason("conflicts", "conflicts outside the union files: %s" % ", ".join(outside))
            else:
                notes.append("conflicts only in union files (%s): keep both sides" % ", ".join(conflicts))
    return {
        "pr": number,
        "ready": not reasons,
        "reasons": reasons,
        "notes": notes,
        "head": head_sha,
        "base": branch,
        "checks": head_checks,
        "base_state": base,
        "conflicts": conflicts if mergeable == "CONFLICTING" else None,
    }


def gate(client, number, sha=None, required=REQUIRED_CHECKS):
    pr = client.pull_request(number)
    head_sha = sha or pr["headRefOid"]
    runs, total = client.check_runs(head_sha)
    head_checks = judge_checks(runs, required, CONDITIONAL_CHECKS)
    # A job added with --require is asked of the pull request, not of the base branch: main is red or
    # green by the checks that every commit on it runs.
    base = base_state(client, pr["baseRefName"], REQUIRED_CHECKS)
    conflicts = None
    if (pr.get("state") or "").upper() == "OPEN" and (pr.get("mergeable") or "").upper() == "CONFLICTING":
        conflicts = conflicting_files(client, pr["baseRefName"], number)
    return decide(number, pr, head_sha, head_checks, base, conflicts, truncated=total > len(runs))


def render(answer):
    lines = ["ready" if answer["ready"] else "not-ready"]
    for item in answer["reasons"]:
        lines.append("- %s: %s" % (item["code"], item["message"]))
    for note in answer["notes"]:
        lines.append("note: %s" % note)
    return "\n".join(lines)


def positive(text):
    try:
        value = int(text)
    except ValueError:
        value = 0
    if value < 1:
        raise argparse.ArgumentTypeError("%r is not a pull request number" % text)
    return value


def commit(text):
    if not SHA.match(text):
        raise argparse.ArgumentTypeError("%r is not a commit id" % text)
    return text


def main(argv=None, runner=run_command):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("number", type=positive, help="pull request number")
    parser.add_argument("--json", action="store_true", help="print the answer and its evidence as JSON")
    parser.add_argument("--repo", help="OWNER/NAME (default: $GITHUB_REPOSITORY, else the current clone's)")
    parser.add_argument("--remote", default="origin", help="git remote that holds the pull request refs (default: origin)")
    parser.add_argument("--sha", type=commit, help="read the checks of this commit instead of the pull request's head")
    parser.add_argument("--require", action="append", default=[], metavar="JOB",
                        help="a check name that must also pass (repeatable)")
    args = parser.parse_args(argv)
    required = REQUIRED_CHECKS + tuple(name for name in args.require if name not in REQUIRED_CHECKS)
    try:
        repo = args.repo or os.environ.get("GITHUB_REPOSITORY")
        if not repo:
            code, out, err = runner(["gh", "repo", "view", "--json", "nameWithOwner", "--jq", ".nameWithOwner"])
            if code != 0 or not out.strip():
                raise GateError("could not tell the repository: pass --repo OWNER/NAME (%s)" % err.strip()[:120])
            repo = out.strip()
        answer = gate(Client(runner, repo, args.remote), args.number, args.sha, required)
    except GateError as error:
        answer = {"pr": args.number, "ready": False,
                  "reasons": [{"code": "gh-error", "message": str(error)}], "notes": []}
    if args.json:
        print(json.dumps(answer, indent=2))
    else:
        print(render(answer))
    return 0 if answer["ready"] else 1


if __name__ == "__main__":
    sys.exit(main())
