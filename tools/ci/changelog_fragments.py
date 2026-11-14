#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Changelog fragments: one small file per pull request, so two pull requests never edit the same line.

  python -I tools/ci/changelog_fragments.py collect
  python -I tools/ci/changelog_fragments.py check [--base REF] [--head REF]
  python -I tools/ci/changelog_fragments.py release [--into NAME]

A pull request adds changelog.d/<slug>.<section>.md instead of a line in CHANGELOG.md. <slug> is
lower-case letters, digits and hyphens; <section> is added, changed, deprecated, removed, fixed or
security (CHANGELOG.md's "### Added", "### Changed", ... headings). The file holds the entry as it
would read in CHANGELOG.md: a line that starts with "- ", and indented lines that continue it. A
file may hold several entries. changelog.d/README.md and files that start with a dot are ignored.

  collect   Print the body of CHANGELOG.md's "## [Unreleased]" with the fragments folded in, grouped
            under their headings after the lines already there; exactly what tools/ci/
            changelog-section.sh Unreleased printed before when there is no fragment. Changes nothing.
  check     For a pull request: fail when its changes (git diff BASE HEAD, BASE default origin/main,
            HEAD default HEAD) touch more than documentation, and add neither a fragment nor a line
            under [Unreleased]. Fails when a fragment in changelog.d is malformed, in any case.
  release   Fold the fragments into CHANGELOG.md's "## [NAME]" (default Unreleased) and delete them.
            Used by tools/ci/bump-version.sh; nothing happens when there is no fragment.

Both forms are accepted: lines written straight under [Unreleased] stay where they are. Standard
library only.
"""

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
FRAGMENT_DIR = "changelog.d"
CHANGELOG = "CHANGELOG.md"
UNRELEASED = "Unreleased"

# Keep a Changelog's order. A heading CHANGELOG.md has that is not in this list stays where it is.
SECTIONS = ("added", "changed", "deprecated", "removed", "fixed", "security")
NAME = re.compile(r"^([a-z0-9][a-z0-9-]*)\.(" + "|".join(SECTIONS) + r")\.md$")

# What a pull request may change without a changelog entry: the paths build-check.yml skips the
# builds for, plus the files that carry the changelog and the version themselves.
DOCS_ONLY = re.compile(
    r"^(docs/.*|site/.*|.*\.md|\.github/ISSUE_TEMPLATE/.*|\.github/labels\.yml|\.github/CODEOWNERS"
    r"|tools/bench/results/.*|changelog\.d/.*|VERSION)$"
)
HUNK = re.compile(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@")


class FragmentError(Exception):
    pass


def heading(name):
    return "## [" + name + "]"


def split_lines(text):
    """Lines without their ends; a trailing newline does not make an empty last line."""
    lines = text.replace("\r\n", "\n").split("\n")
    if text.endswith("\n"):
        lines.pop()
    return lines


def section_range(lines, name):
    """(first, end) line indices of the body of "## [name]" (or "## [name] - date"), or None."""
    start = None
    for i, line in enumerate(lines):
        if start is None:
            if line == heading(name) or line.startswith(heading(name) + " - "):
                start = i + 1
        elif line.startswith("## "):
            return start, i
    return None if start is None else (start, len(lines))


def read_fragments(root):
    """The fragments in root/changelog.d as (slug, section, entry lines), sorted by file name.

    Raises FragmentError naming every malformed file."""
    folder = Path(root) / FRAGMENT_DIR
    found, problems = [], []
    if folder.is_dir():
        for path in sorted(folder.iterdir(), key=lambda p: p.name):
            name = path.name
            if name.startswith(".") or name == "README.md":
                continue
            where = FRAGMENT_DIR + "/" + name
            match = NAME.match(name)
            if not match or not path.is_file():
                problems.append(where + ": the name must be <slug>.<section>.md, slug in a-z, 0-9 and -, section one of " + ", ".join(SECTIONS))
                continue
            try:
                text = path.read_bytes().decode("utf-8")
            except UnicodeDecodeError:
                problems.append(where + ": not UTF-8")
                continue
            lines = text.replace("\r\n", "\n").strip("\n").rstrip().split("\n")
            lines = [line.rstrip() for line in lines]
            if lines == [""]:
                problems.append(where + ": empty")
            elif not lines[0].startswith("- "):
                problems.append(where + ': the entry must start with "- " (the line as it reads in CHANGELOG.md)')
            elif any(line.startswith("#") for line in lines):
                problems.append(where + ': no line may start with "#": the heading comes from the file name')
            else:
                found.append((match.group(1), match.group(2), lines))
    if problems:
        raise FragmentError("\n".join(problems))
    return found


def fold(body, fragments):
    """The lines of a changelog section body with the fragments appended to their ### sections.

    Unchanged when there is no fragment. A section CHANGELOG.md lacks is created before the first
    section that comes after it in SECTIONS order."""
    if not fragments:
        return list(body)
    first = next((i for i, line in enumerate(body) if line.startswith("### ")), len(body))
    preamble = body[:first]
    groups = []  # [heading line, entry lines]
    for line in body[first:]:
        if line.startswith("### "):
            groups.append([line, []])
        else:
            groups[-1][1].append(line)
    for group in groups:
        group[1] = trim(group[1])

    def rank(line):
        title = line[4:].strip().lower()
        return SECTIONS.index(title) if title in SECTIONS else len(SECTIONS)

    for section in SECTIONS:
        entries = [line for _, name, lines in fragments if name == section for line in lines]
        if not entries:
            continue
        wanted = "### " + section.capitalize()
        group = next((g for g in groups if g[0] == wanted), None)
        if group is None:
            group = [wanted, []]
            after = next((n for n, g in enumerate(groups) if rank(g[0]) > SECTIONS.index(section)), len(groups))
            groups.insert(after, group)
        group[1].extend(entries)

    out = [""]
    if trim(preamble):
        out += trim(preamble) + [""]
    for head, lines in groups:
        out += [head, ""] + lines + [""]
    return out


def trim(lines):
    lines = list(lines)
    while lines and not lines[0].strip():
        lines.pop(0)
    while lines and not lines[-1].strip():
        lines.pop()
    return lines


def collect_text(root, name=UNRELEASED):
    changelog = Path(root) / CHANGELOG
    lines = split_lines(changelog.read_bytes().decode("utf-8"))
    span = section_range(lines, name)
    fragments = read_fragments(root)
    if span is None:
        raise FragmentError(CHANGELOG + " has no " + heading(name) + " heading")
    return "".join(line + "\n" for line in fold(lines[span[0] : span[1]], fragments))


def write(text):
    if hasattr(sys.stdout, "buffer"):
        sys.stdout.buffer.write(text.encode("utf-8"))
        sys.stdout.buffer.flush()
    else:
        sys.stdout.write(text)


def release(root, name=UNRELEASED):
    """Fold the fragments into CHANGELOG.md and delete them. Returns the file names it folded."""
    fragments = read_fragments(root)
    if not fragments:
        return []
    changelog = Path(root) / CHANGELOG
    raw = changelog.read_bytes().decode("utf-8")
    lines = split_lines(raw)
    span = section_range(lines, name)
    if span is None:
        raise FragmentError(CHANGELOG + " has no " + heading(name) + " heading to fold the fragments into")
    lines[span[0] : span[1]] = fold(lines[span[0] : span[1]], fragments)
    eol = "\r\n" if "\r\n" in raw else "\n"
    text = eol.join(lines) + eol
    temp = changelog.with_name(changelog.name + ".tmp")
    temp.write_bytes(text.encode("utf-8"))
    os.replace(str(temp), str(changelog))
    names = sorted(p.name for p in (Path(root) / FRAGMENT_DIR).iterdir() if NAME.match(p.name))
    for item in names:
        (Path(root) / FRAGMENT_DIR / item).unlink()
    return names


def git(root, *args):
    result = subprocess.run(
        ["git", "-C", str(root)] + list(args),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        universal_newlines=True,
        encoding="utf-8",
    )
    if result.returncode != 0:
        raise FragmentError("git " + " ".join(args) + " failed: " + result.stderr.strip())
    return result.stdout


def needs_entry(paths):
    """True when one of the changed paths is more than documentation."""
    return any(not DOCS_ONLY.match(path) for path in paths)


def added_ranges(diff):
    """(first line, last line) in the new file of each run of lines a `git diff -U0` adds."""
    ranges = []
    for line in diff.splitlines():
        match = HUNK.match(line)
        if match:
            count = 1 if match.group(2) is None else int(match.group(2))
            if count:
                first = int(match.group(1))
                ranges.append((first, first + count - 1))
    return ranges


def check(root, base, head):
    """Returns the problem as a sentence, or None when the pull request is fine."""
    try:
        read_fragments(root)
    except FragmentError as err:
        return str(err)
    # -z: paths come back raw, not quoted, so a name with an accent or a space still matches.
    fields = git(root, "diff", "--name-status", "-z", "--no-renames", base, head).split("\0")
    changed = dict((path, kind) for kind, path in zip(fields[0::2], fields[1::2]) if path)
    if not needs_entry(changed):
        return None
    for path, kind in changed.items():
        name = path[len(FRAGMENT_DIR) + 1 :]
        if path.startswith(FRAGMENT_DIR + "/") and kind in ("A", "M") and NAME.match(name):
            return None
    if changed.get(CHANGELOG, "D") != "D":
        new = split_lines(git(root, "show", head + ":" + CHANGELOG))
        span = section_range(new, UNRELEASED)
        if span is not None:
            diff = git(root, "diff", "-U0", "--no-renames", base, head, "--", CHANGELOG)
            # span is 0-based and half open; diff lines are 1-based.
            if any(a <= span[1] and b > span[0] for a, b in added_ranges(diff)):
                return None
    return (
        "This pull request changes more than documentation but adds no changelog entry. Add a file "
        + FRAGMENT_DIR
        + "/<short-name>.<section>.md (section: "
        + ", ".join(SECTIONS)
        + ") holding the entry as a line that starts with \"- \", or a line under ## [Unreleased] in "
        + CHANGELOG
        + ". See "
        + FRAGMENT_DIR
        + "/README.md."
    )


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", default=str(ROOT), help="the repository (default: this one)")
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("collect", help="print [Unreleased] with the fragments folded in")
    checking = commands.add_parser("check", help="a pull request that changes code adds a fragment or a line under [Unreleased]")
    checking.add_argument("--base", default="origin/main")
    checking.add_argument("--head", default="HEAD")
    releasing = commands.add_parser("release", help="fold the fragments into CHANGELOG.md and delete them")
    releasing.add_argument("--into", default=UNRELEASED, help="the section to fold into (default: Unreleased)")
    args = parser.parse_args(argv)
    try:
        if args.command == "collect":
            write(collect_text(args.root))
        elif args.command == "check":
            problem = check(args.root, args.base, args.head)
            if problem:
                print("changelog: " + problem, file=sys.stderr)
                return 1
            print("changelog: ok")
        else:
            names = release(args.root, args.into)
            # On stderr: bump-version.sh prints the new version on stdout, and callers read it.
            print("changelog: folded " + str(len(names)) + " fragment(s) into " + heading(args.into) + (": " + ", ".join(names) if names else ""), file=sys.stderr)
    except FragmentError as err:
        print("changelog: " + str(err), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
