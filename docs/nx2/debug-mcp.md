# Debug server and MCP adapter

Agents debug games through a slow loop today: write env flags, ask the owner's PC to build a
package, wait for the owner to launch the game, press a hotkey and send the log back, then copy
dumps. Every new question costs a relaunch, and most cost a rebuild, because every
`EDEN_NX2_*` switch is read once at startup.

This document designs an opt-in debug server inside Guarana and an MCP adapter that agents run on
the PC, so an agent can ask a running game questions directly. Phase 1 is implemented on the
`feat/debug-mcp` branch; the later phases are proposals with the hook points they need.

Line numbers refer to `feat/debug-mcp` at the time of writing.

## Goals and constraints

- The owner's PC (RTX 3080, Windows) runs the game. Agents reach it through bash on a paired
  device: they can run programs and read files there, but cannot see the screen or press keys.
- Game files, keys and firmware never leave that PC, and the server must not give a way to read
  them.
- A normal run must not change: the server is off unless `EDEN_NX2_DEBUG_SERVER` is set, and
  while it is off the only cost is one relaxed atomic load per presented frame.
- No relaunch for a new question, and as few rebuilds as possible for a new tool.

## Architecture

```
agent ──stdio MCP──> tools/nx2/debug_mcp.py ──TCP 127.0.0.1, JSON lines──> Guarana
  (on the PC)          (Python, stdlib only)                               ├─ "DebugServer" thread
                                                                           │    tools: status, screenshot, log_*, ...
                                                                           └─ rasterizer thread
                                                                                takes requests at each frame end
```

### In-process server

`src/core/debugger/nx2_debug_server.cpp` is the transport. It listens on a loopback TCP port
with Boost.Asio, as the GDB stub does (`src/core/debugger/debugger.cpp:147`), and runs one
`io_context` on its own thread named `DebugServer`. Each line a client sends is one request,
`{"id":1,"method":"status","params":{}}`; each answer is one line,
`{"id":1,"result":{...}}` or `{"id":1,"error":"..."}`. The transport knows nothing about the
emulator: it maps method names to handlers. That keeps it unit-testable without a `System`.

`src/core/debugger/nx2_debug_tools.cpp` holds the tools. `Core::System::Impl::Load` starts the
service once a game has loaded (`src/core/core.cpp:476`), and `ShutdownMainProcess` stops it
before anything else (`src/core/core.cpp:481`). The member is declared last
(`src/core/core.cpp:610`), so it is also destroyed first. The tools therefore never see a
half-built or half-destroyed system, and the server works the same under the Qt frontend and
`guarana-cli`.

Why not reuse the GDB stub? It speaks the GDB remote protocol and parks emulation on every stop
(`src/core/debugger/debugger.cpp:82`). It is the right tool for guest breakpoints and stays as it
is (`tools/nx2/gdbrsp.py` drives it), but it has no place for frames, shaders or logs. The new
server copies its threading model, not its protocol.

### Why JSON lines over TCP, with a separate adapter

- MCP's own transports are stdio, which needs the agent to launch the process (it cannot launch
  the emulator, which the owner starts), and Streamable HTTP, which needs sessions and
  server-sent events. Neither fits inside the emulator cheaply.
- A JSON-lines socket is about 200 lines of C++ with libraries Guarana already links (Boost.Asio,
  nlohmann::json, both already used by `core`). Anything can drive it: the adapter, a test,
  or `python -c`.
- The adapter owns everything that is MCP: tool descriptions, input schemas, image content, error
  wording. Those change often, and changing them needs no emulator rebuild. That is the point of
  the whole exercise.
- Loopback TCP works the same on Windows and Linux. Named pipes or Unix sockets would need two
  code paths.

### Threading

Three kinds of work, three places:

1. **Server thread.** Parses requests and runs handlers. Handlers that only read thread-safe
   state run here directly: status, log tail, log filter, and the guest memory reads
   (`read_memory`, `search_memory`; see "Guest and GPU memory" for why that is safe). Handlers run one at a time, so a slow
   request (a capture can take seconds) delays the next one. One agent at a time is the expected
   use.
2. **Rasterizer thread.** Everything that touches the texture cache, pipeline cache, scheduler or
   frame dump must run on the thread that owns them. With the GPU thread split
   (`src/video_core/gpu_split.h:60`), that is the back thread, which also runs
   `RendererVulkan::Composite` and therefore `rasterizer.TickFrame()`
   (`src/video_core/renderer_vulkan/renderer_vulkan.cpp:269`). Requests for it go through
   `src/video_core/nx2_debug_control.h`, a small mutex-guarded block with one atomic "something
   is pending" flag. `RasterizerVulkan::TickFrame` (`src/video_core/renderer_vulkan/vk_rasterizer.cpp:1905`)
   checks the flag and takes the requests at the end of each presented frame. Between frames no
   draw is half-recorded and no pipeline is in use by the caller, so dropping cache entries there
   is safe.
3. **Existing renderer hooks.** The screenshot uses `RendererBase::RequestScreenshot`
   (`src/video_core/renderer_base.cpp:38`), the path the Qt screenshot hotkey uses
   (`src/lichii/bootmanager.cpp:819`). It is already safe to call from another thread, and the
   renderer fills the buffer at the next present (`renderer_vulkan.cpp:350`).

The server thread waits for rasterizer-thread work with a timeout (5 s for screenshots and
rejections, 120 s by default for a capture). If the game is paused or loading, no frame is
presented and the tool fails with "no frame was presented within N s (paused or loading?)"
instead of hanging. A withdrawn capture request is removed from the queue. On shutdown the
service ends the session first, which wakes every waiter, then stops the `io_context`, so
shutdown waits at most about 50 ms for a handler.

Cost when unused: if `EDEN_NX2_DEBUG_SERVER` is unset, `MaybeStartService` returns at its first
`getenv`. No thread, no socket, no log ring. The per-frame check is one relaxed atomic load,
and each guest present stores a steady-clock timestamp in the 4096-entry ring that
`frame_times` and the bug bundle read. The runtime reject check in the lowering is one relaxed atomic load per lowered program. When the
server is on but idle, its thread sleeps in `epoll`/IOCP.

## Safety

- **Loopback only.** `ParseEndpoint` accepts `127.0.0.1`, `::1` and `localhost` and refuses
  every other host, `0.0.0.0` included. The server is reachable only from the PC itself, where
  the agent already has a shell.
- **Off by default.** It needs `EDEN_NX2_DEBUG_SERVER`, which the owner's shortcut sets for a
  debug build. Release packages are unchanged unless someone sets it.
- **Writes only under the dump directory.** `EDEN_NX2_DEBUG_DIR`, by default
  `<log dir>/debug-server`. Clients never send paths. The server names every file itself:
  `capture-<session>-<n>/`, `screenshot-<session>-<n>.png`, `shown-<session>-<n>.png`.
- **Nothing the server returns comes from game files, keys or firmware.** It returns rendered
  pixels, log lines, frame-dump manifests and shader hashes, which the owner already sends today.
  The memory tools read only the application's guest virtual memory and GPU virtual memory,
  through their page tables, and never write either. They cannot reach host memory, the key area
  or the file system: an address resolves only through a page the guest has mapped. The adapter's `capture_image` reads only PNGs inside
  capture directories that it created in this session, and checks for path escapes.
