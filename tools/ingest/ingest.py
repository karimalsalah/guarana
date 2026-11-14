#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Device reports (the device-report issue form) to the community leaderboard on benchmarks.html.

  python -I tools/ingest/ingest.py parse <body.md> [--number N] [--created YYYY-MM-DD] [--login L]
  python -I tools/ingest/ingest.py publish [--dry-run]
  python -I tools/ingest/ingest.py comment [--issue N] [--dry-run]

parse prints what one issue body becomes: its record, or the problems that keep it off the
leaderboard. publish parses every issue labelled device-report again, writes
site/data/community.json as one commit on top of the default branch to bot/community-reports, and
opens, updates or closes that branch's pull request. comment writes, or updates in place, one
comment on a report with its parsed row or what to fix. publish and comment take the repository
from GITHUB_REPOSITORY, the issue from GITHUB_EVENT_PATH and use GITHUB_TOKEN; DRY_RUN=true is
--dry-run. Standard library only. docs/FEEDBACK.md describes the whole loop.
"""

import argparse
import datetime
import importlib.util
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))  # -I leaves the script's directory off sys.path.

import github_api  # noqa: E402
from issue_forms import (GPU_OPTIONS, fenced_blocks, md_escape, normalize_newlines, normalize_text,  # noqa: E402
                         one_line, parse_checkboxes, parse_form, scrub)

ROOT = HERE.parent.parent
DATA_PATH = "site/data/community.json"
BRANCH = "bot/community-reports"
LABEL = "device-report"
COMMENT_MARKER = "<!-- guarana-ingest -->"
PR_MARKER = "<!-- guarana-ingest-pr -->"
SCHEMA_VERSION = 1
PULL_TITLE = "site: community leaderboard data from device reports"
BOT_IDENTITY = ("github-actions[bot]", "41898282+github-actions[bot]@users.noreply.github.com")
# Closing a report this way takes it off the leaderboard at the next run.
EXCLUDED_STATE_REASONS = {"not_planned": "closed as not planned", "duplicate": "closed as a duplicate"}

# The device-report form: option texts as in .github/ISSUE_TEMPLATE/device-report.yml.
GAMES = {
    "Ocarina of Time": ("oot", "The Legend of Zelda: Ocarina of Time"),
    "Mario Kart World": ("mkw", "Mario Kart World"),
    "Donkey Kong Bananza": ("dkb", "Donkey Kong Bananza"),
    "Fire Emblem": ("fe", "Fire Emblem: Fortune's Weave"),
    "Splatoon Raiders": ("raiders", "Splatoon Raiders"),
    "Pokopia": ("pokopia", "Pokémon Pokopia"),
    "Hyrule Warriors": ("hw", "Hyrule Warriors: Age of Imprisonment"),
    "Welcome Tour": ("welcome-tour", "Nintendo Switch 2 Welcome Tour"),
    "Other / not listed": ("other", None),
}
FORM_FACTORS = ("Laptop", "Handheld", "Mini PC", "Desktop")
POWER_SOURCES = ("Plugged in", "On battery")
# The "Before submitting" boxes, by the start of their text.
REQUIRED_CHECKS = (
    ("my game, keys and firmware are dumped from hardware i own",
     "My game, keys and firmware are dumped from hardware I own"),
    ("nothing i attach contains personal data", "Nothing I attach contains personal data"),
)
REQUIRED = ("Device", "Form factor", "Graphics card", "GPU driver", "Emulator build", "Game", "Scene", "Results")
OPTIONAL = ("CPU", "RAM", "Operating system", "Power source", "Power mode", "Settings and switches")
CREDIT_LABEL = "Credit me as"  # optional; the consent for the handle on the Records board
GITHUB_LOGIN = re.compile(r"[A-Za-z0-9](?:[A-Za-z0-9-]{0,38})")

FPS_RANGE = (0.5, 500.0)
P50_RANGE = (0.5, 2000.0)
P99_RANGE = (0.5, 5000.0)
MAX_RUNS = 16

DOC_DESCRIPTION = ("Community device reports for the leaderboard on benchmarks.html: one record per issue "
                   "labelled device-report, keyed by the issue number. Generated from the issues by "
                   ".github/workflows/ingest-reports.yml (tools/ingest/ingest.py); edit the issue, not this file.")
FIELD_DOCS = {
    "fps": "Game fps, the frames the game itself renders, as reported. From the tools/bench row when the "
           "report has one, else parsed from the report's Results.",
    "p50_ms, p99_ms": "Median and 99th percentile frame time in ms, or null when not reported.",
    "baseline": "The figure the report compares with (for example the build without a switch), or null.",
    "scene_ok": "true only when the numbers come from a tools/bench row that is valid and passed its scene "
                "check against the tester's reference screenshot; null otherwise.",
    "source": "tools/bench or manual.",
    "gpu_vendor, gpu_type": "intel, amd, nvidia or other; integrated, discrete or unknown, from the form's "
                            "Graphics card answer.",
    "runs": "Every valid tools/bench row of the report (variant, scene, fps, p50_ms, p99_ms, switches).",
    "notes": "Caveats a reader should see next to the numbers.",
    "credit": "The reporter's GitHub handle when they filled in \"Credit me as\" with the account that opened the "
              "report; null otherwise. It is the only personal detail a record carries, and only by consent.",
    "records": "The Records board: per GPU class (the form's Graphics card answer) and scene, the best row that is "
               "valid with scene_ok true, by game fps, then lower p99, then the earlier report. credit is null "
               "when the reporter did not ask for one.",
}


# ---------------------------------------------------------------------------------------------
# Small helpers

def utc_today():
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d")


def to_number(text):
    return float(text.replace(",", "."))


def rounded(value, digits):
    return None if value is None else round(float(value), digits)


def is_number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def has_label(issue, name):
    return any((label.get("name") if isinstance(label, dict) else label) == name for label in issue.get("labels") or [])


def program_titles(root=ROOT):
    """{program id: title} from the table in AGENTS.md ("Add ids here when a report identifies a new game")."""
    try:
        text = (Path(root) / "AGENTS.md").read_text(encoding="utf-8")
    except OSError:
        return {}
    return {match.group(1).upper(): match.group(2)
            for match in re.finditer(r"^\|\s*([0-9A-Fa-f]{16})\s*\|\s*([^|\n]+?)\s*\|\s*$", text, re.M)}


def load_bench_validator(root=ROOT):
    """bench.validate_row from tools/bench/bench.py, the check CI runs on contributed rows; None
    without it."""
    path = Path(root) / "tools" / "bench" / "bench.py"
    if not path.is_file():
        return None
    spec = importlib.util.spec_from_file_location("guarana_bench", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.validate_row


def minimal_validate(row):
    """The parts of bench.validate_row that need no schema, for use without tools/bench."""
    errors = []
    if row.get("schema_version") != 1:
        errors.append("schema_version is not 1")
    if not isinstance(row.get("valid"), bool):
        errors.append("valid is not a boolean")
    if row.get("valid") and row.get("scene_ok") is not True:
        errors.append("a valid row needs scene_ok true")
    if row.get("valid") and not (is_number(row.get("fps")) and row["fps"] > 0):
        errors.append("a valid row needs fps > 0")
    if not re.fullmatch(r"[0-9a-f]{10}", str(row.get("machine_id", ""))):
        errors.append("machine_id is not 10 hex digits")
    return errors


# ---------------------------------------------------------------------------------------------
# Normalisation

def normalize_driver(raw, logins=()):
    """"32.0.101.9175", "Adrenalin 25.9.1", "Mesa 25.2.3" from what the reporter wrote."""
    text = one_line(raw, 80, logins)
    mesa = re.search(r"(?i)\bmesa\b\D{0,12}(\d+(?:\.\d+){1,3}(?:-[\w.]+)?)", text)
    if mesa:
        return "Mesa " + mesa.group(1)
    adrenalin = re.search(r"(?i)\badrenalin(?:\s+edition)?\D{0,12}(\d+(?:\.\d+){1,3})", text)
    if adrenalin:
        return "Adrenalin " + adrenalin.group(1)
    version = re.search(r"\b\d+(?:\.\d+)+\b", text)
    if version:
        return version.group(0)
    return one_line(raw, 40, logins)


def normalize_build(raw, logins=()):
    """"v20.0.2", "nightly 2026-11-20 (1a2b3c4)", "PR #123 test build (abc1234)", or the text."""
    text = one_line(raw, 120, logins)
    match = re.search(r"(?i)\bguarana-pr(\d+)-([0-9a-f]{7,40})", text)
    if match:
        return f"PR #{match.group(1)} test build ({match.group(2)[:7]})"
    if re.search(r"(?i)\bnightly\b", text):
        date = re.search(r"\b\d{4}-\d\d-\d\d\b", text)
        sha = re.search(r"\b(?=[0-9a-f]*[a-f])(?=[0-9a-f]*\d)[0-9a-f]{7,40}\b", text)
        return "nightly" + (" " + date.group(0) if date else "") + (f" ({sha.group(0)[:7]})" if sha else "")
    match = re.search(r"(?i)(?<![\w.])v?(\d+\.\d+\.\d+)(?![\w.])", text)
    if match:
        return "v" + match.group(1)
    match = re.search(r"(?i)\b(?:pr|pull request)\s*#?(\d+)", text)
    if match:
        return f"PR #{match.group(1)} test build"
    match = re.search(r"\b(?=[0-9a-f]*[a-f])(?=[0-9a-f]*\d)[0-9a-f]{7,40}\b", text)
    if match:
        return match.group(0)[:10]
    return one_line(raw, 60, logins)


def clean_gpu_name(name):
    name = re.sub(r"\((?:R|TM)\)|[®™]", "", name)
    name = re.sub(r"(?i)\(\s*\d+\s*[GM]B\s*\)", "", name)
    return re.sub(r"\s+", " ", name).strip()


_SWITCH = re.compile(r"\b(EDEN_(?:NX2|DKB)_[A-Z0-9_]+)(?:[ \t]*=[ \t]*([^\s,;)]+))?")
_FG_STATE = re.compile(r"(?i)\b(?:frame[ -]?gen(?:eration)?|fg)\b[ \t]*(?:[:=][ \t]*|is[ \t]+|was[ \t]+)?"
                       r"(on|off|enabled|disabled|true|false|yes|no)\b")
_FG_WITHOUT = re.compile(r"(?i)\b(?:no|without)[ \t]+(?:frame[ -]?gen(?:eration)?|fg)\b")
_FG_WITH = re.compile(r"(?i)\bwith[ \t]+(?:frame[ -]?gen(?:eration)?|fg)\b")


def switches_of(text, logins=()):
    found = []
    for match in _SWITCH.finditer(normalize_text(text)):
        item = match.group(1) + ("=" + scrub(match.group(2), logins) if match.group(2) else "")
        if item not in found:
            found.append(item)
    return found[:12]


def frame_generation(*texts):
    for text in texts:
        text = normalize_text(text)
        match = _FG_STATE.search(text)
        if match:
            return "on" if match.group(1).lower() in ("on", "enabled", "true", "yes") else "off"
        if _FG_WITHOUT.search(text):
            return "off"
        if _FG_WITH.search(text):
            return "on"
    return None


# ---------------------------------------------------------------------------------------------
# Manual results: the free text of the Results answer

_NUM = r"\d{1,4}(?:[.,]\d{1,3})?"
_FPS_AFTER = re.compile(
    r"(?i)(?P<approx>~|≈|\b(?:about|around|approx(?:\.|imately)?|roughly|circa)[ \t]+)?"
    r"(?P<a>" + _NUM + r")(?:[ \t]*(?P<sep>->|=>|→|-|–|—|\bto\b)[ \t]*(?P<b>" + _NUM + r"))?"
    r"[ \t]*(?:fps|frames per second|frames/s)\b")
_FPS_BEFORE = re.compile(
    r"(?i)\b(?:fps|frame[ -]?rate)\b[ \t]*(?:[:=]|of|is|was|at)?[ \t]*(?P<approx>~|≈)?(?P<a>" + _NUM + r")"
    r"(?!\d|[.,]\d|[ \t]*(?:ms|%|°|w\b|p\b|hz|mhz|ghz|gb|mb|x\b|s\b|fps))")
_P50 = (re.compile(r"(?i)\b(?:p50|median|50th percentile)\b(?:[ \t]*frame[ -]?times?)?[ \t]*(?:[:=]|of|is|was|at)?"
                   r"[ \t]*~?(?P<v>" + _NUM + r")[ \t]*ms\b"),
        re.compile(r"(?i)(?P<v>" + _NUM + r")[ \t]*ms[ \t]*\(?[ \t]*(?:p50|median)\b"))
_P99 = (re.compile(r"(?i)\b(?:p99|99th percentile)\b(?:[ \t]*frame[ -]?times?)?[ \t]*(?:[:=]|of|is|was|at)?"
                   r"[ \t]*~?(?P<v>" + _NUM + r")[ \t]*ms\b"),
        re.compile(r"(?i)(?P<v>" + _NUM + r")[ \t]*ms[ \t]*\(?[ \t]*(?:p99|99th percentile)\b"))
_P50_P99 = re.compile(r"(?i)\bp50[ \t]*/[ \t]*p99\b[^0-9\n]{0,20}(?P<a>" + _NUM + r")[ \t]*/[ \t]*(?P<b>" + _NUM +
                      r")[ \t]*ms\b")
# A figure that is a low, a peak or a cap, not the result: "1% low 18 fps", "drops to 18 fps",
# "40 fps max", "capped at 30 fps". Only right next to the figure: "max settings, 30 fps" is a result.
_QUALIFIER_BEFORE = re.compile(r"(?i)(?:\b(?:0\.1|1)[ \t]*%[ \t]*lows?|\bmin(?:imum)?|\bmax(?:imum)?|\bpeak(?:s|ed)?|"
                               r"\bworst|\blowest|\bhighest|\bdrops?(?:[ \t]+(?:down[ \t]+)?to)?|\bdips?(?:[ \t]+to)?|"
                               r"\btarget(?:s|ed)?|\bcap(?:ped)?(?:[ \t]+at)?|\blimit(?:ed)?(?:[ \t]+(?:to|at))?)"
                               r"[ \t]*(?:[:=]|of|at|to)?[ \t]*$")
_QUALIFIER_AFTER = re.compile(r"(?i)^[ \t]*\(?[ \t]*(?:(?:0\.1|1)[ \t]*%[ \t]*lows?|min(?:imum)?|max(?:imum)?|peaks?|"
                              r"lows?|highs?|target|cap)\b")
# "14.8, 22.8 fps" or "30/60 fps": several figures before one unit.
_LIST_BEFORE = re.compile(r"(?<![\dx×.,])\d{1,4}(?:[.,]\d+)?[ \t]*(?:,|/|&|\band\b)[ \t]*$")
_BASELINE = re.compile(r"(?i)\b(?:baseline|before|without|stock|default|unpatched|vanilla|unset|previous build|"
                       r"old build|fix off|switch off)\b")
_CLAUSE = re.compile(r"(?i)[.;,!?\n(]|\b(?:but|and|however|while|whereas|though|although|yet)\b")


class Figure:
    def __init__(self, start, end, fps, approximate=False, explicit=None):
        self.start, self.end, self.fps, self.approximate = start, end, fps, approximate
        self.explicit = explicit  # "baseline" or "result" for "X -> Y fps"
        self.p50 = self.p99 = None


def _distance(span, figure):
    start, end = span
    if end <= figure.start:
        return figure.start - end
    if start >= figure.end:
        return start - figure.end
    return 0


def _clause_before(text, start, size=32):
    window = text[max(0, start - size):start]
    cuts = [match.end() for match in _CLAUSE.finditer(window)]
    return window[cuts[-1]:] if cuts else window


def _clause_after(text, end, size=16):
    window = text[end:end + size]
    match = _CLAUSE.search(window)
    return window[:match.start()] if match else window


def parse_results(raw):
    """({"fps", "approximate", "p50_ms", "p99_ms", "baseline"}, None) from free text, or (None, the
    problem). Anything that does not read one way is a problem, never a guess."""
    text = strip_for_numbers(raw)
    figures, taken = [], []
    for match in _FPS_AFTER.finditer(text):
        a, b, sep = match.group("a"), match.group("b"), match.group("sep")
        approx = bool(match.group("approx"))
        if not b and _LIST_BEFORE.search(text[max(0, match.start() - 16):match.start()]):
            return None, ("Results lists several figures before one \"fps\". Keep one result, or a baseline and a "
                          'result ("Baseline 14.8 fps. With EDEN_NX2_GM=11: 22.8 fps"), or paste tools/bench rows.')
        if b and (sep in ("->", "=>", "→") or (sep.lower() == "to" and re.search(r"(?i)\bfrom[ \t]*$",
                                                                                           text[:match.start()]))):
            middle = match.start("b")
            figures.append(Figure(match.start(), middle, to_number(a), approx, "baseline"))
            figures.append(Figure(middle, match.end(), to_number(b), approx, "result"))
        elif b:
            figures.append(Figure(match.start(), match.end(), (to_number(a) + to_number(b)) / 2, True))
        else:
            figures.append(Figure(match.start(), match.end(), to_number(a), approx))
        taken.append((match.start(), match.end()))
    for match in _FPS_BEFORE.finditer(text):
        if not any(start < match.end() and match.start() < end for start, end in taken):
            figures.append(Figure(match.start(), match.end(), to_number(match.group("a")), bool(match.group("approx"))))
    figures.sort(key=lambda figure: figure.start)

    candidates = [figure for figure in figures
                  if figure.explicit or not (_QUALIFIER_BEFORE.search(text[max(0, figure.start - 24):figure.start])
                                             or _QUALIFIER_AFTER.search(text[figure.end:figure.end + 16]))]
    if not figures:
        return None, ('Results names no fps figure. Write it as "22.8 fps", or paste tools/bench rows '
                      "into their field.")
    if not candidates:
        return None, ("Results names only lows, peaks or caps. Add the average game fps, for example "
                      '"Average 22.8 fps".')

    pairs = []
    for match in _P50_P99.finditer(text):
        pairs.append(("p50", (match.start(), match.end()), to_number(match.group("a"))))
        pairs.append(("p99", (match.start(), match.end()), to_number(match.group("b"))))
    for kind, patterns in (("p50", _P50), ("p99", _P99)):
        for pattern in patterns:
            for match in pattern.finditer(text):
                span = (match.start(), match.end())
                if not any(k == kind and s[0] <= span[0] < s[1] for k, s, _ in pairs):
                    pairs.append((kind, span, to_number(match.group("v"))))
    for kind, span, value in pairs:
        nearest = min(candidates, key=lambda figure: _distance(span, figure))
        if getattr(nearest, kind) is None:
            setattr(nearest, kind, value)

    explicit = [figure for figure in candidates if figure.explicit]
    if len(explicit) == 2 and len(candidates) == 2:
        baseline, result = explicit
    elif len(candidates) == 1:
        baseline, result = None, candidates[0]
    elif len(candidates) == 2:
        # The baseline is the figure whose own clause says so: "Baseline 14.8 fps", "14.8 fps without it".
        marked = [i for i, figure in enumerate(candidates)
                  if _BASELINE.search(_clause_before(text, figure.start, 40))
                  or _BASELINE.search(_clause_after(text, figure.end, 30))]
        if len(marked) != 1:
            return None, ("Results has two fps figures and does not say which one is the baseline. Write it as "
                          '"Baseline 14.8 fps. With EDEN_NX2_GM=11: 22.8 fps", or paste tools/bench rows.')
        baseline, result = candidates[marked[0]], candidates[1 - marked[0]]
    else:
        listed = ", ".join(f"{figure.fps:g}" for figure in candidates[:6])
        return None, (f"Results has {len(candidates)} fps figures ({listed}) and it is not clear which one to list. "
                      'Keep one result, or a baseline and a result ("Baseline 14.8 fps. With EDEN_NX2_GM=11: '
                      '22.8 fps"), or paste tools/bench rows.')
    parsed = {"fps": result.fps, "approximate": result.approximate, "p50_ms": result.p50, "p99_ms": result.p99,
              "baseline": None}
    if baseline is not None:
        parsed["baseline"] = {"fps": baseline.fps, "p50_ms": baseline.p50, "p99_ms": baseline.p99}
    return parsed, None


def strip_for_numbers(raw):
    """The Results answer without markup, code fences and switch assignments (EDEN_NX2_GM=11 is not
    a figure)."""
    text = normalize_text(raw)
    text = re.sub(r"(?m)^[ \t]*(?:`{3,}|~{3,}).*$", "", text)
    text = text.replace("`", "").replace("**", "")
    return _SWITCH.sub(lambda match: match.group(1), text)


# ---------------------------------------------------------------------------------------------
# tools/bench rows

def json_objects(text, limit=400):
    """The JSON objects in text (JSON lines, pretty-printed objects or arrays of them), skipping
    anything that is not JSON."""
    decoder, found, index, attempts = json.JSONDecoder(), [], 0, 0
    while len(found) < limit and attempts < 4000:
        starts = [position for position in (text.find("{", index), text.find("[", index)) if position >= 0]
        if not starts:
            break
        start = min(starts)
        attempts += 1
        try:
            value, end = decoder.raw_decode(text, start)
        except (ValueError, RecursionError):
            index = start + 1
            continue
        if isinstance(value, dict):
            found.append(value)
        elif isinstance(value, list):
            found.extend(item for item in value if isinstance(item, dict))
        index = end
    return found


def bench_rows(sections, body):
    """The tools/bench rows of a report: from its "tools/bench rows" answer and any fenced json
    block, without duplicates."""
    texts = [sections.get("tools/bench rows", "")]
    texts += [content for info, content in fenced_blocks(body) if info in ("", "json", "jsonl", "javascript")]
    rows, seen = [], set()
    for text in texts:
        for value in json_objects(text):
            if "schema_version" in value and ("machine_id" in value or "fps" in value):
                key = json.dumps(value, sort_keys=True)
                if key not in seen:
                    seen.add(key)
                    rows.append(value)
    return rows


_TRACE_ENV = re.compile(r"_(?:TRACE|PROFILE|STATS|CHECK|VERIFY|DUMP)\b")


def rank_rows(rows):
    """rows best first: the baseline variant, then rows without tracing switches; newest first within each."""
    def traced(row):
        env = row.get("variant_env") if isinstance(row.get("variant_env"), dict) else {}
        return any(_TRACE_ENV.search(str(key)) for key in env)

    rows = sorted(rows, key=lambda row: str(row.get("timestamp_utc", "")), reverse=True)
    return sorted(rows, key=lambda row: (row.get("variant") != "baseline", traced(row)))


def row_switches(row, logins):
    env = row.get("variant_env") if isinstance(row.get("variant_env"), dict) else {}
    return [f"{scrub(str(key), logins)[:60]}={scrub(str(value), logins)[:80]}" for key, value in list(env.items())[:12]]


def row_name(row, logins):
    return scrub(str(row.get("variant", "?")), logins)[:40]


def summarize_row(row, logins):
    return {"variant": row_name(row, logins),
            "scene": scrub(str(row.get("scene", "")), logins)[:60],
            "fps": rounded(row.get("fps"), 2), "p50_ms": rounded(row.get("p50_ms"), 1),
            "p99_ms": rounded(row.get("p99_ms"), 1), "switches": row_switches(row, logins)}


def select_rows(rows, validate, logins=()):
    """(usable rows, best first; [why each other row was left out])."""
    usable, left_out = [], []
    for row in rows:
        name = row_name(row, logins)
        if row.get("probe"):
            left_out.append(f"{name}: a probe (shaders rejected on purpose)")
            continue
        try:
            errors = validate(row)
        except Exception as error:  # noqa: BLE001 - a malformed row is a row that does not validate
            errors = [f"unreadable ({type(error).__name__})"]
        if errors:
            detail = scrub("; ".join(str(error) for error in errors[:3]), logins)[:200]
            left_out.append(f"{name}: does not pass `bench.py validate` ({detail})")
        elif not row.get("valid"):
            reasons = row.get("invalid_reasons") if isinstance(row.get("invalid_reasons"), list) else []
            detail = scrub(", ".join(str(reason) for reason in reasons), logins)[:120] or "not valid"
            left_out.append(f"{name}: invalid ({detail})")
        else:
            usable.append(row)
    return rank_rows(usable), left_out


# ---------------------------------------------------------------------------------------------
# One report

class Report:
    def __init__(self, number, record=None, problems=(), warnings=(), excluded=None):
        self.number = number
        self.record = record
        self.problems = list(problems)
        self.warnings = list(warnings)
        self.excluded = excluded

    @property
    def listed(self):
        return self.record is not None and not self.problems and not self.excluded


def _option(answer, options):
    folded = {option.casefold(): option for option in options}
    return folded.get(re.sub(r"\s+", " ", answer or "").strip().casefold())


def _in_range(value, bounds):
    return bounds[0] <= value <= bounds[1]


def parse_report(body, number, created=None, logins=(), titles=None, validate=None):
    """(record or None, problems, warnings) of one device report's body."""
    # Rows are read from the body as pasted: NFKC would turn a "™" in a GPU name into "TM" and
    # break the row's machine_id check.
    raw = normalize_newlines(body)
    body = normalize_text(body)
    sections = parse_form(body)
    validate = validate or minimal_validate
    titles = titles or {}
    problems, warnings, notes = [], [], []

    def text(label, limit):
        return one_line(sections.get(label, ""), limit, logins)

    checks = parse_checkboxes(sections.get("Before submitting", ""))
    for prefix, label in REQUIRED_CHECKS:
        if not any(option.startswith(prefix) and ticked for option, ticked in checks.items()):
            problems.append(f'Tick "{label}" under "Before submitting".')
    rows = bench_rows(parse_form(raw), raw)
    for label in REQUIRED:
        if label == "Results" and rows:
            continue
        if not text(label, 400):
            problems.append(f'The "{label}" answer is missing.')
    for label in OPTIONAL:
        if not text(label, 400):
            warnings.append(f'The "{label}" answer is missing; the row is listed without it.')

    form_factor = _option(sections.get("Form factor"), FORM_FACTORS)
    if sections.get("Form factor") and not form_factor:
        problems.append('"Form factor" is not one of the form\'s options: ' + ", ".join(FORM_FACTORS) + ".")
    gpu_option = _option(sections.get("Graphics card"), GPU_OPTIONS)
    if sections.get("Graphics card") and not gpu_option:
        problems.append('"Graphics card" is not one of the form\'s options.')
    game_option = _option(sections.get("Game"), GAMES)
    if sections.get("Game") and not game_option:
        problems.append('"Game" is not one of the form\'s options.')
    power_source = _option(sections.get("Power source"), POWER_SOURCES)

    gpu_short, vendor, kind = GPU_OPTIONS.get(gpu_option, ("Other", "other", "unknown"))
    game_id, title = GAMES.get(game_option, ("other", None))
    record = {
        "issue": number,
        "submitted": created,
        "device": text("Device", 80),
        "form_factor": form_factor,
        "cpu": text("CPU", 80) or None,
        "gpu": gpu_short,
        "gpu_option": gpu_short,
        "gpu_vendor": vendor,
        "gpu_type": kind,
        "driver": normalize_driver(sections["GPU driver"], logins) if sections.get("GPU driver") else None,
        "ram": text("RAM", 60) or None,
        "os": text("Operating system", 60) or None,
        "power_source": power_source,
        "power_mode": text("Power mode", 60) or None,
        "build": normalize_build(sections["Emulator build"], logins) if sections.get("Emulator build") else None,
        "game": title or "Other",
        "game_id": game_id,
        "scene": text("Scene", 160),
        "bench_scene": None,
        "settings": text("Settings and switches", 240) or None,
        "switches": switches_of(sections.get("Settings and switches", ""), logins),
        "frame_generation": frame_generation(sections.get("Settings and switches", ""), sections.get("Scene", "")),
        "fps": None,
        "fps_approximate": False,
        "p50_ms": None,
        "p99_ms": None,
        "baseline": None,
        "source": "manual",
        "scene_ok": None,
        "bench": None,
        "runs": [],
        "credit": None,
        "screenshot": bool(re.search(r"(?i)!\[[^\]]*\]\([^)]+\)|<img\b|user-attachments/assets/|"
                                     r"\.(?:png|jpe?g|webp|gif)\b", sections.get("Screenshot", ""))),
        "notes": notes,
    }

    credit = credit_handle(sections.get(CREDIT_LABEL), logins, warnings)
    record["credit"] = credit

    if rows:
        usable, left_out = select_rows(rows, validate, logins)
        if not usable:
            problems.append(f"None of the {len(rows)} tools/bench row(s) can be listed: " + "; ".join(left_out[:4]) +
                            ". Run the scene again, or remove the rows to report your own measurement under "
                            "Results (it is then listed without the verified-scene badge).")
        else:
            if left_out:
                warnings.append(f"{len(left_out)} tools/bench row(s) left out: " + "; ".join(left_out[:4]) + ".")
            best = usable[0]
            record.update({
                "fps": rounded(best.get("fps"), 2), "p50_ms": rounded(best.get("p50_ms"), 1),
                "p99_ms": rounded(best.get("p99_ms"), 1), "source": "tools/bench", "scene_ok": True,
                "bench_scene": scrub(str(best.get("scene", "")), logins)[:60] or None,
                "switches": row_switches(best, logins),
                "runs": [summarize_row(row, logins) for row in sorted(usable[:MAX_RUNS], key=lambda r: str(r.get("variant")))],
            })
            measured = str(best.get("timestamp_utc", ""))[:10]
            record["bench"] = {"variant": row_name(best, logins),
                               "machine_id": str(best.get("machine_id", ""))[:10],
                               "measured": measured if re.fullmatch(r"\d{4}-\d\d-\d\d", measured) else None,
                               "window_s": rounded(best.get("window_s"), 1) if is_number(best.get("window_s")) else None,
                               "rows": len(rows), "valid_rows": len(usable)}
            if len({row.get("machine_id") for row in usable}) > 1:
                warnings.append("The tools/bench rows come from more than one machine; the row lists the first.")
            hardware = best.get("hardware") if isinstance(best.get("hardware"), dict) else {}
            gpus = [gpu for gpu in hardware.get("gpus") or [] if isinstance(gpu, dict) and isinstance(gpu.get("name"), str)]
            same_vendor = [gpu for gpu in gpus if vendor in gpu["name"].lower()] or gpus
            if same_vendor:
                record["gpu"] = scrub(clean_gpu_name(same_vendor[0]["name"]), logins)[:60] or gpu_short
                if not record["driver"] and isinstance(same_vendor[0].get("driver_version"), str):
                    record["driver"] = normalize_driver(same_vendor[0]["driver_version"], logins)
            if not record["cpu"] and isinstance(hardware.get("cpu"), str):
                record["cpu"] = scrub(hardware["cpu"], logins)[:80]
            if not record["os"] and isinstance(hardware.get("os"), str):
                record["os"] = scrub(hardware["os"], logins)[:60]
            if hardware.get("power_source") == "battery" or "on_battery" in (best.get("warnings") or []):
                notes.append("Measured on battery.")
            title_id = str(best.get("title_id", "")).upper()
            if title_id in titles:
                bench_title = titles[title_id]
                if title is None:
                    record["game"] = bench_title
                elif bench_title.casefold() not in title.casefold() and title.casefold() not in bench_title.casefold():
                    warnings.append(f'The tools/bench row ran "{bench_title}", the form says "{title}"; '
                                    "the row lists the game the benchmark ran.")
                    record["game"] = bench_title
            if not record["screenshot"]:
                warnings.append('No screenshot under "Screenshot": add one so reviewers can see the scene.')
    else:
        parsed, problem = parse_results(sections.get("Results", ""))
        if problem:
            if sections.get("Results"):
                problems.append(problem)
        else:
            record.update({"fps": rounded(parsed["fps"], 2), "fps_approximate": parsed["approximate"],
                           "p50_ms": rounded(parsed["p50_ms"], 1), "p99_ms": rounded(parsed["p99_ms"], 1)})
            if parsed["baseline"]:
                record["baseline"] = {key: rounded(value, 2 if key == "fps" else 1)
                                      for key, value in parsed["baseline"].items()}
            if record["frame_generation"] == "on":
                notes.append("Frame generation was on: game fps counts only the frames the game renders.")
        if not record["screenshot"]:
            problems.append('Add a screenshot of the scene under "Screenshot", so the row can be checked.')

    if record["fps_approximate"]:
        notes.append("Approximate figure.")
    if power_source == "On battery" and "Measured on battery." not in notes:
        notes.append("Measured on battery.")
    for label, value, bounds in (("fps", record["fps"], FPS_RANGE), ("p50 frame time", record["p50_ms"], P50_RANGE),
                                 ("p99 frame time", record["p99_ms"], P99_RANGE)):
        if value is not None and not _in_range(value, bounds):
            problems.append(f"The {label} {value:g} is outside {bounds[0]:g} to {bounds[1]:g}.")
    if record["fps"] is None and rows and not problems:
        problems.append("The chosen tools/bench row has no fps.")
    if record["p50_ms"] is not None and record["p99_ms"] is not None and record["p99_ms"] < record["p50_ms"]:
        problems.append(f"The p99 frame time ({record['p99_ms']:g} ms) is below the p50 ({record['p50_ms']:g} ms).")
    if record["fps"] and record["p50_ms"] and not 1 / 3 <= (1000 / record["p50_ms"]) / record["fps"] <= 3:
        warnings.append(f"{record['fps']:g} fps and a p50 frame time of {record['p50_ms']:g} ms do not match well; "
                        "check that both are game frames.")
    if problems:
        return None, problems, warnings
    return record, problems, warnings


