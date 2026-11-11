# Lichii benchmark

`bench.py` runs one game scene the same way on any machine and writes one JSON row per run, so
results from different PCs, drivers and settings can be compared. It drives the emulator through
its debug server (`EDEN_NX2_DEBUG_SERVER`, see `docs/nx2/debug-mcp.md`). `play.py` starts the
emulator for normal play on an integrated GPU (see "Play").

Needs Python 3.9 or newer and nothing else. Pillow is used for the scene check when installed; a
built-in PNG decoder does the same job without it. Collected results are in [RESULTS.md](RESULTS.md).
To add your machine's rows, follow [CONTRIBUTING-bench.md](CONTRIBUTING-bench.md).

## Run

Close the emulator first, plug the machine in, and leave it alone while it runs.

```
python -I tools/bench/bench.py run \
    --exe <path to lichii> --game <path to the game> \
    --scene tools/bench/scenes/oot-hyrule-field.json \
    --save-backup <folder with the scene's save> --reference <your reference screenshot> \
    [--variant tools/bench/variants/baseline.json --variant <other.json> ...] \
    [--base-ini <qt-config.ini to start every variant from>] \
    [--mod-library <folder of mod folders> --mod "<folder>" ...] \
    [--power-policy PERFBOOSTMODE=0 --power-policy PROCTHROTTLEMIN=5] \
    [--gpu-telemetry <script.ps1>] [--sustain-min 5] [--out tools/bench/results]
```

Add `--dry-run` to print every step without touching anything. For each variant the tool:

1. refuses to start if the emulator already runs or the debug port is in use (one instance only);
2. replaces `<user-dir>/nand/user/save` with a copy of `--save-backup`, so every run loads the same
   save (your own saves are moved aside first and put back when the run ends);
3. when the variant or `--mod` names mods: moves every mod folder in `<user-dir>/load/<TITLEID>/`
   aside and copies the named ones in from `--mod-library`;
4. with `--base-ini`, starts from that file instead of your `qt-config.ini`; then writes the
   variant's settings (`key=value` plus `key\default=false`), its `[DisabledAddOns]` list, and
   forces windowed mode;
5. applies the power policy, if any (see "Power");
6. starts the emulator with the debug server and the variant's environment switches, waits for
   presented frames, plays the scene's timed pad inputs and lets the scene settle;
7. takes a screenshot and compares it with the reference (see "The scene check");
8. issues the variant's `live` debug-server calls and `reject` list, if any;
9. measures three windows (15 s each) and keeps the one with the median fps; reads watts after
   each window and, with `--gpu-telemetry`, the GPU clock and what limits it;
10. with `--sustain-min N`, samples every 10 s for N minutes, each sample with watts and GPU
    clock when available: GPUs that lose clock or power budget as they heat up show it here and
    not in the first minute;
11. with `late_s`, waits that long and measures one more window;
12. reads `memory` and `gpu_costs` from the debug server when the build has them, the window's
    client size and the emulator's working set and private bytes with their peaks;
13. takes a second screenshot, checks it again and saves it under `results/shots/`;
14. closes the emulator's window; if it still runs after 5 s (a confirmation dialog), terminates
    it. Only once the process is gone (it writes its settings on exit) are `qt-config.ini`, the
    mod folders and the power settings put back;
15. copies the emulator log to `results/logs/<row id>.log.txt`, summarizes its tracer lines into
    the row and appends the row to `results/<date>-<machine-id>.jsonl`.

Everything a run changes is restored whatever fails, also after Ctrl+C; a run that was killed is
repaired by the next start. While the emulator runs, a watchdog stops it if free RAM falls under
2 GB (`--ram-floor-gb`), and the CPU time used by every other process is measured. Every wait has
a timeout.

`--user-dir` defaults to a `user` folder next to the emulator, else the per-user data folder
(`%APPDATA%\lichii`, `~/.local/share/lichii`). Paths may use `%VAR%`, `$VAR` and `~`.

### Scenes and variants

A scene (`scenes/*.json`) names the game, the pad inputs that reach the spot
(`[{"wait_s": 10, "buttons": ["A"], "frames": 6}, ...]`: wait, then hold for that many presented
frames; `left_stick` works too), the settle time and the reference image. Its `notes` say what the
save must contain.

A variant (`variants/*.json`) is what you compare. Without `--variant` the baseline runs: your
settings as they are, windowed. Every field but `name` is optional:

| Field | Content |
|---|---|
| `name` | letters, digits, `.`, `_`, `-` |
| `settings` | `{section: {key: value}}` of `qt-config.ini`; `null` hands a key back to its default |
| `env` | environment switches |
| `mods` | mod folder names copied from `--mod-library`; `[]` runs without mods. Without the field, `--mod` applies; without both, your mod folders are left alone |
| `addons_off` | add-on names written to `[DisabledAddOns]` for the scene's title (`[]` enables all) |
| `power_policy` | `{"PERFBOOSTMODE": 0, ...}`, on top of `--power-policy` |
| `live` | `[{"method": "set_shading_rate", "params": {...}}, ...]`: debug-server calls issued after the scene settled and was checked (`power_set`, `gpu_costs` with `enable`, ...). A call that fails makes the row an error |
| `reject` | full 16-digit shader hashes sent with `reject_shaders` after settling and restored before the emulator stops. Such a row is a probe (see below) |
| `live_settle_s` | wait after `live` / `reject` before measuring (default 3, with `reject` 20) |
| `late_s` | wait this long after the three windows and measure one more (`late` in the row) |

A probe (`"probe": true`) answers "what does this shader cost", not "how fast is this machine":
the picture is wrong on purpose, so the scene is checked before the rejection only, and `table`
never lists probe rows.

### Suites

```
python -I tools/bench/bench.py suite tools/bench/suites/igpu-switches.json --exe ... --game ... [run options]
```

A suite (`suites/*.json`) is a list of runs, each a scene, variants (objects or file names) and
options (`window_s`, `settle_s`, `sustain_min`, `sustain_interval_s`, `ram_floor_gb`,
`max_other_cpu_pct`; the command line wins). `base` is merged under every variant. A variant
with `"paired": true` also runs as `<name><suffix>` with the suite's `paired.env` added.
`suite` takes the options of `run` and prints a comparison table at the end; rows go to the same
`.jsonl`, with the suite's name in `suite`.

`igpu-switches.json` measures the switches that may help integrated GPUs (`EDEN_NX2_MESH_CULL`,
`EDEN_NX2_VRS`, `EDEN_NX2_SPIN_SCALE`, `EDEN_NX2_ECO_BACKGROUND`, `EDEN_NX2_LOW_MEMORY`) against
the baseline; the ones that change GPU work are paired with a traced twin
(`EDEN_DKB_FRAME_TRACE=1`). Tracing costs time, so traced rows are compared with
`baseline-trace` only.

### Power

`--power-policy NAME=VALUE` (repeatable) and a variant's `power_policy` set processor power
settings of the active Windows power plan for the run:
`powercfg /setacvalueindex SCHEME_CURRENT SUB_PROCESSOR <NAME> <value>`, then
`/setactive SCHEME_CURRENT`. Names are powercfg aliases such as `PERFBOOSTMODE`,
`PROCTHROTTLEMIN`, `PROCTHROTTLEMIN1`, `PROCTHROTTLEMAX`. The previous values are read first
(`powercfg /qh`), kept in a restore file while changed, and always put back; the row records
both. Only the plugged-in (AC) values are changed. Names the machine does not have are skipped
and listed (`power_policy_partial` warning).

Watts come from the debug server's `power` method. A build without it falls back to the Windows
counters `\Energy Meter(rapl_package0_pkg|pp0|pp1)\Power` through `typeperf`; `power.source`
says which. A machine with neither gets the `no_power_reading` warning.

`--gpu-telemetry <script.ps1>` runs your script (`-Samples 3 -IntervalMs 700`) after the windows
and after every sustained sample. It must print lines `HH:MM:SS  MHz  Watts ... LimitedBy`; their
mean clock, mean watts and most frequent last column become `gpu_mhz`, `gpu_telemetry_w` and
`gpu_limited_by`.

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
| `error` | boot, input, a `live` call, the power policy or a measurement failed or timed out; see `error` |
| `schema` | the row did not pass `validate` |

`warnings` do not invalidate a row: `on_battery`, `no_power_reading`,
`background_cpu_not_measured` (the host cannot report CPU times), `power_policy_partial`,
`reject_not_restored` (the emulator did not answer the un-reject; it was stopped anyway),
`emulator_still_running` (it survived being killed: check your settings file).

Invalid rows stay in the file; `table` shows the newest valid row per scene and variant.

## Row schema (`schema_version` 1)