- **Logs.** `log_tail` returns lines that already go to `eden_log.txt`.
- **No authentication.** Any local process may connect, the same as with the GDB stub. That is
  acceptable on the owner's single-user PC with the switch off by default. If it ever runs on a
  shared machine, add a token file in the dump directory that the adapter reads.

## Tool surface, by how much each cuts the loop

"Today" is the cost of answering the question without the server.

| Tool | Today | With the server | Phase |
|---|---|---|---|
| `status` | Ask the owner what is on screen, read the log header | One call: title, program id, build, paused, presents, fps, renderer, rejected hashes | 1 |
| `frame_times` | Frame trace and manual interval analysis | Guest-present interval statistics and VSync histogram over a timed window | 1 |
| `screenshot` | Owner takes and sends a screenshot | PNG of the next presented frame, as MCP image content | 1 |
| `shown_frames` | Nobody sees generated frames; the owner describes the motion | The next frames the presenter shows, generated ones included: kind, slot, the vblank it was due at and when the present call returned, and one contact sheet (done) | 3 |
| `log_tail`, `log_filter` | Relaunch with a new filter, owner sends the log | Read new lines since the last call, change the filter live | 1 |
| `capture_frame` | Relaunch with `EDEN_NX2_FRAME_DUMP`, owner touches `capture.now`, agent copies the dump | Capture the next frame now, sampled textures and bound buffers optional, get the manifest summary back; `capture_image` returns any pass PNG | 1 |
| `capture_bundle` | Owner zips logs, settings and screenshots by hand | The Report a Bug bundle: logs, settings, screenshot, frame times and a frame capture with shaders, in one zip | 1 |
| `reject_shaders` | Rebuild the env, relaunch, replay to the spot, once per bisection step | Reject or restore hashes live; bisection becomes a loop of reject, screenshot | 1 |
| `pause`, `resume`, `step_frames` | Not possible | Freeze on the broken frame, capture it, step one present | 2 |
| `input`, `input_sequence` (buttons, sticks, frame-timed sequences) | Owner plays to the spot | Agent walks to the spot itself and replays a repro (done) | 2 |
| `shaders` (inventory) | Shader dump and offline sweep | List loaded programs: key, hash, stage, address, lowered or rejected with the error | 2 |
| `switches` (live `EDEN_NX2_*`) | Relaunch per switch | Toggle the observer switches live (classification below) | 2 |
| `read_memory`, `search_memory`, `read_gpu_memory` | GDB stub session or log prints plus a rebuild | Read a range by address, find known values in the heap (done) | 3 |
| `dump_texture`, `dump_buffer` | Frame dump of the whole frame | One resource by GPU address, as PNG plus raw | 3 |
| `gpu_timings` | `EDEN_NX2_GPU_PROFILE` relaunch, zones only | Per-pass GPU time of one frame | 3 |
| Capture options (pass range, shader filter) | Not possible | Smaller captures, for example only passes 40 to 60 | 3 |
| Save and load state | Not possible | Not possible: see "Save states" | none |

### Phase 1 (implemented)

**`status`** `{fps_window_ms?}`. It reads the title and program id that `Load` saw, the build from
`Common::g_build_fullname` and `g_scm_rev`, and `System::IsPaused`. It measures fps from
`Core::g_nx2_present_count` (`src/core/core.cpp:78`) over a short window. It does not call
`GetAndResetPerfStats`, which would reset the Qt status bar's counters.

**`frame_times`** `{seconds?, refresh_hz?}` waits for `seconds` (default 5, > 0, at most 60),
interruptible on shutdown like `status`. It measures intervals between guest presents inside
that window, not display scanout or GPU execution time. `frames` counts those intervals;
`mean_ms`, `fps` (1000 / mean), `p50_ms`, `p90_ms`, `p99_ms`, `min_ms`, `max_ms` describe them.
Percentiles interpolate between sorted samples. `vsync_buckets` gives fractions (0 to 1) under
keys `1`, `2`, `3`, `4+`, using boundaries 1.5, 2.5 and 3.5 refresh intervals (`refresh_hz`
defaults to 60 and must be positive). `stutters` counts intervals strictly above twice the
median. `intervals_ms` contains the last up to 120 intervals in chronological order, rounded
to 0.1 ms. With fewer than two presents, statistics are zero and the interval list is empty.
Only available samples from the 4096-timestamp ring are used; overflow sets `truncated: true`.
For example: `{"method":"frame_times","params":{"seconds":5,"refresh_hz":60}}`.

**`screenshot`** `{height?, save?, timeout_s?}`. It requests a 16:9 layout from
`Layout::DefaultFrameLayout`, and the renderer blits the presented framebuffers into it. The
BGRA result is turned into RGB and encoded as PNG with stb, which `common` already compiles. The
pixel buffer is owned by the completion callback, so a request that times out cannot leave the
renderer writing into freed memory. If the Qt screenshot hotkey has a request pending, the tool
fails instead of taking over the hotkey's request.

**`log_tail`** `{after?, limit?, contains?}`. A `RingBackend` in `src/common/logging.cpp:308`
keeps the last 20,000 formatted lines. It is an ordinary backend that `FmtLogMessageImpl` already
fans out to (`logging.cpp:488`), and it does nothing while its capacity is 0, which is the
default. The service turns it on at boot. A call without `after` returns the newest lines.
`next` pages forward, so an agent can poll "everything since my last look".

**`log_filter`** `{filter}`. It parses the filter with `Filter::ParseFilterString` and applies
it with `SetGlobalFilter` (`logging.cpp:449`), the call the Qt settings dialog makes. Lines
already filtered out were never formatted, so the new filter applies only to later lines.

**`capture_frame`** `{sampled?, buffers?, timeout_s?, manifest_lines?, index?}`. It reuses
`FrameDump`. `FrameDump::Arm` (`vk_frame_dump.cpp:268`) points the next capture at a fresh directory and resets
the per-capture counters, so `EDEN_NX2_FRAME_DUMP` is not needed. When to capture is decided by
`FrameDumpSchedule`, which has no Vulkan state and is unit-tested. It handles both the env
triggers and the debug server's requests. It also sets `RecordingSampledTextures`, which the
graphics and compute pipelines check before they keep the textures each draw and dispatch
samples. The pipelines used to read `EDEN_NX2_FRAME_DUMP_SAMPLED` once at startup, so a sampled
request at runtime found no sampled textures to read back. Now the pipelines keep them only during
a sampled capture, under either trigger. With `buffers: true` the capture also records the uniform
and storage buffers the buffer cache bound for every draw and dispatch (`kind: buffer`, with stage,
binding, GPU address and size; bytes deduplicated in `buffer_<hash>.bin.zst`, copied on the GPU
when the GPU wrote the range) and each dispatch's group counts (`kind: launch`). This is the
buffer part of fix/oot-fog's frame dump (1ad14bebb8). `EDEN_NX2_FRAME_DUMP_SAMPLED` turns on both
textures and buffers. At runtime they are separate, so diffing a constant buffer such as a fog
`c5` across two captures does not pay for every texture readback. `FrameDump::TickFrame`
(`vk_frame_dump.cpp:849`) starts the capture exactly as `capture.now` would. The writer thread
reports completion after it writes the manifest summary (`WriterLoop`, `vk_frame_dump.cpp:710`). The tool returns the directory, the
frame and summary lines, counts per entry kind, the first entries, and the `frame_dump_index.py`
command. With `index: true`, the adapter runs that command itself. The adapter-only
`capture_image {directory, name}` returns one pass PNG as image content.