def credit_handle(answer, logins, warnings):
    """The GitHub handle to show next to a record, or None. Only the account that opened the report
    can be credited, so nobody can put another person's name on a row."""
    first = (answer or "").strip().split(chr(10))[0].strip()
    handle = first.lstrip("@").strip()
    if not handle:
        return None
    author = next((login for login in logins if login), "")
    if not GITHUB_LOGIN.fullmatch(handle) or not author or handle.casefold() != author.casefold():
        warnings.append('"Credit me as" must be the GitHub handle of the account that opens the report; '
                        "the row is listed without a name.")
        return None
    return author


def parse_issue(issue, titles=None, validate=None):
    number = int(issue.get("number") or 0)
    excluded = None
    if not has_label(issue, LABEL):
        excluded = "not labelled device-report"
    elif issue.get("state") == "closed" and issue.get("state_reason") in EXCLUDED_STATE_REASONS:
        excluded = EXCLUDED_STATE_REASONS[issue["state_reason"]]
    logins = [(issue.get("user") or {}).get("login") or ""]
    created = str(issue.get("created_at") or "")[:10] or None
    try:
        record, problems, warnings = parse_report(issue.get("body") or "", number, created, logins, titles, validate)
    except Exception as error:  # noqa: BLE001 - one unreadable report must not stop the others
        record, problems, warnings = None, [f"The report could not be read ({type(error).__name__})."], []
    return Report(number, record, problems, warnings, excluded)


