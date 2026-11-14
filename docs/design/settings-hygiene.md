**This is a proposal, tracked in [#125][125], and not yet built.** Nothing below describes what the code does today unless it says "today" and cites a line. For what the code does now, see [lifecycle.md](../lifecycle.md).

# Design: settings hygiene, the flush and the automatic doctor

Guarana's settings come from many writers: the shipped switch file, the launcher's modes and
profiles, the first-run seed, imports from other emulators, the copy from the old `lichii` folder,
status-bar clicks during play, and the player. Today the files do not record who wrote a value, so
a value nobody chose looks the same as a choice, and it can cost speed or pacing for good. This
design proposes three things:

1. **Provenance**: each value a writer other than the player sets is stamped with its writer.
2. **A flush**, "Reset to known-good": it resets what the player did not choose, with a backup and
   an undo.
3. **An automatic doctor**: safe fixes happen by themselves at set moments; everything else is
   offered or reported.

Paths in `launcher/` are under `launcher/src-tauri/src/` unless given in full. Line numbers are as
of the commit that added this page.

## 0. What is already solved, and what is left

Two merged pull requests already fixed part of the problem:

- **#122** (`profiles.rs:30-56`, `launch.rs:169-204`): with any Intel integrated GPU in the
  machine, prediction on (`ocarina_extrapolation`) leaves `EDEN_NX2_EXTRAPOLATE` out of the
  launcher block at every refresh rate; prediction off still writes `=0`. Custom mode now rewrites
  the launcher block (only the block, not `qt-config.ini`) from the saved profile at every start,
  so a line an older launcher wrote goes away at the next launch.
- **#121** records which switches only the shipped `nx2_switches.txt` set
  (`src/common/switch_origin.h`). A game's switch defaults may replace those values
  (`DecideSwitch`, `src/core/game_presets.cpp:398-408`). It takes back an earlier game's switch
  defaults at the next start in the same process (`game_presets.cpp:447-459`), and leaves this
  process's own switches out of the in-app update's restart (`update_dialog.cpp:45-49`).

What is left:

- **The launcher block sits inside the player's file**, so every line in it counts as the
  player's. Issue #119 is fixed for launcher users on Intel integrated GPUs only when the launcher
  runs before the game. A block an older launcher wrote stays until the next launch through the
  launcher: starting `guarana.exe` from its own shortcut keeps it. Custom mode with no known
  profile rewrites nothing (`launch.rs:193-194`).
- **`EDEN_NX2_FLUID=1`** from the Arc and AMD profiles (`profiles.rs:425-427`) still beats
  Ocarina of Time's measured `FLUID=2`, which every Ocarina set names (`game_presets.cpp:64,75,95,136`),
  at every refresh rate. It cannot simply be left out as `EXTRAPOLATE` was: the fluid presenter
  acts on every game, and only Ocarina has a measured set. Only slice S3 (launcher defaults rank
  with the shipped file) fixes it without costing the other games.
- **Settings files keep no record of who wrote a value** (`src/frontend_common/config.cpp:984-993`):
  the first-run seed, the launcher, an import, a status-bar click and the player all look the same.
- **Nothing runs checks on its own** except the warm-up after a GPU driver change
  (`warm.rs:431-483`), and the doctor has no check for settings, switch files or cache formats
  (`doctor.rs:27-49`).

## 1. What "contaminated" means

A stored value is contaminated when all three hold:

1. It changes what the game does: speed, pacing, image, or whether caches work.
2. The player did not choose it in this build. It came from the shipped file, a launcher profile
   or mode default, the first-run seed, an import from another emulator, the rename copy, a
   status-bar click that became permanent, a preset measured on another device, or a cache or
   marker made for another driver, format or switch set.
3. Something better is known: a game default, the code default, or a rebuild.

Each rule below can be checked from the files on disk, the detected system, and the launcher's own
state. "Evidence" is what the rule reads. "Effect" is what the player sees. "Class" is how the
doctor acts (section 4): **A** fixes on its own, **B** asks first, **C** only reports.

| Id | Rule | Evidence | Effect | Class |
|---|---|---|---|---|
| C1 | The launcher block differs from what this launcher would write now for the saved mode, profile and overrides. Typically an `EDEN_NX2_EXTRAPOLATE=1` from a launcher older than #122, on a machine with an Intel integrated GPU. Separately: the block sets `EDEN_NX2_FLUID=1` while Ocarina of Time is in the library and an Intel or AMD integrated GPU is present | `rewrite_block` (`profiles.rs:545`) run on the file without writing; the detected GPUs. No refresh rate is read, as #122 decided (the rate can change after the check) | At 144 Hz: 58.5 shown frames a second and 655 of 1238 frames dropped, against 91.5 with the measured set ([switches.md](../switches.md), 144 Hz row; issue #119). `FLUID=1` against Ocarina's measured `FLUID=2` at every rate | Stale block: A (rewrite it, as every launch already does). `FLUID`: C until S3; after S3 nothing to do, the game default wins |
| C2 | Both managed blocks in one file name the same switch | The markers (`src/common/guarana_modes.cpp:9-10`, `profiles.rs:21-22`) | The settings page shows Smooth (`FLUID=2`) while the launcher block's `FLUID=1` wins | C |
| C3 | A switch-file line the loader reads wrongly: a byte-order mark, an unquoted ` # note` after the value, or a name the installed build does not know or has retired | The file's bytes; the installed emulator's switch list from its `build-facts.json` (S3), not the launcher's own copy, which can be of another version | The switch silently does nothing (`POWER_POLICY=igpu # x` fails its compare) | C. The loader also learns to accept the first two (S3); it strips ` # note` only from switches whose value is a number or a fixed word, so a folder value such as `EDEN_NX2_PIPELINE_STATS=C:\runs #2` keeps its text |
| C4 | A persistent `EDEN_NX2_*` variable in the user or system environment | `HKCU\Environment` and the system environment, read only | Wins over every file and every launcher choice (`nx2_switches.h:164-167`) | User scope: B. The fix removes it, tells Windows the environment changed, and leaves it out of the environment the launcher gives the emulator until the launcher restarts (its own environment still holds the old value). System scope: C |
| C5 | A launcher block that nothing rewrites any more: Custom mode with no profile, or one this launcher does not know (`launch.rs:194`), or no launcher state at all (the launcher was reset or removed) | Launcher state; the block header (S2 adds the version) | Stale `EXTRAPOLATE`, `FLUID`, `GM` or `POWER_POLICY` lines keep applying | B: remove the block (there is no profile to rewrite it from) |
| C6 | A `[Renderer]` key still holds the value the launcher wrote in a mode (Auto, Smooth, Quality, Battery), and the current mode no longer writes that key: for example `frame_gen_multiplier` and `frame_gen_target_rate` after frame generation went off, or `hdr_swapchain` after HDR went off (`profiles.rs:430-459`). Not in Custom mode: Custom is reached only by applying a profile by hand, so those writes are the player's and are not stamped (S5) | The journal and the `\origin=launcher` stamp (S5) | A value the player never chose waits for the next time its toggle comes back | A: restore the value from before the launcher wrote it. With no recorded value (written before S5): C |
| C7 | A key the launcher writes was changed in the emulator since the launcher wrote it, and the new value has no stamp. A `statusbar` stamp is not a player's edit: the launcher overwrites it at the next start, as today | The journal's written value differs from the file; no `\origin` | Today the launcher silently overwrites the player's choice at every start | Not a flush: the launcher stops overwriting that key (S5), says so on the mode screen with "Hand back to the mode", and the autopilot stops steering that key |
| C8 | A value still stamped `seed <X>`, while the installed emulator's seed for that key is no longer X | `\origin=seed <value>` (S4); the installed emulator's seed table from `build-facts.json` (S3), not the launcher's copy (`profiles.rs:466`), which can be of another version | An old build's seed (for example the FSR3 filter) stays after the seed changes | A: set the installed build's seed, stamped `seed`. Without `build-facts.json`: C |
| C9 | A value stamped `statusbar`, for the filter, anti-aliasing, frame generation, GPU accuracy or renderer chips. Not docked mode: its hotkey is a deliberate, frequent switch | `\origin=statusbar <value>` (S4) | One click on the filter chip costs 3.5 ms per shown frame against 1.7 for the default filter, in every game, for good | B, ticked |
| C10 | A value stamped `import:<emulator>` in `Renderer`, `Cpu` or `Core` | `\origin=import:*` (S4) | Another fork's renderer and accuracy settings, and presets skipped because the value counts as the player's | B |
| C11 | `use_disk_shader_cache=false` or `use_vulkan_driver_pipeline_cache=false` | The two keys (`src/common/settings.h:681-686`) | Every boot compiles from nothing; the warm-up refuses to run | B, ticked when stamped `import:*`. Not A: a player may have turned a cache off on purpose to work around a driver bug |
| C12 | The rename copy did not happen, or happened into an empty folder (issue #107): `lichii\config\qt-config.ini` exists, and `guarana\config\qt-config.ini` is missing or holds only seed values and keys equal to their defaults; or the same for `nx2_switches.txt` | Whether the files exist; a key-by-key comparison of the two files | The player's settings are missing | B: merge the settings keys of `qt-config.ini`, `custom\*.ini` and `nx2_switches.txt` from `lichii`, stamping each copied non-default key `import:lichii`. `[Data Storage]` (`config.cpp:300-322`: `nand_directory`, `sdmc_directory`, which decide where saves are read) and every path key are kept from `guarana` and never copied. When `lichii\nand` or `lichii\sdmc` exist and the `guarana` ones are empty, the row says so and points to Guarana's own migration: the doctor checks only that they exist and copies no save |
| C13 | Ocarina of Time's built-in `Max Resolution` add-on is enabled by the preset, which matches every Intel integrated GPU without matrix cores (`game_presets.cpp:177-186`), and not disabled in Add-Ons, on a GPU that matches the rule but is not the Arc 140T it was measured on (by PCI device id) | `[DisabledAddOns]` (only whether a disable exists); the GPU device ids; the preset rule, mirrored in the launcher | Full-scale rendering that a small integrated GPU (a 4-core Xe3) cannot hold | C, with "Turn it off in Guarana: Properties > Add-Ons". Only disables are stored, so "on by choice" cannot be told from "on by default". The real fix is a narrower preset rule, not part of #125 |
| C14 | A shader cache log whose header has a format version older than the one the installed emulator writes | The first 28 bytes: magic `LXSHCACH`, a CRC, and a 32-bit version at offset 8 (`src/video_core/shader_cache_v2/cache_format.cpp:216-220`); the installed build's version from `build-facts.json` (S3). A newer version means a rollback: left alone, reported as C ("made by a newer Guarana; it is used again after an update") | Every boot of that title is a cold boot, for good (`cache_writer.cpp:137-141`) | A: rename it to `lichii-cache.v2.bin.format<N>`, never delete it. When the installed build writes format N again and no current log exists, the `.format<N>` file is renamed back (A). Without `build-facts.json`: C |
| C15 | Precache markers present while the driver cache file is missing or holds only a header, and only where the emulator keeps that file: the markers' device is an Intel GPU and `use_vulkan_driver_pipeline_cache` is on (after S8, the marker says so itself). NVIDIA, AMD's own driver and RADV keep their own cache (`vulkan_device.cpp:1404-1411`); there the file never exists and the markers are right | File sizes, as the emulator's own check (`vk_pipeline_cache.cpp:1833-1843`, which runs only where the emulator keeps the file, `:868-869`) | The warm-up says it is done, but the first play compiles (issue #106) | A: remove the two lists and `warm-compute.done`, as the emulator's own load does (`shader_warm_builder.cpp:498-512`). Elsewhere: nothing |
| C16 | `.migrating` or `.tmp` files older than an hour whose cache lock is free (`cache_file.cpp:248-256`), with no emulator or warm-up running | Names, times, the lock | Disk space; an interrupted migration (the source log stays, so the temporary file holds nothing else) | B, as the `shader_tmp` check today. Not backed up: the snapshot lists them as deleted, not restorable |
| C17 | Launcher overrides that name a toggle that does not exist | The overrides (`profiles.rs:537`) | The whole apply fails with "Unknown setting" | A: left out of the values applied, with a log line, and kept in `launcher-state.json`, so a newer launcher still has them after a launcher rollback |
| C18 | A global value with a measured cost above the default's on this GPU class, whoever wrote it (values saved before S4 have no stamp) | A short table of measured costs shipped with the launcher (first row: the scaling filter, 3.5 against 1.7 ms per shown frame) | The filter chip clicked before S4 | B, not ticked: "costs about 1.8 ms more per shown frame than the default" |

None of these rules reads a game file, a key, firmware, a save, or NAND or SD card contents. C12
only checks whether two folders exist and are empty.

## 2. Provenance: who wrote each value

### Writers

| Origin | Writer |
|---|---|
| `player` | The player: the real environment, their own lines in a switch file, a Custom override, a change in Guarana's settings dialogs |
| `shipped` | The `nx2_switches.txt` next to the program |
| `launcher` | A launcher mode (Auto, Smooth, Quality, Battery), its community data, or the autopilot. Written as `launcher:<mode>`, or `launcher:community` for a value from community data (`modes.rs:127-135`), so the doctor can say a value was measured on other devices of a broad class. A profile applied by hand (Custom) is the player's and is not stamped |
| `game` | A game preset or game switch default (memory only, never saved) |
| `seed` | The first-run seed |
| `import:<emulator>` | A profile import |
| `statusbar` | A status-bar chip or hotkey during play |
| `recommended` | "Use recommended settings" |

### The marking rule

A writer other than the player stamps the value it writes, together with that value. **The stamp
holds only while the stored value still equals the stamped value.** Any other change, from any
code path, makes the value the player's. A value with no stamp is the player's.

This needs no change to the player's own paths (dialogs, Properties, hotkeys, the settings page):
only the few other writers stamp. It errs on the safe side: a value whose history is unknown is
never flushed without asking.

Two details make the rule hold in the code as it is:

- **A stamp that no longer matches is deleted at the next save** (`Config::WriteSettingGeneric`,
  `config.cpp:968-994`, which every `Reload` runs). Without that, a value changed away and back
  (FSR3, then Nearest, then FSR3 chosen in Configure) would match its old stamp again and count
  as the seed's. A change and change back within one session, between two saves, keeps the stamp;
  that is accepted.
- **The status-bar chips set only the value in memory** (`main_window.cpp:1175,1248,1289,1342,4435`
  call `SetValue`); the file is written later by the normal save. So the chips record
  (key, value, `statusbar`) in a table in memory, and the save writes the stamp when the saved
  value still equals the recorded one.

### Values already on disk

S4 and S5 stamp new writes only. Every value on disk on the day they ship has no stamp, so it is
the player's by the marking rule. The cases already on players' machines (a filter chip click, an
import, a launcher write, a seed) are invisible to C6-C10. The design does not guess them into
class A. Instead:

- **Once, at the first start of a launcher with S5**: for each key the current mode writes, if the
  file's value equals what the launcher would write now, the journal records it as the launcher's,
  with an unknown earlier value. C6 then reports instead of restoring.
- **Once, at the first load of an emulator with S4**: when the import state says an import from
  emulator E completed, and E's `qt-config.ini` still exists, each non-default key whose value
  equals E's is stamped `import:E`. Nothing else is inferred.
- **C18** offers values with a measured cost, whoever wrote them, not ticked.
- Nothing inferred is ever class A.

### Where the stamp is stored

| Place | How | Slices |
|---|---|---|
| `qt-config.ini` and `custom\<id>.ini` | A key `<key>\origin=<origin> <value>` in the same section, beside `<key>\default`. `Reload` keeps unknown keys: the only section it deletes is `[Controls]` (`config.cpp:919-923`). Older builds ignore the key. Writers: the first-run seed (`config.cpp:139-151`), the import (`profile_import.cpp`), recommended settings, the status-bar chips, and the launcher in a mode (`profiles.rs`, including its copy of the seed, `:466`) | S4, S5 |
| `nx2_switches.txt` | By zone, as today (the shipped copy; the player's files). Inside the launcher block, a comment line `# guarana-defaults: EDEN_NX2_EXTRAPOLATE EDEN_NX2_FLUID` names the lines that are launcher defaults. Lines from a Custom override are left out of it and stay the player's. The header gains `launcher <version>`. Older loaders ignore comment lines | S2, S3 |
| The launcher's journal, `%LOCALAPPDATA%\Guarana\hygiene-journal.json` | For each key and switch: the value before the launcher's first write, and the value it last wrote. This lets the launcher hand a key back exactly | S5 |
| Caches | Their headers, as today. S8 adds `pipelineCacheUUID`, the switch variant, and whether this emulator keeps the driver cache file for that device, to the marker stamps | S8 |
| What the installed emulator expects | `<version>\build-facts.json`, generated at build time from the code (shader cache format version, first-run seed table, switch list with value types) and shipped beside the program. The launcher and the emulator are versioned apart (an emulator rollback keeps the newer launcher), so C3, C8 and C14 read the installed build's facts, never a constant in the launcher. An emulator without the file gets those rules as C only | S3 |

### How the emulator uses the stamps

- `LoadNx2Switches` puts names from the launcher block's defaults line in the same tier as the
  shipped copy, so `DecideSwitch` replaces them with a game's measured value, and its log line says
  `over the launcher's profile default` (S3). A player's own line, or a Custom override, still wins.
- Out-of-range enum values are logged with key, raw value and origin at load (S4).
- Not in #125: making the preset decision treat an explicit `player` choice of the default value as
  the player's (open question 2).

## 3. The flush: "Reset to known-good"

### What it resets, by layer

| Layer | Reset without asking (still listed, can be unticked) | Offered, not ticked unless noted | Never touched |
|---|---|---|---|
| Switch files | The launcher block, rewritten from the current mode or the saved Custom profile (C1). With no profile to rewrite it from (C5), it is offered for removal, not removed | Removing the C5 block; commenting out C3 lines (`# guarana-flush: ` prefix); removing a user-scope variable (C4) | The player's lines; the shipped copy; system-scope variables |
| Global settings | Values whose stamp still holds: `launcher:<mode>` for a key the current mode no longer writes, restored to the value from before (C6); `seed` only when the installed build's seed changed, set to the new seed (C8). A seed is never reset to the code default: it is the project's own choice for integrated GPUs (`config.cpp:139-151`) | `statusbar` (ticked), `import:*` (C11 ticked), `recommended`; C11 when unstamped; C18 | Unstamped values, values from a profile applied by hand (Custom), input profiles, game folders, add-on lists, `[Data Storage]` and every other path |
| Per-game settings | none | `import:*` values | Everything else |
| Shader caches | A C14 log renamed aside; C15 markers removed (only where the emulator keeps the driver cache file) | C16 temporary files deleted (not restorable; they hold nothing the log lacks) | The current log, the driver cache, driver blobs, `warm-compute.bin`, logs of a newer format |
| User folder | none | The C12 merge of settings keys from `lichii` (no `[Data Storage]`, no paths) | Everything in `lichii`, which stays as it is; `nand`, `sdmc`, and saves anywhere |
| Launcher state | none (C17 applies in memory; the file keeps the overrides) | none | Installed versions, the game list, the Game Booster journal, overrides |

The deny list is enforced in code, not only by the plan. Every path the flush writes, renames or
deletes must be one of `nx2_switches.txt`, `config\qt-config.ini`, `config\custom\*.ini`, or a
file with a known name in `cache\shader\<16 hex digits>\`, all under the user folder the emulator
reads (the portable `user\` folder beside the program when there is one), plus the flush's own
snapshot folder `guarana-backups\flush-<time>\`. The only other write is the C4 removal of one
named `EDEN_NX2_*` value under `HKCU\Environment`. `keys\`, `nand\`, `sdmc\`, `load\`, `dump\`,
`log\`, firmware, saves, `lichii\` (read only, for C12) and every game folder are refused, and a
test proves it.

### Backup and undo

1. The flush computes its plan, then refuses if `guarana.exe`, `lichii.exe` or a warm-up runs.
2. It writes `guarana-backups\flush-<time>\manifest.json` with `state: pending`, the launcher
   version, and every item: path, action, and a hash of the file before.
3. It copies every file it will change into that folder. If a copy fails, the flush stops with
   nothing changed.
4. It applies the items with atomic writes and renames, and marks each one done.
5. It sets `state: done`. The five newest snapshots are kept.

**Undo** copies the saved files back and renames set-aside caches back, but only for files whose
hash is still the one the flush left, so it never undoes a later change by the player. An
interrupted flush (`state: pending` at launcher start) is rolled back the same way the Game
Booster recovers after a crash (`main.rs:81-83`), but only once neither the emulator nor a warm-up
runs: a player who started `guarana.exe` from its own shortcut before the launcher would
otherwise have the rollback overwritten by the emulator's save when it closes. Until then the
manifest stays `pending` and the Doctor screen shows it.

The automatic passes, a flush and the launch all write the same two files, so they share one
launcher-wide lock. A launch that arrives while the start-up pass holds it waits for the item in
progress (each item is one atomic write), then goes first; the pass resumes after the emulator
exits.

A linked import (C10 with the Link strategy) shares files with the other emulator. The flush
detects a reparse point or a hard-link count above one and asks before it touches such a file.

## 4. The automatic doctor

### Classes

- **A, fixed on its own**: the item is the launcher's own, or a marker whose subject is gone; the
  change is recorded in the journal; and it can be undone. Automatic fixes never delete a file
  with content: they rewrite the launcher's own lines, restore a recorded value, rename a file
  aside, or remove a marker the emulator itself would remove.
- **B, asks first**: anything whose origin is `player`, unknown, `import:*`, `statusbar` or
  `recommended`, and every deletion.
- **C, only reports**: no safe fix exists (C2, C13, a system-scope variable, a measured set used on
  another device class, `FLUID=1` before S3), or the installed build's facts are unknown (C3, C8,
  C14 without `build-facts.json`).

An A fix applies at most once per (rule, subject, value, emulator version, launcher version). That
is recorded in `hygiene-journal.json` before the fix is applied, so a failed write of the record
cannot cause a repeat. If the same problem is back at the next run, it is reported as C instead,
so the launcher never fights the emulator or the player. The launcher block rewrite at every
launch (`launch.rs:169-204`) is the launcher's normal write, not a doctor fix, and is not counted.

No automatic fix touches a value the frame budget or the autopilot steers during play: those are
held in memory and put back at the end of the session (`nx2_autopilot_session.cpp:118-131`), and
every automatic pass runs only while the emulator is closed.

### When it runs

| Moment | Checks | Applies | Budget |
|---|---|---|---|
| Launcher start, after the bundled install, before the automatic warm-up | An interrupted flush (once the emulator is closed); every file rule; C1 rewrites a stale block, so a player who starts `guarana.exe` from its own shortcut also gets this launcher's block | A | In the background; does not delay Home; skipped while the emulator or a warm-up runs |
| After an install, update or rollback of the emulator | C3, C8, C14 against the newly current build's `build-facts.json` (a rollback renames an older build's log back, never aside) | A | In the background |
| After a GPU driver change, before the automatic warm-up | C15 | A | Before the warm-up starts |
| Before every launch, after the launcher makes way for the game and before it writes the mode's settings | C4, C17 (C1 is the launch's own block rewrite) | A; B items are only shown | Under 100 ms; never blocks the launch |
| After the emulator process exits (not when a game stops inside an open emulator: settings are saved only when the emulator closes, `qt_config.cpp:59-62`) | C9 values saved during that session | B, as one Home card | In the background |
| Inside the emulator at boot | The switch-default source line (today, extended in S3), the driver cache markers (today), the raw enum log (S4) | Only the emulator's own existing actions | Part of the boot |

No fix runs while the emulator or a warm-up runs; those wait for the next moment.

### What the player sees

Everything automatic is listed. Nothing happens silently.

## 5. How it looks in the launcher

It uses the Doctor screen's existing parts: check rows, the status badges (`OK`, `CHECK`, `FIX`,
`INFO`), secondary buttons, toasts, and the fine-print style.

- **Doctor tab.** Four new rows join the list after "Shader cache temporary files": "Switch file",
  "Settings", "Shader cache files", "Settings folder". Each row names what was found and where it
  came from, for example: "The launcher's block sets EDEN_NX2_EXTRAPOLATE=1 as a profile default.
  At 144 Hz it turns off Ocarina's measured set." The row's button is the B action ("Remove the
  line", "Copy them over"). Above the list, when something was done automatically, a "Done for
  you" card lists each A fix with its time, one sentence and an "Undo" link (the last 20 entries
  from the journal). Next to "Run again", a secondary button "Reset to known-good…".
- **Reset to known-good.** A sheet with one section per layer: what will be reset (ticked), what is
  offered (not ticked; `statusbar` ticked), and one line "Kept: your own switches and settings,
  keys, firmware, saves and games." It names the backup folder. The confirm button says how many
  things it resets, for example "Reset 7 things"; then a toast "Reset 7 things. Undo", and the
  checks run again. While the emulator runs, the button is disabled with "Close Guarana first."
- **Before a launch.** One fine-print line under the boot progress when the block rewrite or the
  pre-launch pass changed something: "Before the start: the launcher's block no longer sets
  EDEN_NX2_EXTRAPOLATE (left to Guarana on this GPU)." When it found only B items: "2 things to
  look at in Doctor." Nothing when all is clean.
- **Home after a game.** When a status-bar change was saved during the session, one card:
  "Scaling filter changed to Nearest neighbour during play. Keep it for every game?" with "Keep"
  (removes the `statusbar` stamp, so the value becomes the player's and is not offered again) and
  "Put it back" (the C9 flush item, refused while the emulator runs). A player who does not use the
  launcher never sees this card; open question 1 would cover them too.

## 6. Changes to the launcher contract (`launcher/CONTRACT.md`)

- `run_doctor`: new check ids `switch_file`, `settings_file`, `cache_files`, `user_folder`; new fix
  ids `remove_switch_variable`, `comment_switch_line`, `remove_launcher_block`,
  `rename_cache_aside`, `merge_lichii_settings`, `delete_cache_leftovers` (S1 adds the checks, S6
  the fixes).
- `hygiene_plan` returns `HygienePlan { items: HygieneItem[], blocked: string | null }`, where
  `HygieneItem { id, layer, path, action, origin, reason, class: "a" | "b" | "c", ticked }`.
- `hygiene_flush { item_ids }` returns `FlushResult { snapshot, done: string[], failed: {id, error}[] }`.
- `hygiene_undo { snapshot }` returns `FlushResult`.
- `hygiene_log` returns the "Done for you" entries; the event `hygiene://done` follows an automatic
  pass.
- The `launch_game` result gains `tidied: string[]`, which also carries the block rewrite's changed
  lines (S7).

## 7. Failure modes

| Failure | Containment |
|---|---|
| The emulator saves its settings when it closes and undoes a flush | The flush is refused while it runs, as the launcher's apply is today |
| Power loss or a crash during a flush | The manifest is written before any change; the flush is rolled back at the next launcher start |
| The backup cannot be written (disk full, a OneDrive lock) | The flush stops before the first change and says why |
| A stamp is wrong or stale | A stamp holds only while the value equals the stamped value, so a later change always wins |
| An older emulator or launcher reads the files | Stamps are extra keys and the defaults line is a comment: both are ignored |
| The launcher and the emulator keep rewriting the same value | An A fix applies once per (rule, value, versions), then only reports |
| A rule misfires on a player's real choice | Values of player origin are B only; "Keep mine" stores a dismissal per (rule, value) in `launcher-state.json` |
| The launcher and the installed emulator are of different versions (an emulator rollback) | C3, C8 and C14 read the installed build's `build-facts.json`; without it they only report. A log of a newer format is never renamed aside |
| A rule meant for one driver fires on another (C15 on NVIDIA or AMD, where the driver keeps its own cache and the file never exists) | C15 acts only where the emulator keeps the file (an Intel device in the marker, cache setting on; after S8 the marker says so) |
| An automatic pass and a launch write the same file at once | One lock; the launch goes first |
| A linked import makes the files the other emulator's too | Detected by reparse point or link count; the flush asks |
| A portable install, where the launcher's files are not the ones read | `switch_file` reports it; the flush acts only on files the emulator reads, the portable `user\` folder included (open question 3) |
| Undo after the player changed the file again | Undo restores only files whose hash is still the one the flush left, and lists the others |
| An older build wants a cache that was renamed aside | Renamed, not deleted; the name carries its format, and Undo puts it back |

## 8. Slices

Each slice is one pull request. The full list, with files and tests, is in [#125][125].

| Slice | What | Changes behaviour |
|---|---|---|
| S1 | The doctor reports switch-file, settings, cache-file and settings-folder problems | No (report only) |
| S2 | The launcher block names its defaults and its launcher version | No (comments and header only) |
| S3 | Launcher defaults in `nx2_switches.txt` rank with the shipped file; the build ships `build-facts.json` | Yes |
| S4 | Settings files record who wrote a value | No (stamps only) |
| S5 | The launcher keeps a journal of what it writes and hands keys back | Yes: the launcher stops overwriting values changed in the emulator |
| S6 | Reset to known-good, with undo | No (only on request) |
| S7 | The doctor runs on its own | Yes (automatic class A fixes) |
| S8 | Precache markers say "built" only when the driver cache holds it (#106) | Yes (one extra warm-up after updating) |

## 9. Project text that disagrees with the code

These are fixed within the slices named.

- `launcher/src-tauri/src/profiles.rs:223`: the `ocarina_extrapolation` help says "With frame
  generation on"; the switch acts without frame generation, and the test at `:858` asserts the
  sentence. S2 changes both.
- `launcher/src-tauri/src/paths.rs:5-7`: says the launcher writes to the emulator's user folder
  "only on Import and Apply profile". It writes `qt-config.ini` before every launch in a mode, and
  the switch block before every launch in every mode, Custom included (`launch.rs:169-204`). S5.
- `launcher/CONTRACT.md:415` (`refresh_rate`): the check assumes the preset paces to 60; 120 and
  144 Hz have sets of their own (`game_presets.cpp:317-333`). S1.
- `dist/nx2_switches.txt:1,4-5` still names Lichii and `%APPDATA%\lichii`. S3.
- The `set by the player` label in the switch-defaults log line (`game_presets.cpp:536`) also
  covers launcher-block lines. S3.
- `docs/warm-up.md`, "Command line" and "When the launcher calls it": the warm-up does not load
  `custom\<id>.ini`, the launcher passes the global file (`warm.rs:221,286-287`), and the launcher runs
  it on its own only after a GPU driver change. A follow-up, not part of #125.

## Open questions

1. Should status-bar changes be for the session only by default, with "Keep for every game?" when
   the game stops, instead of a stamp plus a ticked flush item?
2. Should an explicit choice of the default value (for example the launcher's frame generation off)
   block a game preset? That needs the preset decision to read a `player` stamp.
3. Portable installs: should the launcher write the files in the portable `user\` folder, or only
   report?
4. Should the uninstaller remove the launcher block? The installer hook would need a launcher
   command for it.

[125]: https://github.com/karimalsalah/guarana/issues/125