**`sample`** `{threads?, seconds?, max_chars?}`. The in-process sampling profiler
(`src/common/nx2_sampler.cpp`, also `EDEN_NX2_SAMPLE` at startup) on demand: it pauses the named
threads (default `GPU,GPUBack`) about 1000 times a second for `seconds` (default 10) and returns
the self, self-and-caller and inclusive counts per function. The report and its folded stacks go
to `samples-<session>-<n>.txt` in the dump directory. Windows only; the threads run slower while
sampled, so read fps from a run without it. A stack ends at its first frame in generated code,
shown as `(jit)`: a guest core thread (`CPUCore_*`) shows `(jit)` while it runs guest code and
the host functions above `(jit)` while the JIT calls out.

**`speed_limit`** `{on?}`. Lifts (`false`) or restores (`true`) the speed limit live and returns
the `previous` and current `on` state, so a test script can boot through the logos and menus
unlimited. Without `on` it only reports the state. The MCP adapter offers it, and `--call` reaches
it, as every other method.

**`host_timing`** `{stats?}`. Turns the per-second `HostTiming wait` log line
(`EDEN_NX2_HOSTTIMING_STATS`) on or off and returns the `previous` and current `stats` state.
Read the lines with `log_tail` (`contains: "HostTiming wait"`).

**`power`** `{window_ms?}`. Host power from the Windows Energy Meter Interface, averaged over
`window_ms` (default 1000; 0 means since the previous call): `package_w`, `cpu_w` (cores, the
`PP0` channel) and `gpu_w` (integrated GPU, `PP1`), with `valid` false when nothing was measured
and `available` false when the host has no such meter. Also `process_cpu_cores` (the emulator's
CPU time per wall time), `gpu_busy_ms` of the last sampled frame (null when not measured) and
the switches below. `policy` and `governor` report the session power policy.

`EDEN_NX2_POWER_POLICY=igpu` (Windows, integrated GPUs only, off by default) trades CPU power
for GPU clock on a shared package power limit. At game boot it saves the active power scheme's
processor performance boost mode and minimum processor state (both core classes, AC and DC),
writes boost off and a 5% minimum, and writes the saved values back at game stop or exit. The
saved values sit in `nx2_power_restore.txt` in the config directory meanwhile, so the next start
restores them after a crash. `policy` has `active`, the `scheme`, each setting's `saved_*` and
`applied_*` values and a `message` (why it is not active: a host that refuses the calls is left
alone). While it runs, a governor samples once a second and steps `spin_scale` down
(1 -> 0.5 -> 0.25 -> 0) when the GPU is busy over 90% of a frame that is slower than the target
and the cores take over half the package power, and back up when the GPU is busy under 70%.
`governor` has its `state` (`gpu_bound`, `cpu_bound`, `balanced`, `no_data`), inputs and
`last_change`. The target is `EDEN_NX2_POWER_POLICY_FPS`, else the `EDEN_NX2_TARGET_FPS` target,
else the `EDEN_NX2_VRS_AUTO` target, else 30.

**`power_set`** `{spin_scale?, eco_background?, idle_spin_us?}`. Sets the power switches live
and returns all three. `spin_scale` (0 to 1, `EDEN_NX2_SPIN_SCALE`) scales the busy-wait bounds
of the GPU front and back threads; 0 blocks at once. `eco_background`
(`EDEN_NX2_ECO_BACKGROUND=1`) puts the background workers (shader jobs, pipeline serialization,
texture decoder, ZeroTrim, ColdHint) on EcoQoS on Windows; it only reaches threads that start
afterwards, so set the variable to cover a whole session. `idle_spin_us`
(`EDEN_NX2_IDLE_SPIN_US`) is how long an idle guest core polls before blocking, -1 for the
per-title default.

**`frame_budget`** `{target_fps?, scale?}`. The frame budget controller
(`EDEN_NX2_TARGET_FPS=<fps>` at launch, off without it; not compiled or run yet). It holds a
Switch 2 title that sizes its own dynamic resolution from GPU time (Ocarina of Time without the
Max Resolution add-on) at a frame rate, in two parts:

- Cadence. The nx2:vi2 present pacer (`PaceToVblank`, the one behind the FPS limit) holds every
  guest present until the last deadline plus `1 / target`, so a frame that is done early still
  takes the whole interval. A frame that is late is not held, and the pacer starts over from a
  present more than one interval late.
- Resolution. Every half second of presents is one tick. Its input is `work_ms`: the time from
  one present's return to the next present's pacing wait, the frame without that wait, as four
  frames in five of the tick stay under (the 80th percentile: a median would pass a tick with
  two late frames in five; `frame_ms` is the median present interval). Above `target * 1.04`
  the GPU time scale (`EDEN_NX2_GPU_TIME_SCALE`, `AmpereTimeSincePresent`) is multiplied by
  1.25, at every such tick: the game measures more GPU time and renders lower. Below
  `target * 0.90` for four ticks in a row it is multiplied by 0.92: the game renders higher.
  Between the two nothing moves.

A game that starts its frames on the nx2:vi2 vsync event waits for the next event after a held
present returns, and that wait counts as frame time: with the event at 60 Hz up to 16.7 ms of
headroom can stay hidden (`work_ms` near `frame_ms` although the GPU idles), and the scale is
then lowered later than it could be. A faster event (`nx2_frame_rate_limit` 120 or 240, for a
game that keeps its speed there) shortens the wait.

The scale stays between 0.05 and the smaller of 4 and the GPU Clocks divisor (Normal 1, Boost
4, Overclock 16): the time a title sees never exceeds the real time, so a larger scale changes
nothing. The scale is latched with each present (`AmpereAnchor`), so a change never moves a
timestamp back within a frame. `state` is what the last tick found:

| `state` | Meaning |
|---|---|
| `off` | No target, or no tick yet |
| `frozen` | Fewer than three presents in the tick, or frames over four targets long (a loading screen): nothing moves |
| `on_target` | Inside the band |
| `slow` | A miss: the scale was raised |
| `saturated` | A miss with the scale at its upper limit: the game has no resolution left to give |
| `fast` | Headroom: counting towards a lower, just lowered, or at the lowest scale |
| `cooldown` | Headroom, but the next lower scale is below the one that last missed |
| `no_effect` | Headroom, but eight lowers in a row left the frame time alone (the game is at its highest resolution, or does not follow the scale); they go on once frames are 8% slower |
| `waiting` | Headroom, but the shading-rate governor gives its coarse rates back first |
| `unanswered` | With `EDEN_NX2_BUDGET_UNANSWERED=1`: a miss, but nine missed ticks in a row (about 4.5 s; raises, or holds at the upper limit after one) left `work_ms` no 12% faster, three ticks in a row, than the fastest of the run's first three: the game does not follow the scale (a patch fixes its resolution, or the frame is not bound by it), so the scale is held. `unanswered_from` is the scale at the start of the run, or at its last answer. A tick without a miss, while the shading-rate governor holds no coarse rates and not under `EDEN_NX2_BUDGET_HOLD_RES`, lets the scale be tried again; under `EDEN_NX2_BUDGET_HOLD_RES` the scale stays held until the target changes |