# ---------------------------------------------------------------------------------------------
# community.json

def empty_document(repo):
    return {"schema_version": SCHEMA_VERSION, "reports": {}, "repository": repo}


def load_document(text):
    try:
        doc = json.loads(text) if text else {}
    except ValueError:
        doc = {}
    if not isinstance(doc, dict) or not isinstance(doc.get("reports"), dict):
        doc = {"reports": {}}
    return doc


def records_of(reports):
    listed = sorted((report for report in reports if report.listed), key=lambda report: report.number)
    return {str(report.number): report.record for report in listed}


def records_board(records):
    """The best validated row per GPU class and scene: records is {issue: record}. Only a tools/bench row
    that is valid with scene_ok true and an exact fps counts; the best has the highest fps, then the lower
    p99, then the earlier report."""
    best = {}
    for record in records.values():
        fps = record.get("fps")
        if record.get("scene_ok") is not True or record.get("source") != "tools/bench" or not is_number(fps)                 or fps <= 0 or record.get("fps_approximate"):
            continue
        scene = record.get("bench_scene") or record.get("scene") or "unknown scene"
        key = (record.get("gpu_option") or record.get("gpu") or "Other", record.get("game") or "Other", scene)
        p99 = record.get("p99_ms") if is_number(record.get("p99_ms")) else float("inf")
        rank = (-fps, p99, record.get("issue") or 0)
        if key not in best or rank < best[key][0]:
            best[key] = (rank, record)
    board = []
    for (gpu, game, scene), (_, record) in sorted(best.items(), key=lambda item: tuple(map(str.casefold, item[0]))):
        board.append({"gpu": gpu, "game": game, "scene": scene, "fps": record["fps"], "p50_ms": record.get("p50_ms"),
                      "p99_ms": record.get("p99_ms"), "issue": record.get("issue"), "credit": record.get("credit"),
                      "device": record.get("device"), "driver": record.get("driver"), "build": record.get("build"),
                      "measured": (record.get("bench") or {}).get("measured") or record.get("submitted")})
    return board


