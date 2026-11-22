#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Replays GPU traces (docs/nx2/gpu-trace.md) and compares their frames.

  trace_replay.py replay --cli <guarana-cli> <trace> --out <dir>
  trace_replay.py compare --base <guarana-cli> --head <guarana-cli> <trace> --out <dir>
  trace_replay.py check --cli <guarana-cli> --corpus tests/gpu_traces/corpus.json [--traces <dir>]
  trace_replay.py bench --cli <guarana-cli> <trace> --out <dir> [--variant NAME[=KEY=VALUE,...]]...
                        [--build NAME=<guarana-cli>]... [--repeat N] [--skip N] [--frames N]
                        [--no-images] [--allow-different] [--noise-margin DB]

<guarana-cli> is the path of the SDL frontend (eden-cli before v20.2).

compare replays one trace on two builds and writes <out>/index.html with the frames side by
side and, per frame, the hashes, the largest channel difference, the mean difference and PSNR.
check replays every corpus trace it can find and compares the frame hashes with the expected
ones for the replaying device class; it exits 1 on a mismatch.

bench is a paired A/B on the same frames. Each variant is a set of environment switches
(EDEN_NX2_...), a build, or both; the first one is the baseline. Every variant replays the trace
--repeat times in turn (A B A B), and bench reports, per variant, whether its frames are the
baseline's bit for bit and the time a frame took to replay. The game does not run, so the scene is
the same by construction and the guest CPU is not part of the time: this measures the emulator's
GPU thread and the host GPU on a fixed workload. It exits 1 when a variant's frames differ from
the baseline's (unless --allow-different).

Some drivers do not replay bit for bit: on Intel's Windows driver two replays of the same build
differ a little in every frame. When the baseline's own runs differ, bench measures by how much
(the lowest PSNR between them, over a sample of the differing frames) and judges each variant
against that noise floor: within --noise-margin dB of it is "within noise", below it "differs".
Without images (--no-images) there is nothing to measure, and a baseline whose runs differ proves
nothing: bench then exits 1.
"""

import argparse
import hashlib
import html
import json
import math
import os
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

SAMPLE_TRACE = "sample"
# The noise floor and a variant's distance from the baseline are measured on at most this many of
# the frames that differ, spread over the replay, on every NOISE_STRIDE-th byte of each: enough for
# the PSNR of noise spread over a frame, and fast enough in pure Python on 2560x1440 frames.
NOISE_FRAMES = 8
NOISE_STRIDE = 16


def replay(cli, trace, out, extra=(), environment=None):
    """Replays `trace` with `switches` added to the environment; the frames.json it wrote."""
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    command = [str(cli), "--replay", str(trace), "--out", str(out), *extra]
    env = {**os.environ, **environment} if environment else None
    result = subprocess.run(command, capture_output=True, text=True, errors="replace", env=env)
    if result.returncode != 0:
        tail = "\n".join(result.stderr.splitlines()[-20:])
        raise RuntimeError(f"{' '.join(command)} failed ({result.returncode}):\n{tail}")
    return json.loads((out / "frames.json").read_text())


def device_class(renderer):
    """The expected-hash key for a replay's renderer string."""
    name = renderer.lower()
    if "llvmpipe" in name or "lavapipe" in name:
        return "lavapipe"
    return name.split()[0] if name else "unknown"


