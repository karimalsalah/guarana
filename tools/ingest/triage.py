#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Triage for a new issue (.github/workflows/triage.yml).

  python -I tools/ingest/triage.py [--event "$GITHUB_EVENT_PATH"] [--dry-run]

Labels the issue from its form answers and from words in them (stutter, frame-pacing, boot-time,
Crash, intel-arc, amd-igpu, handheld), asks once and politely for the log when a crash report has
none (with needs-info), and adds agent-ready to an agent task whose acceptance test says something
checkable. It only adds labels and writes at most one comment: it never removes a label, closes,
locks or marks an issue stale. Standard library only; docs/FEEDBACK.md describes the loop.
"""

import argparse
import json
import os
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))  # -I leaves the script's directory off sys.path.

import github_api  # noqa: E402
from issue_forms import (GPU_OPTIONS, fenced_blocks, form_headings, gpu_labels, normalize_text,  # noqa: E402
                         parse_form, strip_markup, without_code)

CRASH_LABEL = "Crash"  # as in .github/labels.yml and crash.yml
LOG_SECTION = "Log / crash dump"
LOG_MARKER = "<!-- guarana-triage:log-request -->"
# label-issues.yml already asks for the log when an issue arrives labelled Crash (the crash form)
# with an empty "Log / crash dump" answer. Triage asks in every other case, so the two never both
# comment. tests/test_triage.py fails once label-issues.yml stops asking: then set this to False.
LABEL_ISSUES_ASKS_FOR_LOGS = False
# Sections that hold no words of the reporter's own: logs, rows, images, the form's own boxes.
NOT_SCANNED = {LOG_SECTION, "tools/bench rows", "Screenshot", "Before submitting"}

_CLAUSE = re.compile(r"(?i)[.;,!?\n(]|\b(?:but|and|however|while|whereas|though|although|yet)\b")
_NEGATION = re.compile(r"(?i)(?:\b(?:no|not|never|without|none|zero|nor)|n't|\bno (?:longer|more)|\bfree of|\b0)"
                       r"\W*(?:[\w-]+\W+){0,4}$")
_FREE_AFTER = re.compile(r"(?i)^[ -]?free\b")

KEYWORDS = {
    "stutter": re.compile(r"(?i)\b(?:stutter(?:s|ed|ing|y)?|micro[ -]?stutter(?:s|ing)?|hitch(?:es|ing|y)?|"
                          r"frame[ -]?time spikes?|spikes? in (?:the )?frame[ -]?times?|jank(?:y)?)\b"),
    "frame-pacing": re.compile(r"(?i)\b(?:frame[ -]?pacing|pacing|judder(?:s|ing|y)?|uneven frame[ -]?times?|"
                               r"inconsistent frame[ -]?times?|frame[ -]?time variance|cadence)\b"),
    "boot-time": re.compile(r"(?i)\b(?:boot[ -]?times?|slow (?:to )?(?:boot|start|launch|load)(?:s|ing|up)?|"
                            r"start[ -]?up times?|launch times?|time to (?:boot|start|launch|gameplay)|"
                            r"takes? (?:a long time|ages|forever|minutes|\d+ ?(?:s|sec|seconds|min|minutes)) to "
                            r"(?:boot|start|launch|load|get in)|long (?:boot|start[ -]?up|launch)|"
                            r"stuck (?:on|at) (?:the )?(?:boot|loading|splash|startup)(?: screen)?)\b"),
    CRASH_LABEL: re.compile(r"(?i)\b(?:crash(?:es|ed|ing)?|ctd|segfault(?:s|ed)?|segmentation fault|access violation|"
                            r"0xc0000005|device[ _-]?lost|device loss|force[ -]?clos(?:es|ed|ing)|"
                            r"exits? to (?:the )?desktop|closes (?:itself|by itself|on its own|without (?:an )?error))\b"),
}
# Only when the form names no particular GPU ("Other", "Any GPU", or no form at all).
HARDWARE_KEYWORDS = {
    "intel-arc": re.compile(r"(?i)\b(?:intel[ -]?arc|arc[ -]?(?:1[34]0[tv]|[ab]\d{3}m?|b390)|lunar[ -]?lake|"
                            r"meteor[ -]?lake|arrow[ -]?lake|panther[ -]?lake|xe2|xe3|xe-lpg)\b"),
    "amd-igpu": re.compile(r"(?i)\b(?:radeon[ -]?(?:\d{3}m|\d{4}s)|(?:660|680|740|760|780|820|840|860|880|890)m|"
                           r"(?:8040|8050|8060)s|ryzen[ -]?z[12]|z[12][ -]?(?:extreme|go)|rog[ -]?ally|xbox[ -]?ally|"
                           r"legion[ -]?go|steam[ -]?deck|strix[ -]?(?:point|halo)|hawk[ -]?point|van[ -]?gogh|"
                           r"amd[ -]?(?:igpu|apu|integrated))\b"),
    "handheld": re.compile(r"(?i)\b(?:handheld|rog[ -]?ally|xbox[ -]?ally|legion[ -]?go|steam[ -]?deck|msi[ -]?claw|"
                           r"aya[ -]?neo|onexplayer|onexfly|gpd[ -]?win|zotac[ -]?zone)\b"),
}
# Labels this script may add; tests/test_triage.py checks each is in .github/labels.yml.
LABELS = tuple(KEYWORDS) + tuple(HARDWARE_KEYWORDS) + ("needs-info", "agent-ready")

_LOG_LINES = re.compile(r"(?i)\[\s*\d+\.\d+\s*\]|<(?:critical|error|warning|info|debug|trace)>|lichii version|"
                        r"booting game|host crash|eden_log")
_ATTACHMENT = re.compile(r"(?i)github\.com/user-attachments/files/\S+|\]\([^)\s]+\.(?:txt|log|zip|7z)\)")
_PLACEHOLDER = re.compile(r"(?i)^(?:n/?a|tbd|tba|todo|to do|none|nothing|unknown|later|not sure|idk|see above|"
                          r"same as (?:above|the goal)|\?+|-+|\.+)[.!]*$")

LOG_REQUEST = LOG_MARKER + """
Thanks for the report! A crash is much easier to track down with the emulator's log. Could you attach it to this issue?