def make_document(records, repo, previous=None, today=None):
    """The file's content. "updated" moves only when the records change."""
    keep = previous and previous.get("reports") == records and previous.get("updated")
    return {
        "schema_version": SCHEMA_VERSION,
        "description": DOC_DESCRIPTION,
        "repository": repo,
        "issue_url": f"https://github.com/{repo}/issues/{{issue}}",
        "updated": previous["updated"] if keep else (today or utc_today()),
        "count": len(records),
        "fields": FIELD_DOCS,
        "records": records_board(records),
        "reports": records,
    }


def dump_document(doc):
    return json.dumps(doc, indent=2, ensure_ascii=False) + "\n"


def diff_records(old, new):
    changes = []
    for key in sorted(set(old) | set(new), key=lambda k: int(k) if str(k).isdigit() else 0):
        if key not in old:
            changes.append(("added", key, new[key]))
        elif key not in new:
            changes.append(("removed", key, old[key]))
        elif old[key] != new[key]:
            changes.append(("updated", key, new[key]))
    return changes


# ---------------------------------------------------------------------------------------------
# Git: one commit on top of the default branch, pushed with a lease

class GitError(RuntimeError):
    pass


class Git:
    def __init__(self, root=ROOT, remote="origin"):
        self.root, self.remote = str(root), remote

    def run(self, *args, data=None, env=None, check=True):
        full_env = dict(os.environ, GIT_TERMINAL_PROMPT="0", **(env or {}))
        result = subprocess.run(["git", "-C", self.root, *args], input=data, capture_output=True, env=full_env)
        if check and result.returncode:
            raise GitError(f"git {' '.join(args[:2])} failed: {result.stderr.decode('utf-8', 'replace').strip()}")
        return result

    def out(self, *args, **kwargs):
        return self.run(*args, **kwargs).stdout.decode("utf-8").strip()

    def fetch(self, branch):
        """The remote branch's commit after fetching it, or None when it does not exist."""
        ref = f"refs/remotes/{self.remote}/{branch}"
        depth = ["--depth=1"] if self.out("rev-parse", "--is-shallow-repository") == "true" else []
        result = self.run("fetch", "--no-tags", *depth, self.remote, f"+refs/heads/{branch}:{ref}", check=False)
        if result.returncode:
            message = result.stderr.decode("utf-8", "replace")
            if "couldn't find remote ref" in message or "not found" in message.lower():
                self.run("update-ref", "-d", ref, check=False)
                return None
            raise GitError(f"git fetch {branch} failed: {message.strip()}")
        return self.out("rev-parse", ref)

    def show(self, commit, path):
        result = self.run("show", f"{commit}:{path}", check=False)
        return result.stdout.decode("utf-8") if result.returncode == 0 else None

    def parents(self, commit):
        raw = self.out("cat-file", "commit", commit)
        return [line.split()[1] for line in raw.split("\n\n", 1)[0].splitlines() if line.startswith("parent ")]

    def commit_file(self, parent, path, content, message):
        """A commit whose tree is parent's with path set to content (LF, UTF-8); nothing checked out
        is touched."""
        blob = self.out("hash-object", "-w", "--stdin", data=content.encode("utf-8"))
        with tempfile.TemporaryDirectory() as temp:
            index = {"GIT_INDEX_FILE": os.path.join(temp, "index")}
            self.run("read-tree", parent, env=index)
            self.run("update-index", "--add", "--cacheinfo", f"100644,{blob},{path}", env=index)
            tree = self.out("write-tree", env=index)
        name, email = BOT_IDENTITY
        identity = {"GIT_AUTHOR_NAME": name, "GIT_AUTHOR_EMAIL": email,
                    "GIT_COMMITTER_NAME": name, "GIT_COMMITTER_EMAIL": email}
        return self.out("commit-tree", tree, "-p", parent, data=message.encode("utf-8"), env=identity)

    def push(self, commit, branch, expected):
        """Moves the remote branch to commit if it is still at expected ("" = must not exist);
        False when someone else moved it first."""
        lease = f"--force-with-lease=refs/heads/{branch}:{expected or ''}"
        result = self.run("push", "--porcelain", lease, self.remote, f"{commit}:refs/heads/{branch}", check=False)
        if result.returncode == 0:
            return True
        output = (result.stdout + result.stderr).decode("utf-8", "replace")
        if "stale info" in output or "[rejected]" in output or "fetch first" in output:
            return False
        raise GitError(f"git push failed: {output.strip()}")

    def delete(self, branch, expected):
        result = self.run("push", "--porcelain", f"--force-with-lease=refs/heads/{branch}:{expected}", self.remote,
                          f":refs/heads/{branch}", check=False)
        return result.returncode == 0


