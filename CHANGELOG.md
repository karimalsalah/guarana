# Changelog

All notable changes to Guarana. The release notes of a version are its `## [X.Y.Z]` section, as
`tools/ci/release-notes.sh` extracts it: keep links inline (link definitions at the bottom of this
file do not travel with a section), group entries under `###` headings, and never put another
`## ` line inside a version. New entries go under `## [Unreleased]`; `tools/ci/bump-version.sh`
moves them into a version section and dates it at release.

Upstream pull request numbers refer to [Lichii](https://github.com/zortos293/Lichii).

## [Unreleased]

### Added

- FLUID presenter (`EDEN_NX2_FLUID=1`, opt-in, experimental): frames are shown on the display's vblank clock, and frame generation records a prediction for each slot, so generated frames get even 16.7 ms pacing. On the Arc 140T the title screen showed 601 of 601 slots on time (p50 16.7 ms, 100% within 2 ms of the median), and Hyrule Field had p5 16.4, p50 16.7 and p95 33.5 ms with 75% of frames within 2 ms, against 33 to 54% without it. With `EDEN_NX2_CADENCE` it also locks real frames to their slot. See [docs/switches.md](docs/switches.md).
- Controllers: player 1 is configured automatically for a connected gamepad on start and when one is plugged in. Turn it off in the advanced input settings.
- Help > Report a Problem writes a zip with the log and system details, scrubbed of user names and paths, and opens a prefilled GitHub issue; a link over 2000 characters is retried shorter and then copied.
- `eden-cli --warm-pipelines <title id>` builds a title's cached shader pipelines into the driver caches without booting the game or needing the game file. On the Arc 140T it built 4,515 pipelines and programs in 155 s. See [docs/warm-up.md](docs/warm-up.md).
- Opt-in experiments, all off by default and described in [docs/switches.md](docs/switches.md): `EDEN_NX2_GM=15` (bit 4: loads through a loaded pointer skip the run-time buffer search), `EDEN_NX2_UMA` bits 32 and 64 (copies of command list pages in runs, batched buffer copies), `EDEN_NX2_EARLY_SUBMIT`, `EDEN_NX2_EARLY_PAGES` and `EDEN_NX2_EARLY_AFTER` (early read-back downloads), `EDEN_NX2_SHADER_REUSE=1` (graphics pipelines reuse translated shaders, from upstream pull request [#1378](https://github.com/zortos293/Lichii/pull/1378)), and `EDEN_NX2_FLUID_QUEUE` (a queue of its own for the FLUID screen copy; a net loss on the Arc 140T, kept for testing).
- `EDEN_NX2_CACHE_SAFE=1` (opt-in): on Intel's Windows driver, reads of the driver pipeline cache wait for running builds, which avoids a driver crash (IGCIT #1614), and unchanged cache saves are skipped.

### Changed

- Faster boot: the Vulkan check process is skipped when it passed with the same drivers, a direct game launch leaves the Vulkan device list to the settings, a NUP's table of contents is read once per session, and boot phases are timed in the log.
- Ocarina of Time's first boot on an Intel or AMD integrated GPU logs one line with what the game presets did.
- The swapchain logs its present mode, and the Vulkan instance logs what it asks for and lists no layers when none is asked for.
- The shader tool `tools/shader-sweep` reads `EDEN_NX2_GM` like the emulator.

### Fixed

- `eden-cli` output redirected to a file or a pipe is kept instead of lost.
- The Report a Problem window compiles again (a name shadowed the dialog's form layout).

## [20.0.2]

### Fixed

- Linux builds: the generated frame-extrapolation shader header needed `<cstdint>` (GCC).
- A merged unit test referred to a removed parameter.
- Releases publish every platform that built; a missing Windows binary still blocks the release.

## [20.0.1]

### Changed

- `EDEN_NX2_GM=11` is now on by default only for Ocarina of Time (0400486024DEC000), the one game it is verified in, instead of every game through `nx2_switches.txt`. An explicit `EDEN_NX2_GM` still wins. Known issue tracked in #6.

## [20.0.0]

The first Guarana release: upstream Lichii `nx2` at `e633e165f7`, ten upstream pull requests merged
early, three Intel Arc driver fixes, and this fork's own work for integrated GPUs. All numbers below
come from one laptop.

### Highlights

- **Global-memory routing** (`EDEN_NX2_GM=11`, opt-in): 14.75 → 22.83 game fps in Ocarina of Time's
  Hyrule Field with frame generation off. The foliage mesh draw went from 15.7 to 5.8 ms of GPU time.
- **Extrapolated frame generation** for Ocarina of Time (`EDEN_NX2_EXTRAPOLATE=1`, opt-in,
  experimental): generated frames are predicted forward from the game's motion vectors, with the HUD
  masked.
- **iGPU power policy** (`EDEN_NX2_POWER_POLICY=igpu`, opt-in, experimental, Windows).
- **Intel Arc fixes**: light beams, flashing lights and NaN bloom, native float atomics.
- **Tuned builds**: clang-cl ThinLTO, PGO trained on Ocarina of Time, x86-64-v3.

### Measured

Core Ultra 9 285H with Arc 140T iGPU, Intel driver 32.0.101.9175, 32 GB RAM; Ocarina of Time. The
switch A/B was measured at the Hyrule Field overlook. Game fps counts the frames the game renders,
not generated frames.

- `EDEN_NX2_GM=11` against no switch, frame generation off: 14.75 → 22.83 game fps; foliage mesh draw
  15.7 → 5.8 ms.
- Play config (frame generation 3x toward 60): an earlier unreleased build read 19.6 game fps, locked
  at 50 ms frames. v20 read 26.7 game fps live in Castle Town and 28 in the field. These readings are
  not a same-spot A/B yet.

### Upstream: Lichii `nx2` at `e633e165f7`

Picks up 73 newer upstream commits, among them:

- Ocarina of Time white flash fixed: a deleted buffer is cleared from every channel's bindings
  ([Lichii #1459](https://github.com/zortos293/Lichii/pull/1459)).
- Dark water fixed: light-table shadow maps are bound through descriptor arrays, and pointer words
  bind no texture ([Lichii #1485](https://github.com/zortos293/Lichii/pull/1485),
  [Lichii #1523](https://github.com/zortos293/Lichii/pull/1523)).
- GPU hang with native mesh shaders fixed: the task program's mesh workgroup count is zeroed before
  the guest runs ([Lichii #1492](https://github.com/zortos293/Lichii/pull/1492)).
- Cheaper queries, fences, draws and uploads on the GPU threads
  ([Lichii #1515](https://github.com/zortos293/Lichii/pull/1515),
  [Lichii #1516](https://github.com/zortos293/Lichii/pull/1516),
  [Lichii #1519](https://github.com/zortos293/Lichii/pull/1519),
  [Lichii #1520](https://github.com/zortos293/Lichii/pull/1520),
  [Lichii #1521](https://github.com/zortos293/Lichii/pull/1521)).
- Dynarmic x86-64 code generation improvements
  ([Lichii #1531](https://github.com/zortos293/Lichii/pull/1531)).
- Shader translation: SSA phis are created only when needed; upstream reports 19-23% less translation
  time ([Lichii #1468](https://github.com/zortos293/Lichii/pull/1468)).
- Clean shutdown: the emulator waits for a teardown that is still making progress instead of
  terminating it ([Lichii #1576](https://github.com/zortos293/Lichii/pull/1576)).

### Upstream pull requests merged early

- Descriptor flush: texture descriptors are flushed one by one when the pool runs past the GPU
  address space ([Lichii #1155](https://github.com/zortos293/Lichii/pull/1155)).
- Ocarina of Time freeze on ocarina warps fixed: GPU memory registry listeners run outside their
  locks, in order ([Lichii #1218](https://github.com/zortos293/Lichii/pull/1218)).
- Pipeline-rebuild stutter fixed: a masked key keeps the graphics record it already has
  ([Lichii #408](https://github.com/zortos293/Lichii/pull/408)).
- Device memory clamps the size of its cold-memory hints
  ([Lichii #1568](https://github.com/zortos293/Lichii/pull/1568)).
- Shutdown: AM releases the application process it never took
  ([Lichii #1417](https://github.com/zortos293/Lichii/pull/1417)).
- Shader warm-up: the compute precache marker follows the file's contents and the shader lowering
  ([Lichii #1564](https://github.com/zortos293/Lichii/pull/1564)).
- Descriptor reuse: each draw's sampled textures and image descriptors are kept across frames
  ([Lichii #1447](https://github.com/zortos293/Lichii/pull/1447)).
- Guard-house GPU out-of-memory fixed: the texture cache's retire scan resumes where it stopped, so
  unused render targets behind protected ones are retired
  ([Lichii #1571](https://github.com/zortos293/Lichii/pull/1571)).
- iGPU ASTC memory: ASTC textures with uneven mips stay in BC on integrated GPUs and cards under
  6 GiB ([Lichii #948](https://github.com/zortos293/Lichii/pull/948)).
- A suspended command-list copy is polled without a kernel call each time
  ([Lichii #933](https://github.com/zortos293/Lichii/pull/933)).

### Intel Arc fixes

Ported from [Fritchii-Intel-Fixes](https://github.com/LuckyLuckLucky/Fritchii-Intel-Fixes), patches
0001, 0003 and 0010.

- **0001, light beams:** the dynamic primitive topology follows the pipeline's tessellation stages,
  which removes broken light beams and stray triangles on Arc.
- **0003, shuffle bounds:** subgroup shuffles stay in bounds on host subgroups narrower than the
  guest's warp, which fixes flashing lights and NaN bloom on Arc.
- **0010, native float atomics:** 32-bit float storage atomic adds use `VK_EXT_shader_atomic_float`
  where the driver supports it, instead of compare-and-swap loops.

### Guarana changes

- **Global-memory routing (`EDEN_NX2_GM`, opt-in).** Cheaper run-time routing of the shader memory
  accesses that stay global. Bit 1 computes buffer base addresses once per invocation instead of in
  every access, bit 2 makes a load search only the pointer buffers its address is built from, and
  bit 8 (with 2) returns zero when the address is in none of those, instead of falling back to a
  search of every buffer. `11` sets all three.
- **Extrapolated frame generation (`EDEN_NX2_EXTRAPOLATE=1`, opt-in, Ocarina of Time).** Generated
  frames are predicted after the real frame from the game's motion vectors and depth, instead of
  interpolated before it. HUD pixels stay in place, camera cuts show the real frame alone, and HDR
  output pauses it (the pass writes SDR). Needs **Enable Frame Generation**.
- **Present spacing fix.** Generated frames are spaced across the real frame interval instead of
  sent back to back, and the last predicted frame leaves the next real frame free to show as soon as
  it is ready.
- **Red/blue fix in generated frames.** The extrapolation pass writes its output without a declared
  format; declaring one on the B8G8R8A8 swapchain view swapped red and blue.
- **iGPU power policy (`EDEN_NX2_POWER_POLICY=igpu`, opt-in, Windows, integrated GPUs).** For the
  game session, CPU performance boost is turned off and the minimum processor state lowered in the
  active power plan, so more of the shared package power can go to the GPU. The saved values are
  written back at game stop or exit, and at the next start after a crash.
- **Contributor tools.** `tools/bench` (new): a cross-machine benchmark with scenes, suites, profiles,
  a scene check and privacy-scrubbed JSON rows. `tools/nx2/pipeline_stats.py` (new) prints what the
  driver's compiler made of the costliest shaders.

### Builds

- Windows x64 release builds use clang-cl with ThinLTO, PGO from a profile trained on Ocarina of
  Time, and `x86-64-v3` code. They need a CPU with AVX2.
- v20 keeps the upstream names: the program is `lichii.exe`, and its data lives in
  `%APPDATA%\lichii`, the same folder upstream Lichii uses.

### Known issues

- **Micro-stutter in generated-frame pacing.** A fix is in progress: a vblank-locked 16.67 ms
  presenter (`EDEN_NX2_FLUID=1`, experimental).
- **Shader-build hitches on first visits.** Intel's driver takes 0.4-1 s to compile one pipeline.
- **Intel driver crash risk when the pipeline cache is saved during builds**
  ([IGCIT #1614](https://github.com/IGCIT/Intel-GPU-Community-Issue-Tracker-IGCIT/issues/1614)). A fix
  is in progress.
- **16 GB shared-memory devices are below the comfortable floor.** Switch 2 titles use a 12 GiB guest
  memory layout.
- **`EDEN_NX2_GM=11` is measured in Ocarina of Time only.** If another game renders wrong with it,
  remove the switch and report the game.