def read_png(path):
    """Decodes an 8-bit RGBA or RGB PNG (what guarana-cli writes) to (width, height, rgba bytes)."""
    data = Path(path).read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path} is not a PNG")
    offset = 8
    idat = b""
    width = height = channels = 0
    while offset < len(data):
        length, kind = struct.unpack(">I4s", data[offset:offset + 8])
        body = data[offset + 8:offset + 8 + length]
        offset += 12 + length
        if kind == b"IHDR":
            width, height, depth, color, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if depth != 8 or color not in (2, 6) or interlace != 0:
                raise ValueError(f"{path}: only 8-bit RGB(A), non-interlaced PNGs are read")
            channels = 4 if color == 6 else 3
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
    raw = zlib.decompress(idat)
    stride = width * channels
    rows = []
    previous = bytearray(stride)
    position = 0
    for _ in range(height):
        kind = raw[position]
        line = bytearray(raw[position + 1:position + 1 + stride])
        position += 1 + stride
        for i in range(stride):
            left = line[i - channels] if i >= channels else 0
            up = previous[i]
            corner = previous[i - channels] if i >= channels else 0
            if kind == 1:
                line[i] = (line[i] + left) & 0xFF
            elif kind == 2:
                line[i] = (line[i] + up) & 0xFF
            elif kind == 3:
                line[i] = (line[i] + ((left + up) >> 1)) & 0xFF
            elif kind == 4:
                p = left + up - corner
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - corner)
                predictor = left if pa <= pb and pa <= pc else up if pb <= pc else corner
                line[i] = (line[i] + predictor) & 0xFF
        rows.append(bytes(line))
        previous = line
    pixels = b"".join(rows)
    if channels == 3:
        pixels = b"".join(pixels[i:i + 3] + b"\xff" for i in range(0, len(pixels), 3))
    return width, height, pixels


def difference(a, b, stride=1):
    """Largest channel difference, mean difference and PSNR (inf when equal) of two images, over
    every `stride`-th byte."""
    if a[0] != b[0] or a[1] != b[1]:
        return {"size_differs": True}
    first, second = a[2][::stride], b[2][::stride]
    largest = 0
    total = 0
    squares = 0
    for x, y in zip(first, second):
        d = abs(x - y)
        largest = max(largest, d)
        total += d
        squares += d * d
    count = len(first) or 1
    mse = squares / count
    psnr = math.inf if mse == 0 else 10 * math.log10(255 * 255 / mse)
    return {"max": largest, "mean": total / count, "psnr": psnr}


def compare_runs(base_dir, base, head_dir, head):
    rows = []
    base_frames = {frame["index"]: frame for frame in base["frames"]}
    head_frames = {frame["index"]: frame for frame in head["frames"]}
    for index in sorted(set(base_frames) | set(head_frames)):
        row = {"index": index, "base": base_frames.get(index), "head": head_frames.get(index)}
        if row["base"] and row["head"]:
            row["equal"] = row["base"]["hash"] == row["head"]["hash"]
            if not row["equal"] and row["base"].get("file") and row["head"].get("file"):
                row["difference"] = difference(read_png(Path(base_dir) / row["base"]["file"]),
                                               read_png(Path(head_dir) / row["head"]["file"]))
        else:
            row["equal"] = False
        rows.append(row)
    return rows


def write_html(out, rows, base_label, head_label):
    lines = ["<!doctype html><meta charset=utf-8><title>GPU trace comparison</title>",
             "<style>body{font:14px sans-serif}td{padding:4px;vertical-align:top}"
             "img{max-width:640px;image-rendering:pixelated}.bad{background:#fdd}</style>",
             f"<p>base: {html.escape(base_label)}<br>head: {html.escape(head_label)}</p><table>"]
    for row in rows:
        cls = "" if row["equal"] else " class=bad"
        cells = []
        for side in ("base", "head"):
            frame = row[side]
            image = f"<img src='{side}/{frame['file']}'><br>" if frame and frame.get("file") else ""
            cells.append(f"<td>{image}{frame['hash']}</td>" if frame else "<td>missing</td>")
        note = "identical" if row["equal"] else html.escape(json.dumps(row.get("difference", {})))
        lines.append(f"<tr{cls}><td>{row['index']}</td>{''.join(cells)}<td>{note}</td></tr>")
    lines.append("</table>")
    (Path(out) / "index.html").write_text("\n".join(lines))


def parse_variant(text):
    """NAME or NAME=KEY=VALUE[,KEY=VALUE...] as (name, {KEY: VALUE})."""
    name, _, rest = text.partition("=")
    if not name:
        raise ValueError(f"variant {text!r} has no name")
    switches = {}
    for pair in filter(None, rest.split(",")):
        key, separator, value = pair.partition("=")
        if not key or not separator:
            raise ValueError(f"variant {name}: {pair!r} is not KEY=VALUE")
        switches[key] = value
    return name, switches