# ---------------------------------------------------------------------------------------------
# GitHub

def list_reports(github, repo):
    """Every issue labelled device-report, open or closed; pull requests left out."""
    issues = github.paginate(f"/repos/{repo}/issues", labels=LABEL, state="all", sort="created", direction="asc")
    return [issue for issue in issues if "pull_request" not in issue]


def find_pull(github, repo, branch, base):
    owner = repo.split("/")[0]
    pulls = github.get(f"/repos/{repo}/pulls", state="open", head=f"{owner}:{branch}", base=base, per_page=10) or []
    return pulls[0] if pulls else None


def default_branch(github, repo, event):
    branch = ((event or {}).get("repository") or {}).get("default_branch")
    return branch or github.get(f"/repos/{repo}")["default_branch"]


def site_url(repo):
    owner, name = repo.split("/", 1)
    return f"https://{owner.lower()}.github.io/{name}"


# ---------------------------------------------------------------------------------------------
# Text for the pull request and the comment

def fps_text(record):
    if record.get("fps") is None:
        return "n/a"
    value = ("~" if record.get("fps_approximate") else "") + f"{record['fps']:g}"
    baseline = record.get("baseline") or {}
    return f"{baseline['fps']:g} → {value}" if baseline.get("fps") is not None else value


def scene_check_text(record):
    if record.get("scene_ok") is True:
        return "verified scene (tools/bench)"
    return "not verified (manual measurement)"


