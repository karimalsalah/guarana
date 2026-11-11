# Lichii benchmark

`bench.py` runs one game scene the same way on any machine and writes one JSON row per run, so
results from different PCs, drivers and settings can be compared. It drives the emulator through
its debug server (`EDEN_NX2_DEBUG_SERVER`, see `docs/nx2/debug-mcp.md`).

Needs Python 3.9 or newer and nothing else. Pillow is used for the scene check when installed; a
built-in PNG decoder does the same job without it. Collected results are in [RESULTS.md](RESULTS.md).

## Run

Close the emulator first, plug the machine in, and leave it alone while it runs.

```
python -I tools/bench/bench.py run \
    --exe <path to lichii> --game <path to the game> \
    --scene tools/bench/scenes/oot-hyrule-field.json \
    --save-backup <folder with the scene's save> --reference <your reference screenshot> \
    [--variant tools/bench/variants/baseline.json --variant <other.json> ...] \
    [--sustain-min 5] [--out tools/bench/results]
```

Add `--dry-run` to print every step without touching anything. For each variant the tool:

1. refuses to start if the emulator already runs or the debug port is in use (one instance only);
2. replaces `<user-dir>/nand/user/save` with a copy of `--save-backup`, so every run loads the same
   save (your own saves are moved aside first and put back when the run ends);
3. writes the variant's settings into `qt-config.ini` (`key=value` plus `key\default=false`) and
   forces windowed mode; the file is restored byte for byte afterwards, also after a crash or Ctrl+C;
4. starts the emulator with the debug server and the variant's environment switches, waits for
   presented frames, plays the scene's timed pad inputs and lets the scene settle;
5. takes a screenshot and compares it with the reference (see "The scene check");
6. measures three windows (15 s each) and keeps the one with the median fps;
7. with `--sustain-min N`, samples every 10 s for N minutes: GPUs that lose clock or power budget
   as they heat up show it here and not in the first minute;
8. reads `power`, `memory` and `gpu_costs` from the debug server when the build has them;
9. takes a second screenshot, checks it again and saves it under `results/shots/`;
10. stops the emulator (always, whatever failed) and appends the row to
    `results/<date>-<machine-id>.jsonl`.

While the emulator runs, a watchdog stops it if free RAM falls under 2 GB (`--ram-floor-gb`), and
the CPU time used by every other process is measured. Every wait has a timeout.

`--user-dir` defaults to a `user` folder next to the emulator, else the per-user data folder
(`%APPDATA%\lichii`, `~/.local/share/lichii`). Paths may use `%VAR%`, `$VAR` and `~`.

### Scenes and variants

A scene (`scenes/*.json`) names the game, the pad inputs that reach the spot
(`[{"wait_s": 10, "buttons": ["A"], "frames": 6}, ...]`: wait, then hold for that many presented
frames; `left_stick` works too), the settle time and the reference image. Its `notes` say what the
save must contain.

A variant (`variants/*.json`) is what you compare: `settings` (`{section: {key: value}}` of
`qt-config.ini`; `null` hands a key back to its default) and `env` (environment switches).
Without `--variant` the baseline runs: your settings as they are, windowed.

### The scene check

Menu navigation by timed inputs sometimes ends in the wrong save or on a menu, and such a run
measures something else. Each run's screenshots are shrunk to 32x18 and compared with the
reference; a mean absolute difference of 30 (of 255) or more makes the row invalid.

The reference is a screenshot of your own save, so record it once:

```
python -I tools/bench/bench.py run ... --reference tools/bench/reference/oot-hyrule-field.png --record-reference
```

Open the image and check that it shows the scene, then run again without `--record-reference`.
The recording run's rows are invalid on purpose: nothing verified them.

## What a valid row is

A row has `"valid": true` only when all of these hold; otherwise `invalid_reasons` says why:

| Reason | Meaning |
|---|---|
| `wrong_scene` | a screenshot differed from the reference (before or after measuring) |
| `scene_unverified` | there was no reference image to compare with |
| `reference_recorded` | this run wrote the reference |
| `wrong_title` | the emulator reported another game than the scene's `title_id` |
| `background_cpu` | other processes used more than 10% of the machine's CPU capacity during a window (`--max-other-cpu-pct`); an update or indexer in the background is enough to spoil hours of runs |
| `memory_watchdog` | free RAM fell under the floor and the emulator was stopped |
| `error` | boot, input or measurement failed or timed out; see `error` |

`warnings` do not invalidate a row: `on_battery`, `no_power_reading` (the build has no `power`
method, or no sensor), `background_cpu_not_measured` (the host cannot report CPU times).