The scale at which a run of misses began is `miss_scale`: for `cooldown_ticks` more ticks
(30, doubled up to 240 each time a lower made since brings a miss back) a lower stays one step
above it, so the scale does not go back and forth over a resolution step. It is forgotten after
two cool-downs without a miss.

With `EDEN_NX2_VRS_AUTO` on, the shading-rate governor takes the same target and its frame time
without the pacing wait. It may coarsen only once the scale has nothing left to give, from a
miss at the scale's upper limit until the scale leaves that limit or from `unanswered` until the
scale may be tried again (`shading_rate_allowed`; `set_shading_rate` shows `auto.allowed` and the
hold reason), and while it holds coarse rates the scale is not lowered.

`target_fps` sets the target live (1 to 1000; 0 turns the controller off and puts the scale back
to `manual_scale`). `scale` sets the scale by hand; with a target the controller goes on from
it. The reply also has `target_ms`, `samples` (presents of the last tick), `scale`,
`latched_scale` (the scale as the anchor keeps it, within 0.8%), `min_scale`, `max_scale`,
`ticks`, `raises`, `lowers`, `fast_streak`, `blind_lowers`, `last_change` and `unanswered_from`
(0 outside state `unanswered`). A change of the
scale by more than 10% logs one `Frame budget:` line, at most one a second.

**`autopilot`**. The autopilot (`EDEN_NX2_AUTOPILOT=1`, [../autopilot.md](../autopilot.md)), answered at once from its last 2 s window, also under `status.autopilot`: `running`, `windows`, `changes`, `real_fps`, `shown_p50_ms`, `shown_p95_ms`, `shown_even_percent`, `refresh_hz` and its `refresh_source`, `gpu_busy` (negative when not measured), `power` (`ac`, `battery`, `unknown`), `frame_gen_on`, `frame_gen_target`, `frame_gen_multiplier`, `gpu_time_scale` and its base, `resolution_available` (false while the frame budget has a target), `power_policy_available`, `power_policy_applied` and `last_change`. Without the switch `running` is false.

**`gpu_costs`** `{top?, enable?}`. Host GPU time of the last sampled frame (one a second, the
`DKB gpu_work` and `DKB gpu_pass` data without the log lines): `work` per vertex+fragment or
compute shader hash and `passes` per render target, slowest first, `top` of each (default 32).
`enable` turns the sampling on or off (`EDEN_NX2_COST_SAMPLING=1` starts with it on); `frame` is
0 until a frame was sampled, `age_ms` says how old the sample is. Each `work` entry also counts
its `small_draws` (direct draws of at most 6 vertices and one instance) and `side_draws` (draws
into another target than the main scene target: the one with depth and colour whose draws took
the most time, `has_main_target`).
What the driver's compiler made of those shaders is a launch option, not a tool:
`EDEN_NX2_PIPELINE_STATS=<dir>` enables VK_KHR_pipeline_executable_properties, builds every guest
graphics and compute pipeline with `VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR` and appends one
JSON line per build to `<dir>/pipeline_stats.jsonl`: `kind` (`graphics` or `compute`), `pipeline`
(the key's hash), `hashes` (the six stage hashes, or the compute hash), `shader` (fragment or
compute hash), `mesh`, `flags` (the create flags) and `executables`, each with `index`, `name`,
`description`, `stages` (a `VkShaderStageFlags` mask), `subgroup_size` and `statistics` (`name`,
`description`, `format`, `value`). `EDEN_NX2_PIPELINE_STATS_IR=1` adds the capture of internal
representations: each executable gets `representations` (`name`, `description`, `text`, `bytes`,
`file`) and the text ones are written as `<dir>/<shader>_<index>_<name>.txt`, once per file name.
Such a session leaves the driver blob store closed, passes no driver pipeline cache to its builds
and marks nothing as precached, so it compiles everything again and its frame times mean nothing;
the shader cache's own records are used and written as usual. Save a `gpu_costs` reply from a
normal session, then `python3 -I tools/nx2/pipeline_stats.py <dir> --costs <reply.json>` (or a log
with `DKB gpu_work` lines) prints the executables of the costliest shaders, with the statistics
named after spills, fills, scratch space, SIMD or dispatch width, instructions, sends, cycles and
loops marked.
**`gpu_perf`** `{enable?, frames?}`. Hardware GPU counters through VK_KHR_performance_query
(`EDEN_NX2_GPU_PERF=1` at launch enables the extension; without it the reply is
`{supported: false, reason}`). `enable` samples the next `frames` submissions (default 30,
1 to 600) or stops (`false`); the exclusive profiling lock is acquired for the window only
and released once its last result is read, so another profiler can use the GPU between
windows. The reply has `supported`, `sampling`, `frames_sampled`, `counters`
(`[{name, value, unit}]`, averaged over the submissions collected so far; units `pct`, `mhz`,
`cycles`, `ns`, `raw`, ...) and the shortcuts `gpu_busy_pct` (GpuBusy), `stall_pct` (XveStall),
`occupancy_pct` (XveThreadOccupancy) and `freq_mhz` (AvgGpuCoreFrequencyMHz), each null until
a result was read. The whitelist also carries RenderBusy and ComputeEngine0Busy; counters the
driver does not expose are skipped. Call again without `enable` to poll a running window.
This turns "unaccounted frame time" into a named GPU stall: a frame whose GPU time is mostly
`stall_pct` waits on memory or the sampler, not on shader work.

**`set_shading_rate`** `{hashes?, rate?, all?}`. With `EDEN_NX2_VRS` set at launch, every guest
graphics pipeline takes its fragment shading rate (VK_KHR_fragment_shading_rate) as dynamic
state. This sets the fragment size (`1x1`, `2x1`, `1x2`, `2x2`, `4x2`, `2x4`, `4x4`; `2x2` when
omitted) for the draws of the listed fragment shader hashes, from the next draw on, or with
`all: true` for every shader not listed. `1x1` restores. Without hashes it returns the table.
`EDEN_NX2_VRS_HASHES=<hash>:<rate>,...` and `EDEN_NX2_VRS_DEFAULT=<rate>` set the same table at
boot. Rates are clamped to the device's.

