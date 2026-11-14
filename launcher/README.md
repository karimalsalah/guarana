# Guarana Launcher

The Windows installer, first-run setup, launcher, updater and doctor for Guarana. A Tauri 2 app:
the web UI is in `ui/` (Vite + TypeScript), the Rust backend in `src-tauri/`. The commands, types
and events between them are in [CONTRACT.md](CONTRACT.md).

## What it does

- **Installs per user, no admin.** `Guarana-Setup-<version>.exe` (NSIS, current-user mode) installs
  the launcher; the WebView2 bootstrapper is embedded in the installer, so a PC without WebView2 gets it with no download step. The installer carries
  the current emulator build (`src-tauri/resources/emulator.zip`), so the first run works offline.
- **Keeps emulator versions side by side** in `%LOCALAPPDATA%\Guarana\versions\<version>\`;
  `current.json` names the one that runs, and an older one can be made current again (rollback).
- **Updates from GitHub releases**: the stable channel is the newest release, the nightly channel is
  the rolling `nightly` pre-release. It takes `guarana-<version>-windows-x86_64-pgo.zip` (or the
  plain `-windows-x86_64.zip`) and refuses to install it unless its SHA-256 matches the release's
  `SHA256SUMS.txt` (and GitHub's own asset digest, when both exist).
- **Finds what you already have**: keys (`*.keys`, checked to be `name = hex` lines), firmware
  folders (many `.nca`, with `.cnmt.nca`) and games (`.nup .nsp .xci .nx`, title id from a small
  header or the file name). It reads names and small headers only, skips OneDrive placeholders,
  never follows junctions or links, and copies nothing until you choose Import.
- **One switch, self-tuning.** Home has Auto, Smooth, Quality and Battery; Auto picks from this PC's detection and the community's verified settings (`best-settings.json`, cached 24 h, offline-safe). After each game an autopilot reads the log and makes at most one small, undoable change within the mode's limits; Home says what it changed and why.
- **Game Booster (opt-in, off by default).** Three cumulative levels tune Windows for speed only while a game runs and put everything back when it exits, after a crash (from a journal file), or from "Undo all". Level 1 (no admin) runs the emulator at Above Normal priority, turns off power throttling (EcoQoS) for it, and asks Windows for the high-performance GPU for `guarana.exe`. Level 2 adds the High performance (or Ultimate Performance) power plan for the session. Level 3 adds High priority and pauses SysMain and Windows Search while you play. The elevated steps (the power plan and the services) run in a small helper behind one UAC prompt. Security settings are never touched and nothing under `HKLM` is changed except what `powercfg` does. See [CONTRACT.md](CONTRACT.md#game-booster).
- **Ask AI**: Doctor and Report build a scrubbed summary and open a prefilled Claude or ChatGPT chat that points at Guarana's docs.
- **Applies a profile** to the emulator's own files: a marked block in
  `%APPDATA%\guarana\nx2_switches.txt` (your own lines stay as they are) and a few `[Renderer]` keys
  in `%APPDATA%\guarana\config\qt-config.ini`. Both are backed up to `guarana-backups\` first.
- **Starts games** with `guarana.exe --launcher-session -f -g "<game file>"` (`-g/--game` boots the
  file, `-f/--fullscreen` starts fullscreen; `src/core/launch_params.cpp`). `--launcher-session`
  (20.3.0 and later) means the emulator's own window never appears: once the game starts loading,
  its loading screen and then the game fill the screen (until then the launcher's screen stays),
  and the emulator exits when the game stops, so you are back in the launcher. The leave-fullscreen key (Esc by default) stops the game
  (see [CONTRACT.md](CONTRACT.md#launcher-session)). Older versions get `-f -g` only.
  While the game boots, the launch screen shows what the emulator is doing (loading the game,
  loading shaders with the real count, starting the game), read from the boot marks the emulator
  writes to the launcher in a launcher session, and it switches to "Playing" once the game is on
  screen. When the game exits, the launcher comes back to the front.
  `launcher.log` stamps its lines to the millisecond and records each start's timeline (Play
  pressed, command built, process spawned, each boot phase, the exit). With the environment switch
  `EDEN_NX2_LAUNCHER_PAD_IDLE=1` (off by default, not measured) the launcher stops polling
  controllers while a game runs and resumes when it exits
  ([CONTRACT.md](CONTRACT.md#pads-and-the-log-while-a-game-runs)).
  It can warm the title's shader
  cache files into memory first, and prepares a title's shaders for a new GPU driver with
  `guarana-cli.exe --warm-pipelines <title id> -c <qt-config.ini>` (`docs/warm-up.md`).
- **Fits into Windows 11**: double-click a `.nsp`, `.xci`, `.nup` or `.nca` file and Guarana starts
  it; a `guarana://` link starts a game too; a second start hands its file or link to the launcher
  that is already open; the taskbar button shows install and shader warm-up progress; and, once you
  turn them on under Home → Advanced, Windows notifications say when shaders are ready or an update
  is installed. See [Windows integration](#windows-integration).
- **Doctor**: driver, Vulkan, Visual C++ runtime, AVX2, memory and page file, disk, power, HDR,
  controllers, keys, firmware, games, updates, and Guarana's own settings (the switch file, the
  shader cache settings, the shader cache files, and settings left behind in `%APPDATA%\lichii`;
  these only report, and never look at keys, firmware, saves or games). Fixes open the official Windows or vendor page, or
  act inside Guarana's own folders (deleting stale shader-cache temp files, installing an update).
