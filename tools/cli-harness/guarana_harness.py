#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""guarana-cli-harness: machine-readable --json wrapper around guarana-cli (CLI-Anything trial).

Wraps the guarana-cli binary as a BLACK BOX (no emulator source changes) and the in-process
nx2 debug RPC (EDEN_NX2_DEBUG_SERVER=<port>, TCP newline-JSON, one request per connection).
Every subcommand prints exactly ONE JSON object on stdout; log noise goes to stderr.

Envelope: {"ok": bool, "command": str, "data": {...}, "error": null|{"kind","message"},
           "exit_code": int}   (exit_code mirrors the process exit status of the harness)

Commands:
  status          RPC status: title, program_id, scm_rev, fps, switches resolution, ...
  switches        resolved switches from RPC status, or parse a local nx2_switches.txt (--file)
  bench           RPC frame_times: mean/p95/p99 frame intervals, fps, stutters (--seconds N)
  screenshot      RPC screenshot -> PNG file (--out), JSON carries path/width/height/bytes
  launch          start guarana-cli -g <game> with the debug RPC enabled, wait until ready
  log-tail        RPC log_tail (--lines N)
  input           RPC input: press buttons for N frames (--buttons A,B --frames 30)
  rpc             generic passthrough to ANY registered debug method (--params JSON)
  warm-pipelines  headless shader warm-up; parses progress into structured JSON
  nup-pack        --pack-nup wrapper with structured result
  nup-unpack      --unpack-nup wrapper with structured result
  version         guarana-cli --version, captured cleanly

