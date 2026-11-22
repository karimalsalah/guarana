#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Best settings per GPU class, from the community leaderboard data.

  python -I tools/ingest/best_settings.py [--community site/data/community.json]
                                          [--out site/data/best-settings.json] [--check]

Reads the rows of site/data/community.json (written by ingest.py) and writes
site/data/best-settings.json, which the Guarana Launcher fetches
(https://karimalsalah.github.io/guarana/data/best-settings.json) to pick what "Auto" runs.

For each GPU class (Intel Arc iGPU, AMD RDNA3/3.5 iGPU, NVIDIA RTX desktop, NVIDIA RTX laptop, AMD
desktop, other) the file has:

  samples    listed rows in the class
  validated  rows whose numbers come from a tools/bench row that passed its scene check (scene_ok)
  best       the best validated row: the highest game fps among the class's most reported game
             (fps of different games cannot be compared), rows measured on battery last
  settings   the launcher's setting ids that row used, only the ones the row states

The launcher uses a class's settings only when it has enough validated rows (see MIN_VALIDATED in
launcher/src-tauri/src/community.rs) and checks every key and value itself.

Standard library only. The output is deterministic (no clock): "updated" is the input's.
"""

import argparse
import json
import os
import re
import sys

SCHEMA_VERSION = 1
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
COMMUNITY_PATH = os.path.join(ROOT, "site", "data", "community.json")
OUT_PATH = os.path.join(ROOT, "site", "data", "best-settings.json")

# Class id -> label, in file order. The ids are what the launcher reads.
CLASSES = {
    "intel-arc-igpu": "Intel Arc iGPU",
    "amd-rdna3-igpu": "AMD RDNA3 / 3.5 iGPU",
    "nvidia-rtx-desktop": "NVIDIA RTX desktop",
    "nvidia-rtx-laptop": "NVIDIA RTX laptop",
    "amd-desktop": "AMD desktop",
    "other": "Other",
}

DESCRIPTION = ("Best settings per GPU class from validated community device reports (tools/bench rows that "
               "passed their scene check). Generated from site/data/community.json by "
               "tools/ingest/best_settings.py; edit the issues, not this file.")

# The launcher's setting ids a report can state (launcher/src-tauri/src/profiles.rs).
SETTING_IDS = ("fluid_presenter", "ocarina_extrapolation", "gm_routing", "igpu_power_policy", "frame_generation")

_SWITCH = re.compile(r"^\s*(EDEN_[A-Z0-9_]+)\s*=\s*(\S+)\s*$")
_FALSE = {"0", "false", "off", "no", ""}
_AMD_RDNA3_IGPU = ("780m", "760m", "880m", "890m", "z1", "z2")


def _text(row, key):
    value = row.get(key)
    return value.strip() if isinstance(value, str) else ""


def gpu_class(row):
    """The class id of one report row."""
    option = (_text(row, "gpu_option") or _text(row, "gpu")).lower()
    vendor = _text(row, "gpu_vendor").lower()
    kind = _text(row, "gpu_type").lower()
    if vendor == "intel" and kind == "integrated" and option.startswith("intel arc"):
        return "intel-arc-igpu"
    if vendor == "amd" and kind == "integrated" and any(tag in option for tag in _AMD_RDNA3_IGPU):
        return "amd-rdna3-igpu"
    if vendor == "nvidia" and kind == "discrete":
        form = _text(row, "form_factor").lower()
        return "nvidia-rtx-laptop" if form in ("laptop", "handheld") else "nvidia-rtx-desktop"
    if vendor == "amd" and kind == "discrete":
        return "amd-desktop"
    return "other"


def _number(value):
    return value if isinstance(value, (int, float)) and not isinstance(value, bool) else None


def row_settings(row):
    """The launcher settings a row states. A tools/bench row lists every switch it set, so a switch it
    does not list was off; a manual row states only what it names."""
    found = {}
    listed = {}
    for item in row.get("switches") or []:
        match = _SWITCH.match(str(item))
        if match:
            listed[match.group(1)] = match.group(2).strip().lower()
    bench = row.get("source") == "tools/bench"

    def switch(name, as_setting, on):
        if name in listed:
            found[as_setting] = on(listed[name])
        elif bench:
            found[as_setting] = False

    switch("EDEN_NX2_FLUID", "fluid_presenter", lambda v: v not in _FALSE)
    switch("EDEN_NX2_GM", "gm_routing", lambda v: v not in _FALSE)
    switch("EDEN_NX2_POWER_POLICY", "igpu_power_policy", lambda v: v == "igpu")
    # The launcher writes EXTRAPOLATE explicitly either way, so only a listed value is a statement.
    if "EDEN_NX2_EXTRAPOLATE" in listed:
        found["ocarina_extrapolation"] = listed["EDEN_NX2_EXTRAPOLATE"] not in _FALSE
    frame_generation = row.get("frame_generation")
    if frame_generation in ("on", "off"):
        found["frame_generation"] = frame_generation == "on"
    return {key: found[key] for key in SETTING_IDS if key in found}


def is_validated(row):
    return row.get("scene_ok") is True and _number(row.get("fps")) is not None and row.get("fps") > 0


def on_battery(row):
    notes = row.get("notes") if isinstance(row.get("notes"), list) else []
    return row.get("power_source") == "On battery" or any("battery" in str(note).lower() for note in notes)


def _sort_key(row):
    p99 = _number(row.get("p99_ms"))
    # Mains first, then the highest fps, then the lowest p99, then the oldest issue for stability.
    return (on_battery(row), -row["fps"], p99 if p99 is not None else float("inf"), int(row.get("issue") or 0))


def best_row(validated):
    """The best of the validated rows: among the most reported game, see the module docstring."""
    if not validated:
        return None
    counts = {}
    for row in validated:
        game = _text(row, "game_id") or _text(row, "game") or "other"
        counts[game] = counts.get(game, 0) + 1
    # Most reports first; Ocarina of Time (the launcher's focus) wins a tie, then the name.
    game = sorted(counts, key=lambda g: (-counts[g], g != "oot", g))[0]
    pool = [row for row in validated if (_text(row, "game_id") or _text(row, "game") or "other") == game]
    return sorted(pool, key=_sort_key)[0]


def _best_summary(row):
    return {
        "issue": row.get("issue"),
        "game": _text(row, "game") or None,
        "fps": row["fps"],
        "p50_ms": _number(row.get("p50_ms")),
        "p99_ms": _number(row.get("p99_ms")),
        "build": _text(row, "build") or None,
        "driver": _text(row, "driver") or None,
        "on_battery": on_battery(row),
    }


def build(community):
    """The best-settings document for a parsed community.json."""
    reports = community.get("reports") if isinstance(community, dict) else None
    rows = [row for row in (reports or {}).values() if isinstance(row, dict)]
    rows.sort(key=lambda row: int(row.get("issue") or 0))
    by_class = {name: [] for name in CLASSES}
    for row in rows:
        by_class[gpu_class(row)].append(row)
    classes = {}
    for name, label in CLASSES.items():
        members = by_class[name]
        validated = [row for row in members if is_validated(row)]
        best = best_row(validated)
        classes[name] = {
            "label": label,
            "samples": len(members),
            "validated": len(validated),
            "best": _best_summary(best) if best else None,
            "settings": row_settings(best) if best else {},
        }
    return {
        "schema_version": SCHEMA_VERSION,
        "description": DESCRIPTION,
        "updated": community.get("updated") if isinstance(community, dict) and community.get("updated") else None,
        "source": "site/data/community.json",
        "classes": classes,
    }


def dump(doc):
    return json.dumps(doc, indent=2, ensure_ascii=False) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--community", default=COMMUNITY_PATH)
    parser.add_argument("--out", default=OUT_PATH)
    parser.add_argument("--check", action="store_true", help="exit 1 when --out is not up to date; write nothing")
    args = parser.parse_args(argv)
    with open(args.community, encoding="utf-8") as handle:
        text = dump(build(json.load(handle)))
    if args.check:
        try:
            with open(args.out, encoding="utf-8") as handle:
                current = handle.read()
        except OSError:
            current = None
        if current != text:
            print(f"{args.out} is out of date", file=sys.stderr)
            return 1
        return 0
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
