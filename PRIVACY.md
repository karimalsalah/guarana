# Privacy

Guarana is offline-first: the emulator runs entirely on your machine, and no gameplay,
keys, firmware, saves or personal files ever leave it. This page covers the few places
where data can leave your machine, all of them opt-in.

## What stays on your machine
- Keys, firmware, NAND/SD contents, saves, shader and pipeline caches, logs, dumps.
- Settings and per-game add-on choices (`config/` in your user directory).
- Session report cards and bench rows, unless you choose to share them.

## What you can choose to share
- **Device reports** (GitHub issue form): hardware model, GPU, driver version, OS, power
  mode, the emulator build, the game and scene, the settings you used, and your measured
  results. Optional: a screenshot and a credit name. The form asks you to review logs for
  personal information (user names, folder paths, serial numbers) before attaching; the
  ingest triage redacts what it can, but you are the last check. Reports are public GitHub
  issues — do not attach anything private. Credit is optional and pseudonymous is fine.
- **Bug reports**: same rules; attach `eden_log.txt` only after reviewing it. Never attach
  bug bundles, GPU traces, shader caches or memory dumps publicly — ask a maintainer first.
- **Update checks** (if enabled): the updater contacts GitHub Releases to compare versions.
  No identifiers beyond the request itself are sent.

## What the project does with reports
Device reports become rows in `docs/DEVICES.md` and the community leaderboard: per-device
presets and support status. That is the whole pipeline — play, report, everyone on your GPU
gets better defaults. Reports are never sold, never used for advertising, and never linked
to accounts beyond the GitHub identity you post with.

## Your controls
- Delete any report you posted: edit or delete the GitHub issue, or ask a maintainer.
- Opt out of credit: leave the credit field empty.
- Opt out of update checks: Settings → General (or the launcher's update setting).
- The emulator's network services (e.g. the emulated console's online features) are
  disabled by default; nothing connects to Nintendo or any game server.

## Legal
Guarana is not affiliated with Nintendo. Nintendo, Switch and Zelda are trademarks of
Nintendo. Players bring their own keys, firmware and games dumped from hardware they own;
no Nintendo content is distributed. See the [Legal](README.md#legal) section of the README.