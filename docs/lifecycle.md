# What Guarana stores, and when it is flushed, loaded and preloaded

Guarana and its launcher keep state between runs: switch files, settings files, launcher state and
caches. This page lists each kind. For each one it says where it lives, what marks who wrote it,
what resets it, when it is read, and what is read ahead of time. The sections are in the order the
layers take effect when a game starts.

Each rule has a status:

- **today**: the code does this now. The citation is `file:line` in this repository, as of the
  commit that added this page. If a line has moved, search for the function name next to it.
- **proposed (S*n*)**: slice *n* of issue [#125][125] (settings hygiene) would add it. The code does
  not do it yet. The design is in [design/settings-hygiene.md](design/settings-hygiene.md).

Two words have a fixed meaning here:

- **Flush**: reset a stored value, or set it aside, so that the next load gets the default or
  builds it again.
- **Preload**: read or build something before the game needs it, so the game does not wait.

Nothing on this page reads, moves or deletes keys, firmware, NAND or SD card contents, saves or
game files, and nothing proposed would.

## The rules every layer follows

| # | Rule | Status |
|---|---|---|
| R1 | Every stored value has one writer that owns it, and that owner is recorded: a block marker in a switch file, an `<key>\origin` stamp in an ini file, or an entry in the launcher's journal. A value with no record is the player's. | Switch files: in part today, for the shipped copy only (`src/common/switch_origin.h:13-19`). The rest: [proposed (S2, S3, S4, S5)][125] |
| R2 | A flush resets on its own only what a writer other than the player owns, only while the value is still the one that writer wrote, and only to what that writer says now. A seed goes to the installed build's current seed, never to the code default. A launcher key goes back to the value it had before the launcher wrote it. A profile the player applied by hand (Custom) is the player's. Values written before stamps existed are never reset on their own. Everything else is offered, never done silently. | [proposed (S6, S7)][125] |
| R3 | Every flush makes a backup first and can be undone. If the backup fails, nothing changes. The one exception: leftover temporary files are deleted without a backup, and the flush says they cannot be restored. | Launcher writes: whole-file backups today (`launcher/src-tauri/src/profiles.rs:583,645`), ten per file (`launcher/src-tauri/src/util.rs:77-106`). Flush with undo: [proposed (S6)][125] |
| R4 | Nothing reads, moves or deletes keys, firmware, NAND or SD contents, saves, game files, or the settings that say where saves live (`[Data Storage]`). The doctor's `keys` and `firmware` checks only look for presence. | Doctor: today (`launcher/src-tauri/src/doctor.rs:558-564` for firmware). The flush's deny list: [proposed (S6)][125] |
| R5 | Nothing is flushed, and no interrupted flush is rolled back, while `guarana.exe` or a warm-up runs: both save settings when they end and would undo it. | Launcher apply: today (`profiles.rs:606-613`, `util.rs:154-162`). Flush and the automatic doctor: [proposed (S6, S7)][125] |
| R6 | Caches are derived data. A cache that does not match the installed build is set aside (renamed) or ignored, never repaired. A cache made by a newer build is left alone. A marker that describes a cache follows that cache, and is judged only where that cache is the emulator's own file, not the driver's. | In part today: the driver cache and its markers (`src/video_core/renderer_vulkan/vk_pipeline_cache.cpp:1833-1843`), only where the emulator keeps that file (`:868-869`). Renaming aside and more stamps: [proposed (S7, S8)][125] |
| R7 | State a game puts into the process for its session is taken back before the next game in the same process. | Today for held preset values (`src/core/core.cpp:498,532,743`) and game switch defaults (`src/core/game_presets.cpp:447-459`). Debug server values: not covered |
| R8 | Preload only what the next boot reads, in the order it reads it, at background priority, within a memory budget. | In part today: background priority and a budget, but in file-name order, not boot order (`launcher/src-tauri/src/launch.rs:57-96`) |
| R9 | Rules that depend on what a build expects (cache format, seeds, switch names) read the installed emulator's `build-facts.json`, never a constant in the launcher: the two are versioned apart. Without that file, those rules only report. | [proposed (S3 ships the file; S1 and S7 read it)][125] |
| R10 | One writer at a time: the launch, the automatic passes and a flush share one launcher-wide lock for the settings files, and a launch goes first. | [proposed (S6, S7)][125] |

## 1. The launcher's own state

This comes first: the launcher decides what it writes before it starts the emulator. All launcher
paths are in `launcher/src-tauri/src/`.

| Stored | Where | Flushed by | Loaded | Doctor check |
|---|---|---|---|---|
| Mode, profile, overrides, autopilot state, `last_warm_driver` | `%LOCALAPPDATA%\Guarana\launcher-state.json` (`paths.rs:53-55`, `state.rs:16-52`). No stamp | Changing the mode resets the autopilot state for the new mode (today, `modes.rs:318-328`). An override that names an unknown toggle fails the whole apply today (`profiles.rs:537`). [Proposed (S5)][125]: such an override is left out of what is applied, logged, and kept in the file for a newer launcher | At every launch and every mode call (today, `modes.rs:213-243`) | `settings_file` reports overrides that name no toggle: [proposed (S1)][125] |
| Current emulator version | `current.json` (`paths.rs:49-51`) | Rewritten by an update or rollback | At every launch | `emulator_version` (today) |
| Game Booster journal | `booster-journal.json` (`paths.rs:57-61`) | Replayed and removed after a crash, at launcher start (today, `main.rs:81-83`) | Launcher start | none |
| Backups of the two files the launcher writes | A `guarana-backups\` folder beside each file: `%APPDATA%\guarana\guarana-backups\` for `nx2_switches.txt`, `%APPDATA%\guarana\config\guarana-backups\` for `qt-config.ini`. The ten newest copies of each are kept (today, `util.rs:77-106`). The file name carries the time | Rotation only | Never by code | none |
| Hygiene journal: for each key and switch the launcher writes, the value before and the value written | `%LOCALAPPDATA%\Guarana\hygiene-journal.json` | Never on its own; updated at each launcher write | Before every launcher write, and by the flush | [proposed (S5)][125] |
| Flush snapshots | `guarana-backups\flush-<time>\` with a `manifest.json` | The five newest are kept | By Undo, and at launcher start when a flush was interrupted | [proposed (S6)][125] |

## 2. Environment and switch files

The Qt frontend reads the switch files once, at the start of `main()` (today,
`src/lichii/main.cpp:123`). `guarana-cli` reads no switch file (today: `LoadNx2Switches` has that
one caller). The loader is `src/lichii/nx2_switches.h`.

| Stored | Where | Owner | Flushed by | Loaded |
|---|---|---|---|---|
| Real environment variables, user and system | The process environment; persistent ones in `HKCU\Environment` and the system environment | The player. No stamp | Never on its own. The doctor would offer to remove a user-scope `EDEN_NX2_*` variable, and would also leave it out of the environment the launcher gives the emulator: [proposed (S1 reports, S6 offers)][125] | Before `main()` for a few values (`src/common/power_tuning.cpp:34-36`). Always wins over every file (today, `nx2_switches.h:164-167`) |
| The launcher's environment for the emulator | `EDEN_NX2_SHOWN_STATS=1` at every launch (today, `launch.rs:211`); the autopilot reads its log lines after the game | The launcher. Not stored | Gone when the process ends | Like the real environment: wins over every file |
| Shipped copy | `<version>\nx2_switches.txt`, from `dist/nx2_switches.txt`. It sets `SPLIT=2`, `FENCE_BATCH=1` and `EXTRAPOLATE=1` (lines 17, 20, 43) | Packaging. Recorded in memory as "set by the shipped copy alone" (today, `nx2_switches.h:161-163`) | Replaced by every install or update | GUI start, first file |
| Copy beside the AppImage (Linux); portable `user\nx2_switches.txt` | `nx2_switches.h:117-134` | The player | Never | GUI start. A portable copy is read instead of the per-user copy (today, `nx2_switches.h:145`) |
| Per-user copy, the player's own lines | `%APPDATA%\guarana\nx2_switches.txt`; the `lichii` copy only while no `guarana` folder exists (today, `nx2_switches.h:151-158`) | The player | Never. The doctor would report unknown or retired names, a byte-order mark, and text after the value: [proposed (S1)][125]. The loader would strip the mark and an unquoted ` # note` after number values: [proposed (S3)][125] | GUI start, last file, wins over the others |
| GUI mode block | Same file, at the top, between `# >>> Guarana mode` and `# <<< Guarana mode` (`src/common/guarana_modes.cpp:9-10`) | The settings page, by the player's choice of mode | Rewritten when the player changes the mode on that page | Like the player's lines (today) |
| Launcher block | Same file, between `# >>> Guarana launcher` and `# <<< Guarana launcher` (`profiles.rs:21-22`): added at the end the first time, then replaced where it is (`profiles.rs:474-491`) | The launcher. Today every line in it counts as the player's. [Proposed (S2)][125]: the header names the launcher version, and a `# guarana-defaults:` line names the switches that are profile, mode, community or autopilot defaults rather than a Custom choice | Rewritten before every launch through the launcher, in every mode (today): a mode writes it from its plan (`launch.rs:170-173`, `modes.rs:246-259`); Custom rewrites only the block, from the saved profile and overrides (`launch.rs:174-204`, `profiles.rs:599-603`). With an Intel integrated GPU in the machine, `EDEN_NX2_EXTRAPOLATE` is left out when prediction is on (today, `profiles.rs:35-56`). Not rewritten when the emulator starts without the launcher, or in Custom with no known profile (`launch.rs:194`). [Proposed (S7, S6)][125]: rewritten at launcher start when stale, or removed on request when no profile can rewrite it | Like the player's lines today. [Proposed (S3)][125]: names in the defaults line rank with the shipped copy, so a game's switch defaults replace them |
| Game switch defaults (Ocarina of Time on a Windows integrated GPU: a 60, 120 or 144 Hz set, chosen by vendor and display rate) | Process environment | Recorded for this process (today, `switch_origin.h:37-44`, `game_presets.cpp:509`) | Taken back at the next game start in the same process (today, `game_presets.cpp:447-459`). Left out of the environment the in-app update's restart gets (today, `src/lichii/updater/update_dialog.cpp:45-49,138,180`) | At each renderer creation, before the device is built (today, `src/video_core/renderer_vulkan/renderer_vulkan.cpp:308-326`) |

**Precedence today**, highest first (`docs/switches.md`, "Setting a switch";
`game_presets.cpp:398-408`):

1. the real environment, including the launcher's environment for the emulator;
2. the player's files, including the launcher block and the GUI mode block;
3. game switch defaults;
4. the shipped copy;
5. the code default.

[Proposed (S3)][125]: names in the launcher block's defaults line move down to the shipped copy's
rank.

Switches read once per process keep the first game's values. The log line says so on a second
start (today, `game_presets.cpp:539-543`).

**Preloaded:** nothing. Two known order gaps are not covered by #125:
`AsyncReadback::ReadSwitch` runs before the game switch defaults are set
(`src/video_core/video_core.cpp:75`, before the renderer is created at `:89`), and `PowerTuning`
reads its three switches before `main()` (`power_tuning.cpp:34-36`).

**Doctor check `switch_file`** ([proposed, S1][125]) would report:

- a launcher block that differs from what this launcher would write now (typically an
  `EDEN_NX2_EXTRAPOLATE=1` from a launcher older than #122);
- a block that sets `EDEN_NX2_FLUID=1` while Ocarina of Time is in the library and an Intel or AMD
  integrated GPU is present (information only until S3);
- both managed blocks naming the same switch;
- unknown or retired names and text after a value, judged with the installed build's switch list
  (R9), and a byte-order mark;
- a persistent `EDEN_NX2_*` variable;
- a launcher block that nothing rewrites (Custom with no known profile, or no launcher state);
- a portable install, where the launcher's file is not the one read.

## 3. The user folder: the rename copy and imports

| Stored | Where | Stamp | Flushed by | Loaded |
|---|---|---|---|---|
| The rename copy from `lichii` to `guarana` | The whole `%APPDATA%\lichii` folder, copied once, never over an existing `guarana` folder (today, `src/common/fs/data_dir_migration.h:33-38,84-87`, `src/common/fs/path_util.cpp:117-128,210-231`) | none | Never repeated once `guarana` exists, even an empty one (issue #107) | At the first use of the path manager |
| Import from Eden, Citron, Sudachi, Suyu or yuzu | A `<root>.migration` state folder; files copied, moved or linked into `guarana\config` (today, `src/frontend_common/profile_import.cpp`) | Only storage paths are remapped. [Proposed (S4)][125]: imported keys that differ from their default get `<key>\origin=import:<emulator> <value>` | Never, for settings | Before any config is built (today, `src/frontend_common/config.cpp:32`) |

**Doctor check `user_folder`** ([proposed, S1][125]) would report:

- `lichii\config\qt-config.ini` exists, and `guarana\config\qt-config.ini` is missing or holds only
  seed values and defaults (the emulator writes a fresh file at its first start, so checking only
  whether the file exists misses #107); the same for `nx2_switches.txt`;
- `lichii\nand` or `lichii\sdmc` exist while the `guarana` ones are empty (presence only: nothing
  in them is read or copied);
- an import that completed with the Link strategy (the other emulator still writes those files).

The flush would offer to merge the settings keys of the two `qt-config.ini` files and
`config\custom\*.ini`, stamped `import:lichii`. It would leave out `[Data Storage]`
(`nand_directory`, `sdmc_directory`, `config.cpp:300-322`) and every path, and copy nothing else:
[proposed (S6)][125].

## 4. Global emulator settings

| Stored | Where | Stamp | Flushed by | Loaded |
|---|---|---|---|---|
| Every saved setting, as a value plus `<key>\default` | `%APPDATA%\guarana\config\qt-config.ini` (portable: `user\config` beside the program) | Only `\default`, recomputed at every save as "value equals the code default" (today, `config.cpp:984-993`). No schema version. [Proposed (S4)][125]: `<key>\origin=<writer> <value>`, which holds only while the stored value equals the stamped one | Configure > Reset to defaults deletes the file and `custom\`, and the file is created and seeded again (today, `src/lichii/main_window.cpp:4208-4230`). [Proposed (S6)][125]: the flush resets stamped values by rule | At process start and at every `Reload`, which also rewrites the file (today, `config.cpp:913-917`) |
| First-run seed: `scaling_filter=Fsr3`, `gpu_accuracy=Low`, `nvdec_emulation=Gpu`, `async_presentation=true`, `use_asynchronous_shaders=true`, each with `\default=false` | `[Renderer]` (today, `config.cpp:139-151`) | None today: a seed looks like the player's choice. [Proposed (S4)][125]: `origin=seed`, and the same for the launcher's copy of the seed (S5) | [Proposed (S7)][125]: when the installed build's seed changes (R9), a value still stamped `seed` is set to the new seed. Never reset to the code default: the seed is the project's choice | Like any global value |
| `[Renderer]` keys the launcher writes: `hdr_mode`, `hdr_swapchain`, `frame_gen*`, `use_asynchronous_shaders`, `vram_usage_mode`, `resolution_setup`, `fps_limit` | `[Renderer]` | None today. [Proposed (S5)][125]: `origin=launcher:<mode>` (or `launcher:community`) plus the value before, in the journal. A profile applied by hand (Custom) writes no stamp: its values are the player's | Rewritten before every launch in a mode (today, `modes.rs:246-259`). Custom leaves `qt-config.ini` alone (today, `launch.rs:189-204`). [Proposed (S5)][125]: a key changed since the launcher wrote it, without a `statusbar` stamp, is the player's: the launcher leaves it alone and the autopilot stops steering it. A key the mode stops writing gets its old value back. Keys written before S5 are adopted once, with an unknown old value, and only reported | Like any global value |
| Status-bar changes during play (frame generation, anti-aliasing, scaling filter, GPU accuracy) | The chip or hotkey sets the value in memory (today, `main_window.cpp:1175,1248,1289,1342,4435`, through `SetValue`, `src/common/settings_setting.h:379-394`); the global config saves it when the emulator closes (`src/qt_common/config/qt_config.cpp:59-62`) | [Proposed (S4)][125]: the chip records its origin in memory and the save writes `origin=statusbar`; a stamp that no longer matches is deleted at the next save | [Proposed (S6)][125]: the flush offers to put it back, ticked. On a key the launcher writes, the launcher overwrites it at the next start, as today | Like any global value |
| Disabled add-ons per title (only disables are stored) | `[DisabledAddOns]` in the global file (today, `config.cpp:353-373`) | none | Reset to defaults clears them (today, `main_window.cpp:4227`) | Global load |
| Backup for "Use recommended settings" | `[Miscellaneous] recommended_settings_backup`, written by "Use recommended settings" (`src/common/recommended_settings.cpp:55`) | none | Cleared by Restore previous settings (today, `recommended_settings.cpp:77`) | By Restore only |
| First-boot report list | `config\presets_first_boot.txt` (`game_presets.cpp:256-258`) | none | Never, not even by Reset to defaults | At each renderer creation for a listed title |

An enum value out of range (for example from a fork with another enum order) is clamped to the
last value without a log line today (`settings_setting.h:382-385`). [Proposed (S4)][125]: the key
and raw value are logged at load.

**Doctor check `settings_file`** ([proposed, S1][125]) would report:

- `use_disk_shader_cache=false` or `use_vulkan_driver_pipeline_cache=false`;
- values stamped `seed`, `statusbar` or `import:*` (after S4);
- keys the launcher wrote that the current mode no longer writes (after S5);
- values with a measured cost above the default's, whatever their origin, since values saved
  before S4 carry no stamp (offered, not ticked);
- Ocarina of Time's built-in `Max Resolution` add-on, which the preset enables on every Intel
  integrated GPU without matrix cores (`game_presets.cpp:177-186`), on a GPU that is not the
  Arc 140T it was measured on (information only).

## 5. Per-game settings

| Stored | Where | Stamp | Flushed by | Loaded |
|---|---|---|---|---|
| `<key>\use_global` and, when false, the value | `config\custom\<TITLEID>.ini` (today, `config.cpp:43-48`) | `use_global`. [Proposed (S4)][125]: `origin` for imported and recommended values | Reset to defaults empties `custom\` (today, `main_window.cpp:4213-4216`). [Proposed (S6)][125]: the flush offers imported per-game values only | When that title boots (today, `main_window.cpp:2327`) and when Properties opens |

## 6. Game presets (memory only)

| Stored | Where | Stamp | Flushed by | Loaded |
|---|---|---|---|---|
| Held preset values | `preset_values` in `src/common/settings.cpp:303-305` | The value it held; held only while unchanged (`settings.cpp:307-312`) | `ClearPresetValues` at load, headless setup and shutdown (today, `core.cpp:498,532,743`) | At each renderer creation, when the setting is not per-game and the global value equals the default (today, `game_presets.cpp:282-291`) |
| Per-title runtime overrides | Memory | none | Reset at each load (today, `core.cpp:497,531`) | During play |

Preset values are never saved: the per-game writer keeps `use_global=true` for a held value
(today, `config.cpp:976-982`). Because a preset applies only where the global value equals the
code default, a seeded, imported or launcher-written value blocks it just as the player's own
choice does.

## 7. Shader cache log and the boot guard

Cache paths are under `%APPDATA%\guarana\cache\shader\<title id>\`. Emulator files are in
`src/video_core/`.

| Stored | Stamp | Flushed by | Loaded | Preloaded |
|---|---|---|---|---|
| Shader cache log `lichii-cache.v2.bin` | Format version in the header (today, `shader_cache_v2/cache_format.cpp:216-220`); a lowering stamp per program | Compacted by the emulator (`shader_cache_v2/cache_file.cpp:184`). A log of another format version is neither read nor written, for good (today, `shader_cache_v2/cache_writer.cpp:137-141`). [Proposed (S7)][125]: when its format is older than the installed build's (R9), the launcher renames it to `lichii-cache.v2.bin.format<N>`; a newer format (after an emulator rollback) is left alone; a `.format<N>` file is renamed back when its format is current again and no log exists | At every boot and warm-up (today) | When Home is idle, the launcher reads Ocarina of Time's `*.bin` files once per session into the OS file cache (today, `launcher/ui/src/screens/home.ts:302-306`, `launch.rs:57-96`). [Proposed (S7)][125]: only current files, in boot order |
| `.booting` marker and its lock | none | Cleared by the next boot that holds the lock (today, `renderer_vulkan/vk_pipeline_cache.cpp:1687-1703,1845-1861`) | At the start of every load | none |
| Temporary `*.tmp` and `.migrating` files | none | `*.tmp` older than an hour: the doctor deletes them on request (today, `launcher/src-tauri/src/doctor.rs:520-548`). `.migrating`: [proposed (S1 reports, S6 offers)][125] | none | none |

**Doctor check `cache_files`** ([proposed, S1][125]) would report a log whose header has another
format version; a `.migrating` file older than an hour with no emulator running; and old
`vulkan.fork-v*` files that the log has replaced (information only).

## 8. Driver pipeline cache and driver blobs

| Stored | Where | Stamp | Flushed by | Loaded |
|---|---|---|---|---|
| Driver pipeline cache file | `<title id>\vulkan_pipelines.fork-v54.bin` (`pipeline_cache_files.h:30`) | Magic number and cache version only; no driver or device stamp (today, `vk_pipeline_cache.cpp:4635-4675`) | Deleted at load when cut short, of another version, or rejected by the driver (today, `vk_pipeline_cache.cpp:4727-4799`). Written only where the driver does not keep its own cache (today, `:868-869`) | At every boot |
| Driver blobs | `<title id>\lichii-cache.v2.driver.bin` | Device identity, including `pipelineCacheUUID` (today, `shader_cache_v2/driver_blobs.h:27-35`) | Not used for another identity | At every boot |
| The driver's own cache | A folder the driver manages | The driver | The driver's own rules. Nothing in Guarana sees it | By the driver |

**Preloaded:** the warm-up (`guarana-cli --warm-pipelines`, [warm-up.md](warm-up.md)) builds into
the driver cache. The launcher runs it on its own only after a GPU driver change (today,
`launcher/src-tauri/src/warm.rs:431-483`, started at `main.rs:84-87`), and when the player asks.
[Proposed (S7)][125]: also after an emulator update or rollback. Settings and switch changes that
change the stamp are not covered by #125.

## 9. Precache lists and markers

| Stored | Where | Stamp | Flushed by | Loaded |
|---|---|---|---|---|
| `precache-graphics.bin`, `precache-graphics-libraries.bin` | `<title id>\` | Driver version, device id, cache version, and a variant that includes the switches that change the code (today, `vk_pipeline_cache.cpp:507-536`). [Proposed (S8)][125]: also `pipelineCacheUUID` | Removed when the driver cache file is missing or holds only a header after load (today, `vk_pipeline_cache.cpp:1833-1843`), only where the emulator keeps a driver cache file (`:868-869`; not on NVIDIA, AMD's own driver or RADV, `vulkan_common/vulkan_device.cpp:1404-1411`). [Proposed (S7)][125]: the launcher does the same before a warm-up, only where the markers name an Intel GPU and the cache setting is on. [Proposed (S8)][125]: the marker records whether the emulator keeps the file, and is ignored where the driver keeps its own cache | Warm-up only |
| `warm-compute.bin`, `.count`, `.done` | `<title id>\` | `.done`: file hash, driver, device, cache version, and a variant without switches (today, `vk_pipeline_cache.cpp:2704-2707`). [Proposed (S8)][125]: the same variant as the lists | As above (`shader_warm/shader_warm_builder.cpp:498-512` removes the lists and `.done`); `.bin` is rebuilt when stale by size, cache or builder version (today, `shader_warm_builder.cpp:534-563`) | Precache at boot |

A pipeline whose build failed is still listed as built in a warm-up today
(`vk_pipeline_cache.cpp:2449-2459`). [Proposed (S8)][125]: listed only when it built.

The doctor would watch these through `cache_files` ([proposed, S1][125]): a marker present while
the driver cache file is missing or holds only a header (issue #106), judged only where the
emulator keeps that file. On other drivers a missing file is normal, and nothing is reported.

## 10. During play and at shutdown

| Stored | Where | Flushed by |
|---|---|---|
| Autopilot steering (with `EDEN_NX2_AUTOPILOT=1`) | Memory, kept out of saves | Put back at the end of the session (today, `src/core/nx2_autopilot_session.cpp:118-131`) |
| Debug server `power_set`, `frame_budget`, `speed_limit` | Memory | Not reset between games today (`src/core/debugger/nx2_debug_tools.cpp`); not covered by #125 |
| Global settings | `qt-config.ini` | Saved when the emulator closes (today, `qt_config.cpp:59-62`). This is why a flush must wait until the emulator is closed (R5) |

## When the doctor runs

| Moment | What runs | Status |
|---|---|---|
| Doctor screen and Scan | All checks | Today (`launcher/src-tauri/src/commands.rs:177-178`); the new checks: [proposed (S1)][125] |
| Launcher start, after the bundled install | File checks, a stale launcher block rewritten, recovery of an interrupted flush; only while the emulator and a warm-up are closed (R5) | [proposed (S6 recovery, S7 checks)][125] |
| After an install, update or rollback | Cache format, seed changes and switch names, against the newly current build's `build-facts.json` (R9) | [proposed (S7)][125] |
| After a GPU driver change, before the automatic warm-up | Cache markers, where the emulator keeps the driver cache file | [proposed (S7)][125] |
| Before every launch | The launch's own block rewrite (today, `launch.rs:169-204`); a fast pass for persistent variables and unknown overrides | Block: today. Fast pass: [proposed (S7)][125] |
| After the emulator process exits | Status-bar values saved in that session, shown as one Home card | [proposed (S7)][125] |
| Inside the emulator at boot | The switch-default decisions and their source (today, `game_presets.cpp:530-545`); the driver cache markers (today); out-of-range enum values ([proposed, S4][125]) | Mixed |

[125]: https://github.com/karimalsalah/guarana/issues/125
