#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Starts Lichii for normal play on an integrated GPU: checks the machine, applies a profile.

  python -I tools/bench/play.py --exe <lichii> --game <game> [--profile profiles/<name>.json]
      [--power-policy NAME=VALUE ...] [--close <process name> ...] [--dry-run]

Looks for other heavy processes and battery power (it only reports them; --close ends the named
ones), applies the optional processor power settings for the session and restores them when the
emulator exits, sets the profile's environment switches and waits for the emulator. See README.md.
"""

import argparse
import base64
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_PROFILE = os.path.join(HERE, "profiles", "oot-intel-igpu.json")
CPU_HEAVY_PCT = 5.0      # of the whole machine
GPU_HEAVY_MB = 500
# Part of the desktop or of this check itself: listed, never called heavy.
NOT_HEAVY = {"idle", "system", "dwm", "csrss", "explorer", "shellhost", "memory compression", "powershell",
             "python", "conhost"}

SURVEY_SCRIPT = r"""
$ErrorActionPreference = 'SilentlyContinue'
$before = @{}; Get-Process | ForEach-Object { if ($_.TotalProcessorTime) { $before[$_.Id] = $_.TotalProcessorTime.TotalSeconds } }
$watch = [System.Diagnostics.Stopwatch]::StartNew()
$gpu = @{}
(Get-Counter '\GPU Process Memory(*)\Dedicated Usage', '\GPU Process Memory(*)\Shared Usage' -MaxSamples 1).CounterSamples |
    ForEach-Object { if ($_.InstanceName -match 'pid_(\d+)_') { $gpu[[int]$Matches[1]] += $_.CookedValue } }
Start-Sleep -Milliseconds 1200
$seconds = $watch.Elapsed.TotalSeconds; $cores = [Environment]::ProcessorCount
$rows = @(Get-Process | ForEach-Object {
    $cpu = 0
    if ($_.TotalProcessorTime -and $before.ContainsKey($_.Id)) { $cpu = ($_.TotalProcessorTime.TotalSeconds - $before[$_.Id]) / $seconds / $cores * 100 }
    @{ name = $_.ProcessName; pid = $_.Id; cpu_pct = [math]::Round($cpu, 1); ram_mb = [math]::Round($_.WorkingSet64 / 1MB); gpu_mb = [math]::Round($gpu[$_.Id] / 1MB) }
} | Where-Object { $_.cpu_pct -ge 0.5 -or $_.gpu_mb -ge 50 })
$status = Get-CimInstance -Namespace root\wmi -ClassName BatteryStatus | Select-Object -First 1
$battery = Get-CimInstance Win32_Battery | Select-Object -First 1
@{ processes = $rows; power_online = $(if ($status) { [bool]$status.PowerOnline } else { $null });
   battery_pct = $battery.EstimatedChargeRemaining } | ConvertTo-Json -Depth 4 -Compress
