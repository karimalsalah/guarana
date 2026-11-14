#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Checks that every EDEN_NX2_* and EDEN_DKB_* switch the code uses has a row in docs/switches.md.

  python3 -I tools/ci/switch_registry.py [--root DIR]
  python3 -I tools/ci/switch_registry.py [--root DIR] --upstream-ref REF
  python3 -I tools/ci/switch_registry.py --write-upstream REF [--root DIR]

A switch is used where its whole name is a string literal in the code of src/ or launcher/
(C, C++, Objective-C, Rust, TypeScript, JavaScript, Kotlin, Java), outside comments. That covers
std::getenv and every helper that takes the name as an argument (EnvOn, ReadValue, EnvironmentFlag,
EnvSeconds, EnvInt, the texture cache's tuples, ...) without knowing the helpers, and also the
places that set a switch (setenv, putenv("NAME=VALUE"), the launcher's Command::env, the game
presets). TypeScript and JavaScript strings may use double quotes, single quotes or backticks. Names
in comments, and strings that only contain a name (a log message, "NAME=VALUE"), are not uses.

The check fails when a used name has no row in docs/switches.md (a row is a table line whose first
cell names the switch in backticks) and is not upstream's: in tools/ci/switch-registry-upstream.txt
(the switches upstream Lichii had at the last merge) or in src/ of --upstream-ref. Each failure
names the switch and where it is used. It also fails on a literal that looks like a name built at
run time ("EDEN_NX2_" + x, "EDEN_NX2_{}", `EDEN_NX2_${x}`, "EDEN_NX2_Foo"), which this check cannot
follow, unless RUNTIME_PREFIXES lists it, and on a name outside every string literal and comment
(an unquoted object key, a macro), which it cannot classify. It reports, without failing, rows
whose switch is used nowhere any more.

--write-upstream REF rewrites the upstream list from the switches used in src/ of the git
commit REF (read with git archive; nothing is checked out). --upstream-ref REF takes the switches
used in src/ of REF as upstream's too, without writing the list: CI (switch-registry.yml) passes
the tip of the upstream branch that upstream-sync.yml mirrors, so a switch an upstream sync or
merge brings in needs neither a row nor a regenerated list. Standard library only.
"""

import argparse
import bisect
import io
import os
import re
import subprocess
import sys
import tarfile
from pathlib import Path

REGISTRY = "docs/switches.md"
UPSTREAM = "tools/ci/switch-registry-upstream.txt"
SCANNED = ("src", "launcher")
CODE_SUFFIXES = {
    ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inc", ".inl", ".ipp", ".m", ".mm",
    ".rs", ".ts", ".tsx", ".js", ".mjs", ".cjs", ".kt", ".kts", ".java",
}
SKIPPED_DIRS = {".git", "node_modules", "target"}
MARKERS = ("EDEN_NX2_", "EDEN_DKB_")
NAME = re.compile(r"EDEN_(?:NX2|DKB)_[A-Z0-9_]*")
NAME_IN_CODE = re.compile(r"(?<![A-Za-z0-9_])EDEN_(?:NX2|DKB)_[A-Z0-9_]*")
REMOVED_SECTION = "Removed experiments"

# Literals that start like a switch name but are not one, as (path, literal): a name built from
# them at run time would escape the check, so each is listed here with why it does not read one.
RUNTIME_PREFIXES = {
    # Filters the lines of nx2_switches.txt by prefix before they are put in the environment.
    ("src/lichii/nx2_switches.h", "EDEN_NX2_"),
}

# Calls that put a switch in an environment rather than read one; the name still needs its row.
SETTERS = {
    "setenv", "unsetenv", "putenv", "_putenv", "_putenv_s", "SetEnvironmentVariable",
    "SetEnvironmentVariableA", "SetEnvironmentVariableW", "qputenv", "qunsetenv", "set_var",
    "remove_var", "env", "env_remove",
}

# Calls that take one "NAME=VALUE" string.
PUTENV = {"putenv", "_putenv"}

# Comments and literals of the C family, Rust, Kotlin and Java, in the order they can start.
TOKEN = re.compile(
    r"""(?P<line>//[^\n]*)
      | (?P<block>/\*.*?(?:\*/|\Z))
      | (?<![A-Za-z0-9_])(?:u8|[uUL])?R"(?P<cdelim>[^()\\\s"]{0,16})\((?P<craw>.*?)\)(?P=cdelim)"
      | (?<![A-Za-z0-9_])b?r(?P<hashes>\#*)"(?P<rraw>.*?)"(?P=hashes)
      | "(?P<str>(?:\\.|[^"\\\n])*)"
      | `(?P<tpl>(?:\\.|[^`\\])*)`
      | '(?:\\.|[^'\\\n])'
    """,
    re.S | re.X,
)
# The same for TypeScript and JavaScript, whose single quotes start strings, not char literals.
JS_TOKEN = re.compile(
    r"""(?P<line>//[^\n]*)
      | (?P<block>/\*.*?(?:\*/|\Z))
      | "(?P<str>(?:\\.|[^"\\\n])*)"
      | '(?P<sq>(?:\\.|[^'\\\n])*)'
      | `(?P<tpl>(?:\\.|[^`\\])*)`
    """,
    re.S | re.X,
)
JS_SUFFIXES = {".ts", ".tsx", ".js", ".mjs", ".cjs"}
LITERAL_GROUPS = ("str", "sq", "craw", "rraw", "tpl")
CALLER =re.compile(r"([A-Za-z_][A-Za-z0-9_]*)\s*(?:<[^<>()]*>)?\s*\(\s*\(?\s*$")


class Use:
    """One use of a switch: the name, where, and whether that place reads or sets it."""

    def __init__(self, name, path, line, kind):
        self.name, self.path, self.line, self.kind = name, path, line, kind

    def where(self):
        return f"{self.path}:{self.line}"


def scan_text(text, path):
    """The switch uses in one file's text; the literals that look like names built at run time, as
    (path, line, literal); and the names outside every string literal and comment, as
    (path, line, name)."""
    uses, partial, outside = [], [], []
    if not any(marker in text for marker in MARKERS):
        return uses, partial, outside
    newlines = [i for i, char in enumerate(text) if char == "\n"]
    token = JS_TOKEN if os.path.splitext(path)[1].lower() in JS_SUFFIXES else TOKEN
    starts, ends = [], []
    for match in token.finditer(text):
        starts.append(match.start())
        ends.append(match.end())
        groups = match.groupdict()
        group = next((g for g in LITERAL_GROUPS if groups.get(g) is not None), None)
        if group is None:
            continue
        offset, content = match.start(group), groups[group]
        found = NAME.match(content)
        if not found:
            continue
        line = bisect.bisect_right(newlines, offset) + 1
        rest = content[found.end():]
        name = found.group(0)
        caller = CALLER.search(text[max(0, offset - 160):offset - 1])
        caller = caller.group(1) if caller else ""
        if rest == "" and not name.endswith("_"):
            uses.append(Use(name, path, line, "set" if caller in SETTERS else "read"))
        elif rest[:1] == "=" and caller in PUTENV and not name.endswith("_"):
            uses.append(Use(name, path, line, "set"))
        elif rest == "" or rest[:1] not in ("=", ":", " "):
            partial.append((path, line, content))
    for found in NAME_IN_CODE.finditer(text):
        index = bisect.bisect_right(starts, found.start()) - 1
        if index < 0 or found.start() >= ends[index]:
            outside.append((path, bisect.bisect_right(newlines, found.start()) + 1, found.group(0)))
    return uses, partial, outside


def code_files(root):
    """(relative posix path, absolute path) of the code files under src/ and launcher/."""
    for top in SCANNED:
        base = os.path.join(root, top)
        for directory, dirs, files in os.walk(base):
            dirs[:] = sorted(d for d in dirs if d not in SKIPPED_DIRS)
            for name in sorted(files):
                if os.path.splitext(name)[1].lower() in CODE_SUFFIXES:
                    full = os.path.join(directory, name)
                    yield Path(os.path.relpath(full, root)).as_posix(), full


def scan_tree(root):
    """Every switch use, run-time-built literal and name outside a literal under src/ and launcher/
    of `root`."""
    uses, partial, outside = [], [], []
    for relative, full in code_files(root):
        with open(full, encoding="utf-8", errors="replace") as handle:
            found, odd, bare = scan_text(handle.read(), relative)
        uses += found
        partial += odd
        outside += bare
    return uses, partial, outside


def read_registry(text):
    """The switches docs/switches.md has rows for: ({name: line}, {name: line}) for live rows and for
    rows under "Removed experiments". A row is a table line; only its first cell names switches."""
    rows, removed = {}, {}
    section = ""
    for number, line in enumerate(text.splitlines(), 1):
        if line.startswith("## "):
            section = line[3:].strip()
            continue
        if not line.startswith("|"):
            continue
        first = line.split("|")[1] if line.count("|") >= 2 else ""
        for name in re.findall(r"`(EDEN_(?:NX2|DKB)_[A-Z0-9_]+)`", first):
            (removed if section == REMOVED_SECTION else rows).setdefault(name, number)
    return rows, removed


def read_upstream(text):
    """The names of the upstream list: one per line, # starts a comment."""
    names = set()
    for line in text.splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            names.add(line)
    return names


def check(root, extra_upstream=()):
    """Compares the tree with the registry. Returns a dict of what was found, for report().
    `extra_upstream` are more names to take as upstream's (--upstream-ref)."""
    uses, partial, outside = scan_tree(root)
    registry = Path(root, REGISTRY)
    rows, removed = read_registry(registry.read_text(encoding="utf-8") if registry.is_file() else "")
    upstream_file = Path(root, UPSTREAM)
    upstream = read_upstream(upstream_file.read_text(encoding="utf-8")) if upstream_file.is_file() else set()
    upstream |= set(extra_upstream)
    by_name = {}
    for use in uses:
        by_name.setdefault(use.name, []).append(use)
    missing = {name: places for name, places in sorted(by_name.items())
               if name not in rows and name not in upstream}
    stale = {name: line for name, line in sorted(rows.items()) if name not in by_name}
    runtime = [entry for entry in partial if (entry[0], entry[2]) not in RUNTIME_PREFIXES]
    return {
        "found": by_name,
        "rows": rows,
        "removed": removed,
        "upstream": upstream,
        "missing": missing,
        "stale": stale,
        "runtime": runtime,
        "outside": outside,
        "upstream_gone": sorted(upstream - set(by_name)),
    }


def report(result, out, annotate=False):
    """Prints the result; returns the exit code (1 when a switch has no row)."""
    found, rows, upstream = result["found"], result["rows"], result["upstream"]
    documented = sorted(set(found) & set(rows))
    known = sorted(set(found) & (upstream - set(rows)))
    print(f"Switches used in {' and '.join(s + '/' for s in SCANNED)}: {len(found)}; with a row in "
          f"{REGISTRY}: {len(documented)}; upstream's without a row: {len(known)}; missing a row: "
          f"{len(result['missing'])}. Rows whose switch is used nowhere: {len(result['stale'])}.",
          file=out)
    for name, places in result["missing"].items():
        first = places[0]
        verb = "set" if all(place.kind == "set" for place in places) else "used"
        where = ", ".join(place.where() for place in places[:5])
        if len(places) > 5:
            where += f" and {len(places) - 5} more"
        note = ""
        if name in result["removed"]:
            note = f" ({REGISTRY}:{result['removed'][name]} lists it under \"{REMOVED_SECTION}\")"
        message = (f"{name} is {verb} at {where}, but {REGISTRY} has no row for it{note}. "
                   f"If the name is upstream's (`git grep -n {name} origin/upstream -- src`), "
                   f"regenerate {UPSTREAM} as its header says; otherwise add a row to {REGISTRY} "
                   f"(a new default-off switch goes under ## Experimental: switch, values, what it "
                   f"does, measured, code), as AGENTS.md's hard rule 3 asks.")
        if annotate:
            print(f"::error file={first.path},line={first.line}::{message}", file=out)
        print(f"error: {message}", file=out)
    for path, line, name in result["outside"]:
        message = (f"{path}:{line}: {name} appears outside a string literal; this check cannot "
                   f"classify it. Write the name as one whole string literal.")
        if annotate:
            print(f"::error file={path},line={line}::{message}", file=out)
        print(f"error: {message}", file=out)
    for path, line, literal in result["runtime"]:
        message = (f"{path}:{line}: \"{literal}\" looks like a switch name built at run time, which "
                   f"this check cannot follow. Use whole names, or list it in RUNTIME_PREFIXES of "
                   f"tools/ci/switch_registry.py with the names it builds documented.")
        if annotate:
            print(f"::error file={path},line={line}::{message}", file=out)
        print(f"error: {message}", file=out)
    for name, line in result["stale"].items():
        message = (f"{name} has a row ({REGISTRY}:{line}) but is used nowhere in src/ or launcher/: "
                   f"move it to \"{REMOVED_SECTION}\" or delete it if the switch is gone.")
        if annotate:
            print(f"::warning file={REGISTRY},line={line}::{message}", file=out)
        print(f"warning: {message}", file=out)
    if result["upstream_gone"]:
        print(f"note: {len(result['upstream_gone'])} names of {UPSTREAM} are used nowhere any more: "
              f"{', '.join(result['upstream_gone'])}", file=out)
    return 1 if result["missing"] or result["runtime"] or result["outside"] else 0


HEADER = """\
# Switches upstream Lichii (zortos293/Lichii, branch nx2) uses in src/ at commit {commit}.
# tools/ci/switch_registry.py does not ask for a docs/switches.md row for these; every other
# EDEN_NX2_* or EDEN_DKB_* name used in src/ or launcher/ needs one, unless --upstream-ref has it.
#
# Generated, do not edit by hand. CI also takes the names in src/ of the upstream branch's tip as
# upstream's (switch-registry.yml, --upstream-ref), so this list serves runs without that branch
# and names upstream has since dropped. Regenerating it after an upstream merge is housekeeping,
# from the upstream commit that was merged (the merge base of main and the upstream branch):
#
#   git fetch origin upstream
#   python3 -I tools/ci/switch_registry.py --write-upstream "$(git merge-base HEAD origin/upstream)"
#
# A name Guarana added is never listed here: it gets a row in docs/switches.md instead.
"""


def upstream_names(root, ref):
    """The switches used in src/ of the git commit `ref`, read from git archive."""
    commit = subprocess.run(["git", "-C", root, "rev-parse", "--verify", f"{ref}^{{commit}}"],
                            check=True, capture_output=True, text=True).stdout.strip()
    archive = subprocess.run(["git", "-C", root, "archive", "--format=tar", commit, "src"],
                             check=True, capture_output=True).stdout
    names = set()
    with tarfile.open(fileobj=io.BytesIO(archive), mode="r:") as tar:
        for member in tar:
            if not member.isfile() or os.path.splitext(member.name)[1].lower() not in CODE_SUFFIXES:
                continue
            text = tar.extractfile(member).read().decode("utf-8", errors="replace")
            uses, _, _ = scan_text(text, member.name)
            names.update(use.name for use in uses)
    return commit, sorted(names)


def write_upstream(root, ref):
    commit, names = upstream_names(root, ref)
    path = Path(root, UPSTREAM)
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(HEADER.format(commit=commit))
        handle.writelines(name + "\n" for name in names)
    print(f"Wrote {len(names)} names used in src/ at {commit} to {UPSTREAM}.")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--root", default=str(Path(__file__).resolve().parent.parent.parent),
                        help="repository root (default: the one this script is in)")
    parser.add_argument("--write-upstream", metavar="REF",
                        help=f"rewrite {UPSTREAM} from src/ of this git commit and exit")
    parser.add_argument("--upstream-ref", metavar="REF",
                        help="also take the switches used in src/ of this git commit as upstream's "
                             "(CI passes the tip of the upstream branch)")
    args = parser.parse_args(argv)
    if args.write_upstream:
        write_upstream(args.root, args.write_upstream)
        return 0
    extra = upstream_names(args.root, args.upstream_ref)[1] if args.upstream_ref else ()
    return report(check(args.root, extra), sys.stdout,
                  annotate=os.environ.get("GITHUB_ACTIONS") == "true")


if __name__ == "__main__":
    sys.exit(main())