1. In the emulator, open **File → Open Lichii Folders → Log Folder**. On Windows that is `%APPDATA%\\lichii\\log`, or `user\\log` next to `lichii.exe` in a portable install.
2. Take `eden_log.txt`. If you started the emulator again after the crash, the crashed session is `eden_log.txt.old.txt`.
3. The log has folder paths from your machine in it: replace your user name in them (as in `C:\\Users\\<name>`) with `<user>` before you share it.
4. Drag the file into a comment here.

Please don't attach crash dumps (`.dmp`), keys, firmware or game files; a maintainer will ask if something else would help. [Troubleshooting]({troubleshooting}) has more about the log.
"""


def label_names(issue):
    return [label.get("name", "") if isinstance(label, dict) else str(label) for label in issue.get("labels") or []]


def mentions(text, pattern):
    """True when pattern matches somewhere it is not negated ("no crashes", "stutter-free")."""
    for match in pattern.finditer(text):
        window = text[max(0, match.start() - 40):match.start()]
        cuts = [cut.end() for cut in _CLAUSE.finditer(window)]
        clause = window[cuts[-1]:] if cuts else window
        if _NEGATION.search(clause) or _FREE_AFTER.search(text[match.end():match.end() + 6]):
            continue
        return True
    return False


def prose_of(issue, sections, body):
    """The reporter's own words: the title without its form prefix and the answers, without logs,
    code, headings or markup."""
    title = re.sub(r"^\s*\[[^\]]*\]\s*", "", normalize_text(issue.get("title") or ""))
    if sections:
        text = "\n".join(answer for label, answer in sections.items() if label not in NOT_SCANNED)
    else:
        text = re.sub(r"(?m)^[ \t]{0,3}#{1,6}[ \t].*$", "", body)
    return strip_markup(without_code(title + "\n" + text))


def has_log(sections, body):
    answer = sections.get(LOG_SECTION)
    if answer is not None:
        return bool(_ATTACHMENT.search(answer) or _LOG_LINES.search(answer) or len(strip_markup(answer).strip()) >= 200)
    return bool(_ATTACHMENT.search(body) or any(_LOG_LINES.search(content) for _, content in fenced_blocks(body)))


def substantive(text):
    """An acceptance test that says something checkable: a few words, not n/a or tbd."""
    text = re.sub(r"\s+", " ", strip_markup(normalize_text(text))).strip()
    return len(text) >= 20 and len(text.split()) >= 3 and not _PLACEHOLDER.match(text)


def reports_a_problem(body):
    """True for issues that report something going wrong: the forms with a log field, device
    reports and issues without a form. Agent tasks, performance ideas and feature requests that
    mention a crash are about one, so nobody is asked for a log there."""
    headings = form_headings(body)
    return not headings - {"Issue"} or LOG_SECTION in headings or "Device" in headings


def agent_ready(sections):
    if not all(sections.get(label) for label in ("Goal", "Files", "Acceptance test")):
        return False
    if (sections.get("Size") or "").strip().casefold().startswith("large"):
        return False  # the form says to split it first
    return substantive(sections["Acceptance test"])


def triage(issue):
    """(labels to add, whether to ask for the log) for a new issue."""
    body = normalize_text(issue.get("body") or "")
    sections = parse_form(body)
    present = {name.casefold() for name in label_names(issue)}
    prose = prose_of(issue, sections, body)

    labels = {label for label, pattern in KEYWORDS.items() if mentions(prose, pattern)}
    answer = re.sub(r"\s+", " ", sections.get("Graphics card", "")).strip().casefold()
    option = next((option for option in GPU_OPTIONS if option.casefold() == answer), None)
    if option and GPU_OPTIONS[option][1] != "other":
        labels |= gpu_labels(option)
    else:
        labels |= {label for label, pattern in HARDWARE_KEYWORDS.items() if pattern.search(prose)}
    if (sections.get("Form factor") or "").strip().casefold() == "handheld":
        labels.add("handheld")

    crash_form = CRASH_LABEL.casefold() in present
    crash = crash_form or CRASH_LABEL in labels
    label_issues_asks = LABEL_ISSUES_ASKS_FOR_LOGS and crash_form and not sections.get(LOG_SECTION)
    request = crash and reports_a_problem(body) and not label_issues_asks and not has_log(sections, body)
    if request:
        labels.add("needs-info")
    if agent_ready(sections):
        labels.add("agent-ready")
    return sorted(label for label in labels if label.casefold() not in present), request


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--event", default=os.environ.get("GITHUB_EVENT_PATH"), help="the issues event payload")
    parser.add_argument("--dry-run", action="store_true", help="print what it would do and change nothing")
    args = parser.parse_args(argv)
    issue = github_api.read_event(args.event).get("issue") or {}
    if not issue.get("number") or "pull_request" in issue:
        print("No issue in the event")
        return 0
    if (issue.get("user") or {}).get("type") == "Bot":
        print("Opened by a bot: left alone")
        return 0
    labels, request = triage(issue)
    number = issue["number"]
    print(json.dumps({"issue": number, "add_labels": labels, "log_request": request}))
    if args.dry_run:
        return 0
    github, repo = github_api.from_environment()
    if not re.fullmatch(r"[\w.-]+/[\w.-]+", repo):
        print("::error::GITHUB_REPOSITORY is not set")
        return 1
    try:
        if labels:
            github.post(f"/repos/{repo}/issues/{number}/labels", {"labels": labels})
        if request:
            body = LOG_REQUEST.replace("{troubleshooting}",
                                       f"https://github.com/{repo}/blob/HEAD/docs/TROUBLESHOOTING.md#first-find-the-log")
            github_api.upsert_comment(github, repo, number, LOG_MARKER, body)
    except github_api.GitHubError as error:
        print(f"::error::{error}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
