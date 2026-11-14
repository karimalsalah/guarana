# Guarana benchmark

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
    --exe <path to guarana> --game <path to the game> \
    --scene tools/bench/scenes/oot-hyrule-field.json \
    --save-backup <folder with the scene's save> --reference <your reference screenshot> \
    [--variant tools/bench/variants/baseline.json --variant <other.json> ...] \
    [--base-ini <qt-config.ini to start every variant from>] \
    [--mod-library <folder of mod folders> --mod "<folder>" ...] \
    [--power-policy PERFBOOSTMODE=0 --power-policy PROCTHROTTLEMIN=5] \
    [--gpu-telemetry <script.ps1>] [--sustain-min 5] [--fullscreen] [--pacing] [--out tools/bench/results]
```

Add `--dry-run` to print every step without touching anything. For each variant the tool:

1. refuses to start if the emulator already runs or the debug port is in use (one instance only);
2. replaces `<user-dir>/nand/user/save` with a copy of `--save-backup`, so every run loads the same
   save (your own saves are moved aside first and put back when the run ends);
3. when the variant or `--mod` names mods: moves every mod folder in `<user-dir>/load/<TITLEID>/`
   aside and copies the named ones in from `--mod-library`;
4. with `--base-ini`, starts from that file instead of your `qt-config.ini`; then writes the
   variant's settings (`key=value` plus `key\default=false`), its `[DisabledAddOns]` list, and
   forces windowed mode, unless the run is fullscreen (see "Fullscreen runs");
5. applies the power policy, if any (see "Power");
6. starts the emulator with the debug server, with `--pacing` also `EDEN_NX2_SHOWN_STATS=1` (the
   presenter's pacing report in the log, see "Pacing"), and the variant's environment switches, waits for presented
   frames, plays the scene's timed pad inputs and lets the scene settle;
7. takes a screenshot, refuses it when it is black (see "The picture check") and compares it with
   the reference (see "The scene check");
8. issues the variant's `live` debug-server calls and `reject` list, if any;
9. measures three windows (15 s each) and keeps the one with the median fps; reads watts after
   each window and, with `--gpu-telemetry`, the GPU clock and what limits it;
10. with `--sustain-min N`, samples every 10 s for N minutes, each sample with watts and GPU
    clock when available: GPUs that lose clock or power budget as they heat up show it here and
    not in the first minute;
11. with `late_s`, waits that long and measures one more window;
12. reads `memory` and `gpu_costs` from the debug server when the build has them, the window's
    client size and the emulator's working set and private bytes with their peaks;
13. takes a second screenshot, checks it again (scene and picture) and saves it under `results/shots/`;
14. closes the emulator's window; if it still runs after 5 s (a confirmation dialog), terminates
    it. Only once the process is gone (it writes its settings on exit) are `qt-config.ini`, the
    mod folders and the power settings put back;
15. copies the emulator log to `results/logs/<row id>.log.txt`, summarizes its tracer, pacing and
    swapchain lines into the row and appends the row to `results/<date>-<machine-id>.jsonl`.

Everything a run changes is restored whatever fails, also after Ctrl+C; a run that was killed is
repaired by the next start. While the emulator runs, a watchdog stops it if free RAM falls under
2 GB (`--ram-floor-gb`), and the CPU time used by every other process is measured. Every wait has
a timeout.

`--user-dir` defaults to a `user` folder next to the emulator, else the per-user data folder
(`%APPDATA%\guarana`, `~/.local/share/guarana`; the old `lichii` folder where only that exists). Paths may use `%VAR%`, `$VAR` and `~`.

### Scenes and variants

A scene (`scenes/*.json`) names the game, the pad inputs that reach the spot
(`[{"wait_s": 10, "buttons": ["A"], "frames": 6}, ...]`: wait, then hold for that many presented
frames; `left_stick` works too), the settle time and the reference image. Its `notes` say what the
save must contain.

| Scene | Where | The save |
|---|---|---|
| `oot-hyrule-field.json` | Hyrule Field, outdoors | one made in Hyrule Field, in the third file-select entry: hours into the game |
| `oot-kokiri-forest.json` | inside Link's house in Kokiri Forest | the newest save of a new game made in Kokiri Forest: about ten minutes of play. The game is expected to offer to load it by itself and the script presses A once; not yet confirmed in a run (issue #40) |

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
| `fullscreen` | `true`: fullscreen (see "Fullscreen runs"); `false`: windowed even with `--fullscreen`. Without the field, `--fullscreen` decides |
| `pacing` | `true`: sets `EDEN_NX2_SHOWN_STATS=1` (see "Pacing"); `false`: not even with `--pacing`. Without the field, `--pacing` decides |

A probe (`"probe": true`) answers "what does this shader cost", not "how fast is this machine":
the picture is wrong on purpose, so the scene is checked before the rejection only, and `table`
never lists probe rows.

### Suites

```
python -I tools/bench/bench.py suite tools/bench/suites/igpu-switches.json --exe ... --game ... [run options]
```

A suite (`suites/*.json`) is a list of runs, each a scene, variants (objects or file names) and
options (`window_s`, `settle_s`, `sustain_min`, `sustain_interval_s`, `ram_floor_gb`,
`max_other_cpu_pct`; the command line wins). `base` is merged under every variant (so
`"base": {"fullscreen": true}` runs the whole suite fullscreen). A variant
with `"paired": true` also runs as `<name><suffix>` with the suite's `paired.env` added.
`suite` takes the options of `run` and prints a comparison table at the end; rows go to the same
`.jsonl`, with the suite's name in `suite`.

`igpu-switches.json` measures the switches that may help integrated GPUs (`EDEN_NX2_VRS`, `EDEN_NX2_SPIN_SCALE`, `EDEN_NX2_ECO_BACKGROUND`, `EDEN_NX2_LOW_MEMORY`) against
the baseline; the ones that change GPU work are paired with a traced twin
(`EDEN_DKB_FRAME_TRACE=1`). Tracing costs time, so traced rows are compared with
`baseline-trace` only.

`presenter-and-locks.json` measures the default-off switches that touch frame pacing, pipeline
links, lock contention and thread placement (`EDEN_NX2_PRESENT_SPIN`, `EDEN_NX2_GPL_OPTIMIZE_CACHED`,
`EDEN_NX2_SCHED_PAUSE`, `EDEN_NX2_SYNCPOINT_SPLIT`, `EDEN_NX2_HYBRID`, `EDEN_NX2_IDLE_SPIN_US=0`)
against the baseline, fullscreen and with the pacing counters, inside Link's house in Kokiri
Forest. `--scene tools/bench/scenes/oot-hyrule-field.json` runs the same variants in the heavier
scene. Most of these switches have no measured result in `docs/switches.md` yet; rows from this
suite are how they get one.

`slot-compositor.json` measures the slot compositor (`EDEN_NX2_FLUID_COMPOSE`) in two runs,
fullscreen and with the pacing counters, in the same scene. The first compares it with the baseline
and with its diagnostic control (`EDEN_NX2_FLUID_COMPOSE_NATIVE`), which separates what the compute
pass costs from what the smaller frames save. The second keeps the compositor on in every variant
and adds one switch at a time (`EDEN_NX2_FG_PINGPONG`, `EDEN_NX2_FG_MEDIAN_ONCE`,
`EDEN_NX2_SHOWN_GOVERNOR`, `EDEN_NX2_OOT_RENDER_SCALE=75`), each compared with `compose-control`.
Where the compositor's gates are not met (see its row in `docs/switches.md`) it stays off and its
rows equal the baseline.

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

### Fullscreen runs

By default every run is windowed, so that rows stay comparable with each other and with older
rows. But the player plays fullscreen, and there the screen output step (the scaling filter and
frame generation at the display's size, 2880x1800 on some laptops) can cost as much GPU time as
the game. `--fullscreen`, or `"fullscreen": true` in a variant or a suite's `base`, writes
`[UI] fullscreen=true` instead of forcing windowed mode; `[Renderer] fullscreen_mode` stays as your
settings file (or `--base-ini`, the scene or the variant) has it. A run asked to be fullscreen whose
scene or variant settings turn `[UI] fullscreen` off is a windowed row with the warning
`fullscreen_off_in_settings`.

The row records `fullscreen` (what the settings file said when the emulator started),
`fullscreen_requested` (what was asked for; a run that failed before the settings were read is
listed by it) and `output`: the size the emulator presented at, from the `Swapchain:` line in
effect at the last pacing report of its log (else the last `Swapchain:` line, else the window's
client size; a fullscreen swapchain that differs from the window's size gives the warning
`output_size_not_window_size`), with the present mode and, fullscreen, the mode (`borderless`
or `exclusive`; `fullscreen_mode_from` says whether it was the platform default, Borderless on
Windows and Exclusive elsewhere, or set in the settings file; a per-game config can still override
it and is not read). A fullscreen row without a size is invalid (`output_size_unknown`):
nobody could tell what the output step cost. `table`, `compare` and the suite table keep windowed
and fullscreen rows apart: they add a `display` column when any row is fullscreen, match rows
by scene, variant and display mode, and never give a percentage between a windowed and a
fullscreen row (the suite table says `not compared` instead).

### Pacing

`--pacing`, or `"pacing": true` in a variant or a suite's `base`, sets `EDEN_NX2_SHOWN_STATS=1`.
It is off by default because it is more than logging: with `EDEN_NX2_FLUID` the presenter then
also writes GPU timestamps around every copy and times its acquire, copy wait and present, so the
measured path changes. The row's `shown_stats` says whether the switch was on; compare pacing rows
only with rows that also ran with it (no paired measurement of its cost has been made yet).
The presenter then logs, every 10 s, how evenly frames
reached the screen, generated frames included (`video_core/renderer_vulkan/vk_present_manager.cpp`):

```
Fluid presenter (10 s, DXGI WaitForVBlank at 119.88 Hz, a slot every 1 refreshes): 1199 slots, 1136 shown on their slot, 63 missed (...)
Fluid presenter (10 s): 88% of shown frames held one slot, ...; 231 frames received, 1 dropped unshown
Shown frames: 112.4 per second, interval p5 7.9 p50 8.3 p95 9.1 p99 16.6 max 25.0 ms, 82% within 2 ms of the median
```

The first two lines exist only with `EDEN_NX2_FLUID`; the last with any presenter. The row's
`pacing` holds the last complete 10 s report of each in the copied log; it is not aligned with the
measurement windows and can include the steps after them (the `memory` and `gpu_costs` calls, the
last screenshot). The FLUID lines and the `Shown frames` line come from independent 10 s timers, so
they can cover different windows, and a report still buffered when the emulator stopped can be
missing. `head_at_s` and `shown_at_s` give the emulator time (the log prefix) of each report used. The
`frame_times` numbers (`fps`, `p50_ms`, ...) count the guest's frames; `pacing` counts what was
shown. `table` adds the shown-frame columns when any row has them.

### The picture check

A setting can look faster because the picture is broken: a scene drawn black with only the HUD on
top costs little GPU time. The scene check catches that only when there is a good reference, so
every screenshot is also checked on its own, with the scene check's own arithmetic on its 32x18
downscale:

- a cell is near black when even its brightest channel is under half of `scene_threshold` (15 of
  255 by default);
- only the central cells count (columns 6-25 and rows 4-13 of the 32x18 grid), where a HUD does
  not draw: at this downscale every HUD edge lights a whole cell, and a game's full HUD can light
  a fifth of the whole grid;
- a picture is black when at least 85% of those central cells are near black (`picture_dark_pct`
  in the scene file changes that share) and their mean is under `scene_threshold`: against an
  all-black frame the scene check's difference is the picture's mean, so such a picture would
  pass the scene check as a black screen.

So a HUD over black is refused (`black_picture`), and so is an all-black frame. A broken frame
cleared to a colour that is not near black (dark grey, say) is not: only the scene check against
a reference catches it. A black first screenshot stops the run before measuring, and with
`--record-reference` it is not written as the reference. The row's `picture` holds the limits
and each screenshot's `mean`, `spread` (standard deviation of the cell brightness), `dark_pct`
(whole picture), `center_dark_pct` and `center_mean`.

Night scenes and dim interiors are meant to stay valid: a lit scene, even a dark one, keeps one
channel of most cells at 15 or more (the blue of a night sky, moonlit ground, a lamp), and the
downscale averages single stars and lights away; a picture under both limits is darker than a
picture the player can play in. The limits have not yet been checked against real night
screenshots: if a real scene is refused, report it with the screenshot.

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
| `black_picture` | a screenshot was near black but for a HUD, or all black (see "The picture check"); needs no reference |
| `output_size_unknown` | a fullscreen run whose output size neither the log nor the window gave |
| `wrong_title` | the emulator reported another game than the scene's `title_id` |
| `background_cpu` | other processes used more than 10% of the machine's CPU capacity during a window (`--max-other-cpu-pct`); an update or indexer in the background is enough to spoil hours of runs |
| `memory_watchdog` | free RAM fell under the floor and the emulator was stopped |
| `error` | boot, input, a `live` call, the power policy or a measurement failed or timed out; see `error` |
| `schema` | the row did not pass `validate` |

`warnings` do not invalidate a row: `on_battery`, `no_power_reading`,
`background_cpu_not_measured` (the host cannot report CPU times), `power_policy_partial`,
`reject_not_restored` (the emulator did not answer the un-reject; it was stopped anyway),
`emulator_still_running` (it survived being killed: check your settings file),
`fullscreen_off_in_settings` (fullscreen was asked for; the scene's or the variant's settings ran
it windowed), `output_size_not_window_size` (a fullscreen swapchain of another size than the window).

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
| `fullscreen` | bool or null | whether the settings file ran the emulator fullscreen; a row without it ran windowed |
| `fullscreen_requested` | bool | whether the variant asked for fullscreen; tables use it when `fullscreen` is null (the run failed before the settings were read) |
| `shown_stats` | bool or null | whether `EDEN_NX2_SHOWN_STATS` was on (`--pacing`, the variant's `env` or the environment) |
| `output` | object or null | `size` (`WxH`), `size_from` (`swapchain` or `window`), `present_mode`, `images` (swapchain), `fullscreen_mode` and `fullscreen_mode_from` (`default` or `settings`; fullscreen only). A valid fullscreen row has a `size` |
| `pacing` | object or null | from the log (see "Pacing"): `head_at_s`, `shown_at_s` (emulator time of the reports used), `clock`, `display_hz`, `slot_refreshes`, `slots`, `on_slot`, `missed`, `real_received`, `dropped_unshown` (`EDEN_NX2_FLUID` only); `shown_fps`, `interval_p5_ms`, `interval_p50_ms`, `interval_p95_ms`, `interval_p99_ms`, `interval_max_ms`, `within_2ms_pct`. Each key only when its line was logged |
| `picture` | object or null | `limits` (`dark_level`, `dark_pct`, `mean`), `shots` (`[{mean, spread, dark_pct, center_dark_pct, center_mean}]` per checked screenshot), `black`. `black` true requires the `black_picture` reason |

`python -I tools/bench/bench.py validate <file.jsonl or folder> ...` checks rows against this schema
(types, consistency of `valid` and `probe`, the median, the machine id, no home directory paths)
and exits 1 on any problem; a folder stands for the `.jsonl` files in it. On every pull request
that touches `tools/bench/`, CI (`.github/workflows/bench-rows.yml`) runs it on
`tools/bench/results`, checks with `table tools/bench/results --check tools/bench/RESULTS.md` that
`RESULTS.md` is the regenerated page, and runs `test_bench.py`.

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
python -I tools/bench/bench.py table results [--all] [--out RESULTS.md | --check RESULTS.md]
```

Both print markdown: variant, fps, p50, p99, sustained min / avg fps, watts and `scene_ok`; the
display mode when any row is fullscreen, and the shown frames when any row has `pacing`.
`compare` gives a percentage only where both rows are valid and matches rows by scene, variant
and display mode. `table` leaves probe rows out.

## Play

```
python -I tools/bench/play.py --exe <path to guarana> --game <path to the game> \
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
