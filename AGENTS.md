# AGENTS

## Guarana: read this first

Guarana is a fork of [Lichii](https://github.com/zortos293/Lichii) (branch `nx2`), the Nintendo Switch 2 emulator in the yuzu, Eden, Eglantine and Lichii line, licensed GPL-3.0-or-later. It is for integrated GPUs: Intel Arc iGPUs first (Arc 140T, Arc 140V, Meteor Lake, Panther Lake), then the AMD Radeon 780M and 890M in handhelds. It aims at even frame pacing (steady frame times, not only a higher average) and at frame generation that fits an iGPU's power budget.

This section is the fork's. Everything under "Upstream Lichii guidance" is upstream's `AGENTS.md`, kept as it is so that upstream merges stay clean. Where the two disagree, this section wins:

- Our upstream is zortos293/Lichii. "Before writing a fix" below names Eglantine, which is Lichii's own upstream: check Lichii first, then Eglantine.
- Experiment switches may land on `main` behind a default-off `EDEN_NX2_*` switch with a row in `docs/switches.md`. Upstream keeps them off its `nx2` ("Toggles and merging").
- The PC, paths and scripts below (`C:/lx`, `build-fix.sh`, `C:/Users/Zortos/...`) and the model split are the upstream owner's setup. Use the commands in this section instead.

### Rules

1. **Ship only a measured gain on a verified scene.** A verified scene is a `tools/bench` row with `"valid": true` and `"scene_ok": true`: the benchmark checked the game was where the row says. A gain is a paired A/B from one session, baseline and change back to back, with the baseline shown next to the result. A run on the wrong scene is not a result, whatever its fps.
2. **Experiments are `EDEN_NX2_*` environment switches, off by default.** Unset (or `0`) leaves the behaviour exactly as before. A switch that did not help in a paired run is removed, or stays off and says so in `docs/switches.md`.
3. **Document every `EDEN_NX2_*` switch** you add, change or remove in [docs/switches.md](docs/switches.md), in the same pull request: values, status (proven, experimental or diagnostic), measured effect, code location.
4. **Correctness before speed.** A speed-up that can change the image needs a scene check and a known-issues note. A driver-bug fix needs a before and after screenshot or log, even without an fps gain.
5. **Never commit** anything in the list under "Never commit".

### Repo map

| Path | Contents |
|---|---|
| `src/core` | HLE kernel and services, the Switch 2 (`nx2`) services, the debug server (`debugger/nx2_debug_server.cpp`, `debugger/nx2_debug_tools.cpp`), game presets (`game_presets.cpp`), the session power policy (`nx2_power_session.cpp`) |
| `src/video_core` | GPU emulation and the Vulkan renderer: buffer and texture caches, `renderer_vulkan/present/` (frame generation, extrapolation, cadence), frame budget, shading-rate governor, `nx2_uma.h` |
| `src/shader_recompiler` | Maxwell and Ampere programs to SPIR-V. `EDEN_NX2_GM` lives in `ir_opt/global_memory_to_storage_buffer_pass.cpp` and `backend/spirv/` |
| `src/common` | Settings, logging, host memory, power meter, policy, governor and tuning |
| `src/lichii`, `src/qt_common` | The Qt frontend (`guarana.exe`). `src/lichii/nx2_switches.h` loads `nx2_switches.txt` |
| `src/lichii_cmd` | The SDL frontend (`guarana-cli`) |
| `src/frontend_common` | Shared frontend code, config, the update checker (off in our release builds) |
| `src/tests` | Catch2 unit tests, built with `-DLICHII_TESTS=ON` into `build/bin/tests` and a few smaller test programs |
| `tools/nx2` | The debug server's MCP adapter (`debug_mcp.py`), the shader sweep, frame dump index, pipeline statistics, cache metrics, and their `test_*.py` |
| `tools/bench` | The cross-machine benchmark (`bench.py`), the play launcher (`play.py`), scenes, suites, variants, profiles and results |
| `tools/shader-sweep` | The offline shader compiler the sweep drives; builds on its own |
| `scripts` | One-command build and test scripts for Windows and Linux (see "Build") |
| `tools/windows`, `tools/ci` | CI toolchain installers; CI helper scripts such as `changelog-section.sh` |
| `docs` | `switches.md`, `lifecycle.md` (what is stored and when it is flushed), `LANES.md`, `nx2/debug-mcp.md`, `Build.md`, `Coding.md`. `CI.md` describes Eglantine's pipeline, not ours; its PGO retraining steps still apply |
| `dist` | `nx2_switches.txt` (shipped next to the executable), icons, desktop files |
| `.github` | Workflows, issue forms, `labels.yml` |
| `*.profdata` | PGO profiles: `lichii-windows.profdata` (clang-cl), `eglantine.profdata` (Linux clang) |

### Build

One command from a fresh clone, from the repository root. The scripts check the prerequisites, install what they can under `.deps/`, and say how to get the rest; their headers list the options.

| | Release build (as the release workflow) | Unit and tool tests (as build-check) |
|---|---|---|
| Windows | `powershell -ExecutionPolicy Bypass -File scripts\build-windows.ps1` | `powershell -ExecutionPolicy Bypass -File scripts\run-tests.ps1` |
| Linux | `bash scripts/build-linux.sh` | `bash scripts/run-tests.sh` |

Without the scripts, the same build CI checks. Windows, PowerShell: Visual Studio 2022 with "Desktop development with C++", LLVM 22 with `clang-cl` on `PATH` (CI pins 22.1.8; `tools/windows/install-llvm.ps1` runs only in CI), CMake, Ninja and the Vulkan SDK; the configure step downloads Qt, SDL3 and FFmpeg.

```powershell
Set-ExecutionPolicy -Scope Process Bypass -Force; & ./tools/windows/load-msvc-env.ps1; cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DLICHII_TESTS=ON; if ($?) { cmake --build build --parallel }; if ($?) { ctest --test-dir build --output-on-failure }
```

Linux, bash, after installing your distribution's packages from [docs/Deps.md](docs/Deps.md) (CI's Ubuntu 24.04 list is in `build-check.yml`; CI takes Qt 6.8.3 from `install-qt-action`, so point `CMAKE_PREFIX_PATH` at a Qt of your own if the distribution's is older):

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DLICHII_TESTS=ON && cmake --build build --parallel "$(nproc)" && ctest --test-dir build --output-on-failure --parallel "$(nproc)"
```

When these commands and `.github/workflows/build-check.yml` disagree, the workflow is right: fix this file. The programs are `bin/guarana` (`guarana.exe`) and `bin/guarana-cli` in the build directory. Run one group of unit tests by its tag: `build/bin/tests "[frame_budget]"`.

### Verify a change

Without the game, on any machine:

1. **Unit tests.** Put a test next to the logic you changed in `src/tests/<area>/` and add it to `src/tests/CMakeLists.txt`; `src/tests/video_core/frame_budget.cpp` and `src/tests/common/power_governor.cpp` show the pattern. Build with `-DLICHII_TESTS=ON` and run `ctest`.
2. **Python tools.** No build needed: `python3 -I tools/bench/test_bench.py`, `python3 tools/nx2/test_debug_mcp.py`, `python3 tools/nx2/test_cache_metrics.py`, `python3 tools/nx2/test_cache_bench.py`. The `linux-gcc` job runs the `tools/nx2` ones.
3. **Shader changes.** Build `tools/shader-sweep` on its own and run its tests (`tools/shader-sweep/README.md`): synthetic cases and three small captured programs, no game needed. With a local corpus of native dumps and caches, run `tools/nx2/shader_sweep.py` before and after (`--contexts-from` the earlier `results.json`) and compare the two `summary.md`: no new failures. A corpus is game data: it stays on the machine that dumped it.
4. **CI.** Every pull request builds Linux GCC and Windows clang-cl with `-Werror` and runs the tests. The Windows build is uploaded as `guarana-pr<N>-<commit>-windows-x86_64` (kept 14 days) and linked in a comment on the pull request, so a tester with the game can try it without building anything.

With the game, on the owner's or a tester's machine:

1. **Debug server.** The owner starts a build with `EDEN_NX2_DEBUG_SERVER=127.0.0.1:6545`. Agents never start or stop the emulator. Ask it questions with `python -I tools/nx2/debug_mcp.py --server 127.0.0.1:6545 --call '{"method":"frame_times","params":{"seconds":10}}'` (also `status`, `screenshot`, `gpu_costs`, `reject_shaders`; a method without parameters can be named alone, `--call status`, which PowerShell passes intact where it can drop the inner quotes of JSON), or run the script without `--call` as an MCP server. The methods and their limits are in `docs/nx2/debug-mcp.md`; the etiquette is in "Live debug server" below.
2. **Benchmark.** `python -I tools/bench/bench.py run --variant tools/bench/variants/baseline.json --variant <yours>.json ...` on a scene with a reference image, or a suite (`tools/bench/README.md`). Compare with `bench.py compare`. Only rows that are valid with `scene_ok: true` count.
3. **Report** the rows in the pull request's table and in the switch's row in `docs/switches.md`.

### Lanes

| Branch | Takes | CI |
|---|---|---|
| `main` | Reviewed pull requests; always green | build-check; nightly pre-release when it moved |
| `release/vX.Y` | Fixes only, for stabilizing X.Y | build-check; a `vX.Y.Z` tag on it publishes the release |
| `hotfix/*` | One fix, branched from a release tag; merged into its release branch and forward-ported to `main` | build-check |
| `exp/*` | Experiments, behind default-off switches | build-check and the Windows test build |
| `upstream` | Nothing: a mirror of zortos293/Lichii `nx2`, moved only by `upstream-sync.yml` | The sync opens a merge pull request into `main` |

Details, release steps and what to do when the sync stops: [docs/LANES.md](docs/LANES.md).

### Never commit

- Keys (`prod.keys`, `title.keys`, any `*.keys`), firmware, NAND and SD card contents (`nand/`, `sdmc/`), saves.
- Game files in any container (`.nsp`, `.xci`, `.nca`, `.nup`, `.nsz`, `.xcz`) and anything taken from them: RomFS or ExeFS contents, textures, models, audio, video, game code.
- Dumps: shader dumps (`EDEN_NX2_SHADER_DUMP`), frame dumps and debug server captures, RenderDoc captures (`.rdc`), crash and memory dumps (`.dmp`), `EDEN_NX2_PIPELINE_STATS` output.
- Shader and pipeline caches, the emulator's or the driver's.
- Logs that carry user names or paths (`eden_log.txt`, `tools/bench/results/logs/`). `tools/bench` rows are scrubbed and are meant to be committed.
- Personal data of anyone: names that are not commit authors, host names, serial numbers, IP or MAC addresses, account or console IDs.
- Secrets and raw profiles: tokens, signing keys, `*.profraw`.

The synthetic containers in `src/tests/core/nx_fixtures/` are generated test data (see its README), not game files. `.gitignore` covers the common names; it is a safety net, not a licence.

### Commits and pull requests

- Subject: `area: change`, imperative, at most 72 characters, for example `vulkan: keep vertex input dynamic state on Intel 101.x drivers`. The area is the subsystem: `vulkan`, `shader_recompiler`, `texture_cache`, `buffer_cache`, `core`, `kernel`, `frontend`, `bench`, `ci`, `docs`.
- Body: what changed and why. For performance: the scene, device, driver and power mode, before and after (fps, p50 and p99 frame time, or the draw's GPU ms), and how many paired runs. Name every switch added and its default.
- One topic per pull request, with its tests, its `docs/switches.md` rows and a changelog entry: a fragment `changelog.d/<slug>.<section>.md` (see [changelog.d/README.md](changelog.d/README.md); a line under `[Unreleased]` in `CHANGELOG.md` is still accepted until the next release, but it conflicts with other pull requests, so use a fragment). Fill in the template.

### Upstream first

- **Look before you write.** Search zortos293/Lichii (`nx2`, open and closed pull requests, issues) for the game and the symptom, then Eglantine-Community/Eglantine the same way: `gh pr list -R zortos293/Lichii --state all --search "<symptom>"`, `gh issue list -R zortos293/Lichii --state all --search "<game>"`, `gh pr diff <n> -R zortos293/Lichii`. Read the diffs, not only the titles.
- **Port, do not rewrite.** Cherry-pick with `-x`, or keep the author with a `Co-authored-by:` trailer; cite the pull request or commit in the body. An upstream pull request we carry before it is merged goes into `CREDITS.md`.
- **Send fixes back.** A fix that is not specific to integrated GPUs or to our switches goes to zortos293/Lichii as a pull request from a branch based on its `nx2`, without our branding or unrelated switches. Link it from ours.
- **Merge upstream through the `upstream` branch** and its sync pull request, with a merge commit. Never commit to `upstream`.

---

## Upstream Lichii guidance

See `docs/Coding.md` for technical coding guidance.

Keep comments to a reasonable level. Do not overexplain or add/remove comments from unrelated parts of the codebase.

## Game bug reports

Users report game bugs with Lichii logs (`eden_log.txt`, `eden_log.txt.old`). Each `Booting game: <program id>` line starts a session; the build is on the first line (`Lichii Version: ... | nx2-<sha>`). Known program ids:

| Program id | Game |
|---|---|
| 040074A01BF12000 | Donkey Kong Bananza |
| 040077201BE50000 | Kirby Air Riders |
| 04007C6028AA8000 | Yoshi and the Mysterious Book (demo) |
| 0400D4201C6EA000 | Mario Tennis Fever |
| 0400E88025B1A000 | Star Fox |
| 04007EF01F8AE000 | Fire Emblem: Fortune's Weave |
| 0400C3F00006E000 | Mario Kart World |
| 04008E001F098000 | Nintendo Switch Sports Resort |
| 0400A3A01EE4C000 | Pokémon Pokopia |
| 0400A4802437E000 | Drag x Drive Global Jam |
| 040041E0212AE000 | Fast Fusion |
| 04005B902517C000 | Kirby Air Riders Global Test Ride |
| 040057C02159A000 | Nintendo Switch 2 Welcome Tour |
| 040004F027560000 | Orbitals Global Friends Pass |
| 0400380020056000 | Raiders |
| 0400D1D02399A000 | The Duskbloods network test |
| 0400486024DEC000 | The Legend of Zelda: Ocarina of Time |
| 0400245021D2C000 | Yoshi and the Mysterious Book |
| 0400DDB020648000 | Cyberpunk 2077: Ultimate Edition |
| 0100606026F12000 | Final Fantasy Resonance |
| 0100EA80032EA000 | New Super Mario Bros. U Deluxe (Switch 1) |
| 010019A01E2F2000 | Metroid Prime 4: Beyond - Nintendo Switch 2 Edition |
| 0100152000022000 | Mario Kart 8 Deluxe (Switch 1) |
| 040032B023CA4000 | Orbitals |
| 0400921025B32000 | Sonic Frontiers (Switch 2 Edition) |
| 04003F40232DE000 | Hyrule Warriors: Age of Imprisonment |
| 040015100B514001 | Super Mario Bros. Wonder - Nintendo Switch 2 Edition + Meetup in Bellabel Park |
| 01004D300C5AE000 | Kirby and the Forgotten Land (Switch 1) |
| 01007EF00011E000 | The Legend of Zelda: Breath of the Wild (Switch 1) |
| 0100F2C0115B6000 | The Legend of Zelda: Tears of the Kingdom (Switch 1) |
| 01006D0017F7A000 | Mario & Luigi: Brothership (Switch 1) |
| 010051F0207B2000 | Tomodachi Life: Living the Dream (Switch 1) |

Add ids here when a report identifies a new game.

## Before writing a fix

Lichii is a fork of [Eglantine](https://github.com/Djihads80/Eglantine). Do not guess, and do not rewrite a fix upstream already has.

1. `git remote add upstream https://github.com/Djihads80/Eglantine.git && git fetch upstream`.
2. Check `upstream/nx2`, open and closed PRs, unmerged branches, and issues for the game and symptom (`gh pr list -R Djihads80/Eglantine --state all`, `gh issue list -R Djihads80/Eglantine --state all`, `gh pr diff <n>`). Read the diffs, not only the titles.
3. If nx2 is behind `upstream/nx2`, merge upstream first. Port an existing upstream fix before writing a new one, and cite its PR or commit.
4. Write a new fix only when nothing upstream addresses the root cause the log shows.

## Live debug server

Every debug package on the PC launches with `EDEN_NX2_DEBUG_SERVER=127.0.0.1:6545` (in the env argument of `build-fix.sh`; `EDEN_NX2_DEBUG_DIR=<dir>` moves its dumps). Ask the running game from bash on the PC with native Windows Python; MSYS `python` rejects `C:/` paths.

```sh
PY=/c/Users/Zortos/AppData/Local/Programs/Python/Python314/python.exe
$PY -I C:/lx/src/tools/nx2/debug_mcp.py --server 127.0.0.1:6545 --call '{"method":"status"}'
$PY -I C:/lx/src/tools/nx2/debug_mcp.py --server 127.0.0.1:6545 --call '{"method":"screenshot"}' --out C:/Users/Zortos/.capy/work/lichii-debug/shot.png
```

Methods: `status`, `frame_times` (`seconds`, `refresh_hz`), `screenshot`, `shown_frames` (`count`, `height`, `pictures`; generated frames included, with a contact sheet), `log_tail` (`after`, `contains`), `log_filter`, `capture_frame` (the frame dump below, taken now; `sampled`, `index`), `capture_image`, `capture_bundle` (the Report a Bug zip, see `docs/nx2/bug-bundle.md`), `reject_shaders` (`EDEN_NX2_REJECT_HASH` at runtime; `"reject": false` restores), `set_shading_rate` (`hashes`, `rate`, `all`; needs `EDEN_NX2_VRS` at launch), `memory`, `power`, `power_set` (`spin_scale`, `eco_background`, `idle_spin_us`), `gpu_costs` (`top`, `enable`), `gpu_perf` (`enable`, `frames`; hardware GPU busy/stall/occupancy counters, needs `EDEN_NX2_GPU_PERF=1` at launch), `frame_budget` (`target_fps`, `scale`; `EDEN_NX2_TARGET_FPS` at runtime), `set_prepass` (`hashes`, `on`; `EDEN_NX2_PREPASS_HASHES` at runtime), `trace_record` (a GPU trace; `seconds`, `presents`, `path`), `trace_stop`, `trace_status`, `thread_stats` (guest threads' CPU time and waits; `top`; needs `EDEN_NX2_GUEST_THREAD_STATS`), `present_stats` (the FLUID presenter's slot counters, vblank clock and present queue depth; needs `EDEN_NX2_SHOWN_STATS` for the counters), `cache_stats` (texture/buffer cache hit/miss/eviction counters), `readbacks` (guest CPU readback / GPU-flush counters: reads, flush time, reader/GPU-side waits, DMA reads, early downloads), `boot_timing` (the recorded boot phases; `split`: held-draw time, builds by kind and file read time at the first frame and at a `mark`), `sample` (`threads`, `seconds`; host-thread sampling profiler, Windows only), `speed_limit` (`on`), `host_timing` (`stats`), `autopilot` (its last window; needs `EDEN_NX2_AUTOPILOT=1`), `read_memory` (`address`, `size`, `as`), `search_memory` (`pattern`, `start`, `end`, `regions`), `read_gpu_memory` (`gpu_address`, `size`, `as`), `input` (`buttons`, `left_stick`, `right_stick`, `frames`), `input_sequence` (`steps`). Without `--call` the script is a stdio MCP server. See `docs/nx2/debug-mcp.md`.

- Try a live experiment (screenshot, `reject_shaders` A/B, `capture_frame`, `log_tail`) before asking the owner for a new build or shortcut.
- Use full 16-hex-digit hashes; shorter ones are refused.
- Restore every hash you reject before you finish.
- Never start or stop the emulator. If nothing listens, ask the owner to launch the game.
- Keep sessions light while the owner plays: one request at a time, no capture loops, `sampled` only when you need pass inputs.

## RenderDoc and the live server

Use both for graphics investigations. The live server provides emulator state, native shader hashes, logs and reversible shader A/B tests. [RenderDoc](https://github.com/baldurk/renderdoc) inspects the translated Vulkan frame: draw and dispatch state, resource contents, pixel history and shader debugging where supported. Prefer it over extending our dumps when it already exposes the missing evidence.

- Use the live server to identify the failing scene and programs, then capture one affected frame with RenderDoc. Find the first incorrect output and inspect its inputs, bindings, blend/depth state and resource dependencies. Record the build, settings, event IDs and shader hashes so the two views can be correlated.
- The existing integration is `src/core/tools/renderdoc.cpp`, gated by `enable_renderdoc_hotkey`. It requires RenderDoc to be loaded into the process; enabling the hotkey alone does not load it. Ask the owner to launch the capture-enabled package. Never start, stop or inject into their running emulator without their approval.
- [renderdoc-mcp](https://github.com/Linkingooo/renderdoc-mcp) is a third-party replay adapter, not part of RenderDoc. Pin and inspect its version, use a compatible RenderDoc Python module, and verify a real capture opens before relying on it. If an extension, pixel-history query or shader debug operation is unsupported, report that limit rather than treating missing data as a clean result.
- Keep `.rdc` captures on the owner's PC: they can contain game shaders, textures and buffers. Do not upload them to public issues or services. Share only the specific permitted evidence needed for the investigation.
- Capture and replay can alter timing or hide races. Compare with an uncaptured live run before ruling out a synchronization bug. Distinguish measured GPU counters from adapter estimates; use CPU/thread and frame traces for emulator performance.
- A shader replacement that reduces fog or exposure can be a useful visual comparison, but is not proof of an emulator fix. Trace the original pipeline's first incorrect result before shipping a change. Matching shader arithmetic alone does not validate bindings, synchronization or input data.

## Debugging tools

Use these before guessing. All are off unless the variable is set; pass them as the env argument of `build-fix.sh`. Read them from the log at `*:Info` unless noted.

| Symptom | Tool | Output |
|---|---|---|
| A player's bug report (`lichii-bug-*.zip` from Help → Report a Bug or Ctrl+F12) | `python3 -I tools/nx2/bundle_index.py <zip>` | Unpacks it and writes `index.html`: build, game, GPU, description, dropped parts, frame times, the pipelines and shaders of the captured frame, and `frame/index.html` for its passes. Format in `docs/nx2/bug-bundle.md`. |
| Wrong image (white, black, missing objects, smearing) | `EDEN_NX2_FRAME_DUMP=<dir>` with `EDEN_NX2_FRAME_DUMP_AT=<seconds>`, `<frame>f` or `file` (captures once `<dir>/capture.now` exists; default 60); add `EDEN_NX2_FRAME_DUMP_SAMPLED=1` for pass inputs | One frame's pass colour targets, dispatch storage images (3D included) and present source; with `_SAMPLED`, every texture each pass samples and each compute shader samples before its first dispatch (`kind: sampled`, with stage (5 = compute), TIC index, 3D and depth included) as `.png` and `.raw.zst`, plus `manifest.jsonl` with shader hashes, formats, addresses and per-channel stats. `python3 -I tools/nx2/frame_dump_index.py <dir>` builds `index.html`. Find the first pass whose output is wrong, then inspect its shaders. |
| Wrong image to inspect off the PC, or a regression check | GPU trace: the debug server's `trace_record` (`seconds`, default 3), or `EDEN_NX2_GPU_TRACE=<dir>` and touch `<dir>/record.now` (`EDEN_NX2_GPU_TRACE_PRESENTS`, default 180) | `trace-*.lxtrace`: a snapshot plus every submit, CPU write, map and present. `eden-cli --replay <trace> --out <dir>` replays it headless on any Vulkan device (lavapipe included): a PNG and hash per present. See `docs/nx2/gpu-trace.md`. Traces hold game data: keep them private. |
| Rejected or miscompiled shaders | `EDEN_NX2_SHADER_DUMP=<dir>` | Native programs with `.header.bin` sidecars and `failed-compute-*.env` for failed compute pipelines. Replay offline with `tools/nx2/shader_sweep.py` (see `tools/nx2/shader-sweep.md`). |
| AMD-only shader bug without an AMD GPU (miscompiles, results the spec leaves to the driver) | `tools/nx2/radv_offline/compile.py` after `setup.sh`, on a cache record, a cache (`--check-all`, `--program <hash>`), SPIR-V, or a Fossilize capture (`FOSSILIZE=1 FOSSILIZE_DUMP_PATH=<prefix>` with the layer setup.sh builds) | Mesa RADV (ACO) builds the pipelines for gfx1030, gfx1032, gfx1100, gfx1102, gfx1200 and gfx1201 on a fake device: `summary.md` has per-chip wave size, registers, spills and ISA notes, plus SPIR-V findings by shader hash (spirv-val, NaN min/max, float controls, fp16 overflow, uninitialised values, descriptor indices, derivatives in divergent flow, helper invocations, subgroup size); NIR, ACO IR and ISA per stage beside it. `--variant` A/Bs driver variables such as `RADV_PERFTEST=pswave32`. See `tools/nx2/radv_offline/README.md`. |
| A newly lowered program changes the image | `EDEN_NX2_REJECT_HASH=<hash>,...` (native program keys from shader dump file names, or shader hashes from the frame dump) | Those programs are rejected as if unsupported; the log shows `rejected by EDEN_NX2_REJECT_HASH` per hit. Bisect by halving the list. The key covers the captured code, so recompute it when the capture rules change. |
| Draw and render-state questions | `EDEN_NX2_RENDER_TRACE=1`, filter `*:Info HW.GPU:Debug Render.Vulkan:Debug Shader:Debug` | Per-draw state, rejected stages and zero-format texture descriptors. |
| Video playback | `EDEN_NX2_VDEC_TRACE=1`, `EDEN_NX2_VDEC_DUMP=<dir>` | Decoder submit, decode and composite trace; decoded frames. |
| Performance and frame pacing | `EDEN_NX2_GPU_PROFILE=1`, `EDEN_DKB_FRAME_TRACE=1` | GPU thread, back thread and Vulkan worker time per second by zone, GPU waits by the zone they happen in, waits for the back by site, and spin mutex contention; per-present interval, CPU and fence timing. The profile also logs `Async read-back per second` every 10 s: guest reads of GPU-written memory, their waits, and macro DMA reads that waited for the split's back or drained it. |
| A shader is costly on the GPU: what the driver's compiler made of it | `EDEN_NX2_PIPELINE_STATS=<dir>`; add `EDEN_NX2_PIPELINE_STATS_IR=1` for the internal representations | One line per pipeline build appended to `<dir>/pipeline_stats.jsonl`: the stage hashes and, for each of the driver's executables, name, stages, subgroup size (the SIMD width on Intel) and every statistic the driver reports (VK_KHR_pipeline_executable_properties; instruction, spill and fill counts where the driver has them). `_IR` also writes each text representation to `<dir>/<fragment or compute hash>_<executable>_<name>.txt`, the first pipeline's per name. The session builds without the driver blob store and the driver pipeline cache, so builds are slow: do not time it. `python3 -I tools/nx2/pipeline_stats.py <dir> --costs <gpu_costs JSON or log with DKB gpu_work>` prints the executables of the costliest shaders. |
| HostTiming CPU or timer lateness (late guest timers, audio or vsync jitter) | `EDEN_NX2_HOSTTIMING_STATS=1`, or live with the debug server's `host_timing {"stats": true}` | One `HostTiming wait` line per second: waits, waits woken by newly scheduled events, OS sleep and precise (hardware) wait time (ms/s), the current margin before one-shot events, how late OS sleeps ended, how late events fired and how late the one-shot ones among them (guest timers) fired (p50/p99/max µs), and the thread's CPU %. |
| Where each guest thread's frame goes (is the game's main thread over the frame budget?), what the JIT does per core, and why the GPU front waits for its back | `EDEN_NX2_GUEST_THREAD_STATS=1`; `EDEN_NX2_ACQUIRE_STATS=1` gives only the front lines | Every 10 s, in ms per frame: `Guest threads over 10.0 s`, then one line for each of the 8 threads with the most time on a core and for any thread that presents (marked `presents N a frame`): time on a core, split into the JIT (game code, counter polling, hint spin, GPU-cached memory and the read-backs in it) and outside the JIT (SVCs and kernel), time ready without a core, and waits by kind with counts (IPC and its held HLE replies, sync objects, address, condvar, lock, sleep, other). Game code is JIT time minus polling, hint spin and cached memory. One `Guest core N` line per core: JIT time, JIT exits/s by reason, C++ block lookups, blocks compiled, full code cache clears, counter reads/s (and those within 1.7 µs of the last), hints/s by type. At `HW_GPU:Info`, the `Acquires:` lines without the profile, plus `Front waits for the back in 10 s by site` (count and ms), acquire methods not met without a drain and those that drain a second time, and drains for a queued release of unknown value that memory already meets. |
| Guest cores' idle CPU or wake-up latency (time in PhysicalCore::Idle) | `EDEN_NX2_IDLE_STATS=1` | One `Guest core N idle` line per core every 10 s: idle periods, how many the poll caught, the average time polled, how many blocked and how long those took to wake after the interrupt (p50/p99 µs), and the current poll. |
| Present waits on a fence (is the frame's fence submitted when the game presents?) | `EDEN_NX2_PRESENT_FENCE_TRACE=1` | One `nx2:vi2 present fences, last 600 presents` line: presents by their fence at entry (reached, submitted to nvdrv but not reached, beyond everything submitted, not allocated in nvdrv, no fence), each with those that had a suspended channel, auto waits that gave up and the mean/max ms until the fence was reached; how long after the present returned it was reached; the last fence with its guest value and nvdrv max. |
| Back thread idle (what the front does while the back waits) | `EDEN_NX2_BACK_IDLE_TRACE=1` | One `Back idle in 10 s` line: the back's idle ms/s and ms/frame split by the front's state (parse, drain, list writers, semaphore, list copy, queue empty, unknown when the state ring wrapped), and idle periods by length. |
| GPU-written command lists the front waits for, and the native macros they feed | `EDEN_NX2_LIST_MACRO_TRACE=1`; `EDEN_NX2_LIST_MACRO_VERIFY=1` adds the forwarding check | Every 10 s a `List macro trace in 10.0 s` line (GPU-written entries by content: commands, macro data, other method data; and by read: copied at once, after the channel waited for the host GPU's copy, after a drain), up to 8 lines per method whose data they continued and 8 per macro by code hash (calls fed, entries, waited ms, words a call). Each macro's code and first 16 calls go once to `EDEN_NX2_DEBUG_DIR` (else `list-macros` in the log dir) as `list-macro-<3d\|compute>-<hash>.txt`. Data RAM words those calls store are followed: a `List macro reader` line per macro that read one (who stored each word, the writes that carry it as is, the last address an inline upload or 3D constant buffer load put it at, and where the back found that address: a QMD word it loaded or ran, 12-14 being the grid, or a compute or 3D constant buffer); its dump gets `reader call` and `landing:` lines. Front calls that may use the data RAM run on the interpreter while a stored word is live. With verify, a `List macro verify` line per macro: calls that only forward their GPU-written words and to which methods (word -> method map), and those that steer, transform, store them in the data RAM or could not be reproduced; the first 16 such calls per macro are logged as warnings. |
| Draw preparation reuse (would a cross-frame draw template hit, and what do those draws cost?) | `EDEN_NX2_DRAW_REUSE_PROBE=1` | One `Draw reuse per frame` line every 10 s: draws per frame and mean PrepareDraw us by class (key seen last frame, only earlier this frame, new) and path (full, delta); the gate (full-path hits' cost over 1.1 us, ms/frame); images created, deleted and joined, TIC table invalidations and TIC/TSC table moves, texture memo hits and misses per frame. Uses no profile zones, so fast Maxwell3D stays on. |
| Shader stutter or not, boot and cache costs | `EDEN_NX2_STUTTER_TRACE=1` | One `Nx2Stutter` line per frame presented over 33 ms after the last, with its top cause (gfx/compute pipeline built on the GPU thread, pipeline wait, shader capture, texture, buffer, query/fence, present, other) and each GPU thread's ms; `Nx2StutterSummary` every 10 s (frame-time histogram, builds, cache lookups, cache file size); `Nx2Boot` boot phases with commit and peak commit. Frames over `EDEN_NX2_STUTTER_DETAIL_MS` (default 50) also get `Nx2StutterDetail`: each GPU thread's eight largest zones (ms/calls) and the rare sites that ran (device memory allocate/free, staging, buffer and image create/delete/GC, synchronous downloads, `Scheduler::Finish`), with each run over 2 ms by thread; the summary adds the sites per 10 s. The category totals in the summary are every frame's steady cost, not hitch time. `python3 -I tools/nx2/cache_metrics.py parse <log>` turns a log into JSON, `compare` A/B runs; `tools/nx2/cache_bench.py` runs the cold/warm matrix on the PC. |
| Command corruption or pop-in | `EDEN_NX2_PB_CHECK=1`; A/B with `EDEN_NX2_PB_SNAPSHOT=0` or `EDEN_NX2_FETCH_DRAIN=1` | How many pushbuffer entries change between submit and parse. |
| Commands read from memory instead of their submit-time copy (frames ahead, #392) | `EDEN_NX2_LIVE_READ_CHECK=1` | Every 10000 such entries, `Entries parsed from memory (<reason>): N checked, M differ` per reason, plus the first four differing entries per reason. GPU-written pages differ by design (the GPU rewrote them after submit), so compare against a frames-ahead-0 run. |
| Split GPU thread races | `EDEN_NX2_SPLIT_CHECK=1` | Reports scheduler use from the wrong thread. |
| Wrong bytes where the guest reads GPU-written buffer memory | `EDEN_NX2_READ_CHECK=<n>` (1 checks every read download) | Compares 1 in n read downloads with a synchronous flush's copy. Every 10 s a `Read download check` line: bytes that differ, by who wrote them to guest memory, and GPU-modified bytes no download brought (both must stay 0). The first 16 differences get their own line. |
| Wrong 3D state from the split front's register blocks (state writes sent in one entry) | `EDEN_NX2_REG_BLOCK_VERIFY=1` | Each block the back would store at once also runs write by write from the same state, and that result stays. Every 10 s a `Register block verify` line: blocks and writes compared, those that differ (registers, shadow RAM, dirty flags or the fast draw break; must stay 0), and blocks written one by one because a write kept its register's value. The first 16 differing blocks get their own error line. |
| Wrong macro parameters read by DMA on the split's front | `EDEN_NX2_DMA_READ_VERIFY=1` | A macro DMA read made without draining the split drains it anyway and is compared. Every 10 s a `Macro DMA read checks` line: reads, those made without a drain and those that read other words than after a drain (must stay 0), with the first 16 differing reads. |
| GPU back skipped a lock or gather it needed (inline writes, collected CPU writes, buffer usage resets) | `EDEN_NX2_BACKX_VERIFY=1` | Each skip also runs the locked check it replaces and then does the locked work. A `BackX verify mismatch` error (first 64) names the case: an inline write into GPU-written buffer data, a collected CPU write no manager showed, or a usage word set outside the tracked extent. Every 10 s a `BackX gather checks` line: skipped gathers, those that found writes a manager collected after the check (a race the next call covers) and those that found writes with nothing collected since (must stay 0). |
| Upscaler and DLSS contracts | `EDEN_NX2_DLSS_CONTRACT_TRACE=1` | Inputs and outputs of replaced upscaler networks. |
| Service calls | `EDEN_NX2_IPC_TRACE=1` | IPC command trace. |
| Read stalls (loading hitches, NUP vs other containers) | `EDEN_NX2_FS_READ_TRACE=1` | About once a second while the guest reads, one `Guest storage reads` line (`IStorage`, the RomFS) and one `Guest file reads` line (`IFile`): count, MiB, p50/p99/max latency and reads over 1 ms and 16 ms, plus totals and read time since boot. Times cover the backend read (NUP decompression included), not the Switch 1 SD-card throttle. |
| Unmapped device reads (`Unmapped Device ReadBlock`) | `EDEN_NX2_READBLOCK_TRACE=1` | Every read of unmapped device memory, one line per read with its first unmapped address, unmapped byte count and caller (`file:line`). Without it only the first 16 are logged. |
| Device loss (`Device loss occurred!`) | Always on | The `Device fault` lines after it: what VK_EXT_device_fault reports (fault addresses and their type, vendor fault codes). `not supported by the driver` means the driver has no such data. |
| Out of memory (`Image ... failed: -2`, `No memory for the ... image`) | Always on | The `Memory trace` lines before the first failed image: system commit against its limit, DXGI local and non-local use, and each heap's driver usage and budget. On Windows, commit at its limit means RAM plus the page file ran out, not the card; other hosts report no commit limit and log `0 of 0`, which says nothing. The `Host memory:` line under `Texture cache thresholds` says whether host commit or the device budget bounds the cache. |
| Buffer cache growth (`Memory trace: buffer cache` climbing, the largest buffers at ever higher device addresses, GPU time far over the timed work) | `EDEN_NX2_BUFFER_TRACE=1` | Every 10 s, `Buffer trace: N MiB in buffers; in 10 s` buffers created, buffers joined into them (MiB copied), stream leaps and buffers collected, plus `Buffer trace: SMMU space in 10 s` (Switch 2 device ranges allocated and freed). Each buffer of 64 MiB or more gets a `Buffer trace: created` line with its range, the range asked for, the `buffer_cache.h` line that asked, the buffers it joined and whether it took a stream leap (first 256). GBs copied per 10 s mean a buffer is growing by joining the next range each frame. |
| Driver crash, stall or slow pipeline builds | `EDEN_NX2_PIPELINE_BUILD_TRACE=<file>` | A flushed `BEGIN`/`END` line per driver pipeline build with the pipeline and stage hashes; `END` adds the wall time (`ms=`). The last `BEGIN` without an `END` names the shaders the driver crashed or is stuck on. Times include waiting on other builds, so compare runs from the same cleared driver cache rather than single builds. |
| Host crash (log just stops) | Always on (Windows): a `Common <Critical> Host crash:` line, then `eden_crash.dmp` next to the log. Without that line, the WER minidump in `%LOCALAPPDATA%\CrashDumps\lichii.exe.<pid>.dmp` if LocalDumps is set up | The line has the exception code, faulting thread (id and name) and the stack as module+offset; `abort()` covers `std::terminate` and fatal asserts, and a CRT invalid-parameter failure has its own line. Symbolize with `llvm-symbolizer --obj=lichii.exe --inlines --relative-address <offset>` next to the build's `lichii.pdb`, and read the dump with the `minidump` Python package. Fail-fast exits (heap corruption, stack cookie) and kills skip it, and the log then also misses up to 4 KiB of lines below Error that were still buffered. |

## How work is split

- One subagent per game, all running in parallel.
- Small research (upstream PR and issue surveys, log reading): a cheap model, DeepSeek V4.1 Flash at max reasoning when its endpoint is available.
- Hard fixes (emulation, shaders, GPU): GPT-6 Astra at high reasoning.
- When a GPT-6 Astra subagent can't find the cause, switch that subagent to Claude Opus 5.5 at high reasoning.
- The main thread (Opus) orchestrates and reviews.

## Builds

- Cloud machines build with clang and Ninja for compile and unit-test checks.
- The build the owner tests runs on the owner's Windows PC (DESKTOP-4UD89UT, Ryzen 9 5900X), built with clang using 12 jobs (`cmake --build <dir> --parallel 12`).
- On the PC the toolchain is clang-cl (LLVM 22) with the VS 2022 SDK, Qt 6.8.3 in `C:/lx/qt`, Ninja and sccache. Source is `C:/lx/src`, the persistent build dir is `C:/lx/build`, and `C:/lx/configure.sh` holds the configure flags. Build and package a fix with one command: `bash C:/lx/build-fix.sh <branch> <build-name> "<Game> - <fix>" ["ENV=VAL ..."] ["log filter"]`.
- Every fix gets its own debug build on that PC in `C:\Users\Zortos\.capy\work\lichii-debug\<build>\` and a desktop shortcut `Lichii Debug - <Game> - <fix>` that launches it with the owner's existing user data and keeps the log next to the build, so the owner can run it and send the log back.
- Bundle a round's candidate fixes into one build behind env switches, so the owner tests one package and A/Bs it live through the debug server instead of one build per idea.

## Toggles and merging

- Experiment switches (env flags that pick between candidate fixes) never merge to nx2. A proven fix ships as unconditional behaviour, and a disproven one is dropped.
- Diagnostic tools (frame dump, `EDEN_NX2_REJECT_HASH`, traces, the debug server) stay in nx2, off unless their variable is set.
