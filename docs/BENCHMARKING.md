# Benchmarking

Every performance claim in Guarana needs numbers that someone else can reproduce. Only one device has
published numbers so far, so measuring yours is one of the most useful things you can do, and you do
not need to write code for it.

This page explains what we measure and how to make a number others can trust. The tool itself is
documented in [tools/bench/README.md](../tools/bench/README.md), and the step-by-step guide for
contributing rows is [tools/bench/CONTRIBUTING-bench.md](../tools/bench/CONTRIBUTING-bench.md).

## What we measure

| Measure | Meaning |
|---|---|
| **Game fps** | Frames the game itself presents per second, from the intervals between its presents. Generated frames are not counted. |
| `p50_ms`, `p90_ms`, `p99_ms` | Frame time percentiles of the same intervals. p99 shows the worst frames. |
| `stutters` | Intervals longer than twice the median. |
| `vsync_buckets` | The share of frames that took 1, 2, 3, or 4+ refresh intervals. |
| `watts` | Package, CPU and GPU power from the Windows energy meter, where the machine has one. |

These come from the emulator's debug server (`frame_times`, `power`), so they describe the game's
own frames, not what reaches the display. To judge the pacing of generated frames, capture presents
with a present-level tool such as Intel's PresentMon, and say which tool you used.

## Rules for a fair number

1. **Change one thing.** Same machine, scene, save, settings and driver; only the variable you test
   differs.
2. **Warm caches.** Measure after the scene's shaders are built. First-visit hitches are a separate
   measurement.
3. **Plugged in and quiet.** On battery the power limit differs. Close browsers, launchers and
   overlays; the bench marks a row invalid when other programs use more than 10% of the CPU.
4. **Same window.** The bench forces windowed mode, so window size does not change between rows.
5. **Several windows.** The bench measures three 15 s windows and keeps the median one.
6. **Keep every row.** Invalid rows stay in the file, and rows are never edited by hand.
7. **Say what you ran.** Device, driver, power mode, frame generation on or off, switches.

## Quick check: a live reading

Useful to explore, not a published number: nothing checks the scene or the save.

1. Add `EDEN_NX2_DEBUG_SERVER=127.0.0.1:6545` to `%APPDATA%\guarana\nx2_switches.txt` and start the
   game. The server listens on your own machine only.
2. Stand in your scene, leave the controls alone, and run from the repository root (Python 3,
   standard library only):

   ```sh
   python -I tools/nx2/debug_mcp.py --server 127.0.0.1:6545 \
       --call '{"method":"frame_times","params":{"seconds":10,"refresh_hz":60}}'
   ```

   The answer is JSON with `fps`, the percentiles, `stutters` and `vsync_buckets`. In Windows
   PowerShell 5.1, escape the inner quotes: `'{\"method\":\"frame_times\"}'`.
3. Remove the line from `nx2_switches.txt` when you are done.

## Full run with `tools/bench`

About an hour, most of it unattended. The bench boots the game, walks the menus with timed inputs,
checks the scene against a reference screenshot, measures, and puts back everything it changed
(saves, settings, mods, power settings), also after Ctrl+C.

1. **Prepare.** The game, and a save standing in the scene: for `oot-hyrule-field`, Hyrule Field in
   the third file-select entry. Copy that save folder somewhere; it is your `--save-backup`. Plug
   in, close other programs, pause updates.
2. **Set the common options** (PowerShell):

   ```powershell
   $common = '--exe', 'C:\path\guarana.exe', '--game', 'C:\path\game.nup', '--save-backup', 'C:\path\save-backup',
             '--reference', 'tools\bench\reference\oot-hyrule-field.png'
   ```

3. **Record your reference image** once, then open it and check it shows the scene in gameplay:

   ```powershell
   python -I tools/bench/bench.py run @common --scene tools/bench/scenes/oot-hyrule-field.json --record-reference
   ```

4. **Run the suite**, dry run first (it lists every file it will touch):

   ```powershell
   python -I tools/bench/bench.py suite tools/bench/suites/igpu-switches.json @common --dry-run
   python -I tools/bench/bench.py suite tools/bench/suites/igpu-switches.json @common
   ```

5. **Validate and tabulate:**

   ```powershell
   python -I tools/bench/bench.py validate tools/bench/results/<date>-<machine-id>.jsonl
   python -I tools/bench/bench.py table tools/bench/results --out tools/bench/RESULTS.md
   ```

6. **Open a pull request** with exactly two files: your `tools/bench/results/<date>-<machine-id>.jsonl`
   and the regenerated `tools/bench/RESULTS.md`. Describe what the rows cannot say: device model,
   performance mode or TDP, external display, anything that differs from the scene's notes.

Add `--sustain-min 5` to see whether your GPU keeps its clock once it is warm.

## Testing a switch or a setting

The shipped suite covers a set of iGPU switches. To measure something else, write a variant: a small
JSON file you keep anywhere. For example, the switch behind the README's main result:

```json
{"name": "gm-11", "env": {"EDEN_NX2_GM": "11"}}
```

and the frame generation part of the play config, as `qt-config.ini` settings:

```json
{"name": "fg-3x-60", "settings": {"Renderer": {"frame_gen": true, "frame_gen_multiplier": 3, "frame_gen_target_rate": 60}}}
```

Run them against the baseline in one go:

```powershell
python -I tools/bench/bench.py run @common --scene tools/bench/scenes/oot-hyrule-field.json `
    --variant tools/bench/variants/baseline.json --variant gm-11.json
```

Then compare two result files with `bench.py compare <a>.jsonl <b>.jsonl`. It prints a percentage
only where both rows are valid.

## Reproduce the published numbers

[RESULTS.md](../tools/bench/RESULTS.md) has no rows yet. The numbers in the README come from one
laptop (Arc 140T) and predate the public rows. Reproducing them on any device is a first
contribution we would love to merge:

| README row | Variants to run |
|---|---|
| `EDEN_NX2_GM` off → `=11`, frame generation off: 14.75 → 22.83 game fps | `baseline` and `gm-11` |
| v20 play config: 26.7-28 game fps | `gm-11` plus the frame generation settings, `EDEN_NX2_EXTRAPOLATE=1` and `EDEN_NX2_POWER_POLICY=igpu` |

## Evidence for a pull request

A pull request that claims a performance change brings:

- **A paired A/B from one session:** baseline and change run back to back on the same machine and
  scene with `tools/bench`, both rows with `"valid": true` and `"scene_ok": true`, the baseline shown
  next to the result.
- **The `compare` output** in the description, with device, driver and power mode.
- **Proof the image did not change** when the change touches rendering: the scene check, plus
  screenshots or a `capture_frame` from the debug server for the affected passes.
- **Every result, not only the good ones.** A gain on one device and a loss on another are both
  reported.

Reasoning about why something should be faster, a single live reading, or an AI tool's estimate is
not evidence. Rows from more than one device make a change far more likely to become a default.

## Privacy

A row contains model names (CPU, GPU, driver), RAM size, OS version, display mode, power state, the
settings and switches you ran, and the measurements. It contains no user name, host name, serial
number, network address or folder path: strings are scrubbed before a row is written, and `validate`
rejects a row that still holds a home directory path. Nothing is uploaded; screenshots, logs and your
reference image stay on your machine. The full list is in
[CONTRIBUTING-bench.md](../tools/bench/CONTRIBUTING-bench.md#what-a-row-contains).
