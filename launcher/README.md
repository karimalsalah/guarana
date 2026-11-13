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
- **Ask AI**: Doctor and Report build a scrubbed summary and open a prefilled Claude or ChatGPT chat that points at Guarana's docs.
- **Applies a profile** to the emulator's own files: a marked block in
  `%APPDATA%\lichii\nx2_switches.txt` (your own lines stay as they are) and a few `[Renderer]` keys
  in `%APPDATA%\lichii\config\qt-config.ini`. Both are backed up to `guarana-backups\` first.
- **Starts games** with `lichii.exe -f -g "<game file>"` (`-g/--game` boots the file,
  `-f/--fullscreen` starts fullscreen; `src/core/launch_params.cpp`), can warm the title's shader
  cache files into memory first, and prepares a title's shaders for a new GPU driver with
  `eden-cli.exe --warm-pipelines <title id> -c <qt-config.ini>` (`docs/warm-up.md`).
- **Doctor**: driver, Vulkan, Visual C++ runtime, AVX2, memory and page file, disk, power, HDR,
  controllers, keys, firmware, games, updates. Fixes open the official Windows or vendor page, or
  act inside Guarana's own folders (deleting stale shader-cache temp files, installing an update).
- **Report a problem**: zips the emulator's logs, the launcher's log and the system details with
  your user name, profile folder and PC name removed, and opens a prefilled GitHub issue you review
  before sending. Nothing else leaves the machine; there is no telemetry.

It never downloads, bundles or uploads keys, firmware or games.

## Folders

| Folder | Holds |
| --- | --- |
| `%LOCALAPPDATA%\Guarana` | `versions\`, `current.json`, `launcher-state.json`, `launcher.log`, `downloads\`, `reports\` |
| `%APPDATA%\lichii` | The emulator's own user folder (keys, config, caches, logs). The launcher does not move it. |

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
"sha256": "<hex>" }`. Unit tests: `cargo test` in `src-tauri/` (after `npm run icons`).

CI (`.github/workflows/launcher.yml`) builds the installer on pushes that touch `launcher/`,
bundles the latest release's Windows build, uploads `Guarana-Setup-*.exe` as an artifact and,
when it runs for a release tag, attaches it to that release and adds it to `SHA256SUMS.txt`.

The installer is not code-signed yet, so Windows SmartScreen warns on first run.