"""


class Deps:
    """What a session touches outside this process; tests replace these."""
    run_command = staticmethod(bench.run_command)
    running = staticmethod(bench.process_running)
    log = staticmethod(lambda message: print(message, flush=True))
    sleep = staticmethod(bench.time.sleep)

    @staticmethod
    def launch(exe, game, env):
        return subprocess.Popen([exe, game], cwd=os.path.dirname(exe) or None, env=env,
                                stdin=subprocess.DEVNULL)


def survey(run):
    """{processes: [{name, pid, cpu_pct, ram_mb, gpu_mb}], power_online, battery_pct}, or None."""
    encoded = base64.b64encode(SURVEY_SCRIPT.encode("utf-16-le")).decode()
    _, output = run(["powershell", "-NoProfile", "-NonInteractive", "-EncodedCommand", encoded], 90)
    try:
        result = json.loads(output[output.index("{"):])
    except ValueError:
        return None
    processes = result.get("processes") or []
    result["processes"] = [process for process in ([processes] if isinstance(processes, dict) else processes)
                           if isinstance(process, dict) and process.get("name")]
    return result


def heavy_processes(processes, own_image):
    own = os.path.splitext(own_image)[0].lower()
    return [process for process in processes
            if process["name"].lower() not in NOT_HEAVY | {own}
            and ((process.get("cpu_pct") or 0) >= CPU_HEAVY_PCT or (process.get("gpu_mb") or 0) >= GPU_HEAVY_MB)]


def describe_survey(result, own_image):
    """Lines for the user: the top processes, what looks heavy, the power source."""
    if result is None:
        return ["could not list the running processes (needs Windows PowerShell): not checked"]
    lines = []
    processes = result["processes"]
    for title, key, unit in (("CPU", "cpu_pct", "%"), ("GPU memory", "gpu_mb", " MB")):
        top = [process for process in sorted(processes, key=lambda item: -(item.get(key) or 0))[:5]
               if process.get(key)]
        lines.append(f"top {title}: " + (", ".join(f"{process['name']} {process[key]:g}{unit}" for process in top)
                                         or "nothing notable"))
    heavy = heavy_processes(processes, own_image)
    for process in heavy:
        lines.append(f"WARNING: {process['name']} (pid {process['pid']}) uses {process.get('cpu_pct') or 0:g}% CPU "
                     f"and {process.get('gpu_mb') or 0:g} MB GPU memory; an integrated GPU shares both with "
                     f"the game. Close it, or pass --close {process['name']}")
    if not heavy:
        lines.append("nothing heavy is running")
    if result.get("power_online") is False:
        charge = f" ({result['battery_pct']}%)" if result.get("battery_pct") is not None else ""
        lines.append(f"WARNING: on battery{charge}: the processor and GPU are held back; plug the machine in")
    return lines


def close_processes(names, deps):
    """Asks each named process to close, then ends what is left of it."""
    for name in names:
        image = name if os.path.splitext(name)[1] else name + ".exe"
        deps.log(f"closing {image}")
        deps.run_command(["taskkill", "/IM", image], 30)
        deps.sleep(3)
        if deps.running(image):
            deps.run_command(["taskkill", "/F", "/IM", image], 30)


def load_profile(path):
    profile = bench.load_json(path, "profile")
    env = profile.get("env") or {}
    if not isinstance(env, dict):
        raise bench.BenchError(f"profile {path}: env is an object")
    return {"name": str(profile.get("name") or os.path.basename(path)),
            "env": {str(key): str(value) for key, value in env.items()},
            "power_policy": bench.parse_power_policy(profile.get("power_policy") or {})}


def play(args, deps=Deps):
    exe, game = bench.expand(args.exe), bench.expand(args.game)
    profile = load_profile(bench.expand(args.profile))
    values = {**profile["power_policy"], **bench.parse_power_policy(args.power_policy)}
    restore_file = os.path.join(bench.expand(args.state_dir) or bench.default_state_dir(), "power-restore.json")
    policy = bench.PowerPolicy(values, deps.run_command, restore_file)
    image = os.path.basename(exe)
    for path, what in ((exe, "emulator"), (game, "game")):
        if not os.path.exists(path):
            raise bench.BenchError(f"{what} not found: {path}")
    if deps.running(image):
        if not args.dry_run:
            raise bench.BenchError(f"{image} is already running")
        deps.log(f"PROBLEM: {image} is already running; a real start would stop here")

    deps.log(f"profile     {profile['name']}")
    if os.path.isfile(restore_file):
        if args.dry_run:
            deps.log(f"would restore the power settings an earlier session left changed ({restore_file})")
        else:
            deps.log(f"restored the power settings an earlier session left changed: {policy.recover()}")
    for line in describe_survey(survey(deps.run_command), image):
        deps.log(line)
    current = policy.read() if values else {}
    changes = [f"{name} {current[name][0]} -> {value}" for name, value in values.items() if name in current]
    missing = [name for name in values if name not in current]
    env_text = " ".join(f"{key}={value}" for key, value in profile["env"].items()) or "none"
    if args.dry_run:
        for name in args.close or []:
            deps.log(f"would close {name}")
        deps.log("would set (AC, restored on exit): " + (", ".join(changes) or "no power settings"))
        if missing:
            deps.log(f"not on this machine, skipped: {', '.join(missing)}")
        deps.log(f"would set environment: {env_text}")
        deps.log(f"would start {exe} \"{game}\" and wait for it to exit")
        return 0

    close_processes(args.close or [], deps)
    try:
        if values:
            applied = policy.apply()
            deps.log("power settings (AC, restored on exit): " + (", ".join(changes) or "none")
                     + (f"; not on this machine: {', '.join(applied['skipped'])}" if applied["skipped"] else ""))
        deps.log(f"environment: {env_text}")
        process = deps.launch(exe, game, {**os.environ, **profile["env"]})
        deps.log(f"{image} started; waiting for it to exit")
        try:
            return process.wait()
        except KeyboardInterrupt:
            deps.log("interrupted: the emulator keeps running, the power settings are restored now")
            return 130
    finally:
        policy.restore()
        if values:
            deps.log("power settings restored")


def build_parser():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--exe", required=True, help="the emulator executable")
    parser.add_argument("--game", required=True, help="the game file")
    parser.add_argument("--profile", default=DEFAULT_PROFILE, help="profile file (default: profiles/oot-intel-igpu.json)")
    parser.add_argument("--power-policy", action="append", metavar="NAME=VALUE", help="processor power setting "
                        "(powercfg alias, AC value) for the session, e.g. PERFBOOSTMODE=0; repeatable; added to "
                        "the profile's")
    parser.add_argument("--close", action="append", metavar="NAME", help="end this process first (image name, "
                        "e.g. chrome); repeatable. Nothing is closed without it")
    parser.add_argument("--state-dir", help="where the power restore file is kept (default: per user)")
    parser.add_argument("--dry-run", action="store_true", help="print what would be done; change nothing")
    return parser


def main(argv=None):
    try:
        return play(build_parser().parse_args(argv))
    except (bench.BenchError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
