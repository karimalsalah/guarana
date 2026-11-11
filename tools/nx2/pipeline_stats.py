#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""What the GPU driver's shader compiler made of the shaders that cost the most GPU time.

  pipeline_stats.py <dir | pipeline_stats.jsonl> [--costs <file>] [--top N] [--hash H]...
                    [--by-fragment] [--highlighted] [--max-variants N] [--json]

Reads the lines EDEN_NX2_PIPELINE_STATS=<dir> wrote, one per pipeline build: the pipeline's
executables with the statistics the driver reports for each (VK_KHR_pipeline_executable_properties).
A pipeline built again, or by a later session that appended to the file, replaces its earlier line.

--costs orders the shaders by GPU time. It is a debug server `gpu_costs` reply saved as JSON (or
any JSON holding some, such as bench rows), or an emulator log with `DKB gpu_work` lines
(EDEN_DKB_FRAME_TRACE=1); times are averaged over the sampled frames in it. A row is what the
tracer times: `s1:<vertex, task or mesh hash>+s5:<fragment hash>` for draws, `s6:<hash>` for
dispatches. --by-fragment adds the draws of a fragment shader up. Without --costs the shaders are
ordered by their largest instruction count. --hash keeps the rows that name a hash (16 hex digits).

Under each row come the distinct executables of its pipelines: name, stages, subgroup size (the
SIMD width on Intel) and every statistic. Statistic names differ per driver, so all are printed;
those whose name holds one of HIGHLIGHT are marked with `*` and listed first (--highlighted prints
only those).

Standard library only.
"""

import argparse
import json
import os
import re
import sys

STATS_FILE = "pipeline_stats.jsonl"
HIGHLIGHT = ("spill", "fill", "scratch", "simd", "dispatch", "instruction", "send", "cycle", "loop")
STAGES = ((0x1, "vertex"), (0x2, "tess_control"), (0x4, "tess_eval"), (0x8, "geometry"),
          (0x10, "fragment"), (0x20, "compute"), (0x40, "task"), (0x80, "mesh"))
HASH = re.compile(r"[0-9a-f]{16}")
WORK = re.compile(r"(?:s6:([0-9a-f]{16})|s1:([0-9a-f]{16})\+s5:([0-9a-f]{16}))(?:#(\S))?="
                  r"([\d.]+)ms/(\d+)x\(max ([\d.]+)ms")
MESH_KINDS = ("m", "e")


def is_highlighted(name):
    lowered = str(name).lower()
    return any(word in lowered for word in HIGHLIGHT)


def stage_names(mask):
    """A VkShaderStageFlags mask as text."""
    if not isinstance(mask, int) or isinstance(mask, bool):
        return "?"
    names = [name for bit, name in STAGES if mask & bit]
    rest = mask & ~sum(bit for bit, _ in STAGES)
    if rest:
        names.append(f"{rest:#x}")
    return "+".join(names) or "-"


def _checked(item):
    """The line's object with lowercase hashes; raises on anything but a pipeline line."""
    kind = item["kind"]
    hashes = [str(value).lower() for value in item["hashes"]]
    if kind not in ("graphics", "compute") or len(hashes) != (6 if kind == "graphics" else 1):
        raise ValueError("unknown kind or hash count")
    if not all(HASH.fullmatch(value) for value in hashes):
        raise ValueError("not a hash")
    for executable in item["executables"]:
        for statistic in executable.setdefault("statistics", []):
            statistic["name"] = str(statistic["name"])
    item["hashes"] = hashes
    item["pipeline"] = str(item["pipeline"]).lower()
    return item


def load_pipelines(lines):
    """(pipeline lines, number of unreadable lines). The last line of a pipeline key is kept."""
    pipelines = {}
    bad = 0
    for line in lines:
        line = line.strip()
        if not line:
            continue
        try:
            item = _checked(json.loads(line))
        except (ValueError, KeyError, TypeError, AttributeError):
            bad += 1  # a line cut off by the end of the process, or not one of ours
            continue
        pipelines[(item["kind"], item["pipeline"])] = item
    return list(pipelines.values()), bad


def work_key(item):
    """The key the cost tracer times a pipeline's work under."""
    hashes = item["hashes"]
    if item["kind"] == "compute":
        return ("compute", hashes[0])
    return ("draw", hashes[1], hashes[5])


def _add_cost(costs, key, ms, count, max_ms, kind):
    entry = costs.setdefault(key, {"ms": 0.0, "count": 0.0, "max_ms": 0.0, "kinds": set()})
    entry["ms"] += ms
    entry["count"] += count
    entry["max_ms"] = max(entry["max_ms"], max_ms)
    if kind:
        entry["kinds"].add(kind)


