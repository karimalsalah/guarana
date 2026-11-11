# Contributing benchmark rows

You have a different integrated GPU (or driver) and want its numbers in
[RESULTS.md](RESULTS.md). About an hour, most of it unattended. Commands are run from the
repository root; [README.md](README.md) explains every option.

## 1. Prepare

- A Lichii build with the debug server, the game, and a save standing in the scene (the scene
  file's `notes` say where: for `oot-hyrule-field`, Hyrule Field in the third file-select entry).
  Copy that save folder (`<user-dir>/nand/user/save`) somewhere: it is your `--save-backup`.
- Plug the machine in, close browsers, launchers and overlays, pause updates, and do not touch
  it while it runs.
- Set the common options once (PowerShell shown; `--user-dir` only if yours is not the default):

```
$common = '--exe', 'C:\path\lichii.exe', '--game', 'C:\path\game.nup', '--save-backup', 'C:\path\save-backup',
          '--reference', 'tools\bench\reference\oot-hyrule-field.png'
```

## 2. Record your reference image

```
python -I tools/bench/bench.py run @common --scene tools/bench/scenes/oot-hyrule-field.json --record-reference
```

Open `tools/bench/reference/oot-hyrule-field.png`. It must show the scene in gameplay: not a
menu, not a cutscene, not the file select. If it does not, fix the save and record again. The
image is yours (git-ignored); do not submit it. The row of this run is invalid on purpose.

## 3. Run the suite

```
python -I tools/bench/bench.py suite tools/bench/suites/igpu-switches.json @common --dry-run
python -I tools/bench/bench.py suite tools/bench/suites/igpu-switches.json @common
```

Read the dry run first: it lists every file it will touch. The real run takes about 40 minutes
(15 variants) and prints a comparison table. Add `--sustain-min 5` if you can spare the time: it
shows whether the GPU keeps its clock once warm. Do not add `--base-ini`, `--mod` or
`--power-policy` unless you say so in the PR: they make your baseline a different one.

## 4. Validate

```
python -I tools/bench/bench.py validate tools/bench/results/<date>-<machine-id>.jsonl
python -I tools/bench/bench.py table tools/bench/results --out tools/bench/RESULTS.md
```

`validate` must print `row(s) ok`. Then read the file yourself: one JSON object per line.

A row is invalid (`"valid": false`, with the reason in `invalid_reasons`) when

- `wrong_scene`: a screenshot did not match your reference. Timed menu inputs sometimes miss;
  look at `results/shots/<row id>.png` and run the suite again;
- `scene_unverified` / `reference_recorded`: there was no reference yet, or this run wrote it;
- `wrong_title`: another game than the scene's was running;
- `background_cpu`: other processes used over 10% of the CPU during a window. Find the culprit
  (`python -I tools/bench/play.py --exe ... --game ... --dry-run` lists the busiest processes) and rerun;
- `memory_watchdog`: free RAM fell under 2 GB and the emulator was stopped;
- `error`: boot, input or measurement failed; `error` says what;
- `schema`: the row itself is malformed. That is a bug: report it.

Rerunning appends rows; `table` picks the newest valid one per variant. Keep the invalid rows in
the file, and never edit a row by hand: `validate` checks the median, the machine id and the
consistency of `valid`, and a reviewer will ask for a rerun.

Rows with warnings are accepted. `on_battery` rows are not comparable with plugged-in ones, so
rerun on AC if you can. `no_power_reading` only means the `watts` column stays empty.

## 5. Open the pull request

Commit exactly two things:

- `tools/bench/results/<date>-<machine-id>.jsonl` (a new file, or lines appended to your earlier one);
- the regenerated `tools/bench/RESULTS.md`.

In the description, give what the rows cannot say: laptop or desktop model class, thermal or
performance mode, external display, anything that differs from the scene's notes (another time
of day, other equipment), and any `--base-ini`, `--mod` or `--power-policy` you used.

Do not commit `results/shots/`, `results/logs/` or `reference/` (git ignores them). The logs are
the emulator's own and contain paths from your machine.

## What a row contains

Exactly this, and nothing else:

- **Machine**: CPU model name, core and thread counts; GPU model names and driver versions; RAM
  size; OS name, version and build; display mode (`WxH@Hz`); AC or battery, battery percent;
  the name of the active power plan; `machine_id`, a hash of the CPU and GPU model names (two
  PCs of the same model share it).
- **What ran**: time (UTC); scene name and title id; the emulator's file name, build string,
  renderer and the title it reported; the game's file name; variant name, its settings,
  environment switches, live calls and rejected shader hashes; the base ini's file name; mod
  folder names and disabled add-on names; the suite name.
- **Power policy**: the powercfg values applied and the ones they replaced.
- **Measurements**: frame times and fps (windows, sustained samples, late window); watts
  (package, cores, GPU); GPU clock and its limiter, if you passed a telemetry script; the debug
  server's `memory` and `gpu_costs` answers; the emulator's working set and private bytes; its
  window size; CPU use of all other processes together as one percentage (no process names);
  lowest free RAM.
- **Checks**: scene check result and difference, validity, reasons, warnings, the error text.
- **Trace summary** (traced variants): GPU milliseconds per shader hash pair and per render
  target format and size, frame span.
- **Names of two local files**: the screenshot and the log copy. The files themselves stay on
  your machine.

It contains no user name, host name, serial numbers, MAC or IP address, no folder paths, no
list of installed or running programs, and nothing from your saves. Every string is scrubbed
before it is written (home directory to `~`, user name to `<user>`, host name to `<host>`), and
`validate` rejects a row that still has a home directory path. A mod or add-on name you chose is
written as it is: rename the folder if its name is private.
