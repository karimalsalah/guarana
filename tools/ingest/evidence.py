#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""What the community device reports show per GPU family, game and build: the evidence a game preset
can cite.

  python -I tools/ingest/evidence.py [--community site/data/community.json]
                                     [--out site/data/community-evidence.json] [--check]

Reads the records of site/data/community.json and writes site/data/community-evidence.json. ingest.py
publish writes both files in one commit; --check exits 1 when --out is not what the records give.
The output is deterministic (no clock): "updated" is the input's.

What a report can show, under AGENTS.md's rule 1 ("ship only a measured gain on a verified scene"):

  observed  any listed report: numbers as reported. A manual or launcher report has no scene check
            and no check that nothing else ran, so it shows what one machine did, nothing more.
  verified  a tools/bench row that is valid with scene_ok true: the game was where the row says, and
            nothing else used more CPU than the benchmark allows.
  paired    two such rows of one report that differ only in switches or settings that both rows set
            (EDEN_NX2_GM=0 against EDEN_NX2_GM=11), on the same machine, scene, build and power source,
            measured within PAIR_WINDOW_S of each other with the background CPU measured. Only a pair
            measures an effect. The baseline variant sets nothing: it is whatever that install runs
            with (the shipped nx2_switches.txt sets EDEN_NX2_GM=11), so it pairs with nothing.

Rows of different reports are never compared, absolute fps of different scenes or settings are never
averaged, and battery stays apart from mains power. Only reports whose GPU family is known
(device_classes.py) count for a family. Nothing reads this file to change a default.