def commit_message(changes):
    groups = {}
    for kind, key, _ in changes:
        groups.setdefault(kind, []).append(f"#{key}")
    parts = [f"{kind.capitalize()}: {', '.join(numbers[:40])}{' and more' if len(numbers) > 40 else ''}."
             for kind, numbers in groups.items()]
    return (PULL_TITLE + "\n\n" + " ".join(parts) + "\n\nGenerated by tools/ingest/ingest.py from the issues "
            "labelled device-report (.github/workflows/ingest-reports.yml).\n")


def pull_body(changes, reports, repo):
    site = site_url(repo)
    lines = [
        PR_MARKER,
        f"Updates `{DATA_PATH}`, the data of the [community leaderboard]({site}/benchmarks.html#community), from "
        "the issues labelled `device-report`. `ingest-reports.yml` (`tools/ingest/ingest.py`) rewrites this pull "
        "request whenever a report is opened or edited.",
        "",
        "| Report | Change | Device | GPU | Game | Game fps | Scene check |",
        "|---|---|---|---|---|---:|---|",
    ]
    for kind, key, record in changes[:200]:
        lines.append(f"| #{key} | {kind} | {md_escape(record.get('device', ''))} | {md_escape(record.get('gpu', ''))} | "
                     f"{md_escape(record.get('game', ''))} | {md_escape(fps_text(record))} | "
                     f"{scene_check_text(record)} |")
    if len(changes) > 200:
        lines.append(f"\n{len(changes) - 200} more change(s) are in the diff.")
    waiting = [report for report in reports if report.problems and not report.excluded]
    if waiting:
        lines += ["", f"Left out until their problems are fixed (each has a comment saying what): "
                  + ", ".join(f"#{report.number}" for report in waiting[:60]) + "."]
    lines += [
        "",
        "## Before merging",
        "",
        "- [ ] The screenshot of each added or updated report shows the scene it names, and the numbers are "
        "plausible for the device. Rows marked verified scene come from a tools/bench row that passed its "
        "scene check.",
        "- [ ] CI ran. A pull request opened with the repository's token starts no workflows by itself: close and "
        "reopen it to run build-check.",
        "",
        "The file is generated. To change a row, edit its issue. To drop one, close the issue as not planned or "
        "remove its `device-report` label, then run Actions > ingest-reports > Run workflow. Do not push to "
        f"`{BRANCH}`: the next run replaces it.",
    ]
    return "\n".join(lines) + "\n"