def percentile(values, share):
    """The value at `share` (0 to 1) of the sorted values, interpolated; None without values."""
    ordered = sorted(values)
    if not ordered:
        return None
    position = (len(ordered) - 1) * share
    low = math.floor(position)
    high = min(low + 1, len(ordered) - 1)
    return ordered[low] + (ordered[high] - ordered[low]) * (position - low)


def timing_key(results):
    """replay_ms when every frame of every replay has it, else ms.

    replay_ms covers a frame's commands, composite and read-back. ms, which every build writes,
    also holds the hash and PNG encoding of the frame before it, the same for each variant."""
    frames = [frame for result in results for frame in result["frames"]]
    return "replay_ms" if frames and all("replay_ms" in frame for frame in frames) else "ms"


def summarize(runs, skip, key):
    """One variant's replays: per run and overall, without each run's first `skip` frames (the
    shader builds and first uploads)."""
    rows = []
    for result in runs:
        times = [frame[key] for frame in result["frames"][skip:]]
        rows.append({"frames": len(result["frames"]), "timed": len(times),
                     "median_ms": percentile(times, 0.5), "p95_ms": percentile(times, 0.95),
                     "sum_ms": sum(times)})
    medians = [row["median_ms"] for row in rows if row["median_ms"] is not None]
    return {"runs": rows, "median_ms": percentile(medians, 0.5),
            "best_ms": min(medians) if medians else None,
            "worst_ms": max(medians) if medians else None}


def differing_frames(base, other):
    """Indices whose hashes differ between two replays; a frame only one has differs."""
    a = {frame["index"]: frame["hash"] for frame in base["frames"]}
    b = {frame["index"]: frame["hash"] for frame in other["frames"]}
    return sorted(index for index in set(a) | set(b) if a.get(index) != b.get(index))


