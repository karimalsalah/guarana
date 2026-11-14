# Guarana Launcher: backend ↔ UI contract

The launcher is a Tauri 2 app in `launcher/`: Rust backend in `launcher/src-tauri/`, web UI in
`launcher/ui/` (Vite + TypeScript, no heavy framework). It is the Windows installer (Tauri's NSIS
bundle, per-user, no admin), the first-run setup, the game launcher, the updater and the doctor.

Hard rules (legal and trust):
- Never download, bundle, share or upload keys, firmware, games or any file taken from them. The
  launcher only **finds files the user already has on their own disk**, with consent, and copies them
  into the emulator's user folder when the user clicks Import.
- Nothing leaves the machine except: GitHub API/release downloads of Guarana itself, and the
  user-initiated "Report a problem" and "Share my numbers" flows, which open a browser with a prefilled
  issue the user reviews before submitting. No telemetry by default.
- Every path or file name that could identify the user is scrubbed from reports
  (`C:\Users\<name>` → `%USERPROFILE%`, host name removed).

## Commands (`invoke(name, args)`)

| Command | Args | Returns |
|---|---|---|
| `detect_system` | – | `SystemInfo` |
| `list_profiles` | – | `Profile[]` (with `recommended` set from `detect_system`) |
| `get_state` | – | `LauncherState` |
| `check_updates` | `{ channel?: "stable" \| "nightly" }` (default: the installed version's channel) | `UpdateInfo` |
| `install_version` | `{ version: string, channel: "stable" \| "nightly" }` | `InstallResult`; streams `install://progress` events |
| `set_current_version` | `{ version }` | `LauncherState` (rollback = set an older installed version) |
| `locate_files` | `{ scope: "quick" \| "deep", roots?: string[] }` | `Found`; streams `locate://progress`. A deep search with no `roots` covers every fixed drive |
| `import_keys` | `{ path }` | `ImportResult` |
| `add_game_folder` | `{ path }` | `Game[]` |
| `list_games` | – | `Game[]` |
| `apply_profile` | `{ profile_id, overrides?: Record<string,string\|number\|boolean> }` | `AppliedProfile`. `overrides` holds only the toggles that differ from `Toggle.default`. A manual apply sets the mode to `custom` |
| `run_doctor` | – | `Check[]`; streams `doctor://progress` |
| `apply_fix` | `{ fix_id }` | `Check` |
| `prefetch_caches` | `{ title_id }` | `{ bytes_warmed: number }` (reads shader/pipeline cache files into the OS file cache in the background) |
| `launch_game` | `{ title_id?: string, path?: string, profile_id?: string }` | `{ pid: number, follows_boot: boolean }` (`follows_boot`: the emulator writes its boot, a game in a launcher session); emits `game://boot` `BootProgress` while the game boots and `game://exited` `{ code, seconds }`. Runs `<current>\guarana.exe --launcher-session -f -g <file>`, or `-f -g <file>` for a version older than 20.3.0, a nightly or a pre-release (see "Launcher session") |
| `collect_report` | `{ kind: "bug" \| "performance" \| "crash", note?: string }` | `{ zip_path: string, issue_url: string, summary_md: string }` |
| `open_path` | `{ path }` | – (opens Explorer) |
| `open_url` | `{ url }` | – (default browser; only https URLs) |
| `list_controllers` | – | `Controller[]` (connected pads); `controller://changed` carries the new `Controller[]` whenever one connects, disconnects or its battery moves |
| `warm_shaders` | `{ title_id }` | `WarmResult`; streams `warm://progress` (`WarmProgress`). Runs `<current>\guarana-cli.exe --warm-pipelines <title_id> -c %APPDATA%\guarana\config\qt-config.ini` (`eden-cli.exe` in a build from before the rename) hidden at below-normal priority ("Prepare shaders" on a game card). Refused while `guarana.exe` (or `lichii.exe`) runs. The launcher also runs it by itself for every title with a shader cache after a GPU driver change (`auto: true` in the events) |

| `stop_warm_shaders` | – | `StopResult`. Ends the running warm-up (the process is ended; guarana-cli stops politely only on Ctrl+C), waits up to 15 s and reports. A stopped automatic run is not resumed and its driver counts as done. The pending `warm_shaders` call resolves with status `stopped` |
| `pick_folder` | `{ title?: string }` | `string \| null` (Windows folder picker; `null` when cancelled) |
| `get_mode` | – | `ModeStatus`. Refreshes the community data first when it is older than 24 h (6 s at most, then the cache or built-in table) |
| `set_mode` | `{ mode: "auto" \| "smooth" \| "quality" \| "battery" }` | `ModeStatus`. Saves the mode, clears the autopilot's adjustments for it and writes the settings; `note` says when they wait for the emulator or a warm-up to end |
| `autopilot_undo` | – | `ModeStatus`. Undoes the newest autopilot change; that setting is then left alone until the mode changes |
| `autopilot_enable` | `{ enabled: boolean }` | `ModeStatus` |
| `ask_ai` | `{ service: "claude" \| "chatgpt", issues?: string[], question?: string }` | `AskAi`. Builds a scrubbed summary (same scrubbing as reports) and opens `https://claude.ai/new?q=` or `https://chatgpt.com/?q=` in the default browser; the whole URL is at most 1950 characters and the prompt names `https://karimalsalah.github.io/guarana/llms-full.txt` |
| `get_credits` | – | `Credits` `{ as_of, commits, people: { login: string \| null, name: string \| null, commits, coauthored }[], tools: { name, commits }[], source: "builtin" \| "cache" \| "network" }`. The hall of fame (`https://karimalsalah.github.io/guarana/data/legacy.json`, made by `tools/ci/legacy.py`), cached with its ETag in `%LOCALAPPDATA%\Guarana\cache` for 24 h, schema-checked (names stripped of control characters and angle brackets), 6 s at most, then the cache or the copy embedded at build time from `site/data/legacy.json` (`build.rs`; empty when built outside the repository). Shown on the Credits screen (footer), people first and tools apart |
| `get_share_status` | – | `ShareStatus` `{ enabled, offer: ShareOffer \| null }`. After a game session with a clean exit and at least 60 s of gameplay data (`SessionMetrics.gameplay_secs`: the fluid presenter's windows with real frames, else the stutter summaries', else 10 s per `Shown frames` window) the backend keeps one offer (`share.rs`): game fps, shown fps, p50/p95/p99, the share of frames within 2 ms, game, mode, profile, build. `offer.text` is "Your session: 25 game fps, 93% of frames on time. Share it with the community?". `enabled: false` after the setting is off |
| `share_open` | – | `ShareLink` `{ url, url_chars, level: "full" \| "short" \| "clipboard", text }`. The `device-report.yml` form prefilled by field id, text fields only (`device`, `cpu`, `driver`, `ram`, `os`, `power_mode`, `build`, `settings`, `results`, `notes`); the scene, the screenshot and the choices are left to the user. At most 7000 characters (`full`); else the essentials in at most 2000 (`short`); else an empty form (`clipboard`). The UI copies `text` to the clipboard for `short` and `clipboard`, then opens `url`. User name, profile folder and host name are scrubbed like reports. Nothing is sent |
| `share_dismiss` | – | `ShareStatus` ("Not now", and after Share): drops the offer |
| `share_set_prompt` | `{ enabled: boolean }` | `ShareStatus`. The setting to stop asking (Home → Advanced); off also drops the offer |
| `take_launch_request` | – | `LaunchRequest \| null`, once: the game a file opened from Explorer or a `guarana://` link asked for (see "Opening games from Windows"). The UI calls it after its first screen and on every `launch://request` |
| `get_notifications` | – | `Notifications` `{ enabled }` |
| `set_notifications` | `{ enabled: boolean }` | `Notifications`. Windows notifications (Home → Advanced), off by default: shader warm-up finished, update installed. Turning them on shows one right away |
| `get_booster` | – | `BoosterStatus` (the Game Booster: chosen level, whether a session is active, and every level with what it changes on this PC) |
| `set_booster_level` | `{ level: number }` (0 = Off … 3) | `BoosterStatus`. Saves the level; it takes effect at the next launch |
| `booster_undo_all` | – | `BoosterStatus`. Restores every change the booster made right now |

Argument keys are snake_case exactly as written (`invoke("apply_profile", { profile_id, overrides })`):
the commands are declared with `rename_all = "snake_case"`. Errors reject the promise with a
readable string.

When the backend says no (the promise rejects with a sentence to show):
- `launch_game` while `guarana.exe` (or `lichii.exe`) already runs, or while shaders of the same title are being
  prepared (the message has the percentage; it can start when that finishes). A warm-up of another
  title is stopped first and resumes after the game exits.
- `apply_profile` while the emulator or a warm-up runs: both save the settings when they end.
- `stop_warm_shaders` with nothing running answers `{ was_running: false }`, not an error.
- `set_mode`, `autopilot_undo`: the mode is saved even while the emulator or a warm-up runs; the settings are written at the next start (`ModeStatus.note`).
- `install_version` while another install runs (also the first-run install of the bundled build,
  which `get_state` waits for).

## Types

```ts
type Vendor = "intel" | "amd" | "nvidia" | "other";
interface Gpu { name: string; vendor: Vendor; driver_version: string; is_integrated: boolean;
  dedicated_mb: number; shared_mb: number; vulkan: boolean; arch_hint?: string;
  family: string | null; }   // Intel iGPU family matched by PCI id, e.g. "Panther Lake Intel Graphics"
interface SystemInfo {
  os: { name: string; build: number; display_version: string };            // e.g. 26100, "24H2"
  cpu: { name: string; cores: number; threads: number; avx2: boolean; avx512: boolean; hybrid: boolean };
  ram_gb: number; pagefile_gb: number;
  gpus: Gpu[]; primary_gpu: number;                                           // index into gpus
  display: { width: number; height: number; refresh_hz: number; vrr: boolean | null; hdr: boolean | null };
  power: { on_ac: boolean | null; battery_percent: number | null; plan: string | null };
  hags: boolean | null;                                                       // hardware-accelerated GPU scheduling
  device: { manufacturer: string; model: string; form: "laptop" | "handheld" | "desktop" | "unknown";
            profile_hint: string | null };                                    // e.g. "msi-claw-8-ai-plus"
  vc_redist: boolean; webview2: string | null; free_disk_gb: number;
}
interface Profile { id: string; name: string; description: string; recommended: boolean;
  toggles: Toggle[]; }                                                        // shown as switches in the UI
interface Toggle { id: string; label: string; help: string; value: boolean | number | string;
  default: boolean | number | string;   // the profile's own value; `value` has the saved changes merged in
  kind: "bool" | "choice" | "number"; choices?: string[]; experimental?: boolean; }
interface LauncherState { install_dir: string; user_data_dir: string;
  installed: { version: string; channel: string; path: string; installed_at: string }[];
  current: string | null; profile_id: string | null; first_run: boolean;
  keys_present: boolean; firmware_present: boolean; game_folders: string[]; }
interface UpdateInfo { current: string | null; latest: string; channel: string; published_at: string;
  notes_md: string; size_bytes: number; sha256: string; update_available: boolean; }
interface InstallResult { version: string; path: string; verified_sha256: boolean; seconds: number; }
interface Found { keys: { path: string; kind: "prod" | "title" | "unknown"; source_hint: string }[];
  firmware: { path: string; version: string | null; source_hint: string }[];
  games: Game[]; scanned_dirs: number; seconds: number; }
interface Game { title_id: string; title: string; path: string; container: "nup" | "nsp" | "xci" | "nx" | "other";
  size_bytes: number; is_ocarina: boolean; }
interface ImportResult { ok: boolean; dest: string; message: string; }
interface AppliedProfile { profile_id: string; switches_path: string; config_path: string; changed: string[]; }
interface Check { id: string; title: string; status: "ok" | "warn" | "fail" | "info"; detail: string;
  fix_id: string | null; fix_label: string | null; }
interface Controller { id: string; name: string; kind: "xbox" | "playstation" | "switch" | "generic";
  connected: boolean; battery: number | null; }                              // battery 0-100, null when wired/unknown
interface WarmProgress { title_id: string;                                    // 16 hex digits, upper case
  stage: "ready" | "boot-builds" | "precache" | "retry" | "done";
  done: number; total: number; pass: number; percent: number; seconds: number;
  auto: boolean; message?: string; }                                          // auto: the run after a driver change
interface WarmResult { title_id: string;
  status: "done" | "incomplete" | "stopped" | "nothing built" | "nothing to do" | "failed";
  seconds: number; built: number; already: number; left: number;             // from guarana-cli's last line
  exit_code: number | null;   // 0 ok; 1 configuration problem (see message); 3 nothing built even after the
                              // automatic second run (leave the title to the normal boot); 4 stopped or
                              // pipelines left over: run it again
  message: string | null; }
```

type ModeId = "auto" | "smooth" | "quality" | "battery";
interface ModeStatus {
  mode: ModeId | "custom";            // "custom" after a manual Apply under Advanced: nothing changes it until a mode is picked
  profile_id: string; profile_name: string;
  reason: string;                     // one sentence: what the mode chose and why
  overrides: Record<string, string | number | boolean>;
  community: { class: string; label: string; samples: number; validated: number; min_validated: number;
               source: "builtin" | "cache" | "network"; used: boolean; applied: string[] };
  autopilot: AutopilotView;
  note: string | null;
}
interface AutopilotView { enabled: boolean; history: AutopilotChange[]; can_undo: boolean; latest: AutopilotChange | null; }
interface AutopilotChange { id: number; at: string; mode: string; kind: "change" | "action" | "note";
  setting: string; from: boolean | number | string; to: boolean | number | string; because: string;
  text: string;                       // "Autopilot changed the resolution from 1x to 0.75x because the game ran at 12 fps, under 15."
  title_id?: string | null; undone: boolean; }
interface StopResult { was_running: boolean; title_id: string | null; stopped: boolean; message: string; }
interface AskAi { service: "claude" | "chatgpt"; url: string; prompt: string; url_chars: number; }
interface LaunchRequest { source: "file" | "link";  // Explorer, a shortcut or a second start; or guarana://
  game: Game | null;                  // checked; `container` is "other" and `title_id` may be "" for an .nca
  error: string | null; }             // why it can't start, to show as is
interface Notifications { enabled: boolean; }

interface BoosterLevelInfo { level: number; title: string; summary: string;
  elevated: boolean;                   // true when the level shows one Windows permission (UAC) prompt
  changes: string[]; }                 // the exact changes the level makes on this PC, in order
interface BoosterStatus { level: number;   // chosen level: 0 = Off (default) … 3
  active: boolean;                      // a boosted session is running now
  levels: BoosterLevelInfo[];           // Off..3, for the picker and the "what it changes" list
  note: string | null; }

Progress events carry `{ stage: string, done: number, total: number, message?: string }`.

Events: `install://progress` (stages `resolve`, `download`, `verify`, `extract`, `done`; bytes for
`download`, files for `extract`), `locate://progress` (`scan` with done/total = roots, then `done`),
`doctor://progress` (stage = the check id being run, then `done`), `game://boot` (`BootProgress`,
see "Boot progress and the hand-over"), `game://exited`
(`{ code, seconds }`), `controller://changed` (`Controller[]`), `warm://progress` (`WarmProgress`), `autopilot://changed`
(`ModeStatus`, after a game's log was read), `share://changed` (`ShareStatus`, after a session was looked at or the setting changed),
`launch://request` (no payload: a `LaunchRequest` waits for `take_launch_request`),
`booster://changed` (`BoosterStatus`, after the booster applies for a session, restores it, or the level changes).

## Launcher session

`launch_game` starts the emulator with `--launcher-session` before `-f -g <file>` (`launch.rs`,
`src/core/launch_params.cpp`). The emulator then:

- never shows its desktop window (menu bar, status bar, game list): the window first appears as the
  fullscreen loading screen, black, once the game file has been read, and the game follows on the
  same surface. Until then the launcher's own screen stays up, with the boot's progress (see "Boot
  progress and the hand-over" below). The emulator asks for the foreground when its window appears; the launcher
  allows that with `AllowSetForegroundWindow`. It uses the fullscreen mode set in the emulator
  (borderless or exclusive), and plays in the main window even when the player chose separate
  windows there (that setting is saved unchanged). The "leave fullscreen" hotkey (Esc by default)
  stops the game instead, with the usual stop confirmation; the fullscreen toggle does nothing. The
  hint toast, where enabled, names that key and the controller's stop combo instead of the menu;
- exits when the emulation stops, for any reason (the stop or exit hotkey, closing the window, the
  game ending itself), instead of going back to its game list: exit code **0**. A game that did not
  start (a load error, a missing firmware or key, a cancelled user or low-RAM dialog) exits with
  **1** once its error dialog is closed. A crash ends the process with the crash code, as before. A
  restart or a game that starts its own sub-program boots again and does not end the session;
- shows its dialogs (load errors, confirmations, applets such as the software keyboard) as before,
  above the fullscreen window or, before the window appears, on their own.

### Boot progress and the hand-over

While the game boots, the emulator writes its boot to the standard output the launcher gives it
(a pipe, only with `--launcher-session`), one line each, unbuffered: every `Boot timing:` mark with
the same text as in its log (`src/common/boot_timing.cpp`, `SetSessionOutput`), and
`Boot progress: <stage> <done> <total>` for the counts its loading screen shows (stage `prepare`,
`build`, `warm` or `complete`; one line per stage and whole percent,
`Common::BootTiming::SessionProgress`). It stops writing once the game is on screen, but the pipe
stays open until it exits. The log
file is not used for this: it is written in 4 KiB blocks unless per-line flushing is on, and the log
filter can leave the marks out. The launcher reads the pipe to its end, so the emulator's writes,
which block on a full pipe, never wait on it; if the launcher is gone, the first failed write ends
the output.

`boot.rs` turns the lines into `game://boot` events:

    BootProgress { phase: "starting" | "loading" | "shaders" | "precache" | "launching"
                          | "first-frame" | "shown" | "window"; done: number; total: number }

| Phase | From | Words on the launch stage |
|---|---|---|
| `starting` | `frontend:`, `startup:` and `vulkan:` marks | Starting Guarana… |
| `loading` | `boot:`, `core:`, `nup:`, `romfs:` marks | Loading the game… |
| `shaders` | `shaders:` marks, `Boot progress: prepare` and `build` (done of total pipelines) | Loading shaders…, with the count after it |
| `precache` | `Boot progress: warm` (done of total; the emulator's loading screen can skip it) | Preparing shaders ahead…, with the count after it |
| `launching` | `Boot progress: complete`, `guest:` marks | Starting the game… |
| `first-frame` | the mark `frontend: first guest frame presented` (`GRenderWindow::OnFrameDisplayed`, after the first composite of a guest frame; the loading screen still covers it and fades out over 0.5 s) | |
| `shown` | the mark `frontend: loading screen hidden, game shown` (the loading screen's fade is over and the render window is shown; also in the log of every session) | |
| `window` | not from the pipe: a window of the emulator's process has the foreground (`GetForegroundWindow`, polled every 200 ms for up to 15 minutes, for every version) | |

Phases only move forward (a late `vulkan:` mark does not take `loading` back), `shaders` and
`precache` can follow each other on progress lines only (a `shaders:` mark after the precache keeps
`precache`), and an event is emitted only when the phase moves or the counts
change. `done` and `total` are 0 outside `shaders` and `precache`; the count is never shown without a
real one.

The launch stage holds (`screens/launch.ts`) until the game is on screen, then shows the title "is running":

- at `shown` at once, and 1 s after `first-frame` if `shown` does not come;
- at `window` it pauses its animations (the emulator's window covers it, and they would take GPU
  time from the shader build) and goes on following the boot; with no boot lines at all (a version
  before this output), it hands over 1 s after `window` (before this change: 1 s after the start);
- without news it hands over anyway: 20 s with nothing at all (45 s when `launch_game` returned
  `follows_boot`, as the first line comes after the emulator's own start-up), 60 s after the last
  line once the emulator's window has the foreground. Before this change the stage dimmed to black
  and read "Playing" about a second after the start, whatever the emulator did. Once the
  emulator's window has had the foreground the dimming is skipped, as nobody would see it.

The emulator's own window still appears when the game file is read (its fullscreen loading
screen), so the launcher's stage is what the player sees from Play until then, and what they find
on Alt+Tab while the shaders load. Keeping the launcher's stage in front of the emulator until the
first frame (a topmost launcher, or the emulator cloaking its window) is not done: a topmost
launcher would hide the emulator's dialogs (load errors, user selection, the software keyboard)
behind it, and the cloaked window needs a check on a device first.

`game://exited` brings the launcher back as soon as the process is gone: it is restored if
minimized and shown, and brought to the front (`set_focus`) only when the foreground window at the
exit belongs to the emulator, the launcher or nobody: a player who moved to another app keeps it.
`code` is that exit code.
A code of 1 can arrive late (the player may leave the load error open for minutes), so `seconds`
alone does not tell a failed start from a session; the autopilot ignores a code-1 session that
showed no frame (`autopilot::decide`).

Only versions that understand the flag get it: a stable version of 20.3.0 or later
(`LAUNCHER_SESSION_SINCE` in `launch.rs`, compared with `current.json`). The flag was added when the
repository's `VERSION` read 20.2.0; the assumption is that the next release is 20.3.0 or later. A
nightly, a pre-release or any other label gets `-f -g <file>` only, as do older versions the
launcher installs or rolls back to: they show the emulator's window around the game and go back to
its game list when the game stops, until the player closes it. An older build that receives it
ignores it (getopt reports an unrecognized option on stderr and parsing continues), so the gate is
about behaviour, not safety.

## Opening games from Windows

The launcher is a single instance (tauri-plugin-single-instance): a second start brings the running
window to the front and hands over its command line, so every way in below reaches the running
launcher.

- **File associations.** The installer registers `.nsp`, `.xci`, `.nup` and `.nca` (`bundle >
  fileAssociations`, ProgIDs `Guarana.nsp` …) for the current user, plus `OpenWithProgids` entries
  (`src-tauri/windows/installer-hooks.nsh`). Explorer starts `guarana-launcher.exe "<file>"`.
  `args.rs` takes the first argument with one of those extensions (made absolute against the
  starting folder; flags and `guarana:` arguments are skipped); `open.rs` checks it is a file and
  names it from the library entry with the same path, else from its header or file name (an `.nca`
  keeps its file name: the library scan leaves `.nca` to firmware).
- **`guarana://` links** (tauri-plugin-deep-link, scheme `guarana` in `plugins > deep-link`). Windows
  starts the launcher with the link as its only argument; a running launcher gets it through the
  single-instance plugin's `deep-link` feature, a new one reads it at start. Accepted, nothing else:
  - `guarana://launch/<title id>` or `guarana://launch?title_id=<title id>`: 16 hex digits (`0x`
    allowed), a game of the library whose file is still there.
  - `guarana://launch?path=<percent-encoded full path>`: checked as text first, then on disk. A
    drive-letter path (`/` taken as `\`), no network or device path (`\\server\…`, `\\?\`, `\\.\`), no
    `.` or `..` segment, no second `:`, no `< > " | ? *`, control character or reserved device name
    (`CON`, `NUL`, `COM1` …), extension `.nsp` `.xci` `.nup` or `.nca`; it must sit inside one of the
    game folders the user added, and only then is the disk read: a regular file, not a link to one.
    `%20` is a space and `+` stays a plus.
  - Refused: any other scheme, action or parameter, a user name, password, port or fragment, repeated
    parameters, both or neither of a title id and a path, links over 2048 characters. A link only
    ever picks a game for `launch_game`; it never names a program, an argument or a setting.
- **The UI** takes the request with `take_launch_request` and opens the launch screen, which calls
  `launch_game` with the `path` (and `title_id` when known), the same start as a library game. It
  shows `error` instead, and does not navigate while a game starts or runs, before the first run is
  finished or without keys.
- **Notifications** (tauri-plugin-notification) are only sent when `set_notifications` turned them on
  and the launcher is not the active window: a `warm_shaders` or autopilot warm-up that ended
  `done`, `incomplete`, `nothing built` or `failed` (one per game), the automatic warm-up after a GPU
  driver change (one for all games), and a successful `install_version` (also the doctor's
  `install_update`; not the first-run install of the bundled build).
- **Taskbar progress** (Tauri's `set_progress_bar`) follows `install://progress` (download and extract
  as percentages, the other stages pulsing; the first-run install too) and `warm://progress`
  (`percent`), and clears when the job ends, however it ends. An install shows over a warm-up.

## Modes, community data and the autopilot

Home has one switch, **Auto** (default), **Smooth**, **Quality**, **Battery**; everything else is under
"Advanced" (autopilot on/off and history, community data, all settings). A mode is a profile plus
bounded changes to its toggles (`modes.rs`), resolved at `set_mode` and again at every `launch_game`
(power source, community data and the autopilot's changes are read then):

| Mode | Profile | Changes | Resolution bounds (iGPU / dGPU) |
|---|---|---|---|
| Auto | `detect_system` recommendation (battery: `low-power-handheld`) | plus the community settings below | 0.5x-1.25x / 0.75x-2x |
| Smooth | recommendation as if plugged in | fluid presenter on (integrated), async shaders, frame generation off, iGPU one step lower resolution | 0.5x-1x / 0.75x-1.5x |
| Quality | recommendation as if plugged in | one step more resolution, aggressive VRAM with 15 GB RAM or more, HDR auto | 1x-1.25x / 1x-2x |
| Battery | `low-power-handheld` | none | 0.5x-0.75x |

`apply_profile` (Advanced) sets the mode to `custom`; the modes and the autopilot then leave the
settings alone. `launch_game` sets `EDEN_NX2_SHOWN_STATS=1` for the emulator it starts, which makes it
log how evenly frames are shown.

**Community best settings.** `https://karimalsalah.github.io/guarana/data/best-settings.json`, made by
`tools/ingest/best_settings.py` from `site/data/community.json`: `{ schema_version: 1, updated,
classes: { "<class>": { label, samples, validated, best, settings } } }` for the classes
`intel-arc-igpu`, `amd-rdna3-igpu`, `nvidia-rtx-desktop`, `nvidia-rtx-laptop`, `amd-desktop`, `other`.
The launcher caches it with its ETag in `%LOCALAPPDATA%\Guarana\cache` for 24 h, checks the schema (wrong
version, unknown classes, unknown setting ids, invalid values and `validated > samples` are dropped or
rejected), and falls back to the cache, then to a built-in table without community data. Auto applies a
class only with at least 3 validated reports, never on battery, and only `fluid_presenter`,
`ocarina_extrapolation` and `gm_routing`.

**Autopilot** (`autopilot.rs`). When a game started by the launcher exits, it reads the emulator log
(`%APPDATA%\guarana\log\eden_log.txt`, then `eden_log.txt.old.txt`, `eden_log.old.txt`, `eden_log.txt.old` and
the kept game log, the first with a `Booting game:` line written during the session): the last
session's `Shown frames:` lines (interval p50/p95, share within 2 ms; the first of three or more windows
is left out), the `Fluid presenter (10 s...)` stats lines (their presence means it ran; `(N real frames)`
over the window's seconds is the game fps whenever it is on, windows with 0 real frames are ignored),
`Nx2Stutter` causes, `Nx2StutterSummary` (game fps without it), `Nx2Boot`
timing, `Shader precache:` and slow `pipeline built on the GPU thread` lines, and the power source at the
start. After sessions of at least 120 s without a crash it makes at most one change for the next
session, in this order: undo a failed even-frame-pacing trial; resolution one step down (game fps
under 20 on battery, under 15 on mains) inside the mode's bounds; a shader warm-up for a title with
shader-build stutter or a slow start in two sessions in a row (at most once a day); an even-frame-pacing
trial where it is off, the frames were uneven (under 92% within 2 ms in two earlier sessions) and
the unit is on mains, kept only if the next session's `Shown frames` are better and the game fps
is not lower, else turned off and not tried for 30 days; resolution one step up after two sessions
at 58 fps or more with even frames. History (50 entries) with the reason, `autopilot_undo`, and
"Autopilot changed X because Y" on Home. State is in `launcher-state.json`; the only files it
causes to be written are the ones `apply_profile` writes.

## Game Booster

A separate, opt-in setting (`booster.rs`), Off by default, saved as `booster_level` (0–3) in
`launcher-state.json`. While a game started from the launcher runs it tunes Windows for speed, and
puts everything back when the game exits, when the launcher starts after a crash (from
`booster-journal.json`), or from the UI's **Undo all**. The levels are cumulative:

| Level | Admin | Adds |
|---|---|---|
| 1 | no | the emulator at **Above Normal** priority; power throttling (**EcoQoS**) off for it (`SetProcessInformation(ProcessPowerThrottling)`); the **high-performance GPU** asked for `guarana.exe` (`HKCU\Software\Microsoft\DirectX\UserGpuPreferences`, `GpuPreference=2;`, other tokens kept and the previous value restored) |
| 2 | one UAC prompt | the active **power plan** switched to Ultimate Performance (if installed) or High performance for the session, then the previous one restored |
| 3 | one UAC prompt | as 2, but the emulator at **High** priority, and **SysMain** and **Windows Search** paused while you play (only if running) and restarted after |

The two things that need administrator rights — the power-plan switch and the services — are done by a
small elevated helper (the launcher re-run with `--booster-helper`, started with `ShellExecuteEx`
"runas" on its own thread, so the prompt never holds up the launch), so a whole session costs one
prompt: the helper applies them, waits for the emulator to exit, and restores them. When a level has
nothing that needs admin on this PC (the plan is already active or none is installed, and no listed
service is running), no prompt appears, and `BoosterStatus.levels[].elevated` is then false.
Everything else is set by the launcher without elevation (the GPU preference before the emulator
starts, so Windows reads it for the run; priority and EcoQoS on the process once it exists). Every change is
written to `booster-journal.json` before it is made and is undone from it. **Security settings are
never touched** (Defender, firewall, VBS/HVCI/Memory Integrity, SmartScreen, UAC, Windows Update,
BitLocker), and nothing under `HKLM` is written except what `powercfg` itself does. The UI (Home)
shows the picker and, for each level, exactly what it changes and whether it needs the prompt.

## Profiles (first set)

`intel-arc-igpu` (Arc 130T/140T/140V, Meteor/Panther Lake Arc, Arrow Lake-H Intel Graphics by PCI id), `amd-igpu-handheld` (Radeon 780M/880M/890M, Z1/Z2),
`low-power-handheld` (any iGPU on battery: 30 fps cap, lower resolution), `desktop-dgpu`, `safe-defaults`.
Toggles map to `%APPDATA%\guarana\nx2_switches.txt` lines and `qt-config.ini` keys, e.g.
`frame_generation` → `[Renderer] frame_gen=true, frame_gen_multiplier=3, frame_gen_target_rate=60`,
`ocarina_extrapolation` → `EDEN_NX2_EXTRAPOLATE=1`, `gm_routing` → `EDEN_NX2_GM=11`,
`igpu_power_policy` (experimental) → `EDEN_NX2_POWER_POLICY=igpu`, `async_shaders`, `vram_mode`.
Also: `fluid_presenter` → `EDEN_NX2_FLUID=1` (on for `intel-arc-igpu` and `amd-igpu-handheld`;
experimental except on `intel-arc-igpu`), `hdr_auto` → `[Renderer] hdr_mode=2, hdr_swapchain=0`
(on except in `safe-defaults`), `resolution` (choice `0.5x`…`2x` → `resolution_setup`) and
`fps_cap` (choice `off`/`30`/`40`/`45`/`60` → `fps_limit`; `low-power-handheld` uses `0.75x` and
`30`).

## Doctor checks (first set)

GPU driver age vs the known-good list (Intel 32.0.101.9175), Vulkan ICD present, VC++ 2015-2022
x64 runtime, AVX2, RAM and page file (Switch 2 titles want 16 GB RAM + a page file that can grow),
free disk for caches, HAGS state (info), power mode on AC/battery, Intel "Shared GPU Memory Override"
(info + link), Windows Defender exclusion for the cache folder (info + opens Windows Security;
never changed silently), stale shader-cache temp files (fix: delete `*.tmp`), keys present,
firmware present, game folder present, Ocarina of Time found, emulator version up to date,
controller connected (`controller`, info when none), Windows HDR (`windows_hdr`, info only: whether
the primary display can do HDR and whether Windows HDR is on; fix "How to turn on HDR" opens
Microsoft's HDR help page, never toggles it), memory free right now (`memory_now`: free RAM and
the commit against its limit at check time; warns below 3 GB of free RAM and says what to close;
below 6 GB of commit left it warns, with a fix that opens System > About for the page file, only
when the page file is capped or absent, and with a system-managed page file is info only),
display refresh rate (`refresh_rate`: when the game's GPU is integrated, where the Ocarina of
Time preset paces frames to 60, warns when the main display's rate is not a multiple of 60 and the
display offers one, naming the nearest; fix opens Advanced display; on a dedicated GPU it is info
only), displays on the integrated GPU (`displays`: warns when the game's GPU is integrated and
drives two or more active displays; fix opens Display settings).
`Gpu.family` names the Intel integrated GPU family whose PCI ids match the Intel rows of
`FamilyRows` in #78's `src/common/device_profile.cpp` (Meteor Lake 7D55, Arrow Lake-H 7D51/7DD1,
Lunar Lake 64A0/64B0, Panther Lake B080-B087, B090, B0A0). Auto (`recommend`, the community class)
treats a match like a GPU named "Arc", except Panther Lake's "Intel Graphics" (B090, B0A0), which
stays on the low-power profile until a paired benchmark shows the Arc profile suits it.
Fixes that change the system only ever open the official Windows/vendor page; file fixes act only
inside Guarana's own folders.

Check ids, in the order `run_doctor` returns them: `emulator_version`, `gpu_driver`, `vulkan`,
`vc_redist`, `avx2`, `memory`, `memory_now`, `disk`, `power`, `hags`, `intel_shared_memory`,
`windows_hdr`, `refresh_rate`, `displays`, `defender`, `shader_tmp`, `keys`, `firmware`,
`game_folder`, `ocarina`, `controller`.
Fix ids: `install_update`, `open_driver_page`, `open_vulkan_driver_page`, `open_vc_redist`,
`install_vc_redist` (runs the `vc_redist.x64.exe` shipped in the installed Guarana folder with
`/install /quiet /norestart` after the click; Windows' own permission prompt appears, nothing is
elevated silently; the check offers it only when the file is found, else `open_vc_redist`),
`open_memory_settings`, `open_about_settings`, `open_storage_settings`, `open_power_settings`,
`open_graphics_settings`, `open_intel_graphics`, `open_hdr_help`, `open_display_refresh`,
`open_display_settings`, `open_windows_security`, `delete_shader_tmp`,
`open_bluetooth_settings`. `apply_fix` returns the check run again; `install_update` streams
`install://progress` like `install_version`.