def comment_body(report, main_doc, pull, repo):
    site = site_url(repo)
    lines = [COMMENT_MARKER]
    if not report.listed:
        lines += [
            "**Thanks for the device report!** It cannot go on the "
            f"[community leaderboard]({site}/benchmarks.html#community) yet:",
            "",
        ]
        lines += [f"- {md_escape(problem)}" for problem in report.problems]
        lines += ["", "Edit the issue to fix this and the check runs again. "
                  "[tools/bench](https://github.com/" + repo + "/blob/HEAD/tools/bench/CONTRIBUTING-bench.md) "
                  "measures a scene the same way on every machine and writes rows you can paste here."]
    else:
        record = report.record
        on_main = (main_doc.get("reports") or {}).get(str(report.number)) == record
        if on_main:
            status = "It is on the leaderboard."
        elif pull:
            status = f"Pull request #{pull['number']} adds it; it shows once a maintainer merges that."
        else:
            status = "The next pull request that updates the leaderboard adds it; it shows once a maintainer merges that."
        timing = " / ".join("n/a" if record.get(key) is None else f"{record[key]:g} ms" for key in ("p50_ms", "p99_ms"))
        rows = [
            ("Device", f"{record['device']}, {record['form_factor'] or 'n/a'}"),
            ("GPU and driver", f"{record['gpu']}, {record['driver'] or 'n/a'}"),
            ("Build", record["build"] or "n/a"),
            ("Game and scene", f"{record['game']}: {record['scene']}"),
            ("Settings", record["settings"] or "n/a"),
            ("Game fps", fps_text(record)),
            ("Frame time p50 / p99", timing),
            ("Scene check", scene_check_text(record)),
        ]
        lines += [
            f"**Thanks for the device report!** This is the row it adds to the "
            f"[community leaderboard]({site}/benchmarks.html#community). {status}",
            "",
            "| | |",
            "|---|---|",
        ]
        lines += [f"| {name} | {md_escape(value)} |" for name, value in rows]
        if record.get("notes"):
            lines += ["", "Shown with it: " + " ".join(md_escape(note) for note in record["notes"])]
        lines += ["", "If something is wrong, edit the issue: the row follows."]
    if report.warnings:
        lines += ["", "Worth a look:", ""] + [f"- {md_escape(warning)}" for warning in report.warnings]
    if report.listed:
        lines += ["", "<details><summary>The record in site/data/community.json</summary>", "", "```json",
                  json.dumps(report.record, indent=2, ensure_ascii=False).replace("```", "'''"), "```", "", "</details>"]
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------------------------
# Commands

class PublishError(RuntimeError):
    pass


