---
name: bench
description: "Run tools/bench on a device (a scene with variants, or the igpu-switches suite), validate the JSON rows, and turn them into a device report for the device-report issue form or a results pull request. Use when asked to benchmark a machine, measure a switch on a verified scene, or contribute or report device results."
---

# Benchmark a device

`tools/bench/bench.py` starts the emulator itself, swaps in the scene's save, writes settings and can
change the Windows power plan; it puts everything back afterwards. Run it only when the owner asks
for a run on this machine, and start nothing else meanwhile (no build: CLAUDE.md).
`tools/bench/README.md` has every option and the row schema; `tools/bench/CONTRIBUTING-bench.md` is
the contributor's walk-through.

## 1. Prepare, with the owner

- `--exe` (the build), `--game`, `--save-backup` (a copy of `<user-dir>/nand/user/save` standing in
  the scene; the scene file's `notes` say where) and `--reference` (their own screenshot of the scene).
- No reference yet: record it once with `--record-reference`, have the owner check that the image
  shows the scene in gameplay, then run without it. The recording run's rows are invalid on purpose.
- Plugged in; browsers, launchers and overlays closed; updates paused.

## 2. Dry run, then run

```sh
python -I tools/bench/bench.py run --dry-run --exe <exe> --game <game> \
    --scene tools/bench/scenes/oot-hyrule-field.json --save-backup <dir> --reference <png> \
    --variant tools/bench/variants/baseline.json --variant <variant.json>
```

Show the owner what the dry run says it will touch, and run the same command without `--dry-run`
once they agree. The suite, `bench.py suite tools/bench/suites/igpu-switches.json <same options>`,
takes about 40 minutes. Leave out `--base-ini`, `--mod` and `--power-policy` unless the owner asks:
they make the baseline a different one. Let a run finish; every wait has its own timeout.

## 3. Check the rows

```sh
python -I tools/bench/bench.py validate tools/bench/results/<date>-<machine-id>.jsonl   # "row(s) ok"
python -I tools/bench/bench.py compare <a.jsonl> <b.jsonl>
python -I tools/bench/bench.py table tools/bench/results --out tools/bench/RESULTS.md
```

- A result is a row with `"valid": true` and `"scene_ok": true` (AGENTS.md rule 1), paired with a
  baseline from the same session. Show the baseline next to it.
- For an invalid row, give its `invalid_reasons` and the fix from CONTRIBUTING-bench.md:
  `wrong_scene`, look at `results/shots/<id>.png` and rerun; `background_cpu`, find the busy process
  (`python -I tools/bench/play.py --exe <exe> --game <game> --dry-run` lists them) and rerun.
- Probe rows (`reject`) say what a shader costs, not how fast the machine is.
- Never edit a row by hand. Never commit `results/shots/`, `results/logs/` or the reference image:
  the logs hold paths, and the image is a frame of the game.

## 4. Report

- **Results pull request**: commit only the `.jsonl` and the regenerated `RESULTS.md`
  (CONTRIBUTING-bench.md, step 5). The description says what the rows cannot: model class,
  performance mode, external display.
- **Device report**: fill the body below from the rows and show it to the owner. Open the issue only
  when they say so: `gh issue create --title "[Device] <device>, <GPU>" --label device-report
  --body-file <file>`. The owner then adds the screenshot (`results/shots/<id>.png`) in the browser.
  The headings are the labels of `.github/ISSUE_TEMPLATE/device-report.yml`, which
  `label-issues.yml` reads, so keep them exact and answer the dropdowns with one of the form's options.

~~~markdown
### Device
<maker and model>

### Form factor
<Laptop | Handheld | Mini PC | Desktop>

### CPU
<hardware.cpu>

### Graphics card
<one of the form's options, for example: Intel Arc 140T / 140V (Arrow Lake H, Lunar Lake)>

### GPU driver
<hardware.gpus[].driver_version>

### RAM
<hardware.ram_gb> GB, <type, and the GPU's reserved memory if known>

### Operating system
<hardware.os> (<hardware.os_build>)

### Power source
<Plugged in | On battery>

### Power mode
<hardware.power_plan>, <the maker's performance mode>, <power limit if set>

### Emulator build
<emulator.build>

### Game
<one of the form's options, for example: Ocarina of Time>

### Scene
<scene>: <its description>; <window> window

### Settings and switches
<variant_settings and variant_env of each variant, or "none">

### Results
<per variant, baseline first: fps, p50_ms, p99_ms, sustained if measured; three 15 s windows each, tools/bench>

### tools/bench rows
```json
<the rows, one JSON object per line>
```

### Screenshot
<the owner attaches results/shots/<id>.png>

### Anything else
<warnings such as on_battery, background load, temperatures, an external display>
~~~