`auto: <fps>` starts the automatic governor and `auto: 0` stops it (`EDEN_NX2_VRS_AUTO=<fps>` at
launch, with `EDEN_NX2_VRS`). It turns cost sampling on and, on each sampled frame, compares the
median guest present interval of the last second (frames the host generates are not counted)
with the target: after two slow samples the costliest fragment shader goes one level coarser
(`2x1`, `2x2`; `EDEN_NX2_VRS_AUTO_MAX=3` allows `4x4`), after four fast ones the last raise is
taken back. Its table is a layer under the manual one: a hash in `rates` or a non-`1x1` default
always wins, and the governor never touches them. It leaves alone fragment shaders that write
depth, that only draw small direct draws (full-screen and UI quads), that only draw into side
targets, and the hashes of `auto_deny` (`EDEN_NX2_VRS_AUTO_DENY=<hash>,...`; `auto_allow`
removes them). The reply's `auto` has the target, the measured `frame_ms`, the layer (`rates`,
`levels`), the `denied` lists, `last_action` and `hold_reason`. A raise or lower logs one
`VRS auto:` line.
**`set_prepass`** `{hashes?, on?}`. Experiment (`EDEN_NX2_PREPASS_HASHES=<hash>,...` at boot):
the draws of the listed fragment shader hashes are drawn twice, first with a depth-only form of
the fragment shader that keeps only what decides its discards, then with the fragment shader
itself, early fragment tests on, depth compared for equality and not written. The shader then
runs once for each visible pixel instead of once for each layer of alpha-tested geometry. A
pipeline takes the split when its fragment shader discards (or the alpha test is on), writes no
depth, sample mask or storage, no earlier stage writes storage, and there is a depth buffer, no
forced early depth test, transform feedback, alpha to coverage or alpha to one. A draw is split
when depth test and write are on with a less, less-equal, greater or greater-equal compare, and
stencil test, blending of every bound colour target, logic op, transform feedback and the
samples-passed counter are off. `Prepass split:` log lines name the pipelines refused and why,
and the state of the first draws refused. `EDEN_NX2_PREPASS_FORCE=1` at launch takes the split
with an earlier stage that writes storage (it then writes twice) and with the counter on (it
counts the visible fragments once more). `on: false` takes the hashes off the list, from the
next draw on; listing some drops their built pipelines at the next frame end, to be made again
with the split when next drawn (pipelines still building then, or prebuilt and not yet drawn,
keep drawing in one pass: take the hash off and list it again). Without hashes it returns the
list. The reply counts
the `pipelines` built with the split and their `split_draws` and `plain_draws` (state refused).
Both passes use the same shader modules before the fragment stage; should their depths still
differ (holes in the geometry), `EDEN_NX2_PREPASS_INVARIANT=1` at launch decorates Position as
invariant in those pipelines.
**`memory`**. Host memory in bytes. `process`: working set, private working set, commit and their
peaks. `system`: commit against its limit, RAM and free RAM. `guest`: the guest memory's size,
what stayed resident after the last zero-page trim, zero pages trimmed and zero-filled so far, and
bytes committed on demand (`EDEN_NX2_LAZY_GUEST_COMMIT=1`). `renderer`: device memory in use
against the texture cache thresholds, buffer cache, staging buffers, mesh arenas, graphics
pipelines (entries, built, kept limit) and kept SPIR-V; pipeline figures refresh every 600 frames.
`low_memory_mode` says whether `EDEN_NX2_LOW_MEMORY` or the host's RAM (20 GiB or less) turned
the smaller cache limits on.
**`capture_bundle`** `{description?, size_limit_mib?, timeout_s?}` (`index?` in the adapter). It
writes the bug-report bundle that Help → Report a Bug writes ([bug-bundle.md](bug-bundle.md)):
logs, system and settings, screenshot, the last 10 s of frame times, and the next frame captured
with its shaders, SPIR-V and pipeline keys, zipped under `size_limit_mib` (default 19, 1 to 100)
into `<log dir>/bug-reports`. It waits up to `timeout_s` (default 90) for the frame capture and
returns `path`, `bytes`, `dropped`, `frame_error`, `manifest` (the bundle's `bundle.json`) and
`index_command`. In the adapter, `index` runs `bundle_index.py` on the zip and returns its
`index.html`.

**`reject_shaders`** `{hashes?, reject?, timeout_s?}`. This is `EDEN_NX2_REJECT_HASH` at
runtime: the same keys (native program key or shader hash) and the same effect. Pipeline
invalidation turned out to be tractable:

1. The lowering checks the runtime list right after the env list
   (`src/video_core/shader_environment.cpp:642` and the block after it). A rejected program
   throws the same `Shader::RuntimeError`, which the existing rejection path handles: the draw is
   skipped, the error is logged once, and the address is remembered.
2. `ShaderCache::RefreshDebugRejections` (`src/video_core/shader_cache.cpp:67`) runs on the
   rasterizer thread. It finds cached `ShaderInfo`s of lowered native programs whose
   `unique_hash` or `native_program_key` is on the list. It drops them through the same
   `InvalidatePagesInRegion` and `RemovePendingShaders` path (`shader_cache.cpp:369`, `:390`)
   that guest writes to shader memory take. For native code `unique_hash` is the shader hash
   (`GenericEnvironment::Analyze` returns `NativeShaderHash(code)`).
3. It sets `Dirty::Shaders` on every active channel. `RefreshStages` returns early while that
   flag is clear (`shader_cache.cpp:120`), and its `shader_infos` would still point at the
   dropped entries. With the flag set, the next draw looks the program up again, lowers it again,
   and hits the rejection. Compute looks up its program on every dispatch
   (`ComputeShader`, `shader_cache.cpp:254`), so it needs no flag.
4. After an un-reject, every remembered rejection (`rejected_native`) is cleared, so programs
   rejected earlier are lowered again. Programs that really fail are rejected again and logged
   once. The lowered result is identical, so the old pipeline still sits in the pipeline cache
   under the same key and is reused: restoring costs nothing.

The tool waits until the rasterizer applies the change and reports `applied`. A bisection is a
loop: reject half of the candidates, screenshot, compare. Today each step is a rebuild of the
environment plus a relaunch plus a replay.

### Guest and GPU memory (implemented)

All three are read only. Reads return at most 64 KiB. Addresses are hex strings (`"0x8012345000"`)
or JSON numbers, and results give them back as hex strings.

**`read_memory`** `{address, size, as?}` reads the application process's guest virtual memory.
`as` is `hex` (default, one string), `u32`, `u64` (hex strings, since JSON numbers lose bits
past 2^53) or `f32` (`"nan"`, `"inf"` and `"-inf"` for non-finite values). If any page of the
range is unmapped, the call fails and names the first unmapped address.

**`search_memory`** `{pattern, start?, end?, max_hits?, regions?, budget_ms?}` finds a byte
pattern (hex pairs in memory order, so little endian: `1.0f` is `0000803f`; up to 256 bytes) in
mapped guest memory. `regions` is `heap` (default, the heap region from the process page table)
or `all` (the whole address space); `start` and `end` narrow it. Matches may cross page
boundaries but not unmapped gaps. It returns at most `max_hits` (default 64, at most 256)
addresses. It stops after `budget_ms` (default 2000, at most 10000) and then returns
`complete: false` and `next`. Pass `next` as `start` to continue without missing or repeating a
match. The intended use is to find a known float tuple, such as the fog layer constants a
capture shows in `c5`, and from there the time-of-day or dynamic-resolution state. A heap scan
copies every mapped page once, so it keeps one host core busy for up to the budget: run it once,
not in a loop, while the owner plays.

**`read_gpu_memory`** `{gpu_address, size, as?, timeout_s?}` reads GPU virtual memory, for
example query report and semaphore buffers that the game reads back. It runs on the rasterizer
thread at the next frame end, like a capture, so it fails with "no frame was presented" while
the game is paused or loading. It tries the bound channel's address space first, then every
other address space the rasterizer knows, and reads from the first one that maps the whole range
(`address_space` in the result is `Tegra::MemoryManager::GetID`). Otherwise it names the first
unmapped address in the bound channel's address space.

How the reads are safe:

- **Guest memory, on the server thread.** `ReadGuestMemory` (`nx2_debug_tools.cpp`) resolves
  each page with `Memory::GetPointerSilent` (`src/core/memory.cpp:1073`). That is the lock-free
  lookup the guest CPU cores use: one relaxed atomic load of the page entry. Through
  `SparseLargeVector::GetOrDefault` (`src/common/sparse_large_vector.h:91`), an entry that was
  never committed reads as unmapped, so the lookup never commits the sparse table. It never logs,
  and it never calls `HandleRasterizerDownload`. `ReadBlock` would do all three, and its GPU
  flush may only run on the rasterizer thread. A mapped entry always points into the
  `DeviceMemory` backing, which stays mapped and readable for the whole boot (`SEC_COMMIT` on
  Windows, `src/common/host_memory.cpp:153`; a shared memfd mapping elsewhere). If the game
  unmaps a page during the copy, the read returns stale bytes instead of faulting.
  `GetPointerSilent` masks the top 16 bits, so addresses at or above 2^48 count as unmapped
  instead of aliasing low memory. The service stops before the process is destroyed, so the
  page table outlives every read.
- **What a guest read can show.** Guest threads keep running, so a value can change, or tear,
  while it is copied. Read it twice when that matters. On pages the GPU caches track
  (`RasterizerCachedMemory`), the read returns the CPU copy: GPU writes that are not written back
  yet are missing. `capture_frame` with `buffers` shows the GPU side.
- **GPU memory, on the rasterizer thread.** Each `Tegra::MemoryManager` belongs to an nvdrv
  address space that the GPU thread uses for every draw. The server thread cannot know whether an
  address space is still alive, so it does not touch them. The request goes through
  `nx2_debug_control.h` like a capture. `TickFrame` collects the bound `gpu_memory` and the
  `address_spaces` under `config_mutex`, then calls `IsFullyMappedRange` and `ReadBlockUnsafe`
  (`src/video_core/memory_manager.cpp:742`, `:474`), the reads the GPU itself makes. It does not
  flush the GPU caches, so the result is what guest memory holds, not data that is still only in
  GPU buffers.

### Input (implemented)

**`input`** `{buttons?, left_stick?, right_stick?, frames, timeout_s?}` holds the buttons and
sticks for `frames` presented frames and then releases them. It returns when they are done.
**`input_sequence`** `{steps: [{buttons?, left_stick?, right_stick?, frames}, ...], timeout_s?}`
runs the steps back to back, at most 1000 steps and 36000 frames per call. A step with no
buttons and no sticks injects nothing, so it works as a pause between moves.

- **Buttons** are `A B X Y L R ZL ZR Plus Minus Up Down Left Right StickL StickR` (the
  `NpadButton` names). Home and Capture are not offered: they would leave the game. Unknown
  names and unknown step keys are refused, so a typo fails instead of doing nothing.
- **Sticks** are `[x, y]` in -1..1, up is +y, scaled by `HID_JOYSTICK_MAX`.

Where it hooks in: `EmulatedController::SetInputInjector` installs a function that
`GetNpadButtons` and `GetSticks` call while it is set
(`src/hid_core/frontend/emulated_controller.cpp`). The npad service reads those two getters on
every update (`src/hid_core/resources/npad/npad.cpp:449`). Injected buttons are ORed into the
owner's, and an injected stick replaces the owner's stick and its `StickL*` or `StickR*`
direction bits (threshold half of full scale, like a real stick). It changes no mapping, profile
or setting, so it works with whatever the owner has mapped and leaves nothing behind. The input
goes to player 1, or to the handheld controller when player 1 is not connected; the result names
which. When no injector is installed, each getter costs one atomic load.

Timing is by presents, not wall time. `RunInput` starts the script at `g_nx2_present_count + 1`,
the next present. The injector looks up the active step from the present count at every read
(`InputAt`), so a step changes at the first npad update after its present: the npad updates
every 1 ms (`src/hid_core/resource_manager.cpp:48`). A slow frame stretches the step in time but
not in frames, so the same sequence moves the camera the same way on every run.

Release: the injector returns nothing from the end of the last step on, so the input ends on its
exact frame even before the handler removes it. The handler removes it on every exit path
(`SCOPE_EXIT`): when the steps are done; on timeout, when frames stop for `timeout_s` (default
`frames / 20 + 10`, at most 900); when the game shuts down; and when the client disconnects. The
transport gives every handler a `ClientGone` probe, a non-blocking peek at the request's socket
(`nx2_debug_server.cpp`), which the wait polls every 2 ms. The adapter waits as long as the
emulator's default timeout, so it does not drop a long sequence early.

Use it when the owner has put the controller down: injected buttons add to whatever the owner
presses. For an A/B test, run the same `input_sequence` from the same spot under each variant
(for example with a `reject_shaders` change in between), then compare screenshots or captures.

### Shown frames (implemented)

`screenshot` returns the guest's frame, drawn again from the framebuffers, never a frame that
frame generation made. **`shown_frames`** `{count?, height?, pictures?, each?, save?, timeout_s?}`
returns what reached the screen: the next `count` (1 to 32, default 12) frames as the presenter
sends them to the swapchain, after frame generation, with the timing the presenter knew.

Per frame (`frames`): `kind` (`real`, `predicted` for EDEN_NX2_EXTRAPOLATE, `interpolated` for
Lossless Scaling or EDEN_NX2_FLUID_INTERP), `presenter` (`fluid` for the EDEN_NX2_FLUID vblank
clock, `present_thread` or `renderer_thread` for arrival order), `slot` (the vblank it was shown
for; null in arrival order), `batch` (the real frame it is or derives from), and in microseconds,
rounded, relative to the first frame's present: `scheduled_us` (when the slot's vblank was due),
`presented_us` (taken just after the present call returned on the CPU: not the scanout, which
the presenter does not observe), `late_us` (presented less scheduled; negative is early),
`interval_us` (since the previous frame) and `age_us` (presented less when the real frame it
derives from was recorded). `deferred_present` marks a frame that EDEN_NX2_FLUID_COPY_AHEAD
presents at the next slot's wake, so its `presented_us` is the copy's submission (about a refresh
early, also in the intervals next to it) and its `late_us` and `age_us` are null. Scheduled times,
late and age exist only on the FLUID clock; the answer's `notes` say so otherwise. FLUID frames
shown in arrival order (interpolated ones, or before the clock starts) also have a null `slot`
and `scheduled_us`. The answer also counts `kinds`, counts the frames with a picture in
`pictures` (a number, unlike the request's boolean), and gives `span_us`.

Pictures: each shown frame is blitted (linear filter) to `height` lines (144 to 540, default 270)
at the frame's aspect ratio, and the answer has one contact sheet, `sheet_png_base64`, with every
frame in order, left to right and then down (rows of four, a near-square grid past 16 frames). A
strip above each tile gives its kind: green real, orange predicted, blue interpolated; a grey tile
had no picture. Each frame's `tile` is its rectangle in the sheet. `each: true` (socket only) also
returns every frame as its own PNG, `save: true` writes the sheet to
`shown-<session>-<n>.png` in the dump directory. The adapter returns the sheet as image content
and `--call ... --out sheet.png` saves it. HDR output gives the encoded values cut to 8 bits, not
tone-mapped (`hdr: true`).

Where it hooks in (`src/video_core/nx2_shown_frames.h`, `renderer_vulkan/vk_shown_capture.cpp`):

- The request goes through its own small mutex-guarded block with two relaxed flags, like
  `nx2_debug_control.h`. The thread that presents takes it at the next present: the present
  thread (`FluidShow`, or `PresentThread` in arrival order) or the renderer thread without one
  (`PresentManager::Present`). It answers after `count` frames.
- `Frame::shown_kind` is set where frames are generated (`RecordFluidFrames`,
  `RecordFluidInterpolated` and the arrival-order generation loop in `renderer_vulkan.cpp`) and
  reset to `real` with the frame.
- The picture is recorded into the screen copy's own command buffer on the graphics queue
  (`CopyToSwapchainImpl`), where the frame's image is already in `TRANSFER_SRC_OPTIMAL`: a blit
  into one small image, a copy into that frame's part of a host-visible buffer, a barrier to host
  reads. After the last frame the present thread waits for that frame's copy fence (at most
  500 ms; every capture copy was submitted to the same queue before it) and reads the buffer
  back. The frame is not given back for reuse before then, so the fence is still its own. A
  presenter destroyed mid-capture (the game stops) answers with the frames it has, without
  pictures, so the flags drop and the next capture is taken.
