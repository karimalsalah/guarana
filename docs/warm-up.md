# Headless pipeline warm-up

`guarana-cli --warm-pipelines <title id>` (`eden-cli` before v20.2) builds a title's cached shader pipelines into the GPU driver's caches without booting the game and without the game file. Run it after an install or a driver update, so the first start of the game does not pay for the driver compiles (measured on the Arc iGPU: async pipeline builds take 400 to 1000 ms each on Intel's driver, and the 6007 cached pipelines of Ocarina of Time take 5.85 s at boot).

Status: measured on the Arc 140T, a full run built 4,515 pipelines and programs in 155 s. See "Risks" below.

## Command line

```
guarana-cli --warm-pipelines 0400486024DEC000 [-c <config file>] [-n]
```

- `<title id>`: 16 hex digits (`0x` allowed): the folder name under the shader directory, `<shader dir>/<title id>/` (`lichii-cache.v2.bin`, `vulkan_pipelines.*`, the driver blobs).
- `-c <file>`: the configuration to take the settings from. Without it, the global one. Pipelines are keyed by settings (resolution scale, GM, renderer options, ...), so a launcher that keeps a per-title configuration passes that file, or the cached pipelines built are not the ones the game asks for.
- The renderer must be Vulkan with `use_disk_shader_cache` on; otherwise it exits 1 at once.
- It needs a graphics session (a hidden window is made for the Vulkan surface): not a service or ssh session without a display.

## What it does

1. Lowers its own priority to below normal (the shader workers already run at low thread priority inside that), hides its window (`EDEN_NX2_HIDE_WINDOW`).
2. Creates the kernel's device memory, the GPU and the Vulkan renderer for the title id (`System::SetupHeadlessVideo`), applies the game presets and overrides for that id, as a boot does. No guest process, no game file, no keys.
3. Calls the renderer's own disk-cache load (`LoadDiskResources`) with the headless warm-up flag set (`ShaderWarm::SetWarmOnly`), which makes it the shader precache that the "Precache Shaders" loading screen runs, plus:
   - the pipelines that sessions used are built first (the boot builds), all the others as the precache's background jobs, and the compute programs of `warm-compute.bin` when the title has that file;
   - cached pipelines an earlier warm-up or precache already built for this driver (`precache-graphics.bin`, keyed by driver version, device id, cache version and build variant) are skipped, the used (boot-build) ones too, and the boot builds are recorded in that list as well, so a second run builds only what is new;
   - the precache builds in chunks of 500: between chunks nothing builds, the driver cache is saved and then the list is written, so a run that is killed starts again from its last chunk (the list never names a pipeline the saved driver cache lacks);
   - it waits for the background prebuild too, and saves the driver pipeline cache with all of it in.
4. Shuts the renderer down (that saves shader cache v2, the used list and the driver cache again) and exits.

No new compiler path: it is the boot's `LoadPipelinesV2` / `PrecacheShaders` with the flag. The cache is never used for a draw, only built into.

## Output

Progress goes to stdout, one line per 5% of each stage; logs go where the emulator's usual log goes.

```
warm: title 0400486024dec000, renderer ready after 1.4 s
warm: pass 1 boot-builds 0% (0/1346) 1.9 s
warm: pass 1 boot-builds 5% (70/1346) 2.6 s
...
warm: pass 1 boot-builds 100% (1346/1346) 9.8 s
warm: pass 1 precache 5% (230/4660) 11.0 s
...
warm: done after 142.3 s: 6006 cached graphics pipelines, 0 already built for this driver, 0 left
```

The last line is `warm: done|incomplete|stopped|nothing built after <s> s: <graphics pipelines for this device>, <already built for this driver>, <left>`. A launcher can show the percentage lines as they come; "pass N" counts the extra passes (below).

## Exit codes

| Code | Meaning | What to do |
|---|---|---|
| 0 | Everything cached is built into the driver's caches (or was already). | Done. |
| 1 | Bad title id, not the Vulkan renderer, disk shader cache off, or the renderer could not be created. | Fix the configuration; see stderr. |
| 3 | Nothing was built: the last start of this title's cache ended while it was building (a driver crash or kill), and the cache's own guard builds none that time. The run cleared the guard. | Run it again once. If it ends with 3 repeatedly, a driver build crashes: leave the title to the normal boot. |
| 4 | Stopped (Ctrl+C, or the launcher ended the process politely), or cached pipelines are left for want of room. | Run it again: the next run starts from what was built. |

SIGINT is handled: running builds end, the caches are saved, then the process exits with 4. A hard kill leaves the append-only shader cache intact (a cut-off tail is dropped at the next read) but loses the driver cache builds since the last save, and leaves the "ended while building" guard, so the next run, and the next boot, build nothing once (code 3).

The lists of built pipelines (`precache-graphics.bin`, `precache-graphics-libraries.bin`) and `warm-compute.done` say what the driver cache file holds, so they follow it. They are written only after a save of that file succeeded (a failed save writes none; the next run tries again). A list also carries the build variant, a hash of what decides the SPIR-V and the pipelines the driver caches beyond the driver itself: the variant `warm-compute.done` carries (the SPIR-V emission stamp, so an update that may emit other SPIR-V rebuilds once, mostly from the driver's cache; the resolution scale; the loop safety, bloom and debug settings) and the `EDEN_NX2_*` switches known to change them (`UNDEF_ZERO`, `NO_TENSOR`, `GM`, `TASK_DENSITY`, `SW_MATRIX_SHARED`, `PREPASS_INVARIANT`, `NO_FLOAT_ATOMICS`, `FORCE_MESH_FALLBACK`, `GPL_FAST_LINK`, `GPL_OPTIMIZE`); a list of another variant reads as empty. `warm-compute.done` does not carry those switches. When the driver cache file is missing or empty at load, or the load deletes it (cut short, of another version, rejected by the driver), the lists and `warm-compute.done` are removed too, and everything is built again.

## When the launcher calls it

- After installing or first importing a title, if it has a shader cache folder (a title nobody ever started has no records: the run ends at once with `0 cached graphics pipelines`).
- After a GPU driver update. The driver cache, the driver blobs and `precache-graphics.bin` are stamped with the driver version and device id (the list also with the build variant, so a new resolution scale or an emulator update that changes the SPIR-V rebuilds too); a new driver invalidates them, and this run rebuilds all of it. Detect it by comparing the driver version the launcher knows with the one it saw at the last warm-up.
- After an emulator update that changes the shader recompiler (the lowering stamp): cached programs are lowered again by the next boot anyway; the warm-up does that, then builds.
- Not while a game is running: it takes the cores. Run it with the emulator closed. It takes the title cache's lock, and another emulator loading the same title makes it exit with code 3.

Use a time limit that matches the machine; the first run is the long one (minutes for a thousand-pipeline title on a cold driver). Show it as a background task; the launcher should pass the same `-c` file as it passes to boot the title.

## Limits and risks

- The renderer is created without a guest process. Everything in `SetupHeadlessVideo` is what `Load` does for the GPU, but `Load` was the only path that ever ran it; a subsystem that assumes a process would fail at the renderer's creation (exit 1) or at shutdown.
- It treats the title as a Switch 1 program (`CurrentProgramIsSwitch2` false). Nothing the shader pipelines are built from reads it; the ASTC and memory-layout choices that do are not made.
- Graphics pipelines only come from the cache records. A pipeline whose record was never written (the title never drew it), or a pipeline built with other settings than the ones given, is built at play time as before.
- At most 1 GiB of captured shader environments are held for the precache and 256 MiB for the prebuild per pass. A bigger cache is built in extra passes (up to 8, each takes what the last left); if a pass takes nothing new the run ends with code 4.
- The driver cache is saved when the run ends; a crash of the driver in the middle loses the run's builds (not the cache before it).
- On Intel's Windows driver the driver cache save waits for the running builds (`Device::SerializesPipelineCacheReads`); the prebuild is held until the precache's save, as at boot.
- The Qt frontend does not have the option; it is a mode of the SDL frontend `guarana-cli` (`eden-cli` before v20.2).

## Boot without the wait (`EDEN_NX2_BOOT_ASYNC=1`)

What the boot of a title waits on, in `PipelineCache::LoadDiskResources` / `LoadPipelinesV2` (`video_core/renderer_vulkan/vk_pipeline_cache.cpp`), in order. `EmuThread::run` (`qt_common/render/emu_thread.cpp`) starts the guest only when this returns.

1. The shader cache log is read and its payload hashes checked on the shader workers (`ReadLogFile`), the programs are registered, the used list is read.
2. The driver pipeline cache file is read and handed to the driver (`LoadVulkanPipelineCache`, hundreds of MB on a big cache). With the switch it runs on another thread beside step 1.
3. The records are planned (stale programs lowered again, keys moved, the used list applied) and every boot build is submitted as a Soon job.
4. `boot.Wait()`: every boot build is translated and built. This is the bulk: 3388 pipelines at about 1.7 ms of driver work each. The "disk cache read, N of M pipelines built so far" line is printed only when step 3 is done, just before this wait, so the 2 s before it is steps 1 to 3 and the rest of the 5.9 s is this wait plus step 5.
5. The driver cache is read back and written (`SerializeVulkanPipelineCache`, after the wait).
6. Only with the precache setting or the warm-up: `PrecacheShaders`.

This split comes from reading the code; the `Boot timing:` lines added inside the load (below) give the real numbers.

With the switch, step 4 waits for the compute programs only (they are stored in a map the GPU thread uses without a lock). The cached graphics pipelines the boot builds go to the background prebuild's queue (`ScheduledPrebuild`) as Soon jobs, so they run before any background prebuild job, in the order recent sessions first used them (below). A draw that needs one finds it there (`CurrentGraphicsPipelineSlowPath` takes it, waiting for that single job if it is running, or running it at once if queued), so a pipeline is never built twice. Step 5 is skipped; the driver cache is saved when the last boot build ends on a driver that can read it during builds (not Intel's on Windows, `Device::CanReadPipelineCacheDuringBuilds`), otherwise at exit as before, and during play as before.

The `.booting` guard (and its lock) is kept until the last queued build ends, so a driver crash in one of them is still found by the next boot. A clean stop (close, reload) removes it. The log is kept in memory until its last queued record is translated (it is read in place, not copied).

The boot's Vulkan instance is created on another thread at the start of `System::Load`, before the kernel and the application process (RomFS) are set up, and taken by the renderer when it is made (`PrepareRenderer`, `PrecreateInstance`). It uses its own library handle and dispatch table. If the window type or validation setting differs, or creation failed, the renderer creates its own as before. The overlap is only as long as the kernel and process set-up (about 0.5 to 1 s of the 4.15 s measured); the rest of the instance time is the Intel driver's own (its 450 MB shader cache is read at instance creation and the time grows with its size). To start earlier, the frontend can call `PrecreateInstance` as soon as the launch is requested.

Not covered, and why:

- Compute programs still build before the guest starts. Moving them needs the same hand-over for `compute_cache` that the graphics side has through the prebuild queue.
- A cached record that translates but lacks values (`incomplete`) is not tombstoned by the async path; it stays in the log and is tried again at the next boot. (The synchronous path removes it.)
- With the guest running, the shader workers build the queue at low thread priority beside the guest cores: expect more CPU contention in the first seconds.

## Order and new pipelines

Each session writes, beside its Used record, a `UseOrder` record (record kind 10, same key; builds that predate it ignore it) with, for every pipeline it used: the milliseconds since its cache load began at the first use, and whether the game had to wait for the pipeline (it was not built before the draw asked for it). It is written with the used list (when a new pipeline is first used and 15 s have passed, and at exit).

- Boot builds are submitted in the order of the newest session that recorded the pipeline's first use, so the start of play is covered first. Pipelines no session timed keep the cache's order, after the timed ones. (Not applied to compute programs.) This applies without the switch too.
- Pipelines a recent session built during play are added to the boot builds even when that session was too short to write a used list (under 64 pipelines); the log line `pipelines recent sessions built during play` says how many. They are also what the next `--warm-pipelines` builds first, as part of the used list.
- Records are kept for the same number of sessions as the used list. The first-use time is looked up by pipeline key, so after the cache moves records to new keys (a new lowering) the timing of the moved ones is lost for one session.

## Warm-up reruns

`--warm-pipelines` skips every cached pipeline already in `precache-graphics.bin` for this driver, the used ones too, and records boot builds in it after the driver cache is saved (see above). The `warm:` progress lines and the final line keep their format. A rerun with nothing new reads the log, finds nothing to build and ends.

## Measuring boot and warm-up

Arc 140T setup of [switches.md](switches.md), Ocarina of Time (0400486024DEC000), plugged in, the same window size and save. Run each variant three times, alternating A (no switch) and B (`EDEN_NX2_BOOT_ASYNC=1`), with `EDEN_NX2_STUTTER_TRACE=1` in both. Cold = delete the title's shader cache folder and the driver's own cache (the Intel shader cache folder) before the run; warm = a second run right after.

1. Boot timing: `grep "Boot timing" eden_log.txt`. The lines to read, in A and B:
   - `vulkan: instance created` (4150 ms in A) and, in B, `vulkan: instance creation started ahead of the renderer` (its time against `core: load started`) and `vulkan: instance taken from the early creation ... took N ms`, the wait left when the renderer needed it.
   - `shaders: cache files and driver cache read, building` (steps 1 and 2; B reads the driver cache beside the log: expect the longer of the two, not the sum), `shaders: shader cache log read and checked`, `shaders: driver pipeline cache loaded`.
   - `shaders: planned, N compute programs and M graphics pipelines to build`, `shaders: M graphics pipelines queued for the background, waiting for N compute programs`, `shaders: boot builds done` (A: all of them; B: the compute programs only), `shaders: driver pipeline cache saved` (A only), `shaders: LoadPipelinesV2 done`.
   - `shaders: disk cache load done` and `guest: GPU started, guest cores released` (12.26 s in the measurement), `frontend: first guest frame presented` (19.56 s). The target: the release time falls by steps 4 and 5, and the gap from release to first frame does not grow in B.
   - `frontend: loading screen hidden, game shown`: the loading screen's 0.5 s fade is over and the render window is shown; the time the game is on screen.
   - B only: `shaders: async boot builds done` and the `Nx2Boot event=boot_async_end seconds=` line: when the queue drained. Compare with A's `shader_load_end seconds=`.
2. `Nx2Boot` events (`tools/nx2/cache_metrics.py parse <log>` keeps events it does not know): `shader_load_end` (`queued=` is the compute programs plus the graphics boot builds in both variants), `boot_async_end`, `first_frame`, with the commit and peak commit: B holds the log and the queued pipelines longer.
3. `Nx2StutterSummary` every 10 s: `builds` (pipelines built on the GPU thread, which B must not raise in the first minute), the lookup counts (`GraphicsBackground`: pipelines taken from the queue, the async boot ones included; `GraphicsPrebuilt`: found built in the cache; `GraphicsRecorded` and `GraphicsNew`: built by the draw), and the frame-time histogram. In B the first 30 s after release are the cost: read the frames over 33 ms (`Nx2Stutter` lines, top cause "pipeline wait") against A's same minute. Expect `GraphicsBackground` to carry most of what A had as `GraphicsPrebuilt`.
4. Cold, then warm: with the shader cache and the driver cache deleted (the first boot builds from nothing and writes the records), a second boot (records exist, driver cache warm: the warm case above), and a third, which uses the order and the built-in-play records the second wrote. In the log: `pipelines recent sessions built during play, N of them added to the boot builds` and, in `Nx2StutterSummary`, the builds of the third boot against the second.
5. Warm-up: `guarana-cli --warm-pipelines 0400486024DEC000` on a cleared driver cache: the final `warm:` line (155 s for 4515 pipelines and programs measured). Kill it halfway (`taskkill /f`) and run it again: the `already built` count of the final line should be about what the first run had built (to the last 500), and the run should end in about the remaining share of the time. A third run ends within seconds with `0 left`.
6. Correctness: B's first minute of play against A at the same points (`capture_frame` at three route points: no missing objects), the `Shader cache v2` lines the same in A and B except for `incomplete` records, and no `Device loss`. After killing the emulator during the async builds (`taskkill /f` in the first seconds), the next boot must log `the last boot ended while building cached pipelines; building none at boot this time`: that is the guard working.
