---
name: guarana-cli-harness
description: Use when driving the Guarana emulator autonomously (status/bench/screenshot/launch/warm-up/NUP). JSON-only CLI harness.
version: 0.1.0
license: GPL-3.0-or-later
metadata:
  hermes:
    tags: [emulation, guarana, cli, json, agent-native]
    related_skills: [emulator-perf-lab]
    origin: CLI-Anything trial (HKUDS/CLI-Anything) — wrap, never rebuild the emulator
---

# guarana-cli-harness (DRAFT — CLI-Anything trial)

Machine-readable harness around `guarana-cli` (headless SDL frontend of the Guarana Switch
emulator) and its in-process nx2 debug RPC. Every command prints **exactly one JSON object**
on stdout; emulator log noise never pollutes stdout. Standard library only, Python 3.10+.

Entry point: `tools/cli-harness/guarana_harness.py` (this repo).
Tests: `py -3.13 -m unittest discover -s tools/cli-harness -v` (18 tests, no emulator needed).

## Envelope (every command)

```json
{"ok": true, "command": "status", "data": {...}, "error": null, "exit_code": 0}
```

On failure: `ok:false`, `error:{"kind","message"}`, process exit 1. Error kinds:
`cli_not_found`, `rpc_unreachable`, `rpc_error`, `bad_server`, `bad_title_id`, `bad_params`,
`timeout`, `spawn_failed`, `launch_exited`, `empty_screenshot`.

## Commands

| Command | Wraps | Key data fields |
|---|---|---|
| `status [--fps-window-ms N]` | RPC `status` | title, program_id, build, scm_rev, fps, real_fps, frame_gen_on, switches resolution |
| `switches [--file PATH]` | RPC status `switches` field, or local `nx2_switches.txt` | switches map (KEY→VALUE) |
| `bench [--seconds N] [--refresh-hz N]` | RPC `frame_times` | mean/p95/p99 intervals, fps, stutters, vsync shares |
| `screenshot [--out PATH] [--height N] [--inline]` | RPC `screenshot` | path, width, height, bytes (base64 only with `--inline`) |
| `launch --game PATH [--fullscreen] [--user 0-7] [--gdb-port N] [--wait-s N] [--log PATH]` | `guarana-cli -g` + `EDEN_NX2_DEBUG_SERVER` | pid, server, ready, status, log |
| `log-tail [--lines N]` | RPC `log_tail` | recent emulator log lines |
| `input --buttons A,B [--frames N]` | RPC `input` | input result |
| `rpc METHOD [--params JSON]` | ANY registered debug method | raw result |
| `warm-pipelines --title-id HEX [--timeout S]` | `guarana-cli --warm-pipelines` | title_id, pipelines_cached/built/already_built/left, elapsed_s, build_rate_per_s, outcome, exit_code |
| `nup-pack --dump DIR [--out FILE]` | `guarana-cli --pack-nup` | archive, elapsed_s, exit_code, stderr_tail |
| `nup-unpack --nup FILE [--out DIR]` | `guarana-cli --unpack-nup` | out_dir, exit_code, stderr_tail |
| `version` | `guarana-cli --version` | version (log spam filtered), raw |

Global flags: `--server HOST:PORT` (default `127.0.0.1:6545`, env `GUARANA_DEBUG_SERVER`),
`--cli PATH` (env `GUARANA_CLI`, else `C:/lb/build-1120/bin/guarana-cli.exe`,
`C:/lb/build-1118/bin/guarana-cli.exe`, PATH), `--timeout S`.

## Examples (Windows: native C:/ paths)

```bash
py -3.13 tools/cli-harness/guarana_harness.py --cli C:/lb/build-1120/bin/guarana-cli.exe version
py -3.13 tools/cli-harness/guarana_harness.py status
py -3.13 tools/cli-harness/guarana_harness.py bench --seconds 10 --refresh-hz 120
py -3.13 tools/cli-harness/guarana_harness.py screenshot --out C:/lb/shots/frame.png
py -3.13 tools/cli-harness/guarana_harness.py launch --game C:/games/oot.xci --wait-s 90
py -3.13 tools/cli-harness/guarana_harness.py warm-pipelines --title-id 0400486024DEC000
py -3.13 tools/cli-harness/guarana_harness.py rpc gpu_costs --params '{}'
```

## Debug RPC facts (verified, yuzu-lineage nx2_debug_server)

- Enable with `EDEN_NX2_DEBUG_SERVER=<port>` — the value is the **PORT** (e.g. `6545`), not `1`.
  The server only listens once emulation boots. `launch` sets this for you.
- TCP `127.0.0.1:<port>`, newline-delimited JSON, **one request per connection**:
  `{"id":1,"method":"...","params":{}}\n`. The harness retries connects with backoff.
- Registered methods (src/core/debugger/nx2_debug_tools.cpp): status, frame_times, screenshot,
  shown_frames, log_tail, log_filter, capture_frame, capture_bundle, reject_shaders,
  set_shading_rate, set_prepass, sample, trace_record/stop/status, speed_limit, host_timing,
  power, power_set, gpu_costs, gpu_perf, frame_budget, autopilot, memory, read_memory,
  search_memory, read_gpu_memory, input, input_sequence, input_latency, thread_stats,
  present_stats, cache_stats, readbacks, boot_timing. Reach any of them via `rpc METHOD`.
- `switches`, `stats`, `gpu_costs`, `real_fps` are FIELD names in responses, not callable
  methods (except gpu_costs which IS a method) — calling a field name returns `unknown method`.
- A refused connection usually means the process exited, not a wrong port. Check `log-tail`:
  a clean exit shows `SerializeVulkanPipelineCache` then `TerminateEmulationThread`.

## Measurement discipline (from emulator-perf-lab — do not skip)

- `status.fps` counts PRESENTS including generated frames; read `real_fps` / `frame_gen_on`
  separately when frame generation is on.
- Menu fps is not gameplay fps. Gate every bench sample on a screenshot: gameplay PNGs are
  ~2 MB, loading/black frames <300 KB. Use `screenshot` + vision before quoting numbers.
- Verify the binary's commit via `status.scm_rev`, not the build directory name.

## warm-pipelines exit semantics (src/lichii_cmd/lichii.cpp)

0 = done · 1 = failed (bad args / no Vulkan renderer) · 3 = run_again (last run ended while
building) · 4 = incomplete (stopped, or pipelines left for another pass). The harness maps
these to `data.outcome` and keeps the raw code in `data.exit_code`.

## Limitations (trial scope)

- Black-box wrapper: no emulator source changes; NUP metadata read-back (`nup inspect/edit`)
  is not wrapped yet — use `nup-unpack` + file reads for verification.
- `launch` detaches the emulator (Windows DETACHED_PROCESS flags) so it survives the calling
  agent turn; stop it via `rpc power_set` or the GUI, not by killing the harness.
- No REPL/stateful session yet (CLI-Anything generator phase deferred); each invocation is
  stateless, which suits cron/agent lanes.