- The dedicated copy queue (`EDEN_NX2_FLUID_QUEUE`) is a compute or transfer family that cannot
  blit. While a capture with pictures waits or runs, every screen copy takes the graphics-queue
  path, which that switch already uses for frames whose size or format differ from the
  swapchain's. `pictures: false` leaves the copy where it is and gives the timing exactly as
  played.

Cost:

- Idle: per shown frame, two relaxed loads of the two flags (one before the screen copy, one
  after the present); no lock, no call.
- Capturing with pictures: per frame one blit of the frame (for example 2880x1800 to 432x270)
  and a copy of the small picture (466 KB at 432x270) on the graphics queue, not measured yet;
  a mutex and a vector push on the presenting thread. Memory: one small image and a buffer of
  `count * width * height * 4` bytes (5.6 MB at the defaults on a 16:10 screen, 60 MB at 32
  frames of 540 lines), allocated at the first captured frame and freed after the answer. The
  last frame's fence wait blocks the presenting thread (and `WaitPresent`, which takes the same
  lock) until that frame's copy is done. With the dedicated queue the copies run behind the
  game's rendering for the capture's duration, so slots can be missed that would not be missed
  otherwise. The contact sheet at 32 frames of 540 lines is about 5200x3300 pixels on a 16:10 screen: ask for
  fewer or smaller frames when the image goes to an agent.
