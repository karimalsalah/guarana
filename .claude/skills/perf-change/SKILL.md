---
name: perf-change
description: "The measured-gain workflow for every performance, pacing, power or memory change: a hypothesis with evidence, the change behind a default-off EDEN_NX2_* switch, a paired A/B on a verified tools/bench scene, the numbers in the pull request and docs/switches.md, then promote or drop. Use when proposing, implementing or reviewing an optimization or a new switch, or any change that claims fps, frame-time, power or memory gains."
---

# Performance change: measure, then decide

AGENTS.md rules 1 to 4 decide this: only a measured gain on a verified scene ships, experiments are
`EDEN_NX2_*` switches that are off by default, every switch has a row in `docs/switches.md`, and
correctness comes before speed.

## 1. Hypothesis, with evidence

Write down before coding: what is slow, the evidence (a `gpu_costs` reply, `EDEN_NX2_GPU_PROFILE`,
`EDEN_NX2_GUEST_THREAD_STATS`, `EDEN_NX2_STUTTER_TRACE`, the `sample` profiler, or
`EDEN_NX2_PIPELINE_STATS` for what the driver compiled), why the change should help, how much you
expect and the scene that shows it. Then look for prior work: the idea's row in `docs/switches.md`
(an "experimental, no gain" row is a result) and upstream (the `upstream-port` skill).

## 2. The change, behind a switch that is off

- One switch per idea, `EDEN_NX2_<NAME>`, read once at start like the switches next to it
  (`grep -rn "EDEN_NX2_" src` shows the pattern). Unset or `0` must leave the old behaviour exactly.
- Unit-test the logic where it can be tested (`src/tests/<area>/`, as `video_core/frame_budget.cpp`
  does) and run `bash scripts/run-tests.sh` or `powershell -ExecutionPolicy Bypass -File
  scripts/run-tests.ps1`.
- Add the switch's row to `docs/switches.md` under Experimental: values, what it does,
  "No verified-scene measurement", code location. A `release/vX.Y` branch takes no new switches.

## 3. A/B on a verified scene

- A variant file `tools/bench/variants/<name>.json`, for example
  `{"name": "<name>", "env": {"EDEN_NX2_<NAME>": "1"}}`.
- Baseline and variant in one session, back to back (the `bench` skill):
  `python -I tools/bench/bench.py run ... --variant tools/bench/variants/baseline.json --variant tools/bench/variants/<name>.json`.
  Near the noise, run the pair again in the other order: baselines in a scene can wander between
  sessions (switches.md notes about 14 to 16 fps in Hyrule Field).
- Only rows with `"valid": true` and `"scene_ok": true` count. A wrong scene is not a result.
- Check the picture: compare the rows' screenshots, and for a GPU change a screenshot or frame
  capture of the affected pass. A gain that changes the image is a bug until shown otherwise.
- Debug-server numbers (`frame_times`, `gpu_costs`) steer the work; the pull request's numbers come
  from rows. Start no build while a run is going (CLAUDE.md).

## 4. Numbers in the pull request

- The template's "Measured result" table: device (CPU, GPU, driver, power), scene, metric, before,
  after, runs. Paste or link the rows, or the `bench.py compare` output.
- "Switches added or changed": each switch, its values and its default (off).
- The commit body: scene, device, driver and power mode, before and after (fps, p50 and p99 frame
  time, or the draw's GPU ms) and the number of paired runs. Subject `area: change`.
- The switch's row in `docs/switches.md` gets the same measurement, and `CHANGELOG.md` a line under
  `[Unreleased]`. Say what was not measured: other GPUs, drivers, games.

## 5. Promote or drop

- **A gain**: the row becomes "proven" and the switch stays off until it has been checked beyond
  the scene it was measured in. Making it the default is a later pull request with those
  measurements: unconditional in code (the switch removed), or on by default with the switch kept to
  turn it off ("default on"), or only for the titles it helps (`src/core/game_presets.cpp`).
- **No gain, noise or slower**: remove the code and say so in the pull request, or leave it off with
  the result written in its row. Keep the numbers either way, so nobody retries the idea blind.
