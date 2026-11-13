# Headless pipeline warm-up

`eden-cli --warm-pipelines <title id>` builds a title's cached shader pipelines into the GPU driver's caches without booting the game and without the game file. Run it after an install or a driver update, so the first start of the game does not pay for the driver compiles (measured on the Arc iGPU: async pipeline builds take 400 to 1000 ms each on Intel's driver, and the 6007 cached pipelines of Ocarina of Time take 5.85 s at boot).

Status: written against the tree without a build; the lead's first run is the test. See "Risks" below.

## Command line

```
eden-cli --warm-pipelines 0400486024DEC000 [-c <config file>] [-n]
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
   - cached pipelines an earlier warm-up or precache already built for this driver (`precache-graphics.bin`, keyed by driver version, device id and cache version) are skipped, so a second run is quick;
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

## When the launcher calls it

- After installing or first importing a title, if it has a shader cache folder (a title nobody ever started has no records: the run ends at once with `0 cached graphics pipelines`).
- After a GPU driver update. The driver cache, the driver blobs and `precache-graphics.bin` are stamped with the driver version and device id; a new driver invalidates them, and this run rebuilds all of it. Detect it by comparing the driver version the launcher knows with the one it saw at the last warm-up.
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
- The Qt frontend does not have the option; it is a mode of the SDL frontend `eden-cli`.