Invalid rows stay in the file; `table` shows the newest valid row per scene and variant.

## Row schema (`schema_version` 1)

One JSON object per line. `null` means "not measured".

| Field | Type | Content |
|---|---|---|
| `schema_version` | int | 1 |
| `timestamp_utc` | string | `YYYY-MM-DDThh:mm:ssZ`, when the variant started |
| `machine_id` | string | 10 hex digits: SHA-256 of the lower-cased CPU name and sorted GPU names |
| `scene`, `title_id` | string | scene name and 16-digit program id |
| `variant` | string | variant name |
| `variant_settings` | object | `{section: {key: value}}` as written to the ini |
| `variant_env` | object | environment switches |
| `emulator` | object | `exe` (file name) and, from `status`, `title`, `program_id`, `build`, `renderer` |
| `game` | string | file name of the game |
| `hardware` | object | `cpu`, `cpu_cores`, `cpu_threads`, `gpus` (`[{name, driver_version}]`), `ram_gb`, `os`, `os_build`, `display` (`WxH@Hz`), `power_source` (`AC`, `battery`, `unknown`), `battery_pct`, `power_plan` |
| `valid` | bool | true exactly when `invalid_reasons` is empty |
| `invalid_reasons`, `warnings` | string lists | see above |
| `error` | string or null | what failed |
| `scene_ok` | bool or null | scene check result; null when not checked |
| `scene_diff` | number or null | largest difference to the reference, 0 to 255 |
| `fps`, `mean_ms`, `p50_ms`, `p90_ms`, `p99_ms`, `max_ms`, `stutters`, `vsync_buckets` | number or null | the median window's `frame_times` values |
| `windows_fps` | number list | fps of the three windows, in order |
| `window_s` | number | window length |
| `sustained` | object or null | `minutes`, `interval_s`, `fps_min`, `fps_avg`, `fps_first`, `fps_last`, `watts_avg`, `samples` (`[{t_s, fps, p50_ms, p99_ms, other_cpu_pct, package_w?, cpu_w?, gpu_w?}]`) |
| `watts` | number or null | mean package power over the three windows |
| `power` | object or null | mean `package_w`, `cpu_w`, `gpu_w` |
| `memory`, `gpu_costs` | object or null | the debug server's answers, as they are |
| `system` | object | `other_cpu_pct` (other processes, mean of the three windows, percent of all cores), `other_cpu_max_pct` (worst window or sustained sample), `other_cpu_limit_pct`, `emulator_cpu_pct`, `ram_free_min_gb` |
| `screenshot` | string or null | path under the results folder |

`python -I tools/bench/bench.py validate <file.jsonl> ...` checks rows against this schema (types,
consistency of `valid`, the median, the machine id, no home directory paths) and exits 1 on any
problem. CI runs it on contributed rows.

## Privacy: what is collected

Only what the table above lists. In particular a row contains:

- CPU and GPU model names, GPU driver version, RAM size, OS name and build, display mode,
  AC or battery, power plan name;
- file names (not paths) of the emulator and the game;
- timings, power and memory figures, and the settings you chose to compare.

It contains no user name, host name, serial number, MAC or IP address, and no folder paths. Before
a row is written, every string in it is scrubbed: home directories become `~`, the user name
`<user>`, the host name `<host>`. `validate` refuses rows in which a home directory path is left.
The machine id is derived from model names only, so two PCs of the same model share it.

Nothing is uploaded. Rows and screenshots stay in `results/` until you send them; screenshots
(`results/shots/`) and your reference image are git-ignored. Read your `.jsonl` before you submit it.

## Submit a row

1. Record your reference and run at least the baseline, ideally with `--sustain-min 5`, on AC power.
2. `python -I tools/bench/bench.py validate tools/bench/results/<your file>.jsonl`
3. `python -I tools/bench/bench.py table tools/bench/results --out tools/bench/RESULTS.md`
4. Open a pull request with your `results/<date>-<machine-id>.jsonl` (new file, or lines appended
   to your earlier one) and the regenerated `RESULTS.md`. Mention anything a reader should know:
   thermal mode, external display, a save that differs from the scene's notes.

Do not edit rows by hand, and keep the invalid ones: they cost nothing and show what went wrong.

## Compare

```
python -I tools/bench/bench.py compare results/<a>.jsonl results/<b>.jsonl
python -I tools/bench/bench.py table results [--all] [--out RESULTS.md]
```

Both print markdown: variant, fps, p50, p99, sustained min / avg fps, watts and `scene_ok`.
`compare` gives a percentage only where both rows are valid.

## Tests

`python -I tools/bench/test_bench.py` runs without the emulator: the run is tested against a fake
debug server.
