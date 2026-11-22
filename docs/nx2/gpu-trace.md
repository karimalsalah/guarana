# GPU traces

A GPU trace (`.lxtrace`) holds what the emulated GPU consumed over a window, in the order it consumed it, after a snapshot of the GPU state at the window's start. It records guest inputs (command words, guest memory, engine registers, maps and presents), never host Vulkan calls, so a replay runs the shader translation, the caches and the Vulkan backend of the build that replays it.

Traces can contain game code, shaders, textures, buffers and personal information. Do not publish
game-derived traces or add them to the repository. Ask a maintainer for handling instructions
before sharing the minimum necessary material, and share only what you have permission to
disclose. A private transfer does not by itself establish permission to redistribute game data.
Use synthetic traces for public examples and tests.

## Replay

```sh
guarana-cli --replay <trace> --out <dir> [--frames N] [--device N] [--no-images]
```

`guarana-cli` is the SDL frontend (`eden-cli` before v20.2). The replay needs no game, no window and no keys. It brings up the GPU on a bare `Core::System` (`System::InitializeForGpuReplay`: kernel, Host1x and the GPU, no process, audio or services), restores the snapshot and feeds the records through the GPU in order. It writes per present:

- `frame-NNNN.png`: the composited frame at the size of its largest layer (crop rectangle), through the renderer's screenshot path.
- `frames.json`: per frame its file, size, XXH3-128 hash of the RGBA bytes and two times, plus the program id and the device that replayed it. `ms` is the wall time since the previous frame was composited, so it also holds that frame's hash and PNG encoding. `replay_ms` is only the frame's own commands, composite and read-back: the number to compare two replays by.

`--no-images` writes no PNG files: `frames.json` still has every hash and time.

The window is a `VK_EXT_headless_surface` surface (`WindowSystemType::Headless`), so any Vulkan driver with that extension works, Mesa's lavapipe included. On Windows, where the loader lists no such extension (Intel's driver 32.0.101.9175 on an Arc 140T has none), the replay makes a window that is never shown and presents to it without vertical sync, with `EDEN_NX2_FLUID` and `EDEN_NX2_EXTRAPOLATE` set to 0 unless the environment already sets them.

`guarana-cli --gpu-trace-sample <trace>` writes a synthetic trace that needs no game: one Switch 2 channel whose four frames are two CPU-written images and two sets of 3D engine clears (whole and scissored). The unit tests read it back (`[gpu_trace]`). With `--no-presents` after the path it writes the same stream without its presents, which the replay must refuse (see "Recording").

## Bench: a paired A/B on the same frames

```sh
python3 -I tools/nx2/trace_replay.py bench --cli <guarana-cli> <trace> --out <dir> \
    --variant base --variant gm0=EDEN_NX2_GM=0 [--build head=<another guarana-cli>] \
    [--repeat 3] [--skip 10] [--frames N] [--no-images] [--allow-different]
```

A variant is a set of environment switches, a build, or both; the first one is the baseline. Every variant replays the trace `--repeat` times in turn (A B A B), and the report (`<dir>/bench.json`, and a table on the terminal) says for each one:

