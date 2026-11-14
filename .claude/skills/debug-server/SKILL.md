---
name: debug-server
description: "Drive an emulator that is already running with EDEN_NX2_DEBUG_SERVER through the debug MCP adapter (tools/nx2/debug_mcp.py): status, screenshot, frame_times, gpu_costs, log_tail, input and input_sequence, reject_shaders A/B, capture_frame. Use when asked to look at, measure or A/B-test a running game. Never starts or stops the emulator."
---

# Live debug server

The emulator answers on `127.0.0.1:6545` when the owner starts it with
`EDEN_NX2_DEBUG_SERVER=127.0.0.1:6545` (`docs/nx2/debug-mcp.md` has every method and limit). Two ways in:

- **MCP**: `.mcp.json` registers `emulator-debug`
  (`python -X utf8 -I tools/nx2/debug_mcp.py --server 127.0.0.1:6545`), so its tools are
  `mcp__emulator-debug__<method>`. Claude Code asks once to approve the project's server.
- **Shell**: `python -I tools/nx2/debug_mcp.py --server 127.0.0.1:6545 --call '{"method":"status"}'`,
  plus `--out shot.png` for a screenshot. On Windows use the native Python: MSYS `python` rejects
  `C:/` paths.

The adapter offers exactly these methods: `status`, `frame_times`, `screenshot`, `log_tail`,
`log_filter`, `capture_frame`, `capture_image`, `reject_shaders`, `set_shading_rate`, `set_prepass`,
`sample`, `read_memory`, `search_memory`, `read_gpu_memory`, `input`, `input_sequence`, `host_timing`,
`power`, `power_set`, `gpu_costs`, `frame_budget`, `memory`. Anything else (`speed_limit`, `pause`,
`step_frames`, `shaders`, `switches`) fails with "unknown tool", through MCP and `--call` alike.

## Rules

1. **Never start, stop, restart or kill the emulator**, and leave its settings, saves and shortcuts
   alone unless the owner asks. "nothing listens on 127.0.0.1:6545" means it is not running: ask the
   owner to launch the game with the variable set, then wait.
2. **One call at a time, each bounded.** `frame_times` 5 to 30 s (60 at most), `sample` 15 s at
   most, `search_memory` once within its `budget_ms`, `capture_frame` with its default timeout, input
   sequences of seconds. No polling or capture loops while the owner plays; `sampled` and `buffers`
   only when you need the pass inputs.
3. **Put back everything you change before you finish, also after an error.** Every
   `reject_shaders` gets its `{"hashes": [...], "reject": false}`, and a final `reject_shaders {}`
   lists none of yours. `set_shading_rate` back to `1x1` (and `auto: 0` if you started it),
   `set_prepass` with `on: false`, `power_set`, `frame_budget` and `log_filter` back to the `previous`
   values of their first reply, `gpu_costs {"enable": false}` and `host_timing {"stats": false}` if
   you turned them on.
4. **Full 16-hex-digit hashes** only; shorter ones are refused.
5. **Input adds to the owner's.** Use `input` and `input_sequence` only when the owner has put the
   controller down, and say first what you will press.
6. **Captures, screenshots and memory reads are game data**: they stay on the owner's machine
   (AGENTS.md, "Never commit").

## Recipes

- What is on screen: `status`, then `screenshot {"height": 720}`.
- Frame rate: `frame_times {"seconds": 10, "refresh_hz": 60}` gives fps, p50/p90/p99 ms, VSync
  shares and stutters. Take three, report the median, and use the same window for every variant.
- Costliest shaders: `gpu_costs {"enable": true}`, wait two seconds, `gpu_costs {"top": 16}`
  (`work` per shader pair, `passes` per render target), then `gpu_costs {"enable": false}`.
- Shader A/B or bisection: `screenshot`, `reject_shaders {"hashes": ["<16 hex digits>"]}` (wait for
  `applied`), `screenshot` and `frame_times`, then restore. Halve the list at each step.
- The same camera path for every variant: one `input_sequence` from the same spot, for example
  `{"steps": [{"right_stick": [1, 0], "frames": 60}, {"frames": 30}]}`.
- Logs: `log_tail {"contains": "Nx2Stutter", "limit": 200}`, then page with `after` set to the
  reply's `next`. `log_filter` affects later lines only.
- One frame's passes: `capture_frame {"index": true}`, then `capture_image` with its `directory`
  and an entry `name` for that pass's PNG.
- Power and memory: `power {"window_ms": 1000}`, `memory`.

Live numbers guide the work; a pull request's result comes from `tools/bench` rows (the `bench` and
`perf-change` skills). Report each number with what produced it (method, parameters, variant, scene),
and say that the state is restored.