Standard library only (Python 3.10+). Windows: pass native C:/ paths.
See SKILL.md next to this file for the agent-facing contract.
"""

import argparse
import base64
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import time

DEFAULT_SERVER = "127.0.0.1:6545"
CLI_CANDIDATES = (
    "C:/lb/build-1120/bin/guarana-cli.exe",
    "C:/lb/build-1118/bin/guarana-cli.exe",
)
# Exit codes of guarana-cli --warm-pipelines (src/lichii_cmd/lichii.cpp).
WARM_EXIT_MEANING = {0: "done", 1: "failed", 3: "run_again", 4: "incomplete"}


class HarnessError(Exception):
    def __init__(self, kind, message):
        super().__init__(message)
        self.kind = kind
        self.message = message


def emit(command, data=None, error=None, exit_code=0):
    """Print the single JSON envelope on stdout and return the process exit code."""
    envelope = {
        "ok": error is None,
        "command": command,
        "data": data if data is not None else {},
        "error": error,
        "exit_code": exit_code,
    }
    print(json.dumps(envelope, ensure_ascii=False))
    return exit_code


def fail(command, kind, message, exit_code=1):
    return emit(command, error={"kind": kind, "message": str(message)}, exit_code=exit_code)


# ---------------------------------------------------------------- binary / RPC


def find_cli(explicit=None):
    if explicit:
        if not os.path.isfile(explicit):
            raise HarnessError("cli_not_found", f"no guarana-cli at {explicit}")
        return explicit
    env = os.environ.get("GUARANA_CLI")
    if env and os.path.isfile(env):
        return env
    for candidate in CLI_CANDIDATES:
        if os.path.isfile(candidate):
            return candidate
    which = shutil.which("guarana-cli")
    if which:
        return which
    raise HarnessError(
        "cli_not_found",
        "guarana-cli not found; pass --cli PATH or set GUARANA_CLI (looked in "
        + ", ".join(CLI_CANDIDATES) + ")",
    )


def parse_server(text):
    host, _, port = text.rpartition(":")
    if not host:
        host, port = "127.0.0.1", text
    try:
        return host, int(port)
    except ValueError:
        raise HarnessError("bad_server", f"not a host:port or port: {text!r}")


def rpc_call(server, method, params=None, timeout_s=30.0, retries=3, backoff_s=0.5):
    """One request per connection, newline-delimited JSON; retry connects with backoff.

    A refused connection usually means the emulator has not booted (or exited) — the RPC
    only listens once emulation is up.
    """
    host, port = parse_server(server)
    request = json.dumps({"id": 1, "method": method, "params": params or {}}) + "\n"
    last_error = None
    for attempt in range(max(1, retries)):
        try:
            with socket.create_connection((host, port), timeout=timeout_s) as sock:
                sock.sendall(request.encode("utf-8"))
                sock.settimeout(timeout_s)
                chunks = []
                while True:
                    chunk = sock.recv(65536)
                    if not chunk:
                        break
                    chunks.append(chunk)
                    if b"\n" in chunk:
                        break
            raw = b"".join(chunks).split(b"\n", 1)[0]
            response = json.loads(raw.decode("utf-8"))
            if "error" in response and response["error"] is not None:
                err = response["error"]
                message = err.get("message", str(err)) if isinstance(err, dict) else str(err)
                raise HarnessError("rpc_error", f"{method}: {message}")
            return response.get("result", response)
        except (ConnectionError, OSError, socket.timeout) as exc:
            last_error = exc
            if attempt + 1 < max(1, retries):
                time.sleep(backoff_s * (2**attempt))
    raise HarnessError(
        "rpc_unreachable",
        f"cannot reach debug RPC at {host}:{port} after {retries} tries ({last_error}); "
        "is the emulator running with EDEN_NX2_DEBUG_SERVER=<port> and a game booted?",
    )


def run_cli(cli, args, timeout_s=600):
    """Run guarana-cli as a black box; capture stdout/stderr separately."""
    try:
        proc = subprocess.run(
            [cli, *args],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=timeout_s,
        )
    except subprocess.TimeoutExpired:
        raise HarnessError("timeout", f"guarana-cli {' '.join(args)} exceeded {timeout_s}s")
    except OSError as exc:
        raise HarnessError("spawn_failed", str(exc))
    return proc


# ------------------------------------------------------------------- parsing


def parse_switches_file(text):
    """nx2_switches.txt: KEY=VALUE lines, '#' comments, blank lines ignored."""
    switches = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        key, sep, value = line.partition("=")
        if sep:
            switches[key.strip()] = value.strip()
    return switches


WARM_PROGRESS_RE = re.compile(
    r"warm: (?P<title>[0-9A-Fa-f]+): (?P<stage>precache|boot-builds) "
    r"(?P<step>\d+)/(?P<total>\d+) .*? (?P<built>\d+)/(?P<cached>\d+) pipelines built"
    r" \((?P<rate>[\d.]+) pipelines/s\)"
)
WARM_FINAL_RE = re.compile(
    r"warm: (?P<outcome>done|stopped|nothing built|incomplete) after (?P<elapsed>[\d.]+) s: "
    r"(?P<cached>\d+) cached graphics pipelines, (?P<already>\d+) already built for this "
    r"driver, (?P<left>\d+) left"
)


def parse_warm_output(stdout, exit_code):
    """Turn --warm-pipelines log lines into the structured result the perf-lab lane needs."""
    data = {
        "exit_code": exit_code,
        "outcome": WARM_EXIT_MEANING.get(exit_code, "unknown"),
        "title_id": None,
        "pipelines_cached": None,
        "pipelines_built": None,
        "pipelines_already_built": None,
        "pipelines_left": None,
        "elapsed_s": None,
        "build_rate_per_s": None,
    }
    for line in stdout.splitlines():
        progress = WARM_PROGRESS_RE.search(line)
        if progress:
            data["title_id"] = progress.group("title").upper()
            data["pipelines_built"] = int(progress.group("built"))
            data["pipelines_cached"] = int(progress.group("cached"))
            data["build_rate_per_s"] = float(progress.group("rate"))
            continue
        final = WARM_FINAL_RE.search(line)
        if final:
            data["outcome"] = final.group("outcome").replace(" ", "_")
            data["elapsed_s"] = float(final.group("elapsed"))
            data["pipelines_cached"] = int(final.group("cached"))
            data["pipelines_already_built"] = int(final.group("already"))
            data["pipelines_left"] = int(final.group("left"))
    return data


NUP_WROTE_RE = re.compile(r"Wrote (?P<path>.+?) in (?P<elapsed>[\d.]+) s")


def tail_lines(text, count=10):
    lines = [ln for ln in text.replace("\r", "\n").splitlines() if ln.strip()]
    return lines[-count:]


# ------------------------------------------------------------------ commands


def cmd_status(args):
    params = {}
    if args.fps_window_ms is not None:
        params["fps_window_ms"] = args.fps_window_ms
    return emit("status", rpc_call(args.server, "status", params, timeout_s=args.timeout))


def cmd_switches(args):
    if args.file:
        with open(args.file, "r", encoding="utf-8-sig") as handle:
            return emit("switches", {"source": args.file,
                                     "switches": parse_switches_file(handle.read())})
    status = rpc_call(args.server, "status", timeout_s=args.timeout)
    return emit("switches", {"source": f"rpc:{args.server}",
                             "switches": status.get("switches", {}),
                             "program_id": status.get("program_id"),
                             "scm_rev": status.get("scm_rev")})


def cmd_bench(args):
    params = {"seconds": args.seconds}
    if args.refresh_hz is not None:
        params["refresh_hz"] = args.refresh_hz
    return emit("bench", rpc_call(args.server, "frame_times", params,
                                  timeout_s=max(args.timeout, args.seconds + 10)))


def cmd_screenshot(args):
    params = {"height": args.height, "inline": True}
    result = rpc_call(args.server, "screenshot", params, timeout_s=max(args.timeout, 35))
    png = base64.b64decode(result.get("png_base64", ""))
    if not png:
        raise HarnessError("empty_screenshot", "RPC returned no PNG data")
    out = args.out or os.path.join(os.getcwd(), "guarana-screenshot.png")
    with open(out, "wb") as handle:
        handle.write(png)
    data = {"path": os.path.abspath(out), "width": result.get("width"),
            "height": result.get("height"), "bytes": len(png)}
    if args.inline:
        data["png_base64"] = result["png_base64"]
    return emit("screenshot", data)


def build_launch(cli, args):
    """Pure argv/env builder for launch (unit-testable without spawning anything)."""
    argv = [cli, "-g", args.game]
    if args.fullscreen:
        argv.append("-f")
    if args.user is not None:
        argv += ["-u", str(args.user)]
    if args.gdb_port is not None:
        argv += ["-d", str(args.gdb_port)]
    if args.launcher_session:
        argv.append("--launcher-session")
    env = dict(os.environ)
    env["EDEN_NX2_DEBUG_SERVER"] = str(parse_server(args.server)[1])
    return argv, env


def cmd_launch(args):
    cli = find_cli(args.cli)
    argv, env = build_launch(cli, args)
    flags = 0
    if os.name == "nt":
        # Detach so the emulator survives this harness process (and the calling agent turn).
        flags = 0x00000008 | 0x00000200 | 0x01000000  # DETACHED|NEW_GROUP|BREAKAWAY_JOB
    log_path = args.log or os.path.join(os.getcwd(), "guarana-launch.log")
    log_handle = open(log_path, "ab")
    try:
        proc = subprocess.Popen(argv, env=env, stdout=log_handle, stderr=log_handle,
                                creationflags=flags)
    finally:
        log_handle.close()
    data = {"pid": proc.pid, "server": args.server, "game": args.game,
            "log": os.path.abspath(log_path), "argv": argv}
    deadline = time.monotonic() + args.wait_s
    status = None
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise HarnessError("launch_exited",
                               f"guarana-cli exited early with code {proc.returncode}; "
                               f"see {log_path}")
        try:
            status = rpc_call(args.server, "status", retries=1, timeout_s=5)
            break
        except HarnessError:
            time.sleep(1.0)
    data["ready"] = status is not None
    data["status"] = status
    if status is None and args.wait_s > 0:
        data["note"] = f"RPC not up within {args.wait_s}s; process still running"
    return emit("launch", data)


def cmd_log_tail(args):
    return emit("log-tail", rpc_call(args.server, "log_tail", {"lines": args.lines},
                                     timeout_s=args.timeout))


def cmd_input(args):
    buttons = [b.strip() for b in args.buttons.split(",") if b.strip()]
    return emit("input", rpc_call(args.server, "input",
                                  {"buttons": buttons, "frames": args.frames},
                                  timeout_s=max(args.timeout, args.frames / 30 + 10)))


def cmd_rpc(args):
    try:
        params = json.loads(args.params) if args.params else {}
    except json.JSONDecodeError as exc:
        raise HarnessError("bad_params", f"--params is not JSON: {exc}")
    return emit("rpc", {"method": args.method,
                        "result": rpc_call(args.server, args.method, params,
                                           timeout_s=args.timeout)})


def cmd_warm_pipelines(args):
    cli = find_cli(args.cli)
    title = args.title_id.lower().removeprefix("0x")
    if not re.fullmatch(r"[0-9a-f]{1,16}", title) or int(title, 16) == 0:
        raise HarnessError("bad_title_id", f"{args.title_id!r} is not a hex title id")
    proc = run_cli(cli, ["--warm-pipelines", title], timeout_s=args.timeout)
    data = parse_warm_output(proc.stdout, proc.returncode)
    data["title_id"] = data["title_id"] or title.upper()
    if proc.returncode != 0:
        data["stderr_tail"] = tail_lines(proc.stderr)
    return emit("warm-pipelines", data, exit_code=0 if proc.returncode == 0 else 1)


def cmd_nup_pack(args):
    cli = find_cli(args.cli)
    argv = ["--pack-nup", args.dump] + ([args.out] if args.out else [])
    proc = run_cli(cli, argv, timeout_s=args.timeout)
    data = {"exit_code": proc.returncode, "dump": args.dump}
    match = NUP_WROTE_RE.search(proc.stderr)
    if match:
        data["archive"] = match.group("path")
        data["elapsed_s"] = float(match.group("elapsed"))
    data["stderr_tail"] = tail_lines(proc.stderr)
    return emit("nup-pack", data, exit_code=0 if proc.returncode == 0 else 1)


def cmd_nup_unpack(args):
    cli = find_cli(args.cli)
    argv = ["--unpack-nup", args.nup] + ([args.out] if args.out else [])
    proc = run_cli(cli, argv, timeout_s=args.timeout)
    data = {"exit_code": proc.returncode, "nup": args.nup,
            "out_dir": args.out, "stderr_tail": tail_lines(proc.stderr)}
    return emit("nup-unpack", data, exit_code=0 if proc.returncode == 0 else 1)


VERSION_LINE_RE = re.compile(r"ApplyLaunchParams: (?P<version>.+)$")
LOG_LINE_RE = re.compile(r"^\[\s*[\d.]+\]\s")


def parse_version_output(stdout):
    """guarana-cli -v mixes log spam with the result; pull the real version string out."""
    lines = [ln.strip() for ln in stdout.splitlines() if ln.strip()]
    for line in lines:
        match = VERSION_LINE_RE.search(line)
        if match:
            return match.group("version").strip(), lines
    clean = [ln for ln in lines if not LOG_LINE_RE.match(ln)]
    return (clean[-1] if clean else (lines[-1] if lines else None)), lines


def cmd_version(args):
    cli = find_cli(args.cli)
    proc = run_cli(cli, ["--version"], timeout_s=30)
    version, lines = parse_version_output(proc.stdout)
    return emit("version", {"cli": cli, "exit_code": proc.returncode,
                            "version": version, "raw": lines})


# --------------------------------------------------------------------- main


def build_parser():
    parser = argparse.ArgumentParser(
        prog="guarana_harness",
        description="Machine-readable JSON harness around guarana-cli + the nx2 debug RPC.")
    parser.add_argument("--server", default=os.environ.get("GUARANA_DEBUG_SERVER", DEFAULT_SERVER),
                        help=f"debug RPC host:port (default {DEFAULT_SERVER})")
    parser.add_argument("--cli", help="path to guarana-cli (else $GUARANA_CLI, else known builds)")
    parser.add_argument("--timeout", type=float, default=30.0, help="RPC/op timeout seconds")
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("status", help="RPC status (title, fps, scm_rev, switches resolution)")
    p.add_argument("--fps-window-ms", type=int, default=None)
    p.set_defaults(func=cmd_status)

    p = sub.add_parser("switches", help="resolved switches (RPC) or a local nx2_switches.txt")
    p.add_argument("--file", help="parse this nx2_switches.txt instead of querying the RPC")
    p.set_defaults(func=cmd_switches)

    p = sub.add_parser("bench", help="RPC frame_times benchmark")
    p.add_argument("--seconds", type=float, default=5.0)
    p.add_argument("--refresh-hz", type=float, default=None)
    p.set_defaults(func=cmd_bench)

    p = sub.add_parser("screenshot", help="RPC screenshot saved to a PNG file")
    p.add_argument("--out", help="output PNG path (default ./guarana-screenshot.png)")
    p.add_argument("--height", type=int, default=720)
    p.add_argument("--inline", action="store_true", help="also embed png_base64 in the JSON")
    p.set_defaults(func=cmd_screenshot)

    p = sub.add_parser("launch", help="start guarana-cli with the debug RPC and wait for ready")
    p.add_argument("--game", required=True, help="game path passed to -g")
    p.add_argument("--fullscreen", action="store_true")
    p.add_argument("--user", type=int, choices=range(8), default=None)
    p.add_argument("--gdb-port", type=int, default=None, help="-d GDB stub port")
    p.add_argument("--launcher-session", action="store_true")
    p.add_argument("--wait-s", type=float, default=60.0, help="seconds to wait for the RPC")
    p.add_argument("--log", help="emulator log file (default ./guarana-launch.log)")
    p.set_defaults(func=cmd_launch)

    p = sub.add_parser("log-tail", help="RPC log_tail")
    p.add_argument("--lines", type=int, default=50)
    p.set_defaults(func=cmd_log_tail)

    p = sub.add_parser("input", help="RPC input: press buttons for N frames")
    p.add_argument("--buttons", required=True, help="comma list, e.g. A,B,Plus")
    p.add_argument("--frames", type=int, default=30)
    p.set_defaults(func=cmd_input)

    p = sub.add_parser("rpc", help="generic passthrough to any registered debug method")
    p.add_argument("method")
    p.add_argument("--params", help="JSON object of parameters")
    p.set_defaults(func=cmd_rpc)

    p = sub.add_parser("warm-pipelines", help="headless shader warm-up, structured result")
    p.add_argument("--title-id", required=True, help="16 hex digits, e.g. 0100...")
    p.add_argument("--timeout", type=float, default=1800.0, dest="timeout",
                   help="warm-up budget seconds (default 1800)")
    p.set_defaults(func=cmd_warm_pipelines)

    p = sub.add_parser("nup-pack", help="wrap --pack-nup with a structured result")
    p.add_argument("--dump", required=True)
    p.add_argument("--out", help="output .nup path")
    p.set_defaults(func=cmd_nup_pack)

    p = sub.add_parser("nup-unpack", help="wrap --unpack-nup with a structured result")
    p.add_argument("--nup", required=True)
    p.add_argument("--out", help="output directory")
    p.set_defaults(func=cmd_nup_unpack)

    p = sub.add_parser("version", help="guarana-cli --version, captured cleanly")
    p.set_defaults(func=cmd_version)
    return parser


def main(argv=None):
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except HarnessError as exc:
        return fail(args.command, exc.kind, exc.message)
    except (OSError, ValueError) as exc:
        return fail(args.command, "error", str(exc))


if __name__ == "__main__":
    sys.exit(main())
