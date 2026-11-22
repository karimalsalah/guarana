#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The "Thanks to" line that ends every release's notes.

  python -I tools/ci/thanks.py --repo OWNER/NAME --head SHA [--tag vX.Y.Z | --base REF]

Prints one line, "Thanks to @alice, @bob and Carol Name.", naming the people who authored a commit
or are named in a `Co-authored-by:` trailer between the previous release and HEAD: the same people,
found the same way, as the hall of fame (tools/ci/legacy.py, whose code this reuses). AI tools and
bots are not people and are left out; a person without a GitHub login is named in plain text.
Prints nothing when the range holds no person, or when GitHub cannot be asked: the release does not
fail over its thanks.

The range starts at --base (a tag or a commit), else at the tag before --tag (see previous_tag),
else, for the first release, at Guarana's first day (legacy.SINCE). Standard library only.
"""

import argparse
import os
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))  # -I leaves the script's directory off sys.path.

import legacy  # noqa: E402  (also puts tools/ingest on sys.path)
import github_api  # noqa: E402

TAG = re.compile(r"^v(\d+)\.(\d+)\.(\d+)(?:-rc(\d+))?$")
PAGE = 100
MAX_PAGES = 30


def tag_key(name):
    """Sort key of a release tag, or None for any other tag. A final release sorts after its candidates."""
    match = TAG.match(name or "")
    if not match:
        return None
    major, minor, patch, rc = match.groups()
    return (int(major), int(minor), int(patch), 1 if rc is None else 0, int(rc or 0))


def previous_tag(names, current):
    """The release tag before `current`. A final release skips its own candidates: its notes cover
    everything since the previous final release. None when there is no earlier release tag."""
    here = tag_key(current)
    if here is None:
        return None
    candidates = []
    for name in names:
        key = tag_key(name)
        if key is None or name == current or key >= here:
            continue
        if here[3] == 1 and key[:3] == here[:3]:
            continue
        candidates.append((key, name))
    return max(candidates)[1] if candidates else None


def compare_commits(github, repo, base, head):
    commits = []
    for page in range(1, MAX_PAGES + 1):
        data = github.get(f"/repos/{repo}/compare/{base}...{head}", per_page=PAGE, page=page) or {}
        batch = data.get("commits") or []
        commits.extend(batch)
        if len(batch) < PAGE:
            break
    return commits


def range_commits(github, repo, head, base=None, tag=None, since=legacy.SINCE):
    if not base and tag:
        tags = [item.get("name") for item in github.paginate(f"/repos/{repo}/tags")]
        base = previous_tag(tags, tag)
    if base:
        return compare_commits(github, repo, base, head)
    return github.paginate(f"/repos/{repo}/commits", sha=head, since=since)


def handle(person):
    return "@" + person["login"] if person["login"] else person["name"]


def thanks_line(commits):
    """'Thanks to @a, @b and C.' for the people in these commits; '' when there are none."""
    ledger = legacy.build_ledger(commits)
    people = sorted(ledger.people.values(),
                    key=lambda p: (p["login"] is None, (p["login"] or p["name"] or "").casefold()))
    names = [handle(person) for person in people]
    if not names:
        return ""
    joined = names[0] if len(names) == 1 else ", ".join(names[:-1]) + " and " + names[-1]
    return f"Thanks to {joined}."


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repo", default=os.environ.get("GITHUB_REPOSITORY", ""))
    parser.add_argument("--head", required=True, help="the commit the release is built from")
    parser.add_argument("--base", help="start after this tag or commit")
    parser.add_argument("--tag", help="the release tag; the range starts at the tag before it")
    args = parser.parse_args(argv)
    if not re.fullmatch(r"[\w.-]+/[\w.-]+", args.repo or ""):
        parser.error("--repo owner/name (or GITHUB_REPOSITORY) is required")
    try:
        github, _ = github_api.from_environment()
        line = thanks_line(range_commits(github, args.repo, args.head, args.base, args.tag))
    except (github_api.GitHubError, OSError, ValueError) as error:
        print(f"::warning::no thanks line: {error}", file=sys.stderr)
        return 0
    if line:
        print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