def publish(github, git, repo, base, branch=BRANCH, dry_run=False, titles=None, validate=None, log=print,
            today=None):
    """Rebuilds the records from every report and brings the branch and its pull request in line;
    returns a summary dict."""
    for _attempt in range(3):
        main_sha = git.fetch(base)
        if not main_sha:
            raise PublishError(f"the default branch {base} was not found")
        bot_sha = git.fetch(branch)
        reports = [parse_issue(issue, titles, validate) for issue in list_reports(github, repo)]
        records = records_of(reports)
        main_doc = load_document(git.show(main_sha, DATA_PATH))
        bot_text = git.show(bot_sha, DATA_PATH) if bot_sha else None
        bot_doc = load_document(bot_text) if bot_text else None
        previous = bot_doc if bot_doc and bot_doc.get("reports") == records else main_doc
        doc = make_document(records, repo, previous, today)
        changes = diff_records(main_doc.get("reports") or {}, records)
        pull = find_pull(github, repo, branch, base)
        summary = {"reports": len(reports), "listed": len(records), "changes": len(changes),
                   "problems": sorted(report.number for report in reports if report.problems and not report.excluded),
                   "excluded": sorted(report.number for report in reports if report.excluded),
                   "pull": pull["number"] if pull else None, "pushed": False}
        if not changes:
            if dry_run:
                log("[dry run] nothing to add" + (f"; would close #{pull['number']}" if pull else ""))
                return summary
            if pull:
                github.post(f"/repos/{repo}/issues/{pull['number']}/comments",
                            {"body": "Nothing left to add: every report in this pull request is on "
                                     f"`{base}` already, or was removed. Closing it; the next report opens a new one."})
                github.patch(f"/repos/{repo}/pulls/{pull['number']}", {"state": "closed"})
                summary["closed"] = pull["number"]
            if bot_sha:
                git.delete(branch, bot_sha)
            return summary
        content = dump_document(doc)
        current = bot_sha and bot_text == content and git.parents(bot_sha) == [main_sha]
        if not current:
            commit = git.commit_file(main_sha, DATA_PATH, content, commit_message(changes))
            if dry_run:
                log(f"[dry run] would push {commit[:10]} to {branch} with {len(changes)} change(s):\n{content}")
            elif not git.push(commit, branch, bot_sha):
                log(f"{branch} moved while this run worked; starting over")
                continue
            else:
                summary["pushed"] = True
        body = pull_body(changes, reports, repo)
        if dry_run:
            log(f"[dry run] would {'update #' + str(pull['number']) if pull else 'open'} the pull request:\n{body}")
            return summary
        if pull:
            if pull.get("title") != PULL_TITLE or pull.get("body") != body:
                github.patch(f"/repos/{repo}/pulls/{pull['number']}", {"title": PULL_TITLE, "body": body})
            return summary
        try:
            created = github.post(f"/repos/{repo}/pulls", {"title": PULL_TITLE, "head": branch, "base": base,
                                                           "body": body, "maintainer_can_modify": True})
        except github_api.GitHubError as error:
            if error.status == 422 and "already exists" in error.message:
                continue
            raise PublishError(f"could not open the pull request ({error}). Allow GitHub Actions to create pull "
                               "requests: Settings > Actions > General > Workflow permissions. The branch "
                               f"{branch} has the data; a maintainer can open the pull request by hand.") from None
        summary["pull"] = created["number"]
        return summary
    raise PublishError(f"{branch} kept moving; run the workflow again")


def comment(github, repo, number, data_file, titles=None, validate=None, dry_run=False, log=print):
    """Writes or updates the one comment on report #number; returns what it did."""
    issue = github.get(f"/repos/{repo}/issues/{number}")
    if "pull_request" in issue or not has_label(issue, LABEL):
        return "skipped"
    report = parse_issue(issue, titles, validate)
    if report.excluded:
        return "skipped"
    try:
        main_doc = load_document(Path(data_file).read_text(encoding="utf-8"))
    except OSError:
        main_doc = {"reports": {}}
    pull = find_pull(github, repo, BRANCH, default_branch(github, repo, github_api.read_event()))
    body = comment_body(report, main_doc, pull, repo)
    return github_api.upsert_comment(github, repo, number, COMMENT_MARKER, body, dry_run=dry_run, log=log)


def write_summary(lines):
    path = os.environ.get("GITHUB_STEP_SUMMARY")
    if path:
        with open(path, "a", encoding="utf-8") as handle:
            handle.write("\n".join(lines) + "\n")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    parse_cmd = commands.add_parser("parse", help="print what one issue body becomes")
    parse_cmd.add_argument("body", help="a file with the issue body (markdown), - for stdin")
    parse_cmd.add_argument("--number", type=int, default=0)
    parse_cmd.add_argument("--created", help="YYYY-MM-DD")
    parse_cmd.add_argument("--login", action="append", default=[], help="the reporter's login, scrubbed from the text")
    publish_cmd = commands.add_parser("publish", help="rebuild community.json and update its pull request")
    publish_cmd.add_argument("--dry-run", action="store_true")
    publish_cmd.add_argument("--repo", default=os.environ.get("GITHUB_REPOSITORY", ""))
    publish_cmd.add_argument("--branch", default=BRANCH)
    comment_cmd = commands.add_parser("comment", help="comment the parsed row on one report")
    comment_cmd.add_argument("--issue", type=int, help="the issue number (default: the event's issue)")
    comment_cmd.add_argument("--dry-run", action="store_true")
    comment_cmd.add_argument("--repo", default=os.environ.get("GITHUB_REPOSITORY", ""))
    args = parser.parse_args(argv)
    dry_run = getattr(args, "dry_run", False) or os.environ.get("DRY_RUN", "").lower() == "true"
    titles, validate = program_titles(), load_bench_validator()

    if args.command == "parse":
        body = sys.stdin.read() if args.body == "-" else Path(args.body).read_text(encoding="utf-8")
        record, problems, warnings = parse_report(body, args.number, args.created, args.login, titles, validate)
        json.dump({"record": record, "problems": problems, "warnings": warnings}, sys.stdout, indent=2,
                  ensure_ascii=False)
        sys.stdout.write("\n")
        return 1 if problems else 0

    github, _ = github_api.from_environment()
    if not re.fullmatch(r"[\w.-]+/[\w.-]+", args.repo or ""):
        parser.error("--repo owner/name (or GITHUB_REPOSITORY) is required")
    try:
        if args.command == "publish":
            event = github_api.read_event()
            summary = publish(github, Git(), args.repo, default_branch(github, args.repo, event), args.branch,
                              dry_run, titles, validate)
            print(json.dumps(summary))
            write_summary([
                "### Community leaderboard",
                f"{summary['listed']} of {summary['reports']} device reports listed; {summary['changes']} change(s) "
                f"against the default branch" + (f", pull request #{summary['pull']}" if summary.get("pull") else "") + ".",
                ("Reports with problems: " + ", ".join(f"#{n}" for n in summary["problems"]) + ".")
                if summary["problems"] else "No report has problems.",
            ])
        else:
            number = args.issue or int((github_api.read_event().get("issue") or {}).get("number") or 0)
            if not number:
                parser.error("--issue is required outside an issue event")
            print(comment(github, args.repo, number, ROOT / DATA_PATH, titles, validate, dry_run))
    except (github_api.GitHubError, PublishError, GitError) as error:
        print(f"::error::{error}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