def parse_work_log(lines):
    """(costs, frames) from a log's `DKB gpu_work` lines; ms and count are sums over the frames."""
    costs = {}
    frames = 0
    for line in lines:
        at = line.find("DKB gpu_work ")
        if at < 0:
            continue
        frames += 1
        for match in WORK.finditer(line, at):
            compute, vertex, fragment, kind, ms, count, max_ms = match.groups()
            key = ("compute", compute) if compute else ("draw", vertex, fragment)
            _add_cost(costs, key, float(ms), int(count), float(max_ms), kind or "")
    return costs, frames


def _snapshots(value):
    """Every `gpu_costs` reply inside a JSON value: an object with a `work` list."""
    if isinstance(value, dict):
        work = value.get("work")
        if isinstance(work, list):
            return [value]
        return [found for child in value.values() for found in _snapshots(child)]
    if isinstance(value, list):
        return [found for child in value for found in _snapshots(child)]
    return []


def parse_costs_json(value):
    """(costs, frames) from `gpu_costs` replies; ms and count are sums over those with work."""
    costs = {}
    frames = 0
    for snapshot in _snapshots(value):
        entries = [entry for entry in snapshot["work"] if isinstance(entry, dict)]
        if not entries:
            continue
        frames += 1
        for entry in entries:
            try:
                vertex = str(entry["vertex_hash"]).lower()
                fragment = str(entry.get("fragment_hash") or "0" * 16).lower()
                ms = float(entry["total_ms"])
                count = float(entry.get("draws") or 0)
                max_ms = float(entry.get("max_ms") or 0.0)
            except (KeyError, TypeError, ValueError):
                continue
            compute = bool(entry.get("compute")) or not fragment.strip("0")
            key = ("compute", vertex) if compute else ("draw", vertex, fragment)
            _add_cost(costs, key, ms, count, max_ms, str(entry.get("kind") or ""))
    return costs, frames


def load_costs(text):
    """(costs, frames) from a file's text: JSON with gpu_costs replies (one document, or one a
    line), else an emulator log."""
    try:
        return parse_costs_json(json.loads(text))
    except ValueError:
        pass
    lines = text.splitlines()
    replies = []
    for line in lines:
        if line.lstrip().startswith("{"):
            try:
                replies.append(json.loads(line))
            except ValueError:
                pass
    return parse_costs_json(replies) if replies else parse_work_log(lines)


def _group_key(key, by_fragment):
    if by_fragment and key[0] == "draw":
        return ("fragment", key[2])
    return key


def _shaders(key):
    if key[0] == "compute":
        return f"s6:{key[1]}"
    if key[0] == "fragment":
        return f"s5:{key[1]}"
    return f"s1:{key[1]}+s5:{key[2]}"


def _is_number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def _largest_instruction_count(items):
    largest = 0
    for item in items:
        for executable in item["executables"]:
            for statistic in executable["statistics"]:
                value = statistic.get("value")
                if "instruction" in statistic["name"].lower() and _is_number(value):
                    largest = max(largest, value)
    return largest


def executable_variants(items):
    """The distinct executables of some pipelines, each with how many of the pipelines have it."""
    variants = {}
    for item in items:
        seen = set()
        for executable in item["executables"]:
            signature = json.dumps(
                [executable.get("name"), executable.get("stages"), executable.get("subgroup_size"),
                 [[statistic["name"], statistic.get("value")]
                  for statistic in executable["statistics"]]])
            if signature in seen:
                continue
            seen.add(signature)
            variant = variants.setdefault(signature, {"executable": executable, "pipelines": 0})
            variant["pipelines"] += 1
    result = []
    for variant in variants.values():
        executable = variant["executable"]
        statistics = [{"name": statistic["name"],
                       "description": statistic.get("description"),
                       "format": statistic.get("format"),
                       "value": statistic.get("value"),
                       "highlight": is_highlighted(statistic["name"])}
                      for statistic in executable["statistics"]]
        statistics.sort(key=lambda statistic: not statistic["highlight"])
        result.append({"index": executable.get("index"),
                       "name": executable.get("name"),
                       "description": executable.get("description"),
                       "stages": stage_names(executable.get("stages")),
                       "subgroup_size": executable.get("subgroup_size"),
                       "pipelines": variant["pipelines"],
                       "statistics": statistics})
    return result