def worst_psnr(first_dir, first, second_dir, second, frames=NOISE_FRAMES, stride=NOISE_STRIDE):
    """The lowest PSNR over a sample of the frames two replays disagree on: inf when they agree,
    0 when one replay lacks a frame, None when a frame has no image to measure."""
    differing = differing_frames(first, second)
    if not differing:
        return math.inf
    sample = differing[::max(1, len(differing) // frames)][:frames]
    a = {frame["index"]: frame for frame in first["frames"]}
    b = {frame["index"]: frame for frame in second["frames"]}
    worst = math.inf
    for index in sample:
        x, y = a.get(index), b.get(index)
        if not x or not y:
            return 0.0
        if not x.get("file") or not y.get("file"):
            return None
        x_path, y_path = Path(first_dir) / x["file"], Path(second_dir) / y["file"]
        if not x_path.exists() or not y_path.exists():
            return None
        result = difference(read_png(x_path), read_png(y_path), stride)
        worst = min(worst, result.get("psnr", 0.0))
    return worst


def finite(value):
    """A PSNR for the report: rounded, None for inf (identical) and for unknown."""
    return None if value is None or math.isinf(value) else round(value, 2)


def bench(variants, trace, out, repeat=3, skip=10, extra=(), run=replay, margin=1.0,
          noise_frames=NOISE_FRAMES, noise_stride=NOISE_STRIDE):
    """Replays `trace` under each of `variants` (dicts with name, cli and switches; the first is
    the baseline), `repeat` times in turn, and returns the report bench.json holds."""
    out = Path(out)
    results = {variant["name"]: [] for variant in variants}
    for round_index in range(repeat):
        for variant in variants:
            directory = out / variant["name"] / f"run-{round_index + 1}"
            results[variant["name"]].append(
                run(variant["cli"], trace, directory, extra, variant["switches"]))
    key = timing_key([result for runs in results.values() for result in runs])
    base_name = variants[0]["name"]
    reference = results[base_name][0]
    report = {"trace": Path(trace).name, "renderer": reference.get("renderer", ""),
              "program_id": reference.get("program_id", ""), "repeat": repeat, "skip": skip,
              "timing": key, "variants": []}
    # How much the baseline differs from itself: the floor every variant is judged against.
    base_runs = results[base_name]
    floor = math.inf
    for index in range(1, len(base_runs)):
        psnr = worst_psnr(out / base_name / "run-1", base_runs[0],
                          out / base_name / f"run-{index + 1}", base_runs[index],
                          noise_frames, noise_stride)
        floor = None if psnr is None or floor is None else min(floor, psnr)
    report["noise_floor_db"] = finite(floor)
    report["noise_margin_db"] = margin
    base_median = None
    for variant in variants:
        runs = results[variant["name"]]
        row = {"name": variant["name"], "cli": str(variant["cli"]),
               "switches": variant["switches"], **summarize(runs, skip, key)}
        # Its own runs first: a variant that does not repeat itself proves nothing either way.
        row["repeats_itself"] = all(not differing_frames(runs[0], other) for other in runs[1:])
        differing = differing_frames(reference, runs[0])
        row["differing_frames"] = differing
        # A replay without frames compared nothing: it is not identical to anything.
        row["identical_to_baseline"] = (bool(reference["frames"]) and not differing and
                                        row["repeats_itself"])
        worst = worst_psnr(out / base_name / "run-1", reference, out / variant["name"] / "run-1",
                           runs[0], noise_frames, noise_stride) if reference["frames"] else None
        row["worst_psnr_db"] = finite(worst)
        # Within noise: no further from the baseline than the baseline is from itself (less the
        # margin). A baseline that repeats itself bit for bit leaves no room: identical or not.
        row["within_noise"] = row["identical_to_baseline"] or (
            floor is not None and not math.isinf(floor) and worst is not None and
            worst >= floor - margin)
        if differing:
            index = differing[0]
            frames = [{frame["index"]: frame for frame in result["frames"]}.get(index)
                      for result in (reference, runs[0])]
            if all(frame and frame.get("file") for frame in frames):
                first = out / base_name / "run-1" / frames[0]["file"]
                second = out / variant["name"] / "run-1" / frames[1]["file"]
                if first.exists() and second.exists():
                    row["first_difference"] = {"frame": index,
                                               **difference(read_png(first), read_png(second))}
        if variant is variants[0]:
            base_median = row["median_ms"]
        elif base_median and row["median_ms"] is not None:
            row["change_percent"] = (row["median_ms"] / base_median - 1) * 100
        report["variants"].append(row)
    return report


def bench_table(report):
    """The report as the lines bench prints."""
    def number(value):
        return "-" if value is None else f"{value:.2f}"

    floor = report.get("noise_floor_db")
    lines = [f"{report['trace']} on {report['renderer'] or 'an unnamed device'}: "
             f"{report['repeat']} runs each, first {report['skip']} frames not timed, "
             f"times are {report['timing']}" +
             (f"; the baseline's own runs differ by up to PSNR {floor:.1f} dB, the noise floor"
              if floor is not None else ""),
             f"{'variant':<20} {'frames':>6} {'median':>9} {'best':>9} {'worst':>9} {'p95':>9} "
             f"{'change':>8}  image"]
    for row in report["variants"]:
        runs = row["runs"]
        p95 = percentile([run["p95_ms"] for run in runs if run["p95_ms"] is not None], 0.5)
        change = f"{row['change_percent']:+.1f}%" if "change_percent" in row else ""
        if not runs or not runs[0]["frames"]:
            image = "no frames"
        elif not row["repeats_itself"] and floor is None:
            image = "its own runs differ"
        elif row is report["variants"][0]:
            image = "baseline"
        elif row["identical_to_baseline"]:
            image = "identical"
        elif row.get("within_noise"):
            worst = row.get("worst_psnr_db")
            image = "within noise" + (f" (worst PSNR {worst:.1f} dB)" if worst is not None else "")
        else:
            image = f"{len(row['differing_frames'])} frames differ"
            first = row.get("first_difference")
            if first and "max" in first:
                image += (f" (frame {first['frame']}: largest channel difference "
                          f"{first['max']}, PSNR {first['psnr']:.1f} dB)")
        lines.append(f"{row['name']:<20} {runs[0]['frames'] if runs else 0:>6} "
                     f"{number(row['median_ms']):>9} {number(row['best_ms']):>9} "
                     f"{number(row['worst_ms']):>9} {number(p95):>9} {change:>8}  {image}")
    return lines


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as file:
        for block in iter(lambda: file.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def check(cli, corpus_path, traces_dir, out, require, run=replay, generate=subprocess.run):
    corpus = json.loads(Path(corpus_path).read_text())
    failures = 0
    for entry in corpus["traces"]:
        trace_id = entry["id"]
        work = Path(out) / trace_id
        work.mkdir(parents=True, exist_ok=True)
        if entry.get("source") == "generated":
            trace = work / "trace.lxtrace"
            generator = entry.get("generator", "--gpu-trace-sample")
            generate([str(cli), generator, str(trace), *entry.get("generate", [])],
                     check=True)
        else:
            trace = Path(traces_dir or ".") / entry["file"]
            if not trace.exists():
                print(f"{trace_id}: skipped, {trace} is not here")
                failures += 1 if require else 0
                continue
            if entry.get("sha256") and sha256(trace) != entry["sha256"]:
                print(f"{trace_id}: FAIL, {trace} does not match the corpus sha256")
                failures += 1
                continue
        refused = entry.get("expect_failure")
        if refused is not None:
            # A trace the replay must refuse, with this text in what it says.
            try:
                run(cli, trace, work / "frames")
            except RuntimeError as error:
                if refused in str(error):
                    print(f"{trace_id}: refused as expected ({refused})")
                else:
                    print(f"{trace_id}: FAIL, refused for another reason than "
                          f"'{refused}':\n{error}")
                    failures += 1
            else:
                print(f"{trace_id}: FAIL, replayed although it must be refused ({refused})")
                failures += 1
            continue
        start = entry.get("roundtrip_from")
        extra = [] if start is None else ["--record", str(work / "recorded.lxtrace"),
                                          "--record-from", str(start)]
        environment = entry.get("environment")
        if environment:
            result = replay(cli, trace, work / "frames", extra, environment)
        else:
            result = run(cli, trace, work / "frames", extra)
        device = device_class(result.get("renderer", ""))
        if start is not None:
            if environment:
                again = replay(cli, work / "recorded.lxtrace", work / "recorded",
                               environment=environment)
            else:
                again = run(cli, work / "recorded.lxtrace", work / "recorded")
            tail = [frame["hash"] for frame in result["frames"]][start:]
            if [frame["hash"] for frame in again["frames"]] != tail:
                print(f"{trace_id}: FAIL, the trace recorded from present {start} replays to "
                      f"other frames")
                failures += 1
                continue
            print(f"{trace_id}: recorded from present {start}, {len(tail)} frames replay the same")
        expected = entry.get("expected", {}).get(device)
        hashes = [frame["hash"] for frame in result["frames"]]
        if expected is None:
            print(f"{trace_id}: no expected frames for {device}; got {json.dumps(hashes)}")
            continue
        if hashes != expected:
            print(f"{trace_id}: FAIL on {device}")
            for index, (got, want) in enumerate(zip(hashes, expected)):
                if got != want:
                    print(f"  frame {index}: {got}, expected {want}")
            if len(hashes) != len(expected):
                print(f"  {len(hashes)} frames, expected {len(expected)}")
            failures += 1
        else:
            print(f"{trace_id}: {len(hashes)} frames match on {device}")
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    one = commands.add_parser("replay")
    one.add_argument("--cli", required=True)
    one.add_argument("trace")
    one.add_argument("--out", required=True)
    two = commands.add_parser("compare")
    two.add_argument("--base", required=True)
    two.add_argument("--head", required=True)
    two.add_argument("trace")
    two.add_argument("--out", required=True)
    corpus = commands.add_parser("check")
    corpus.add_argument("--cli", required=True)
    corpus.add_argument("--corpus", required=True)
    corpus.add_argument("--traces", help="directory holding the corpus traces")
    corpus.add_argument("--out", help="work directory (default: a temporary one)")
    corpus.add_argument("--require", action="store_true",
                        help="fail when a corpus trace is missing instead of skipping it")
    paired = commands.add_parser("bench")
    paired.add_argument("--cli", required=True, help="the build every variant replays on")
    paired.add_argument("trace")
    paired.add_argument("--out", required=True)
    paired.add_argument("--variant", action="append", default=[], metavar="NAME[=KEY=VALUE,...]",
                        help="switches added to the environment; the first variant is the "
                             "baseline (default: one variant, base, without switches)")
    paired.add_argument("--build", action="append", default=[], metavar="NAME=CLI",
                        help="another build for the variant NAME (added when it is not one)")
    paired.add_argument("--repeat", type=int, default=3)
    paired.add_argument("--skip", type=int, default=10,
                        help="frames at the start of each run that are not timed")
    paired.add_argument("--frames", type=int, default=0, help="replay only the first N presents")
    paired.add_argument("--no-images", action="store_true",
                        help="hashes and times only (needs builds that know --no-images)")
    paired.add_argument("--allow-different", action="store_true",
                        help="exit 0 when a variant's frames differ from the baseline's")
    paired.add_argument("--noise-margin", type=float, default=1.0, metavar="DB",
                        help="how far below the baseline's own noise floor (PSNR) a variant may "
                             "fall and still count as within noise (default 1.0)")
    args = parser.parse_args()

    if args.command == "bench":
        try:
            parsed = [parse_variant(text) for text in args.variant] or [("base", {})]
            builds = dict(text.split("=", 1) for text in args.build)
        except ValueError as error:
            parser.error(str(error))
        names = [name for name, _ in parsed]
        if len(set(names)) != len(names):
            parser.error("two variants share a name")
        parsed += [(name, {}) for name in builds if name not in names]
        variants = [{"name": name, "cli": builds.get(name, args.cli), "switches": switches}
                    for name, switches in parsed]
        extra = (["--frames", str(args.frames)] if args.frames else []) + (
            ["--no-images"] if args.no_images else [])
        report = bench(variants, args.trace, args.out, max(args.repeat, 1), max(args.skip, 0),
                       extra, margin=args.noise_margin)
        (Path(args.out) / "bench.json").write_text(json.dumps(report, indent=2) + "\n")
        print("\n".join(bench_table(report)))
        baseline = report["variants"][0]
        if not baseline["runs"] or baseline["runs"][0]["frames"] == 0:
            print("The baseline replayed no frames: nothing was compared.")
            return 1
        if not baseline["repeats_itself"] and report["noise_floor_db"] is None:
            print("The baseline's own runs differ and there are no images to measure by how much: "
                  "the comparison proves nothing.")
            return 1
        different = [row["name"] for row in report["variants"][1:] if not row["within_noise"]]
        return 1 if different and not args.allow_different else 0
    if args.command == "replay":
        result = replay(args.cli, args.trace, args.out)
        for frame in result["frames"]:
            print(f"{frame['index']:4} {frame['hash']} {frame['ms']:8.1f} ms")
        return 0
    if args.command == "compare":
        out = Path(args.out)
        base = replay(args.base, args.trace, out / "base")
        head = replay(args.head, args.trace, out / "head")
        rows = compare_runs(out / "base", base, out / "head", head)
        write_html(out, rows, args.base, args.head)
        differing = [row["index"] for row in rows if not row["equal"]]
        print(f"{len(rows)} frames, {len(differing)} differ: {differing}; see {out / 'index.html'}")
        return 1 if differing else 0
    with tempfile.TemporaryDirectory() as temporary:
        failures = check(args.cli, args.corpus, args.traces, args.out or temporary, args.require)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