- Capturing without pictures: the mutex and the vector push only.

A capture that does not see `count` frames within `timeout_s` (default 10, at most 60) fails and
is withdrawn; frames still shown later finish it and the answer is dropped. One capture runs at a
time.
### Telemetry (implemented)

Four tools expose internals the log lines already measure, as JSON an agent can compare across
an A/B run without parsing logs.

**`thread_stats`** `{top?}`. Guest threads' CPU time and waits, from the
`EDEN_NX2_GUEST_THREAD_STATS` counters (`src/core/hle/kernel/nx2_thread_stats.cpp`,
`Kernel::Nx2ThreadStats::Snapshot`). Per thread, busiest first: `on_core_ms` split into
`jit_ms` (and within it `game_code_ms`, `counter_poll_ms`, `hint_spin_ms`, `cached_memory_ms`,
`read_back_ms`) and `outside_jit_ms` (SVCs and kernel), `ready_ms` (runnable without a core),
`held_replies_ms` (HLE replies held back), `presents`, and `waits` by kind (`ipc`,
`sync_objects`, `address_arbiter`, `condvar`, `lock`, `sleep`, `other`) each with `ms` and
`count`. Times are totals since the thread first ran, converted from guest counter ticks with
`cntfrq_hz`. `enabled` is false without the switch, and the rows are then empty. `top` (default
16, at most 128) limits the rows; `threads_total` counts them all.