- **image**: whether its frames are the baseline's bit for bit. Where they are not, how many differ and, with images kept, the largest channel difference and PSNR of the first one.
- **noise**: some drivers do not replay bit for bit (on Intel's Windows driver every frame of two replays of the same build differs a little, PSNR 37 to 45 dB). When the baseline's own runs differ, bench takes the lowest PSNR between them, over a sample of the differing frames, as the noise floor, and reports a variant no further from the baseline than that (less `--noise-margin`, 1 dB by default) as within noise. Without images (`--no-images`) a baseline whose runs differ proves nothing, and bench exits 1.
- **time**: the median, best, worst and 95th percentile of the time a frame took to replay (`replay_ms`, or `ms` when a build in the comparison is older than `replay_ms`), without each run's first `--skip` frames, which build shaders and upload textures.

It exits 1 when a variant's frames differ from the baseline's (unless `--allow-different`) or the baseline does not repeat itself.

What it is good for, and what it is not:

- The game does not run. The scene is the same by construction, so there is no save to load, no reference screenshot to match and no run on the wrong scene.
- The time is the emulator's GPU thread and the host GPU on a fixed workload, back to back and as fast as they go. The guest CPU, frame pacing and the presenter are not in it: a change there needs `tools/bench` on the running game. A gain here is a gain in GPU-side cost, not yet in frames per second.
- A switch that changes the image on purpose (a resolution scale, a filter) differs by design: use `--allow-different` and read the numbers.
- Traces hold game data and stay on the machine that recorded them. Rows from a bench can be shared; the trace cannot.

`eden-cli --gpu-trace-sample-draws <trace>` writes one that exercises the 3D draw path: four frames of 300 overlapping quads each (a third of them indexed) drawn by a minimal Switch 2 vertex and pixel program pair, with a constant buffer update before every draw, scissor, blend, color mask and viewport changes, and clears between them.

`eden-cli --gpu-trace-sample-uploads <trace>` writes the same draws with new vertex and index data written before every eighth of a frame, so the buffer cache uploads between draws.

## Recording

A recording starts at the GPU thread's next command boundary where no channel waits on a semaphore, and ends after a number of presents (180 by default: 3 s at 60 fps). Start it with:

- the debug server: `trace_record` `{seconds?, presents?, path?}` (default 3 s, into the dump directory), `trace_stop`, `trace_status`;
- or `EDEN_NX2_GPU_TRACE=<dir>`: touching `<dir>/record.now` starts `<dir>/trace-<time>.lxtrace` (`EDEN_NX2_GPU_TRACE_PRESENTS=<n>` sets the length).

`trace_record` returns at once, and the snapshot comes first: in a 3D scene it is gigabytes and can take a minute (3 GB in Ocarina of Time's Hyrule Field), during which the game stands still and `trace_status` waits. **Keep the game open until `trace_status` shows `requested: false`.** `last_presents` then says how many presents the trace holds. A game closed during the snapshot leaves a well-formed trace with no presents: the recorder logs `holds no presents` as an error and reports it in `last_error`, and `--replay` refuses such a trace (`the trace has no frame to replay`) instead of writing an empty `frames.json`. A recording that ends short of the presents asked for logs a warning and replays up to where it ended.

What happens (`video_core/gpu_trace/recorder.cpp`):

1. **Snapshot.** The GPU split drains and the 3D and compute engines take their state back from its front. Then the recorder writes every GPU address space and its mappings, from a map log each address space keeps, and the engine and global state. GPU-written data in the caches is flushed to guest memory, and every guest page the mappings reach is copied (zero pages skipped, identical pages stored once).
2. **CPU writes.** For the window, the recorder holds a cached-page reference on every page it copied, so each guest CPU write to them takes the rasterizer write path, where it is noted after the store. Before each later GPU command, the bytes written since the previous command are recorded with their current content, so GPU results on the rest of the page stay the replay's own. Recording needs the JIT CPU backend: NCE notes a write when it faults, before the store lands, so a recording under NCE fails to start. CPU-heavy scenes run slower while recording.
3. **Commands.** Each command list is recorded when the GPU thread takes it: its GPFIFO entries, the words copied at submit and the entries still deferred. GPU memory maps are recorded when they happen, with the SMMU mapping and content of any new pages.
4. **Presents.** A composite request is recorded at the GPU tick that runs it, in order with the commands.

`guarana-cli --replay <trace> --out <dir> --record <new trace> --record-from <present>` records a replay from a present on. Replaying the new trace must give the same frames as the original from that present: that round trip checks the recorder, the snapshot and the restore against each other.

A channel that starts submitting after the snapshot ends the recording before its first submit, and a GPU shutdown ends it at once; both traces replay up to that point. Not recorded yet: SMMU unmaps (the replay keeps the stale mapping), sparse backing changes, the order in which suspended channels resume, and Switch 2 semaphore region changes after the snapshot. There is no Qt hotkey yet: use the debug server or the file trigger.

## File format

A 64-byte header (magic `LXGTRACE`, format version, header size), then one zstd stream of records: a 16-byte header (`type`, `size`) and `size` payload bytes. `video_core/gpu_trace/trace_file.h` defines every record. In order:

1. `Config`: JSON with the program id, whether it is a Switch 2 title, the memory layout and the output size.
2. Maps: `AddressSpace` (a GMMU with its page geometry), `GmmuMap`, `SmmuMap` (device pages to physical pages, so aliasing survives).
3. `Channel` per GPU channel, then `State` records: engine registers, the pusher's method state, the Switch 2 macro RAM, QMD scheduler state, syncpoints and semaphore regions (`gpu_trace/state.h`).
4. `Memory`: physical ranges and their content. Content is a `Blob` stored once and named by its XXH3-128 hash, so identical pages cost nothing after the first.
5. `SnapshotEnd`, then the stream: `Memory` (a CPU write the GPU sees from that point on), `Submit` (GPFIFO entries, prefetch words, submit-time word copies), map changes, `Present` (the layers and fences of a composite request) and `Resume`.
6. `End`.

Physical addresses are offsets into guest memory (`Core::DeviceMemory`). The replay maps them through a process page table whose addresses are those offsets.

## Lavapipe

Lavapipe creates the device (every required feature, extension and limit is there). Its gaps:

| Gap | What the replay does |
|---|---|
| Subgroups are 8 wide and can't be forced to 32 | The guest warp size is requested only where the device's subgroup size range includes 32. Warp operations (SHFL, VOTE, ballot) then run on 8 lanes and give other results than a GPU: frames that use them differ. |
| No depth bounds test | The existing fallback skips the test. |
| No ASTC | The existing CPU decode. |
| MSAA 1x and 4x only | 2x and 8x targets fail to create. |
| CPU rasterizer | Correct, slow: a trace replays at a few frames a second. |

Frames match bit for bit only between runs of the same build on the same driver. Across drivers they differ by precision, so expected frames are kept per device class.

## What a trace can't hold

- Guest CPU timing. A bug that comes from the order between the CPU and the GPU thread replays in the recorded order and may not reproduce.
- Content that never reaches guest memory: resolution-scaled images and MSAA targets that can't be downloaded. Record at 1x for exact traces.
- Driver bugs: those need the same driver to replay.