def build_rows(pipelines, costs=None, frames=0, top=10, hashes=(), by_fragment=False):
    """The table's rows, costliest first: shaders, GPU time per sampled frame and executables."""
    groups = {}
    for item in pipelines:
        groups.setdefault(_group_key(work_key(item), by_fragment), []).append(item)
    merged = {}
    for key, cost in (costs or {}).items():
        _add_cost(merged, _group_key(key, by_fragment), cost["ms"], cost["count"], cost["max_ms"],
                  "")
        merged[_group_key(key, by_fragment)]["kinds"] |= cost["kinds"]
    order = sorted(merged, key=lambda key: (-merged[key]["ms"], key))
    if not merged or hashes:
        # Unmeasured shaders follow, the largest programs first
        order += sorted((key for key in groups if key not in merged),
                        key=lambda key: (-_largest_instruction_count(groups[key]), key))
    wanted = {value.lower() for value in hashes}
    if wanted:
        order = [key for key in order if wanted & set(key[1:])]
    rows = []
    for key in order[:max(top, 0)]:
        items = groups.get(key, [])
        cost = merged.get(key)
        per_frame = max(frames, 1)
        mesh = any(item.get("mesh") for item in items) or bool(
            cost and cost["kinds"] & set(MESH_KINDS))
        rows.append({
            "rank": len(rows) + 1,
            "kind": "compute" if key[0] == "compute" else "mesh" if mesh else "draw",
            "shaders": _shaders(key),
            "ms": cost["ms"] / per_frame if cost else None,
            "draws": cost["count"] / per_frame if cost else None,
            "max_ms": cost["max_ms"] if cost else None,
            "draw_kinds": sorted(cost["kinds"]) if cost else [],
            "pipelines": len(items),
            "executables": executable_variants(items),
        })
    return rows


def _number(value, digits):
    return "-" if value is None else f"{value:.{digits}f}"


def format_rows(rows, highlighted_only=False, max_variants=8):
    """The rows as lines of text."""
    lines = [f"{'#':>3}  {'ms/frame':>9}  {'calls':>8}  {'max ms':>8}  {'kind':<7}  "
             f"{'pipelines':>9}  shaders"]
    for row in rows:
        lines.append(f"{row['rank']:>3}  {_number(row['ms'], 3):>9}  "
                     f"{_number(row['draws'], 1):>8}  {_number(row['max_ms'], 3):>8}  "
                     f"{row['kind']:<7}  {row['pipelines']:>9}  {row['shaders']}")
        if not row["executables"]:
            lines.append("       no executables: no pipeline of these shaders was built with "
                         "EDEN_NX2_PIPELINE_STATS, or the driver reported none")
        for variant in row["executables"][:max(max_variants, 0)]:
            lines.append(f"       [{variant['index']}] {variant['name']}  "
                         f"stages={variant['stages']}  subgroup={variant['subgroup_size']}  "
                         f"in {variant['pipelines']} of {row['pipelines']} pipelines")
            if variant["description"]:
                lines.append(f"           {variant['description']}")
            for statistic in variant["statistics"]:
                if highlighted_only and not statistic["highlight"]:
                    continue
                mark = "*" if statistic["highlight"] else " "
                lines.append(f"         {mark} {statistic['name']}: {statistic['value']}")
        hidden = len(row["executables"]) - max(max_variants, 0)
        if hidden > 0:
            lines.append(f"       ... {hidden} more distinct executables (--max-variants)")
    return lines


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Driver statistics of the pipelines of the costliest shaders "
                    "(EDEN_NX2_PIPELINE_STATS).")
    parser.add_argument("stats", help=f"{STATS_FILE}, or the directory it is in")
    parser.add_argument("--costs", help="a gpu_costs reply as JSON, or a log with DKB gpu_work "
                                        "lines")
    parser.add_argument("--top", type=int, default=10, help="rows to print (default 10)")
    parser.add_argument("--hash", action="append", default=[], metavar="HASH",
                        help="only rows naming this shader hash (16 hex digits); repeatable")
    parser.add_argument("--by-fragment", action="store_true",
                        help="add up the draws of each fragment shader")
    parser.add_argument("--highlighted", action="store_true",
                        help="print only the highlighted statistics")
    parser.add_argument("--max-variants", type=int, default=8,
                        help="distinct executables to print per row (default 8)")
    parser.add_argument("--json", action="store_true", help="print the rows as JSON")
    args = parser.parse_args(argv)
    for value in args.hash:
        if not HASH.fullmatch(value.lower()):
            parser.error(f"--hash takes 16 hex digits, not {value!r}")

    path = os.path.join(args.stats, STATS_FILE) if os.path.isdir(args.stats) else args.stats
    with open(path, encoding="utf-8", errors="replace") as file:
        pipelines, bad = load_pipelines(file)
    costs, frames = {}, 0
    if args.costs:
        with open(args.costs, encoding="utf-8", errors="replace") as file:
            costs, frames = load_costs(file.read())
    rows = build_rows(pipelines, costs, frames, args.top, args.hash, args.by_fragment)
    if args.json:
        json.dump({"pipelines": len(pipelines), "unreadable_lines": bad, "sampled_frames": frames,
                   "rows": rows}, sys.stdout, indent=1)
        sys.stdout.write("\n")
        return 0
    print(f"{len(pipelines)} pipelines in {path}" + (f", {bad} unreadable lines" if bad else ""))
    if args.costs:
        print(f"GPU time from {args.costs}: {len(costs)} shader rows over {frames} sampled frames")
        if not costs:
            print("no gpu_costs work and no DKB gpu_work line in it: ordered by instruction count")
    else:
        print("no --costs: ordered by the largest instruction count")
    for line in format_rows(rows, args.highlighted, args.max_variants):
        print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