- **Report a problem**: zips the emulator's logs, the launcher's log and the system details with
  your user name, profile folder and PC name removed, and opens a prefilled GitHub issue you review
  before sending. Nothing else leaves the machine; there is no telemetry.

It never downloads, bundles or uploads keys, firmware or games.

## Folders

| Folder | Holds |
| --- | --- |
| `%LOCALAPPDATA%\Guarana` | `versions\`, `current.json`, `launcher-state.json`, `launcher.log`, `booster-journal.json` (the Game Booster's undo record), `downloads\`, `reports\` |
| `%APPDATA%\guarana` | The emulator's own user folder (keys, config, caches, logs), copied from `%APPDATA%\lichii` by the emulator on its first start. The launcher does not move it, and keeps using `lichii` until then or with an emulator from before the rename. |

## Windows integration

Everything below uses Tauri's own bundler settings and three official plugins, called from Rust
only; none of them is exposed to the page (`capabilities/default.json` is unchanged). The details
and every check are in [CONTRACT.md](CONTRACT.md#opening-games-from-windows).

| What | How | Where |
| --- | --- | --- |
| `.nsp` `.xci` `.nup` `.nca` open with Guarana | `bundle > fileAssociations` (ProgIDs `Guarana.<ext>`), registered by the installer for the current user | `src-tauri/tauri.conf.json`, `src-tauri/src/args.rs`, `src-tauri/src/open.rs` |
| Listed under "Open with" even when another program is the default; Explorer refreshes at once | NSIS hooks: `OpenWithProgids` values and `SHChangeNotify`, removed again on uninstall | `src-tauri/windows/installer-hooks.nsh` |
| One launcher at a time; a second start passes its file or link on | tauri-plugin-single-instance 2 with its `deep-link` feature | `src-tauri/src/main.rs` |
| `guarana://` links | tauri-plugin-deep-link `=2.6.3`, scheme in `plugins > deep-link > desktop` | `src-tauri/src/args.rs`, `src-tauri/src/open.rs` |
| Notifications: shaders ready, update installed | tauri-plugin-notification `=2.8.1`; off until switched on in Home → Advanced, and only while the launcher is in the background | `src-tauri/src/notify.rs` |
| Taskbar progress for installs (also the first-run one) and shader warm-ups | Tauri's `set_progress_bar` | `src-tauri/src/taskbar.rs` |

The installer is per user (`installMode: currentUser`), so Tauri's NSIS template writes the
associations and the `guarana` scheme under `HKCU\Software\Classes`, without admin rights. The
uninstaller puts back the program each extension opened with before and removes the scheme.

Links start a game the same way as Play does, never anything else:

```text
guarana://launch/0400486024DEC000
guarana://launch?title_id=0400486024DEC000
guarana://launch?path=D%3A%5CGames%5COcarina%20of%20Time.nup
```

A title id must be in your library. A path must be a full path on a drive to an `.nsp`, `.xci`,
`.nup` or `.nca` file inside one of the game folders you added: the launcher checks that as text
before it looks at the disk, then that the file is there, and says why when it refuses. A file
elsewhere opens from Explorer or with `guarana-launcher.exe "<file>"`. Browsers ask before they hand
a link to Guarana. To try one, run `Start-Process "guarana://launch/<title id>"` in PowerShell.

`npm run dev` registers `guarana://` for the development build when no installed Guarana owns it
(debug builds only). File associations come only from the installer.

## Build

Prerequisites: Rust stable with the MSVC target (Visual Studio 2022 Build Tools for the linker),
Node 20 or newer.

```powershell
cd launcher
npm ci             # Tauri CLI and the icon renderer (pinned in package-lock.json)
npm run icons      # assets/brand/logo-mark.svg -> src-tauri/icons/ (not committed)
npm run dev        # the UI's dev server plus the app, with reload
npm run build      # src-tauri/target/release/bundle/nsis/Guarana_<version>_x64-setup.exe
```

`npm run build` installs and builds `ui/` itself (`beforeBuildCommand`). To bundle an emulator
build locally, put the release zip at `src-tauri/resources/emulator.zip` and, optionally,
`src-tauri/resources/emulator.json` with `{ "version": "20.0.2", "channel": "stable",
"sha256": "<hex>" }`. Unit tests: `cargo test` in `src-tauri/` (after `npm run icons`); the
command-line and `guarana://` parsing is tested in `src-tauri/src/args.rs` and `src-tauri/src/open.rs`.

CI (`.github/workflows/launcher.yml`) builds the installer on pushes that touch `launcher/`,
bundles the latest release's Windows build, uploads `Guarana-Setup-*.exe` as an artifact and,
when it runs for a release tag, attaches it to that release and adds it to `SHA256SUMS.txt`.

The installer is not code-signed yet, so Windows SmartScreen warns on first run.