Standard library only.
"""

import argparse
import datetime
import json
import os
import re
import statistics
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)  # -I leaves the script's directory off sys.path.

import device_classes  # noqa: E402

SCHEMA_VERSION = 1
ROOT = os.path.dirname(os.path.dirname(HERE))
DATA_PATH = "site/data/community-evidence.json"
COMMUNITY_PATH = os.path.join(ROOT, "site", "data", "community.json")
OUT_PATH = os.path.join(ROOT, *DATA_PATH.split("/"))
# Two rows further apart than this are not one session.
PAIR_WINDOW_S = 2 * 3600

DESCRIPTION = ("What the community device reports show per GPU family (the engine's Common::GpuFamily), game "
               "and build: counts of reports and devices, and the effects measured by paired tools/bench rows. "
               "Generated from site/data/community.json by tools/ingest/evidence.py; edit the issues, not this "
               "file. Nothing reads it to change a default.")
FIELD_DOCS = {
    "tiers": "observed: any listed report, numbers as reported. verified: a tools/bench row that is valid with "
             "scene_ok true. paired: two such rows of one report that differ only in switches or settings both "
             "rows set, on the same machine, scene, build and power source, measured within two hours of each "
             "other with the background CPU measured. Only a pair measures an effect; a baseline row sets "
             "nothing and pairs with nothing.",
    "devices": "Distinct devices: reports with the same device model, CPU and GPU family count once, the model "
               "and CPU compared by their letters and digits. Two machines of the same model count once, and one "
               "machine whose model is written two different ways counts twice: check the issues before trusting "
               "a count.",
    "families": "Every GPU family of the engine (src/common/device_profile.cpp) with its label and how many "
                "reports and devices it has: all of them, verified ones and paired ones.",
    "coverage": "Per GPU family, game and build: reports and devices (all, verified, paired), their power "
                "source and the issues. Counts only.",
    "effects": "Per GPU family, game, scene, change, context (the switches and settings both rows set alike), "
               "build and power source: the pairs. fps_change_pct and p99_change_pct are the rows with `set` "
               "against the rows with `against`, in percent (a lower p99 is better), within_2ms_change_pt in "
               "percentage points. per_device holds each device's median over its pairs; min, median and max "
               "are over the devices.",
    "unresolved": "Reports whose GPU family is not known, by their Graphics card answer, with the families they "
                  "may be. The GPU's exact name (a tools/bench row, or the launcher's \"GPU:\" line) resolves "
                  "them.",
    "outside": "Reports whose GPU is in no family: discrete cards, older or smaller integrated GPUs.",
}

_OFF = {"0", "off", "false", "no", ""}
_SWITCH = re.compile(r"^\s*([A-Z][A-Z0-9_]*)\s*=\s*(.*?)\s*$")
_TIME = re.compile(r"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$")


def _number(value):
    return value if isinstance(value, (int, float)) and not isinstance(value, bool) else None


def _text(value):
    return value.strip() if isinstance(value, str) else ""


def _tokens(text):
    """text for comparing devices: no parenthesized parts or trademark signs, lower-case words."""
    text = re.sub(r"\([^)]*\)", " ", _text(text).lower())
    return " ".join(re.findall(r"[a-z0-9]+", text))


def _round(value, digits=1):
    return None if value is None else round(value, digits)


def _spread(values):
    values = [value for value in values if value is not None]
    if not values:
        return None
    return {"min": _round(min(values)), "median": _round(statistics.median(values)), "max": _round(max(values))}


# ---------------------------------------------------------------------------------------------
# One record

def device_class(record):
    """The record's device_class, or one made from its GPU fields for a record written before ingest
    named the family."""
    found = record.get("device_class")
    if isinstance(found, dict) and found.get("status") in ("family", "none", "unresolved"):
        family = found.get("family")
        if found["status"] == "family" and family not in device_classes.FAMILIES:
            return device_classes.result("unresolved", source=found.get("from"), name=found.get("name"))
        candidates = [item for item in found.get("candidates") or [] if item in device_classes.FAMILIES]
        return device_classes.result(found["status"], family if found["status"] == "family" else None,
                                     candidates if found["status"] == "unresolved" else (), found.get("from"),
                                     found.get("name"))
    bench = [record.get("gpu")] if record.get("source") == "tools/bench" else []
    return device_classes.classify(record.get("gpu_option"), record.get("gpu_vendor"), record.get("gpu_type"), bench)


def device_key(record, found):
    """Model, CPU and GPU by their letters and digits: "ASUS Zenbook S 14 (UX5406)" is "Asus Zenbook S14"."""
    def compact(text):
        return _tokens(text).replace(" ", "")

    return "|".join((compact(record.get("device")), compact(record.get("cpu")),
                     found.get("family") or compact(found.get("name")) or compact(record.get("gpu_option"))))


def power_of(value):
    value = _text(value).lower()
    if value in ("ac", "plugged in", "mains"):
        return "ac"
    if value in ("battery", "on battery"):
        return "battery"
    return None


def _runs(record):
    return [run for run in record.get("runs") or [] if isinstance(run, dict)]


def _shared(values):
    """The one value every item has, or None."""
    values = set(values)
    return values.pop() if len(values) == 1 else None


def report_build(record):
    runs = _runs(record)
    return (_shared(_text(run.get("build")) for run in runs) if runs else None) or _text(record.get("build")) or "unknown"


def report_power(record):
    runs = _runs(record)
    if runs:
        return _shared(power_of(run.get("power")) for run in runs)
    return power_of(record.get("power_source"))


def is_verified(record):
    return record.get("source") == "tools/bench" and record.get("scene_ok") is True and bool(_runs(record))


def _switches(run):
    found = {}
    for item in run.get("switches") or []:
        match = _SWITCH.match(str(item))
        if match:
            found[match.group(1)] = match.group(2)
    return found


def _settings(run):
    settings = run.get("settings")
    return {str(key): str(value) for key, value in settings.items()} if isinstance(settings, dict) else {}


def _time(run):
    value = run.get("measured")
    if not isinstance(value, str) or not _TIME.match(value):
        return None
    return datetime.datetime.strptime(value, "%Y-%m-%dT%H:%M:%SZ")


def _pairable(run):
    return (_number(run.get("fps")) or 0) > 0 and run.get("quiet") is True and _time(run) is not None


def difference(a, b):
    """({key: (a's value, b's value)} where the rows differ, [the keys both set alike]), or None when they
    do not differ or a key that differs is set by only one of them. Settings are named setting:<key>."""
    diff, same = {}, []
    for prefix, first, second in (("", _switches(a), _switches(b)), ("setting:", _settings(a), _settings(b))):
        for key in sorted(set(first) | set(second)):
            if first.get(key) == second.get(key):
                same.append(f"{prefix}{key}={first[key]}")
            elif key not in first or key not in second:
                return None
            else:
                diff[prefix + key] = (first[key], second[key])
    return (diff, same) if diff else None


def _order(value):
    """Off values first, then numbers by value, then text."""
    text = value.strip().lower()
    try:
        number = float(text)
    except ValueError:
        number = None
    return (text not in _OFF, number is None, number if number is not None else 0.0, text)


def record_pairs(record):
    """The pairs of one record: [(set run, against run, set values, against values, context)]. Each run
    with the later values of a change is paired with the nearest run with the earlier ones."""
    runs = [run for run in _runs(record) if _pairable(run)]
    nearest = {}
    for i, first in enumerate(runs):
        for j in range(i + 1, len(runs)):
            second = runs[j]
            same = all(first.get(key) == second.get(key) for key in ("machine_id", "scene", "build", "power"))
            seconds = abs((_time(first) - _time(second)).total_seconds())
            found = difference(first, second) if same and seconds <= PAIR_WINDOW_S else None
            if not found:
                continue
            diff, context = found
            keys = sorted(diff)
            if [_order(diff[key][0]) for key in keys] <= [_order(diff[key][1]) for key in keys]:
                against, chosen, against_values, set_values = i, j, {k: diff[k][0] for k in keys}, {k: diff[k][1] for k in keys}
            else:
                against, chosen, against_values, set_values = j, i, {k: diff[k][1] for k in keys}, {k: diff[k][0] for k in keys}
            change = (tuple(set_values.items()), tuple(against_values.items()), tuple(context))
            key = (change, chosen)
            if key not in nearest or (seconds, against) < nearest[key][0]:
                nearest[key] = ((seconds, against), set_values, against_values, context)
    pairs = []
    for (change, chosen), ((_, against), set_values, against_values, context) in sorted(nearest.items(), key=lambda item: item[0][1]):
        pairs.append((runs[chosen], runs[against], set_values, against_values, context))
    return pairs


def _change_text(values):
    return ", ".join(f"{key}={value}" for key, value in values.items())


def pair_metrics(chosen, against):
    def ratio(key):
        a, b = _number(chosen.get(key)), _number(against.get(key))
        return (a / b - 1) * 100 if a is not None and b else None

    within = _number(chosen.get("within_2ms_pct")), _number(against.get("within_2ms_pct"))
    return {"fps_change_pct": ratio("fps"), "p99_change_pct": ratio("p99_ms"),
            "within_2ms_change_pt": within[0] - within[1] if None not in within else None}


# ---------------------------------------------------------------------------------------------
# The file

def build(community):
    """The evidence document for a parsed community.json."""
    reports = community.get("reports") if isinstance(community, dict) else None
    records = sorted((record for record in (reports or {}).values() if isinstance(record, dict)),
                     key=lambda record: int(record.get("issue") or 0))
    order = {family: index for index, family in enumerate(device_classes.FAMILIES)}
    families = {family: {"label": label, "reports": 0, "devices": set(), "verified_devices": set(),
                         "paired_devices": set()} for family, label in device_classes.FAMILIES.items()}
    coverage, effects, unresolved, outside = {}, {}, {}, []
    for record in records:
        issue = int(record.get("issue") or 0)
        found = device_class(record)
        if found["status"] == "none":
            outside.append(issue)
            continue
        if found["status"] != "family":
            key = (_text(record.get("gpu_option")) or _text(record.get("gpu")) or "unknown",
                   tuple(found["candidates"]))
            unresolved.setdefault(key, []).append(issue)
            continue
        family, device = found["family"], device_key(record, found)
        game = _text(record.get("game")) or "Other"
        verified = is_verified(record)
        pairs = record_pairs(record) if verified else []
        summary = families[family]
        summary["reports"] += 1
        summary["devices"].add(device)
        if verified:
            summary["verified_devices"].add(device)
        if pairs:
            summary["paired_devices"].add(device)

        cell = coverage.setdefault((order[family], game, report_build(record)), {
            "family": family, "game": game, "title_id": None, "build": report_build(record),
            "reports": 0, "devices": set(), "verified_reports": 0, "verified_devices": set(), "paired_reports": 0,
            "paired_devices": set(), "ac": 0, "battery": 0, "power_unknown": 0, "issues": []})
        cell["title_id"] = cell["title_id"] or record.get("title_id")
        cell["reports"] += 1
        cell["devices"].add(device)
        cell["verified_reports"] += verified
        cell["paired_reports"] += bool(pairs)
        if verified:
            cell["verified_devices"].add(device)
        if pairs:
            cell["paired_devices"].add(device)
        cell[report_power(record) or "power_unknown"] += 1
        cell["issues"].append(issue)

        for chosen, against, set_values, against_values, context in pairs:
            power = power_of(chosen.get("power"))
            key = (order[family], game, str(chosen.get("scene")), _change_text(set_values),
                   _change_text(against_values), tuple(context), str(chosen.get("build")), power or "")
            effect = effects.setdefault(key, {
                "family": family, "game": game, "title_id": record.get("title_id"), "scene": chosen.get("scene"),
                "change": f"{_change_text(set_values)} against {_change_text(against_values)}",
                "set": set_values, "against": against_values, "context": list(context),
                "build": chosen.get("build"), "power": power, "pairs": 0, "by_device": {}, "issues": []})
            effect["pairs"] += 1
            effect["by_device"].setdefault(device, []).append(pair_metrics(chosen, against))
            if issue not in effect["issues"]:
                effect["issues"].append(issue)

    def counted(cell):
        cell = dict(cell)
        for key in ("devices", "verified_devices", "paired_devices"):
            cell[key] = len(cell[key])
        return cell

    metric_names = ("fps_change_pct", "p99_change_pct", "within_2ms_change_pt")
    effect_list = []
    for _, effect in sorted(effects.items(), key=lambda item: item[0]):
        per_device = []
        for metrics in effect.pop("by_device").values():
            device = {"pairs": len(metrics)}
            for name in metric_names:
                values = [item[name] for item in metrics if item[name] is not None]
                device[name] = _round(statistics.median(values)) if values else None
            per_device.append(device)
        per_device.sort(key=lambda item: tuple(item[name] if item[name] is not None else 0.0 for name in metric_names)
                        + (item["pairs"],))
        effect.update({"devices": len(per_device), "per_device": per_device})
        for name in metric_names:
            effect[name] = _spread(item[name] for item in per_device)
        effect_list.append(effect)

    return {
        "schema_version": SCHEMA_VERSION,
        "description": DESCRIPTION,
        "updated": community.get("updated") if isinstance(community, dict) and community.get("updated") else None,
        "source": "site/data/community.json",
        "reports": len(records),
        "fields": FIELD_DOCS,
        "families": {family: counted(summary) for family, summary in families.items()},
        "coverage": [counted(cell) for _, cell in sorted(coverage.items(), key=lambda item: item[0])],
        "effects": effect_list,
        "unresolved": [{"answer": answer, "candidates": list(candidates), "reports": len(issues), "issues": issues}
                       for (answer, candidates), issues in sorted(unresolved.items())],
        "outside": {"reports": len(outside), "issues": outside},
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
            print(f"{args.out} is not what {args.community} gives: run python -I tools/ingest/evidence.py",
                  file=sys.stderr)
            return 1
        return 0
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