One JSON object per line. `null` means "not measured". Fields from `id` down were added later
and are optional, so older rows stay valid.

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
| `sustained` | object or null | `minutes`, `interval_s`, `fps_min`, `fps_avg`, `fps_first`, `fps_last`, `watts_avg`, `gpu_mhz_min`, `samples` (`[{t_s, fps, p50_ms, p99_ms, other_cpu_pct, package_w?, cpu_w?, gpu_w?, gpu_mhz?, gpu_telemetry_w?, gpu_limited_by?}]`) |
| `watts` | number or null | mean package power over the three windows |
| `power` | object or null | mean `package_w`, `cpu_w`, `gpu_w`; `source` (`debug_server` or `energy_meter`) |
| `memory`, `gpu_costs` | object or null | the debug server's answers, as they are |
| `system` | object | `other_cpu_pct` (other processes, mean of the three windows, percent of all cores), `other_cpu_max_pct` (worst window or sustained sample), `other_cpu_limit_pct`, `emulator_cpu_pct`, `ram_free_min_gb` |
| `screenshot` | string or null | name under the results folder |
| `id` | string | `<date>-<machine-id>-<scene>-<variant>-<hhmmss>`; names the screenshot and the log |
| `suite` | string or null | the suite that ran the row |
| `probe` | bool | true exactly when `reject` is not empty |
| `base_ini` | string or null | file name of `--base-ini` |
| `mods`, `addons_off` | string list or null | mod folder and add-on names; null when left alone |
| `reject`, `live` | lists | rejected shader hashes; the live calls |
| `power_policy` | object or null | `applied`, `previous` (`{name: value}`), `skipped` (names) |
| `gpu_mhz`, `gpu_telemetry_w`, `gpu_limited_by` | number / number / string or null | from `--gpu-telemetry` |
| `late` | object or null | `after_s`, `fps`, `p50_ms`, `p99_ms` and the power and GPU readings of the late window |
| `window` | string or null | client size of the emulator's window, `WxH` |
| `process_memory` | object or null | `working_set_mb`, `working_set_peak_mb`, `private_mb`, `private_peak_mb` |
| `log` | string or null | `logs/<id>.log.txt`: the copy of the emulator log (not in git) |
| `trace` | object or null | from the last `DKB gpu_work`, `gpu_pass` and `gpu_frame` log lines: `work` (its totals and `top`: the 8 costliest shader pairs with `ms`, `count`, `max_ms`), `pass` (totals and `top` render targets), `frame` (`gpu_frame_span_ms`, `gpu_submission_sum_ms`, `gpu_busy_ms`, submissions). Present when the variant set `EDEN_DKB_FRAME_TRACE=1` |

`python -I tools/bench/bench.py validate <file.jsonl> ...` checks rows against this schema (types,
consistency of `valid` and `probe`, the median, the machine id, no home directory paths) and
exits 1 on any problem. CI runs it on contributed rows.

## Privacy: what is collected

Only what the table above lists; [CONTRIBUTING-bench.md](CONTRIBUTING-bench.md) has the complete
list. A row contains no user name, host name, serial number, MAC or IP address, and no folder
paths. Before a row is written, every string in it is scrubbed: home directories become `~`, the
user name `<user>`, the host name `<host>`. `validate` refuses rows in which a home directory
path is left. The machine id is derived from model names only, so two PCs of the same model
share it.

Nothing is uploaded. Rows, screenshots and logs stay in `results/` until you send them;
screenshots (`results/shots/`), logs (`results/logs/`) and your reference image are git-ignored.
The logs are the emulator's own and do contain paths: do not attach one without reading it.

## Compare

```
python -I tools/bench/bench.py compare results/<a>.jsonl results/<b>.jsonl
python -I tools/bench/bench.py table results [--all] [--out RESULTS.md]
```

Both print markdown: variant, fps, p50, p99, sustained min / avg fps, watts and `scene_ok`.
`compare` gives a percentage only where both rows are valid. `table` leaves probe rows out.

## Play

```
python -I tools/bench/play.py --exe <path to lichii> --game <path to the game> \
    [--profile tools/bench/profiles/oot-intel-igpu.json] \
    [--power-policy PERFBOOSTMODE=0 ...] [--close <process name> ...] [--dry-run]
```

For playing, not measuring: no debug server, no settings or saves touched. It

1. lists the processes using the most CPU and GPU memory and warns about heavy ones (an
   integrated GPU shares its memory and its power budget with them). It closes nothing unless
   you name it with `--close`;
2. warns when the machine runs on battery;
3. applies the profile's and the command line's power policy for the session (as in "Power") and
   restores it when the emulator exits. The previous values are also kept in a restore file
   (`%LOCALAPPDATA%\lichii-bench\power-restore.json`, shared with `bench.py`), so a session that
   was killed is repaired by the next start of either tool;
4. starts the emulator with the game and the profile's environment switches and waits for it.

`--dry-run` prints all of this, including the current and the new power values, and changes
nothing. A profile (`profiles/*.json`) is `{name, env, power_policy}`. `oot-intel-igpu.json`
ships with an empty `env` on purpose: a switch is added only with rows that show a measured gain.

It is a Python script like `bench.py`, not PowerShell: it shares the power-policy code and its
tests, and needs no change of the script execution policy.

## Tests

`python -I tools/bench/test_bench.py` runs without the emulator: the run is tested against a fake
debug server, and `powercfg`, `typeperf`, the telemetry script and `taskkill` against a fake
command runner. No test changes a power setting, launches the emulator or ends a process.
