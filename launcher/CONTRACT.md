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
| `launch_game` | `{ title_id?: string, path?: string, profile_id?: string }` | `{ pid: number }`; emits `game://exited` `{ code, seconds }` |
| `collect_report` | `{ kind: "bug" \| "performance" \| "crash", note?: string }` | `{ zip_path: string, issue_url: string, summary_md: string }` |
| `open_path` | `{ path }` | – (opens Explorer) |
| `open_url` | `{ url }` | – (default browser; only https URLs) |
| `list_controllers` | – | `Controller[]` (connected pads); `controller://changed` carries the new `Controller[]` whenever one connects, disconnects or its battery moves |
| `warm_shaders` | `{ title_id }` | `WarmResult`; streams `warm://progress` (`WarmProgress`). Runs `<current>\eden-cli.exe --warm-pipelines <title_id> -c %APPDATA%\lichii\config\qt-config.ini` hidden at below-normal priority ("Prepare shaders" on a game card). Refused while `lichii.exe` runs. The launcher also runs it by itself for every title with a shader cache after a GPU driver change (`auto: true` in the events) |

| `stop_warm_shaders` | – | `StopResult`. Ends the running warm-up (the process is ended; eden-cli stops politely only on Ctrl+C), waits up to 15 s and reports. A stopped automatic run is not resumed and its driver counts as done. The pending `warm_shaders` call resolves with status `stopped` |
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

Argument keys are snake_case exactly as written (`invoke("apply_profile", { profile_id, overrides })`):
the commands are declared with `rename_all = "snake_case"`. Errors reject the promise with a
readable string.

When the backend says no (the promise rejects with a sentence to show):
- `launch_game` while `lichii.exe` already runs, or while shaders of the same title are being
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
  dedicated_mb: number; shared_mb: number; vulkan: boolean; arch_hint?: string; }
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
  seconds: number; built: number; already: number; left: number;             // from eden-cli's last line
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

Progress events carry `{ stage: string, done: number, total: number, message?: string }`.

Events: `install://progress` (stages `resolve`, `download`, `verify`, `extract`, `done`; bytes for
`download`, files for `extract`), `locate://progress` (`scan` with done/total = roots, then `done`),
`doctor://progress` (stage = the check id being run, then `done`), `game://exited`
(`{ code, seconds }`), `controller://changed` (`Controller[]`), `warm://progress` (`WarmProgress`), `autopilot://changed`
(`ModeStatus`, after a game's log was read), `share://changed` (`ShareStatus`, after a session was looked at or the setting changed).

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
(`%APPDATA%\lichii\log\eden_log.txt`, then `eden_log.txt.old.txt`, `eden_log.old.txt`, `eden_log.txt.old` and
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

## Profiles (first set)

`intel-arc-igpu` (Arc 130T/140T/140V, Meteor/Panther Lake Arc), `amd-igpu-handheld` (Radeon 780M/880M/890M, Z1/Z2),
`low-power-handheld` (any iGPU on battery: 30 fps cap, lower resolution), `desktop-dgpu`, `safe-defaults`.
Toggles map to `%APPDATA%\lichii\nx2_switches.txt` lines and `qt-config.ini` keys, e.g.
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
Microsoft's HDR help page, never toggles it).
Fixes that change the system only ever open the official Windows/vendor page; file fixes act only
inside Guarana's own folders.

Check ids, in the order `run_doctor` returns them: `emulator_version`, `gpu_driver`, `vulkan`,
`vc_redist`, `avx2`, `memory`, `disk`, `power`, `hags`, `intel_shared_memory`, `windows_hdr`,
`defender`, `shader_tmp`, `keys`, `firmware`, `game_folder`, `ocarina`, `controller`.
Fix ids: `install_update`, `open_driver_page`, `open_vulkan_driver_page`, `open_vc_redist`,
`install_vc_redist` (runs the `vc_redist.x64.exe` shipped in the installed Guarana folder with
`/install /quiet /norestart` after the click; Windows' own permission prompt appears, nothing is
elevated silently; the check offers it only when the file is found, else `open_vc_redist`),
`open_memory_settings`, `open_storage_settings`, `open_power_settings`, `open_graphics_settings`,
`open_intel_graphics`, `open_hdr_help`, `open_windows_security`, `delete_shader_tmp`,
`open_bluetooth_settings`. `apply_fix` returns the check run again; `install_update` streams
`install://progress` like `install_version`.