**`present_stats`** `{}`. The present thread's counters
(`PresentManager::GetPresentStats`, `src/video_core/renderer_vulkan/vk_present_manager.cpp`):
the FLUID presenter's `slots`, `hits`, `missed` (= `no_frame` + `queued` + `overslept`),
`not_done`, `uncovered`, `rolled`, `spare`, `stale`, `in_order`, `late`, `dropped`, `received`,
`held` (frames shown for 1/2/3/4+ refreshes), `age_ms` (real frame's age at display) and
`latency_ms` (record to GPU work done), and under `present_thread` the acquire, copy-wait,
submit-lock and present-call times (mean and max) and the slot margin before the vblank. The
counters fill with `EDEN_NX2_SHOWN_STATS` at launch (`shown_stats` says whether it is set);
the `vblank` clock (`refresh_hz`, `index`, `divisor`, `lead_ms`, `latency_ms`),
`swapchain_images`, `present_frames`, `queued_frames` and `free_frames` are live regardless.
Vulkan only. The show-path counters are read under the swapchain lock; the arrival counters
are a best-effort snapshot of monotonic values, fine for a debug read.

**`cache_stats`** `{}`. Texture and buffer cache hit/miss/eviction counters since boot
(`VideoCommon::Nx2CacheStats::g_counters`, `src/video_core/nx2_cache_stats.h`, counted in
`TextureCache::FindOrInsertImage`/`DeleteImage`/the collector and
`BufferCache::FindBuffer`/`CreateBuffer`/`DeleteBuffer`/the collector). Per cache: `hits`,
`misses`, `hit_rate`, `created`, `deleted`, `evicted` (the collector's), and for textures
`evicted_downloaded` (those that waited for a GPU download first) with `device_usage` against
`expected_threshold` and `critical_threshold`; for buffers `bytes`. Also `staging_bytes`,
`graphics_pipelines` and `built_pipelines` from `Common::g_memory_counters`. Always counted
(a few relaxed atomic adds per lookup); no switch needed.

**`boot_timing`** `{}`. The boot phases `Common::BootTiming` recorded
(`src/common/boot_timing.cpp`, `RecordedPhases`): every `Mark` and `Span` since the process
started, oldest first, each with `phase`, `at_ms` (since process start) and `took_ms`, plus
`now_ms`. The last 256 phases are kept. The same boundaries the `Boot timing:` log lines show,
without parsing the log; boot phases logged before the server starts are included.

### Phase 2: control and input (about 4 to 5 days)

- **pause, resume, step_frames.** The core has `System::Pause` and `Run` (`src/core/core.cpp:279`).
  The frontends own the run state, though: Qt's `EmuThread` decides when to call `Run`, so a
  server that called `Pause` directly would desynchronise the UI. The frontend registers
  pause and resume callbacks with the service, and Qt implements them with the same slot its
  pause button uses. `step_frames {n}` resumes, waits until `g_nx2_present_count` advances by
  `n`, then pauses. That is present-granular, not exact, because the GPU drains a frame or two
  behind the CPU. Use a capture to get an exact frame.
- **input** and **input_sequence** are done; see "Input".
- **shaders.** An inventory from `ShaderCache::lookup_cache` (key, hash, size, address) and
  `rejected_native` (address plus the exception message), copied on the rasterizer thread like
  the reject change. Each entry carries its stage and whether it was lowered, rejected by the
  lowering, or rejected by a hash.
- **switches.** A registry that replaces the `static const bool x = std::getenv(...)` pattern for
  switches that can change safely. It keeps the env var as the initial value, and the getter
  becomes a relaxed atomic load.

Live switch classification, from the switches in `src/`:

| Class | Switches | Live? |
|---|---|---|
| Observers (trace and log only) | `RENDER_TRACE`, `IPC_TRACE`, `IPC_SUMMARY`, `VDEC_TRACE`, `GPU_PROFILE`, `DLSS_CONTRACT_TRACE`, `QUERY_TRACE`, `METHOD_TRACE`, `SVC_TRACE`, `MEM_TRACE`, `PULLER_TRACE`, `QMD_TRACE`, `CB24_TRACE`, `RT_TRACE`, `TC_LOG`, `MME_LOG`, `WINDOW_TRACE`, `FPS_LOG`, `PB_CHECK`, `SPLIT_CHECK`, `EDEN_DKB_FRAME_TRACE` | Yes. Flip at a frame end. Some keep per-frame counters (`render_trace_frame`) that must reset when turned on. `GPU_PROFILE` is an `inline const bool` (`src/video_core/nx2_profile.h:74`) read by every zone; it becomes an atomic. |
| Dumps | `SHADER_DUMP`, `VDEC_DUMP`, `TEXTURE_DUMP`, `PB_DUMP`, `MME_DUMP` | Yes, as on and off only, writing into a subdirectory of the debug dump directory. Clients never pass the path. |
| Behaviour, cached | `REJECT`, `REJECT_HASH` (done), `EXPERIMENTAL_GEOMETRY`, `NO_MESH`, `NO_TENSOR`, `SKIP_VS`, `UNDEF_ZERO`, `FORCE_LOD0` | With invalidation. The first group changes lowering, so it needs the reject path above for every native program. `FORCE_LOD0` changes samplers, so it needs a sampler cache flush. |
| Behaviour, A/B races | `PB_SNAPSHOT`, `FETCH_DRAIN`, `FLUSH_DRAWS`, `SYNC_SUBMIT`, `FENCE_FLUSH_US`, `IDLE_SPIN_US` | Probably, at a frame end. They change timing, not state, but the point of an A/B race test is a clean run, so a relaunch is often still the honest answer. |
| Not live | `SPLIT` (thread topology set at boot, `gpu_split.cpp:89`), `CNTFRQ` (guest-visible clock rate, set in `Load`), `VRAM_LIMIT_MIB` (device creation), `SPARSE_*` and `PRIVATE_DEVICE_RANGES` (decided when memory is mapped), `EMPTY_SYSTEM_DATA`, `AUTO_STUB` (service setup), `MME_JIT` (compiled macros), `DLSS_REPLACEMENT` and `DLSS_PRESET` (pipelines and models built at first use), `FRAMES_AHEAD`, `SWAP_INTERVAL` and `VSYNC_HZ` (pacing state in `nx2_gsv.h`) | No. Changing them mid-run either has no effect or leaves state built under the old value. |

### Phase 3: memory, resources and timing (about 1 to 1.5 weeks)

- **read_memory**, **search_memory** and **read_gpu_memory** are done; see "Guest and GPU memory".
- **dump_texture** `{gpu_address}` and **dump_buffer** `{gpu_address, size}`. Look the address
  up in the texture cache on the rasterizer thread and reuse `FrameDump::Record`'s readback,
  decode and PNG path for one image.
- **gpu_timings**. Bracket each frame-dump pass with timestamp queries. The scheduler already
  writes timestamps into a query pool for its trace (`src/video_core/renderer_vulkan/vk_scheduler.h:95`).
  Return pass index, draw range, shaders and GPU microseconds for one frame. This extends
  `EDEN_NX2_GPU_PROFILE`, which only splits CPU time on the GPU thread into zones.
- **capture options**: `pass_range` and `shader` filters in `FrameDump::BeginPass` and
  `CaptureDispatch`, so a sampled capture of a 300-pass frame does not write gigabytes.

### Save states

The core cannot save state. There is no serialisation of the kernel, services, the dynarmic and
NCE CPU state, or the GPU caches, and Eden and yuzu never had it. The `saveState` in
`src/lichii/main_window.cpp` is Qt window geometry. Adding save states is a project of its own,
not a tool. The practical substitute is a reproduction script: phase 2 input sequences, recorded
once (`EDEN_NX2_KEY_RECORD` already records keys in `guarana-cli`) and replayed after boot.

## Running it on the PC

1. Build the branch as usual:
   `bash C:/lx/build-fix.sh feat/debug-mcp debug-mcp "Any - debug server" "EDEN_NX2_DEBUG_SERVER=127.0.0.1:6545"`.
   Optionally add `EDEN_NX2_DEBUG_DIR=C:/Users/Zortos/.capy/work/lichii-debug/debug-mcp/dumps`
   so dumps land next to the build.
2. The owner launches the game from the shortcut. The log shows
   `Debug server listening on 127.0.0.1:6545, dump dir ...`.
3. The agent runs the adapter with native Windows Python. The MSYS `python` in Git Bash rejects
   `C:/` paths, so call the Windows interpreter by its full path. From bash, `--call` runs one
   request, prints its JSON result, and saves any PNG to `--out`:

   ```
   PY=/c/Users/Zortos/AppData/Local/Programs/Python/Python314/python.exe
   $PY -I C:/lx/src/tools/nx2/debug_mcp.py --server 127.0.0.1:6545 --call '{"method":"status"}'
   $PY -I C:/lx/src/tools/nx2/debug_mcp.py --server 127.0.0.1:6545 \
       --call '{"method":"screenshot","params":{"height":1080}}' --out C:/lx/shot.png
   $PY -I C:/lx/src/tools/nx2/debug_mcp.py --server 127.0.0.1:6545 \
       --call '{"method":"reject_shaders","params":{"hashes":["3ddfb1d7621c4755"]}}'
   $PY -I C:/lx/src/tools/nx2/debug_mcp.py --server 127.0.0.1:6545 \
       --call '{"method":"input_sequence","params":{"steps":[{"right_stick":[1,0],"frames":60},{"frames":30}]}}'
   ```

   The request is `{"method": <tool>, "params": {...}}`, the same format the socket uses. Adapter-only
   tools work too (`capture_image`, and `index` on `capture_frame`). On failure, `--call` prints
   `{"error": ...}` and exits with status 1. Without `--call`, the script is a stdio MCP server for
   an MCP client.

   Hashes must be all 16 hex digits. A shorter hash is refused rather than zero-padded, because a
   padded hash matches nothing. Returned paths use forward slashes.

The adapter reconnects after the game restarts. A boot through a guest relaunch stops and starts
the server again on the same port.

## Phase 1 limits

- Vulkan only for `capture_frame`, `reject_shaders` and `read_gpu_memory`. The drain point is in
  `RasterizerVulkan`. OpenGL would need the same three lines in `RasterizerOpenGL::TickFrame`,
  plus a frame dump it does not have.
- A runtime capture uses up an `EDEN_NX2_FRAME_DUMP_AT` capture that has not fired yet. They share
  one `FrameDump`.
- One request at a time. A second client waits while a capture runs.
- `log_tail` sees lines logged after the game finished loading. The boot itself is in
  `eden_log.txt`.
- Runtime rejections last for one boot. Starting the next session clears them.
