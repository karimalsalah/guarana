# Changelog

All notable changes to Guarana. The release notes of a version are its `## [X.Y.Z]` section, as
`tools/ci/release-notes.sh` extracts it: keep links inline (link definitions at the bottom of this
file do not travel with a section), group entries under `###` headings, and never put another
`## ` line inside a version. New entries go under `## [Unreleased]`; `tools/ci/bump-version.sh`
moves them into a version section and dates it at release.

Upstream pull request numbers refer to [Lichii](https://github.com/zortos293/Lichii).

## [Unreleased]

### Added

- `EDEN_NX2_GPL_OPTIMIZE_CACHED=1` (experiment, off by default): with `EDEN_NX2_GPL=1`, pipelines loaded from the shader cache get the optimized link in the background at their first draw, as pipelines built during play do, instead of drawing fast-linked until they are evicted and rebuilt (issue #103). The `Nx2Perf gpl` line now counts fast-linked draws with no optimized link coming and the links queued at a first draw. Not measured; plan in [docs/switches.md](docs/switches.md).
- `tools/bench/suites/presenter-and-locks.json`: one command measures the default-off pacing, pipeline link, lock and thread placement switches (`EDEN_NX2_PRESENT_SPIN`, `EDEN_NX2_GPL_OPTIMIZE_CACHED`, `EDEN_NX2_SCHED_PAUSE`, `EDEN_NX2_SYNCPOINT_SPLIT`, `EDEN_NX2_HYBRID`, `EDEN_NX2_IDLE_SPIN_US=0`) against the baseline, fullscreen with the pacing counters, in the Kokiri Forest scene or any scene given with `--scene`.
- Welcome: a person's first merged pull request and first device report get one thank-you comment with one next step ([welcome.yml](.github/workflows/welcome.yml)).
- Agent kit: [docs/agents/README.md](docs/agents/README.md#what-costs-nothing-today) lists the agent tiers that cost nothing today, with the vendors' stated limits.
- Pull request template: a note for a first pull request, a tests item for contributors who cannot build (CI runs the tests), and an `Assisted-by:` trailer item; CONTRIBUTING.md says why a first-time contributor's checks may wait for approval.

### Changed

- The launcher build pins Node 22 (`actions/setup-node`), so a runner image change cannot move Node outside what the locked vite accepts.

### Fixed

- Ocarina of Time on an Intel integrated GPU with a 120 Hz or 144 Hz display gets its locked set out of the box, with interpolated in-between frames ([#119](https://github.com/karimalsalah/guarana/issues/119)). The shipped `nx2_switches.txt` sets `EDEN_NX2_EXTRAPOLATE=1`, which turned `EDEN_NX2_FLUID_INTERP` off. A game's switch defaults now replace a value that only the shipped `nx2_switches.txt` set; the real environment and your own `nx2_switches.txt` still win. The 120 Hz and 144 Hz sets name `EDEN_NX2_EXTRAPOLATE=0`, the only value of any set that differs from the shipped file's. A value that a game's defaults or a switch file put in the environment no longer passes to the new version started after an in-app update, where it would count as yours, nor to a second game started in the same process. The `Switch defaults for this game and GPU` log line now lists the values it set, the ones it set over the shipped file (with that file's value) and the ones the player set. Not measured after the change; the 144 Hz set's own rows were measured with `EDEN_NX2_EXTRAPOLATE=0` ([docs/switches.md](docs/switches.md#defaults-for-ocarina-of-time-on-a-windows-integrated-gpu)).
- Launcher: with an Intel integrated GPU, the launcher no longer writes `EDEN_NX2_EXTRAPOLATE=1` into your own `nx2_switches.txt` ([#119](https://github.com/karimalsalah/guarana/issues/119)). At 115-125 or 140-148 Hz that line won over the emulator's locked 120 and 144 Hz sets for Ocarina of Time, so the presenter predicted frames instead of interpolating one for every refresh (one 144 Hz Panther Lake laptop, single runs at the same cadence divisor of 7: 15.5 real and 58.5 shown frames a second with the line, 20.6 and 84.1 without it). With "Ocarina of Time: predict frames forward" on (the default), the line is now left out at every refresh rate, primary GPU or not, and the emulator decides at each start: its 60 Hz Intel set and the shipped file still predict at other rates. Off still writes 0, and machines without an Intel integrated GPU get 1 as before. In Custom mode the launcher's block (only that, not `qt-config.ini`) is now rewritten at every start, so a line an older launcher wrote goes too. The toggle's help says so. Takes effect with the emulator change that makes the 120 and 144 Hz sets name `EDEN_NX2_EXTRAPOLATE=0` (#119); with an older emulator build the shipped file's 1 still applies. Unit tests only; not compiled or run on a device here.
- The switch registry check passes on main again: `tools/ci/switch-registry-upstream.txt` is regenerated from upstream Lichii's `57b75fec`, the commit the upstream integration merged, which adds upstream's `EDEN_NX2_GPU_TRACE` and `EDEN_NX2_GPU_TRACE_PRESENTS` (upstream [#1655](https://github.com/zortos293/Lichii/pull/1655)) and drops `EDEN_NX2_LIST_HLE_VERIFY`, which nothing uses any more. The `upstream` mirror still stops at the previous merge, so CI's `--upstream-ref` did not have them.
- Shader translation ([#105](https://github.com/karimalsalah/guarana/issues/105)): on GPUs with hardware float atomic adds (Intel), a float atomic add left on a global address no longer calls an undefined function (invalid SPIR-V); and with `EDEN_NX2_GM` bit 2, a Vertex B program joined to a Vertex A one now searches its own buffers, not the ones at its old indices.
- **FLUID presenter timing at refresh rates other than 60 and 120 Hz** ([#104](https://github.com/karimalsalah/guarana/issues/104)): `EDEN_NX2_FLUID_INTERP` batches no longer carry lateness of more than one slot from rounding and late real frames into every later batch, the screen copy wait (`EDEN_NX2_FLUID_QUEUE_HOST_WAIT`) ends before the next slot's wake instead of possibly making the present thread sleep through it at 75 or 144 Hz, and "% of shown frames held one slot" counts slots of four or more refreshes correctly. Not measured. Real frames on whole slots, with or without jitter, plan as before; at 60 and 120 Hz the wait bound is unchanged when the copy wait starts at the present offset (unit tests).
- Launcher: builds again with the dependency updates (sysinfo 0.40, zip 9, windows 0.62). sysinfo's `system` and `disk` features are turned on, a failed sysinfo start leaves the Win32 and registry answers in place, and the power-plan calls follow the windows 0.62 signatures; behaviour is unchanged.
- The README no longer shows its agent, contributor and hall-of-fame sections twice (a merge doubled them), and the tools/ci tests pass again after the actions/checkout bump: the pin test reads the commit from build-check.yml, and the legacy, ingest and triage workflow tests accept any major version. The report tools (`tools/ingest`) know the three fields the bug forms gained in #53 (display refresh rate, switches set, frame generation) and the three new "Graphics card" options (Arc 130T / 130V, Radeon 8060S, Steam Deck), so their tests pass again and such a report is no longer filed under "Other".
- After the rename ([#107](https://github.com/karimalsalah/guarana/issues/107)): a player who already imported a profile or started fresh in Lichii is no longer asked again, and `guarana-cli` no longer refuses to start for them; the debug adapter offers `speed_limit` and `autopilot`, also through `--call`; and the game list's **Report a problem with this game…** opens Help > Report a problem (a zip and a GitHub form here) instead of posting to upstream Lichii's compatibility site.
- Launcher: the launch stage is quiet again while it follows the game's boot. 20.3.0 brought back the 240 px bar, with an endless sweep until shader counts arrived, and a separate count line, and dropped the spinner from the status line. Now the stage shows one status line with a small spinner: the boot phase in words and, while shaders load, the real shader count after it. Nothing loops except that spinner, which hides on a failure. See [launcher/CONTRACT.md](launcher/CONTRACT.md#boot-progress-and-the-hand-over).
- Shader precache markers no longer say "built" for pipelines the driver's cache file does not hold ([#106](https://github.com/karimalsalah/guarana/issues/106)): the lists of precached graphics pipelines carry the build variant (the precache marker's variant, which holds the SPIR-V emission stamp, the resolution scale and the loop safety, bloom and debug settings, plus the `EDEN_NX2_*` switches known to change the SPIR-V or the cached link), the lists and `warm-compute.done` are written only after a save of the driver cache succeeded (on Intel's Windows driver the precache is now saved on the loading screen instead of only at exit), and they are removed when that file is missing, empty or deleted at load. Not measured.
- The build check can pass again: two steps that came from upstream looked for `eden-cli` and `lichii.exe`, which this fork builds as `guarana-cli` and `guarana.exe` (the GPU trace replay on Linux, the version resource check on Windows). The macOS release build no longer stops at a parameter in `core.cpp` that shadows a field.
- The switch registry check no longer breaks after an upstream merge: CI reads upstream's switch names from upstream itself (the mirror branch had stopped following, because `upstream-sync` cannot push upstream's workflow changes without the `UPSTREAM_SYNC_TOKEN` secret), and the checked-in upstream list is regenerated from upstream `fec99576`, which adds `EDEN_NX2_TC_TRACE`.
- Claims from Git Bash: a `/claim` that Git Bash rewrote into a Windows path now gets a one-line reply on how to claim; the docs give `echo /claim | gh issue comment <n> -R karimalsalah/guarana --body-file -`.
- Docs and site after the v20.3.0 release: the README and the site's front page call v20.3 released instead of "coming" (the old `#coming-in-v203` link still lands), say the locked 120 Hz set is the default for Ocarina of Time on Intel iGPUs at 120 Hz, and the front page names 20.3.0 as the current version; the pacing and warm-up wins on the guide pages and in the FAQ are dated (v20.1, default since v20.2) instead of "next release"; [docs/switches.md](docs/switches.md#defaults-for-ocarina-of-time-on-a-windows-integrated-gpu) gains the 120 Hz set's row; the 20.3.0 notes have one "Changed" group instead of two; and [docs/warm-up.md](docs/warm-up.md) no longer describes the chunks of 500 the precache no longer has.
- Lichii and `eden-cli` leftovers of the rename: the bug report form links Guarana's releases and names the "Guarana build", the log line and File > Open Guarana Folders (`tools/ingest` reads the new label and still the old one); the Report a Bug dialog says to send the zip privately to the Guarana maintainers by direct message, without the log bot's `/bug` command; the shipped `nx2_switches.txt` names `%APPDATA%\guarana` and the portable `user` folder; `guarana-cli` names itself in its legacy-import error; the migration tooltip says again that earlier Lichii builds, not Guarana's, kept their cache in Eden's folder; and `tools/shader-sweep/build_nup_windows.py` finds `guarana-cli.exe` in `build.ninja`, while the trace replay tool and docs name `guarana-cli`.
- Launcher messages say what happened: Prepare shaders gives the reason when it could not prepare anything (an older build, or no shader cache yet) instead of "Shaders were already prepared"; Home drops its "vX is out" link and the Updates tab's dot once that version is in use; Doctor says "Opened" only for fixes that open a page, and the install and delete fixes say what they did; the release notes card shows a sentence instead of an endless skeleton when the update check fails. The launcher's `rust-version` is 1.95, what the locked sysinfo 0.40 needs, and the UI README names Vite 8, the committed lockfile and what Apply sends.

## [20.3.0] - 2026-11-14

### Changed

- **Ocarina of Time on a 144 Hz display: interpolated frames on the 144 Hz vblank clock, real frames held to 24 a second** on Windows Intel integrated GPUs (primary display at 140 to 148 Hz). One device, single runs (a Panther Lake laptop in Kokiri Forest): 91.5 shown frames a second against 50.5 with the 60 Hz configuration, 917 of 1069 slots on their slot; the real frame shown is older: 89.9 ms against 57.0 ms; outdoors 17.6 to 24.1 real frames a second. A paired A-B-A-B on the same laptop held in both pairs: 79 to 83 shown frames a second against 44 to 51, 24 real frames against 20 to 22, with the real frame shown 20 to 30 ms later. The boot log's switch-defaults line now names the primary display's rate found and the set chosen because of it, and says when `EDEN_NX2_EXTRAPOLATE` overrides a locked set's interpolation (see [docs/switches.md](docs/switches.md)).
- **Ocarina of Time on a 120 Hz display is a locked 120 by default** on Windows Intel integrated GPUs: interpolated in-between frames on the vblank clock, 20 real frames a second, the time-critical present thread. Other refresh rates keep the 60 Hz configuration.
- **A locked 120 Hz while Link runs.** `EDEN_NX2_FLUID_INTERP` now stretches a batch over a late real frame instead of leaving slots empty, and `EDEN_NX2_FLUID_RT=1` runs the present thread time-critical in the MMCSS Games task. In Hyrule Field on the Arc 140T, with 20 real frames a second: 1181-1200 of 1200 slots on time per 10 s, 118-120 shown frames a second, p99 9.6-13.6 ms (see the rows in [docs/switches.md](docs/switches.md)).
- **FLUID at 120 Hz.** `EDEN_NX2_SHOWN_RATE=120` (or a frame generation target of 120) shows a frame every refresh of a 120 Hz display, with a frame pool of 16 (`EDEN_NX2_FLUID_FRAMES`), about 100 ms of predictions per real frame and `EDEN_NX2_CADENCE_DIVISOR` for 24 or 30 real frames. Five swapchain images are now the default there: with three the screen copy waited for a free image, and in Hyrule Field on the Arc 140T slots on time went from 790-801 to 1134-1137 of 1200. `EDEN_NX2_FLUID_INTERP=1` puts interpolated frames on the same vblank clock, so moving objects follow their own motion rather than the camera's. Also new in [docs/switches.md](docs/switches.md): `EDEN_NX2_FLUID_COPY_AHEAD`, `EDEN_NX2_SWAPCHAIN_IMAGES`, `EDEN_NX2_FLUID_QUEUE` 6 and 7 (the copy engine), `EDEN_NX2_FLUID_HOST_WAIT_US`, `EDEN_NX2_BUDGET_CEILING`, present-thread timing under `EDEN_NX2_SHOWN_STATS`, and `EDEN_NX2_DEPTH_CLEAR_COVERS` (Fritchii's EXP56). Fence-flagged payload releases stay synchronized at low accuracy (zortos293/Lichii#1099, tibzejoker's #874).
- **Ocarina of Time's frames reach the screen on time.** On Windows integrated GPUs the screen copy now runs on a queue of its own (`EDEN_NX2_FLUID_QUEUE=5` with `EDEN_NX2_FLUID_QUEUE_HOST_WAIT=1`) and the frame budget no longer raises the resolution into the cadence's headroom (`EDEN_NX2_BUDGET_HOLD_RES=1`), by default. In Hyrule Field on the Arc 140T, frames that reached the screen after their refresh went from about 200 per 10 s to 0, and slots shown on time from 475-491 to up to 599 of 600 with 99.5% of frames evenly paced (two paired rounds).

- CI checks contributed benchmark rows. On pull requests that touch `tools/bench/`, a new `bench-rows` job validates the rows under `tools/bench/results`, fails when `RESULTS.md` is not regenerated from them, and runs the benchmark tool's tests (the Linux build job runs those tests too). `bench.py validate` accepts a folder and `bench.py table` gains `--check` ([#43](https://github.com/karimalsalah/guarana/issues/43)).

- A borderless fullscreen Vulkan window covers the screen exactly, so it can get independent flip: no desktop composition, up to a refresh less latency. OpenGL keeps the extra row.
- Launcher setup: Scan no longer shows "Step 1 of 2" or a profile picker, since setup ends in Auto mode and neither did anything. Continue no longer waits for the health checks; they finish in the background.
- Launcher Home: the game and Play come first, and Play is the only orange button. The library follows, then Game Booster, controllers and setup under "This PC", with the Booster folded to one line that shows its level. In Doctor, Copy diagnostics is a secondary button, so the fix buttons stand out.
- Launcher launch: Playing from the library carries the game's title into the launch stage. The stage shows one status line instead of a looping placeholder bar, then the game's title followed by "is running" rather than "Playing", since the launcher cannot see the game's first frame. The welcome stage stops animating after its intro, and the hero art pulses three times on arrival.
- `EDEN_NX2_HYBRID=1` (off by default) now also places threads on hybrid CPUs with four performance cores (Lunar Lake, Panther Lake-H): the GPU front, back and Vulkan worker keep the P cores, the guest cores and latency threads may also use the E cores, and the low-power island stays out (on Lunar Lake, whose only E cores sit on that island, those cores are used as the E cores). Six-P CPUs are unchanged; 2P+8E CPUs are still left to Windows. Not measured on four-P parts; plan in [docs/switches.md](docs/switches.md).
- `EDEN_NX2_BUDGET_UNANSWERED=1` (experimental, off by default): the frame budget notices when the game does not follow its GPU time scale (for example Ocarina of Time with the Max Resolution add-on): after nine missed ticks (about 4.5 s) that leave the frame time alone it holds the scale, shows state `unanswered` on the debug server and says so once in the log; with `EDEN_NX2_VRS` and `EDEN_NX2_VRS_AUTO` (and a device with per-draw shading rates) the shading-rate governor may coarsen from there. Not measured; see [docs/autopilot.md](docs/autopilot.md#when-the-scale-does-not-answer).
- The README opens with what Guarana is, the measured proof, three setup steps, device status, a 10-minute device test ([#55](https://github.com/karimalsalah/guarana/issues/55)) and how to contribute (`/claim`, good first issues).

### Added

- Debug server: `shown_frames` returns the next frames the presenter actually shows, generated ones included: each one's kind (real, predicted, interpolated), slot, the vblank it was due at and when the present call returned, and the age of its real frame, with one contact sheet of their pictures, so an agent sees what the player sees between real frames. See [docs/nx2/debug-mcp.md](docs/nx2/debug-mcp.md#shown-frames-implemented).
- Portable installs (a `user` folder next to `guarana.exe`) read `user\nx2_switches.txt` in place of the per-user copy, the Guarana settings page saves its mode there, and the log says `Portable mode: on` or `off` ([#36](https://github.com/karimalsalah/guarana/issues/36)). See [docs/FAQ.md](docs/FAQ.md).

- Frame pacing experiments, off by default and described in [docs/switches.md](docs/switches.md): `EDEN_NX2_QUEUE_LIMIT` (keeps the GPU queue short so screen copies and read-backs wait less), `EDEN_NX2_FLUID_OFFSET_US` (the screen copy's window before its flip), `EDEN_NX2_FLUID_SPLIT_SUBMIT` (the real frame goes to the GPU before its predictions), `EDEN_NX2_FLUID_QUEUE=5` with `EDEN_NX2_FLUID_QUEUE_HOST_WAIT` (a default-priority copy queue that cannot stall rendering) and `EDEN_NX2_BUDGET_HOLD_RES`; and `EDEN_NX2_EXTRAPOLATE_HUD_FILL`, which stops the band of scenery beside the HUD from lagging in predicted frames.

- Experimental `EDEN_NX2_CACHE_MMAP=1`, off by default and described in [docs/switches.md](docs/switches.md): the boot load memory-maps the shader cache log instead of copying the whole file, and decodes its programs on the shader job workers instead of on one thread. Same records and programs either way (unit tests compare both paths); not measured yet ([#11](https://github.com/karimalsalah/guarana/issues/11), stage 1).

- Device profile: at boot the log gets one `Device profile:` line with the GPU (vendor, model, integrated or discrete, driver, device-local memory, shared memory), the CPU (logical and physical cores, hybrid P, E and LPE cores), installed RAM, AC or battery, the display's refresh rate, whether there is an NPU, and the device class (`IntegratedLowPower`, `Integrated`, `DiscreteEntry`, `DiscreteHigh` or `Unknown`) with a short label. Game presets receive the class; no default changes with it yet. See [docs/DEVICES.md](docs/DEVICES.md#the-device-profile-in-the-log).
- CI checks the switch registry: a pull request that uses an `EDEN_NX2_*` or `EDEN_DKB_*` switch with no row in [docs/switches.md](docs/switches.md) and not upstream's fails, naming the switch and where it is used; a row whose switch is used nowhere is a warning (`tools/ci/switch_registry.py`, `switch-registry.yml`). Rows for `EDEN_NX2_GPL_OPTIMIZE`, `EDEN_NX2_GPL_OPTIMIZE_THREADS` and `EDEN_NX2_GPL_FAST_LINK`.

- `EDEN_NX2_UNOPT_FIRST=1` (experiment, off by default, Intel's Windows driver with `EDEN_NX2_GPL` only): a new shader library is first built with optimization disabled, meant to shorten the wait of a draw that needs it (not measured; the driver may not build it faster), and its optimized build is swapped in from the background, with a log line per swap and a 10 s summary. Not measured yet; the plan is in [docs/switches.md](docs/switches.md) (issue #13).

- GPU families: the `Device profile:` line gains `gpu_family=`, the GPU's model family by its PCI IDs: Intel's Meteor Lake, Arrow Lake-H, Lunar Lake and Panther Lake integrated graphics (the Arc B370 and B390 apart from the small 4-core part), AMD's Radeon 780M, 880M, 890M and 8060S, and the Steam Deck's two GPUs. A device class is broad (an Arc 140T and the small Panther Lake GPU are both `Integrated`); a family is what a measured preset can be matched to. Game presets can now match a class or a family; no default changes with either yet ([#24](https://github.com/karimalsalah/guarana/issues/24)).
- Launcher: Auto recognises Intel integrated GPUs named only "Intel(R) Graphics" by their PCI ids (Arrow Lake-H 7D51) and, when plugged in, picks the Intel Arc integrated profile for them; the scan screen and copied diagnostics name the family matched. The doctor gains three checks: memory free right now (free RAM and commit against its limit), the display refresh rate when frames are paced to 60 (naming the nearest multiple of 60 the display offers), and two or more displays on an integrated GPU. Their fixes only open a Windows settings page or say what to close.

- Launcher session: a game started from the launcher no longer shows the emulator's own window. With `--launcher-session`, which the launcher passes to 20.3.0 and later, `guarana.exe` opens straight into the fullscreen loading screen and the game, and exits when the game stops (exit code 0, or 1 when the game did not start) instead of going back to its game list, so the launcher returns at once. There the leave-fullscreen key (Esc) stops the game, and the hint toast says so. Older versions keep `-f -g`. See [launcher/CONTRACT.md](launcher/CONTRACT.md#launcher-session).

- Launcher: the launch screen follows the game's boot. In a launcher session the emulator writes its boot marks and shader counts to the launcher (stdout, only with `--launcher-session`), and the screen shows the phase in words and, while shaders load, the real shader count on a filling bar; it holds until the game is on screen (the new log mark `frontend: loading screen hidden, game shown`). Without news it hands over after 20 s (45 s for an emulator that writes its boot), or 1 s after the emulator's window takes the foreground when the emulator reports no boot (older versions); before, it handed over about 1 s after the start. The launcher comes back to the front when the game exits, unless the player is in another app. See [launcher/CONTRACT.md](launcher/CONTRACT.md#boot-progress-and-the-hand-over).

- The benchmark can measure what the player sees. `bench.py run --fullscreen` (or `"fullscreen": true` in a variant) runs fullscreen instead of forcing windowed and records the output size; windowed and fullscreen rows are listed apart and never compared. `--pacing` sets `EDEN_NX2_SHOWN_STATS=1` (off by default: with FLUID it adds GPU timestamps to presenter copies) and puts the presenter's last 10 s report from the log in the row (shown frames a second, interval percentiles, share within 2 ms of the median, FLUID slots on time). A screenshot whose centre is black, HUD or not, makes the row invalid (`black_picture`) even without a reference. New scene `oot-kokiri-forest` for a ten-minute save in Kokiri Forest ([#40](https://github.com/karimalsalah/guarana/issues/40)).

### Fixed

- A running samples (occlusion) query keeps its count up to 32 banks (8,192 slots) instead of 4 before it is dropped. Windows past 1,024 slots made occlusion-tested objects blink in Ocarina of Time on a Radeon 890M (upstream issue [#1428](https://github.com/zortos293/Lichii/issues/1428) by ronaldpena007-ops, [#21](https://github.com/karimalsalah/guarana/issues/21)).
- Guest writes that find the caches busy no longer block the game thread (upstream [#1602](https://github.com/zortos293/Lichii/pull/1602), +4.4% in Ocarina of Time on the author's PC), and pointer-slot compute rebuilds compile on a worker, which removes 2.5-3.4 s freezes in Hyrule Field (upstream [#995](https://github.com/zortos293/Lichii/pull/995)).
- Every release file comes with a signed build provenance attestation (`gh attestation verify <file> --repo karimalsalah/guarana`), and the release workflows Authenticode-sign `guarana.exe`, `guarana-cli.exe` and the installer with Azure Artifact Signing once its secrets are set. How to check a download and how to turn signing on: [docs/SIGNING.md](docs/SIGNING.md). Also weekly grouped Dependabot updates, `.github/CODEOWNERS`, installer builds for launcher pull requests, and "Rapid releases" in [docs/LANES.md](docs/LANES.md).
- Agent kit: a front door for AI coding agents ([docs/agents/README.md](docs/agents/README.md)) and a living [HANDOVER.md](HANDOVER.md). `/claim` and `/unclaim` on an issue set a `claimed` label and an assignee; a claim older than 72 hours that no open pull request links to is released daily (`agent-claims.yml`). Optional `@claude` and `@gemini-cli` agents for maintainers run only when their secret is set (`agents.yml`). Copilot cloud agent gets setup steps. The debug MCP server has an entry for the official MCP registry (`tools/nx2/server.json`, bundled by `tools/nx2/mcpb_pack.py`). The research behind it: [docs/agents/RESEARCH-2026-11.md](docs/agents/RESEARCH-2026-11.md).
- Launcher: Windows integration. `.nsp`, `.xci`, `.nup` and `.nca` files open with Guarana from Explorer (registered per user, and listed under Open with); `guarana://launch/<title id>` and `guarana://launch?path=<file>` links start a game from your library or game folders, checked strictly; a second start hands its file or link to the launcher already open; the taskbar button shows install and shader warm-up progress; and Windows notifications, off until you turn them on under Home → Advanced, say when shaders are ready or an update is installed. See [launcher/README.md](launcher/README.md#windows-integration).
- Site: the front page follows the README's order (what Guarana is, the measured numbers with their machine, three setup steps, device status, the coming release, how to help) and no longer calls the shipped presenter "in progress".
- Launcher: a **Game Booster** with three opt-in levels (Off by default) that tune Windows for speed only while a game started from Guarana runs, and undo everything when it exits, when the launcher restarts after a crash (from a journal file), or from an "Undo all" button. Level 1 (no admin): the emulator at Above Normal priority, power throttling (EcoQoS) off for it, and the high-performance GPU asked for `guarana.exe`. Level 2 (one UAC prompt): the High performance, or Ultimate Performance when installed, power plan for the session. Level 3: High priority and pausing SysMain and Windows Search while you play. The elevated steps run in a small `--booster-helper` process behind one prompt, and no security setting is ever touched (Defender, firewall, VBS/HVCI, SmartScreen, UAC, Windows Update, BitLocker). See [launcher/CONTRACT.md](launcher/CONTRACT.md#game-booster).

- Agent kit: the OpenAI Codex CLI reads only the first 32 KiB of `AGENTS.md` by default and ours is longer, so it silently dropped the end of the rules. `.codex/config.toml` raises the limit once the folder is trusted, and the README, [docs/AI-QUICKSTART.md](docs/AI-QUICKSTART.md) and [docs/agents/README.md](docs/agents/README.md) give the `-c project_doc_max_bytes=65536` override.

## [20.2.0] - 2026-11-13

### Added

- Guarana window and play mode: Guarana Dark theme (new default), a Guarana settings page (play mode, frame generation, resolution, controller, report a problem), a compact brand performance overlay, per-monitor window memory, a menu hint when a game goes fullscreen, a clean release window title and icon, and a first-run panel on the empty game list. Settings: `guarana_remember_window`, `guarana_play_toast`, `guarana_overlay_compact`, `guarana_mode`. See `docs/user/ui.md`.
- Smooth mode is the pacing measured best in Ocarina of Time: `EDEN_NX2_FLUID=2` (predictions as of when a frame can be shown, so the real frame is shown and predictions reach under one frame interval ahead) with `EDEN_NX2_CADENCE=3` (20 real frames per second, one per 20 Hz game tick). Hyrule Field on the Arc 140T, one paired session against `EDEN_NX2_FLUID=1`: slots shown on time 430 to 536 of 600, evenly paced frames (within 2 ms of the median) 62% to 88%, screen copies stuck behind rendering 100 to 45 per 10 s.

- Launcher: after a session with at least a minute of gameplay, a card offers to share your numbers (game fps, frame pacing, device, driver) as a prefilled device report you review and submit yourself; a setting stops it asking.

- **Graphics pipeline library fast-link** (`EDEN_NX2_GPL=1`, opt-in, experimental): a pipeline is linked from cached per-stage shader libraries (`VK_EXT_graphics_pipeline_library`), so a pipeline whose stages were seen before costs a fast link instead of a 400 to 1000 ms driver build, and an optimized link replaces it in the background (`EDEN_NX2_GPL_OPTIMIZE=0` skips that). Off on AMD's Windows driver unless `EDEN_NX2_GPL_FAST_LINK=1`. Port of upstream pull requests [#1173](https://github.com/zortos293/Lichii/pull/1173), [#1177](https://github.com/zortos293/Lichii/pull/1177) and [#1185](https://github.com/zortos293/Lichii/pull/1185) by PantACRO4life and justin-wq1337, adapted to this tree. Not measured on a verified scene yet; the plan is in [docs/switches.md](docs/switches.md).
- Opt-in experiments, all off by default and described in [docs/switches.md](docs/switches.md):
  - `EDEN_NX2_READ_FAST=1`: an async read's sync operation is served ahead of the GPU split's queue.
  - `EDEN_NX2_HYBRID=1`: threads are placed by role on hybrid Intel CPUs (performance cores for the guest, GPU and Vulkan threads) on Windows.
  - `EDEN_NX2_HITCH_FINISH_TRACE`, `EDEN_NX2_HITCH_GC_BATCH`, `EDEN_NX2_HITCH_GC_CLEAN_FIRST` and `EDEN_NX2_HITCH_UMA_BUDGET_MIB`: where full GPU waits come from, batched and reordered texture-collector downloads, and a smaller memory budget cap on integrated GPUs. Analysis in [docs/hitches.md](docs/hitches.md).
  - `EDEN_NX2_GPUCOST_QUERY` and `EDEN_NX2_GPUCOST_PRESENT`: fewer barriers per query sync, and no redundant clear before a full-frame present.
  - `EDEN_NX2_BOOT_ASYNC=1`: the Vulkan instance is created ahead of the renderer, and boot does not wait for cached graphics pipelines; they are built from the order recent sessions first used them, and an interrupted warm-up resumes.
  - `EDEN_NX2_PREDICT=1`, with `EDEN_NX2_FLUID`: a small online model of the real-frame interval schedules the predictions. See [docs/frame-generation.md](docs/frame-generation.md).
  - `EDEN_NX2_AUTOPILOT=1`: every 2 s steers frame generation target, GPU time scale and, on battery, the power policy to the display.
  - New diagnostics from the upstream merge: `EDEN_NX2_PERF_VERIFY`, `EDEN_NX2_FAST_DRAW_VERIFY`, `EDEN_NX2_REG_BLOCK_VERIFY` and `EDEN_NX2_BACKX_VERIFY`.
- Frame generation: an extrapolation debug overlay (`EDEN_NX2_EXTRAPOLATE_DEBUG=2`), `EDEN_NX2_EXTRAPOLATE_HUD_DILATE` and `EDEN_NX2_FLUID_SPARE`, and a Fluid presenter that counts its misses by cause. See [docs/frame-generation.md](docs/frame-generation.md).

### Changed

- **Ocarina of Time is smooth out of the box on Windows integrated GPUs.** Frame generation is on (3 frames per real frame toward 60, predicted from the game's motion) and the pacing measured best is the default: the vblank-locked presenter showing each real frame (`EDEN_NX2_FLUID=2`), 20 real frames a second, one per game tick (`EDEN_NX2_CADENCE=3`), early read-backs and, on Intel, pipeline library fast links. In Hyrule Field on the Arc 140T, slots shown on time went from 430 to 536 of 600 and evenly paced frames from 62% to 88% against the earlier presenter setting. Only for settings and switches left at their defaults; `EDEN_NX2_NO_PRESETS=1` or Automatic presets off turns it off. See "Defaults for Ocarina of Time" in [docs/switches.md](docs/switches.md).
- **Guarana rebrand** (player-visible): the programs are `guarana.exe` and `guarana-cli.exe` (`lichii.exe` and `eden-cli.exe` before), the data folder is `%APPDATA%\guarana`, and on first start your settings, saves and caches are copied over from `%APPDATA%\lichii` (copy only: staged, rolled back on failure, the old folder stays; the copied settings name the new folder, so saves, the SD card and mods are read from it). Windows version info, installer, desktop and metainfo files, icons, translations and the About credits say Guarana; the launcher, the release workflow and the docs use the new names and fall back to the old ones for older builds. Internal identifiers do not change. The first boot after the rename recompiles shaders once, because Intel's driver cache is per program name.
- Integrated GPU measurements in Ocarina of Time (Hyrule Field, Arc 140T) with the frame generation, hitch and boot changes of this release: 24.5 to 25.2 game fps, p99 frame time 47 ms and 0 stutters in 30 s in two clean runs, against p99 135 to 158 ms and 37 to 92 stutters for the v20 desktop setup.
- Upstream merge: [#1580](https://github.com/zortos293/Lichii/pull/1580) (the zero-page pass reads less and backs off to 5 minutes), [#1582](https://github.com/zortos293/Lichii/pull/1582) (Android docs), [#1585](https://github.com/zortos293/Lichii/pull/1585) (3D state writes sent to the GPU back thread as register blocks), [#1586](https://github.com/zortos293/Lichii/pull/1586) (back-thread micro-cuts and a dirty-memory page-crossing fix), [#1589](https://github.com/zortos293/Lichii/pull/1589) (the DLSS quality setting changes Ocarina of Time's render resolution), [#1590](https://github.com/zortos293/Lichii/pull/1590) (vi2 timestamp recorders), [#1591](https://github.com/zortos293/Lichii/pull/1591) (`GPUDirtyMemoryManager::Collect` takes `size_t`) and [#1592](https://github.com/zortos293/Lichii/pull/1592).

### Fixed

- Frame generation (FLUID presenter): a late real frame is rolled to the next slot instead of leaving it empty, no stale frame of an old scene is shown after a camera cut, the HUD edge mask covers anti-aliased text and icon edges so they are not moved with the scene, and when real frames come as fast as the slots the real frame is shown as drawn.
- Shader translation: a narrowed global-memory load (`EDEN_NX2_GM` bit 2) kept its buffer list in the flags that the shader info pass reads as the coherent bit, so every narrowed program was marked coherent. The routing now uses a separate field (public issue #6).

### Removed

- Experiments a paired run showed gave no gain, or harm, with their code: `EDEN_NX2_MESH_SIMD`, `EDEN_NX2_IGPU_VRAM_MIB`, `EDEN_NX2_DEPTH_OPTIMAL`, `EDEN_NX2_RT_USAGE` and `EDEN_NX2_RT_USAGE_SIZE`, `EDEN_NX2_ALIAS_LAZY`, `EDEN_NX2_DEFER_WFI`, `EDEN_NX2_MESH_OUT` and `EDEN_NX2_MESH_VRS_PX`, `EDEN_NX2_MESH_CULL`, and the disabled dynamic fragment shading rate path. They are listed under "Removed experiments" in [docs/switches.md](docs/switches.md).

## [20.1.0] - 2026-11-13

### Added

- **Guarana Setup**, the Windows installer and launcher (per-user, no admin, WebView2 included): one switch for how it runs (Auto, Smooth, Quality, Battery), finds your own keys, firmware and games on your PC with your consent, configures your controller, an autopilot that retunes after each session with undo, a doctor with fixes, Report a problem and Ask AI (a prefilled Claude or ChatGPT chat with scrubbed diagnostics), a Credits screen, and shader warm-up after driver changes. Every release attaches `Guarana-Setup-<version>.exe`.
- Contributing with any AI agent: one-line setups for Claude Code, Codex CLI, Gemini CLI, Cursor, Copilot, Windsurf, Aider and Cline in [docs/AI-QUICKSTART.md](docs/AI-QUICKSTART.md), the whole project context for a web chat in one URL ([llms-full.txt](https://karimalsalah.github.io/guarana/llms-full.txt)), and "Solve it with AI" links on agent-ready issues and bug reports.
- Credit for everyone: a hall of fame rebuilt daily from the git history ([CONTRIBUTORS.md](CONTRIBUTORS.md), the site), contributors in the emulator's About dialog and the launcher's Credits screen, and release notes that thank every contributor by handle.
- Community data: device reports feed a leaderboard and a records board with a "Beat it" button; the [Open wins](https://karimalsalah.github.io/guarana/wins.html) page lists measurable problems with their numbers; a triage bot labels new issues and asks once for a missing log.
- Guides for what players search for: playing Ocarina of Time on a PC, fixing stutter, Intel Arc, handhelds and controller setup; a blog; a 20-second promo with an original score; an NPU benchmark (`tools/npu/`) with the measured verdict on the [roadmap](ROADMAP.md#a1-npu-feasibility).
- Discord for real-time help: [discord.gg/urz7vMdRM](https://discord.gg/urz7vMdRM).
- FLUID presenter (`EDEN_NX2_FLUID=1`, opt-in, experimental): frames are shown on the display's vblank clock, and frame generation records a prediction for each slot, so generated frames get even 16.7 ms pacing. On the Arc 140T the title screen showed 601 of 601 slots on time (p50 16.7 ms, 100% within 2 ms of the median), and Hyrule Field had p5 16.4, p50 16.7 and p95 33.5 ms with 75% of frames within 2 ms, against 33 to 54% without it. With `EDEN_NX2_CADENCE` it also locks real frames to their slot. See [docs/switches.md](docs/switches.md).
- Controllers: player 1 is configured automatically for a connected gamepad on start and when one is plugged in. Turn it off in the advanced input settings.
- Help > Report a Problem writes a zip with the log and system details, scrubbed of user names and paths, and opens a prefilled GitHub issue; a link over 2000 characters is retried shorter and then copied.
- `eden-cli --warm-pipelines <title id>` builds a title's cached shader pipelines into the driver caches without booting the game or needing the game file. On the Arc 140T it built 4,515 pipelines and programs in 155 s. See [docs/warm-up.md](docs/warm-up.md).
- Opt-in experiments, all off by default and described in [docs/switches.md](docs/switches.md): `EDEN_NX2_GM=15` (bit 4: loads through a loaded pointer skip the run-time buffer search), `EDEN_NX2_UMA` bits 32 and 64 (copies of command list pages in runs, batched buffer copies), `EDEN_NX2_EARLY_SUBMIT`, `EDEN_NX2_EARLY_PAGES` and `EDEN_NX2_EARLY_AFTER` (early read-back downloads), `EDEN_NX2_SHADER_REUSE=1` (graphics pipelines reuse translated shaders, from upstream pull request [#1378](https://github.com/zortos293/Lichii/pull/1378)), and `EDEN_NX2_FLUID_QUEUE` (a queue of its own for the FLUID screen copy; a net loss on the Arc 140T, kept for testing).
- `EDEN_NX2_CACHE_SAFE=1` (opt-in): on Intel's Windows driver, reads of the driver pipeline cache wait for running builds, which avoids a driver crash (IGCIT #1614), and unchanged cache saves are skipped.

### Changed

- Faster boot: the Vulkan check process is skipped when it passed with the same drivers, a direct game launch leaves the Vulkan device list to the settings, a NUP's table of contents is read once per session, and boot phases are timed in the log.
- Ocarina of Time's first boot on an Intel or AMD integrated GPU logs one line with what the game presets did.
- The swapchain logs its present mode, and the Vulkan instance logs what it asks for and lists no layers when none is asked for.
- The shader tool `tools/shader-sweep` reads `EDEN_NX2_GM` like the emulator.

### Fixed

- Windows release zips now include the licence texts (`LICENSE.txt`, `LEGAL.md`, `LICENSES/`) and a `SOURCE.txt` naming the exact source commit.
- CI: a merged unit test regained a closing brace; the standalone thread-worker wake test stubs `PowerTuning::TagBackgroundThread`; the Qt frontend links QuaZip on every platform (Report a Problem zips logs).
- `eden-cli` output redirected to a file or a pipe is kept instead of lost.
- The Report a Problem window compiles again (a name shadowed the dialog's form layout).
- A GPU puller torn down while waiting no longer leaves stale semaphore waiters behind (upstream [#1592](https://github.com/zortos293/Lichii/pull/1592)).
- Windows: the loading screen no longer freezes in borderless fullscreen (upstream [#1558](https://github.com/zortos293/Lichii/pull/1558)).
- Logging no longer ends the emulator when formatting fails while the host is out of memory (upstream [#1475](https://github.com/zortos293/Lichii/pull/1475)).
- MSVC builds: the selected user index is cast to the launch parameter type (upstream [#1596](https://github.com/zortos293/Lichii/pull/1596)).

## [20.0.2]

### Fixed

- Linux builds: the generated frame-extrapolation shader header needed `<cstdint>` (GCC).
- A merged unit test referred to a removed parameter.
- Releases publish every platform that built; a missing Windows binary still blocks the release.

## [20.0.1]

### Changed

- `EDEN_NX2_GM=11` is now on by default only for Ocarina of Time (0400486024DEC000), the one game it is verified in, instead of every game through `nx2_switches.txt`. An explicit `EDEN_NX2_GM` still wins. Known issue tracked in #6.
### Documentation

- `docs/switches.md`: a row for each of the 13 switches `dist/nx2_switches.txt` names or sets (#37).

## [20.0.0]

The first Guarana release: upstream Lichii `nx2` at `e633e165f7`, ten upstream pull requests merged
early, three Intel Arc driver fixes, and this fork's own work for integrated GPUs. All numbers below
come from one laptop.

### Highlights

- **Global-memory routing** (`EDEN_NX2_GM=11`, opt-in): 14.75 → 22.83 game fps in Ocarina of Time's
  Hyrule Field with frame generation off. The foliage mesh draw went from 15.7 to 5.8 ms of GPU time.
- **Extrapolated frame generation** for Ocarina of Time (`EDEN_NX2_EXTRAPOLATE=1`, opt-in,
  experimental): generated frames are predicted forward from the game's motion vectors, with the HUD
  masked.
- **iGPU power policy** (`EDEN_NX2_POWER_POLICY=igpu`, opt-in, experimental, Windows).
- **Intel Arc fixes**: light beams, flashing lights and NaN bloom, native float atomics.
- **Tuned builds**: clang-cl ThinLTO, PGO trained on Ocarina of Time, x86-64-v3.

### Measured

Core Ultra 9 285H with Arc 140T iGPU, Intel driver 32.0.101.9175, 32 GB RAM; Ocarina of Time. The
switch A/B was measured at the Hyrule Field overlook. Game fps counts the frames the game renders,
not generated frames.

- `EDEN_NX2_GM=11` against no switch, frame generation off: 14.75 → 22.83 game fps; foliage mesh draw
  15.7 → 5.8 ms.
- Play config (frame generation 3x toward 60): an earlier unreleased build read 19.6 game fps, locked
  at 50 ms frames. v20 read 26.7 game fps live in Castle Town and 28 in the field. These readings are
  not a same-spot A/B yet.

### Upstream: Lichii `nx2` at `e633e165f7`

Picks up 73 newer upstream commits, among them:

- Ocarina of Time white flash fixed: a deleted buffer is cleared from every channel's bindings
  ([Lichii #1459](https://github.com/zortos293/Lichii/pull/1459)).
- Dark water fixed: light-table shadow maps are bound through descriptor arrays, and pointer words
  bind no texture ([Lichii #1485](https://github.com/zortos293/Lichii/pull/1485),
  [Lichii #1523](https://github.com/zortos293/Lichii/pull/1523)).
- GPU hang with native mesh shaders fixed: the task program's mesh workgroup count is zeroed before
  the guest runs ([Lichii #1492](https://github.com/zortos293/Lichii/pull/1492)).
- Cheaper queries, fences, draws and uploads on the GPU threads
  ([Lichii #1515](https://github.com/zortos293/Lichii/pull/1515),
  [Lichii #1516](https://github.com/zortos293/Lichii/pull/1516),
  [Lichii #1519](https://github.com/zortos293/Lichii/pull/1519),
  [Lichii #1520](https://github.com/zortos293/Lichii/pull/1520),
  [Lichii #1521](https://github.com/zortos293/Lichii/pull/1521)).
- Dynarmic x86-64 code generation improvements
  ([Lichii #1531](https://github.com/zortos293/Lichii/pull/1531)).
- Shader translation: SSA phis are created only when needed; upstream reports 19-23% less translation
  time ([Lichii #1468](https://github.com/zortos293/Lichii/pull/1468)).
- Clean shutdown: the emulator waits for a teardown that is still making progress instead of
  terminating it ([Lichii #1576](https://github.com/zortos293/Lichii/pull/1576)).

### Upstream pull requests merged early

- Descriptor flush: texture descriptors are flushed one by one when the pool runs past the GPU
  address space ([Lichii #1155](https://github.com/zortos293/Lichii/pull/1155)).
- Ocarina of Time freeze on ocarina warps fixed: GPU memory registry listeners run outside their
  locks, in order ([Lichii #1218](https://github.com/zortos293/Lichii/pull/1218)).
- Pipeline-rebuild stutter fixed: a masked key keeps the graphics record it already has
  ([Lichii #408](https://github.com/zortos293/Lichii/pull/408)).
- Device memory clamps the size of its cold-memory hints
  ([Lichii #1568](https://github.com/zortos293/Lichii/pull/1568)).
- Shutdown: AM releases the application process it never took
  ([Lichii #1417](https://github.com/zortos293/Lichii/pull/1417)).
- Shader warm-up: the compute precache marker follows the file's contents and the shader lowering
  ([Lichii #1564](https://github.com/zortos293/Lichii/pull/1564)).
- Descriptor reuse: each draw's sampled textures and image descriptors are kept across frames
  ([Lichii #1447](https://github.com/zortos293/Lichii/pull/1447)).
- Guard-house GPU out-of-memory fixed: the texture cache's retire scan resumes where it stopped, so
  unused render targets behind protected ones are retired
  ([Lichii #1571](https://github.com/zortos293/Lichii/pull/1571)).
- iGPU ASTC memory: ASTC textures with uneven mips stay in BC on integrated GPUs and cards under
  6 GiB ([Lichii #948](https://github.com/zortos293/Lichii/pull/948)).
- A suspended command-list copy is polled without a kernel call each time
  ([Lichii #933](https://github.com/zortos293/Lichii/pull/933)).

### Intel Arc fixes

Ported from [Fritchii-Intel-Fixes](https://github.com/LuckyLuckLucky/Fritchii-Intel-Fixes), patches
0001, 0003 and 0010.

- **0001, light beams:** the dynamic primitive topology follows the pipeline's tessellation stages,
  which removes broken light beams and stray triangles on Arc.
- **0003, shuffle bounds:** subgroup shuffles stay in bounds on host subgroups narrower than the
  guest's warp, which fixes flashing lights and NaN bloom on Arc.
- **0010, native float atomics:** 32-bit float storage atomic adds use `VK_EXT_shader_atomic_float`
  where the driver supports it, instead of compare-and-swap loops.

### Guarana changes

- **Global-memory routing (`EDEN_NX2_GM`, opt-in).** Cheaper run-time routing of the shader memory
  accesses that stay global. Bit 1 computes buffer base addresses once per invocation instead of in
  every access, bit 2 makes a load search only the pointer buffers its address is built from, and
  bit 8 (with 2) returns zero when the address is in none of those, instead of falling back to a
  search of every buffer. `11` sets all three.
- **Extrapolated frame generation (`EDEN_NX2_EXTRAPOLATE=1`, opt-in, Ocarina of Time).** Generated
  frames are predicted after the real frame from the game's motion vectors and depth, instead of
  interpolated before it. HUD pixels stay in place, camera cuts show the real frame alone, and HDR
  output pauses it (the pass writes SDR). Needs **Enable Frame Generation**.
- **Present spacing fix.** Generated frames are spaced across the real frame interval instead of
  sent back to back, and the last predicted frame leaves the next real frame free to show as soon as
  it is ready.
- **Red/blue fix in generated frames.** The extrapolation pass writes its output without a declared
  format; declaring one on the B8G8R8A8 swapchain view swapped red and blue.
- **iGPU power policy (`EDEN_NX2_POWER_POLICY=igpu`, opt-in, Windows, integrated GPUs).** For the
  game session, CPU performance boost is turned off and the minimum processor state lowered in the
  active power plan, so more of the shared package power can go to the GPU. The saved values are
  written back at game stop or exit, and at the next start after a crash.
- **Contributor tools.** `tools/bench` (new): a cross-machine benchmark with scenes, suites, profiles,
  a scene check and privacy-scrubbed JSON rows. `tools/nx2/pipeline_stats.py` (new) prints what the
  driver's compiler made of the costliest shaders.

### Builds

- Windows x64 release builds use clang-cl with ThinLTO, PGO from a profile trained on Ocarina of
  Time, and `x86-64-v3` code. They need a CPU with AVX2.
- v20 keeps the upstream names: the program is `lichii.exe`, and its data lives in
  `%APPDATA%\lichii`, the same folder upstream Lichii uses.

### Known issues

- **Micro-stutter in generated-frame pacing.** A fix is in progress: a vblank-locked 16.67 ms
  presenter (`EDEN_NX2_FLUID=1`, experimental).
- **Shader-build hitches on first visits.** Intel's driver takes 0.4-1 s to compile one pipeline.
- **Intel driver crash risk when the pipeline cache is saved during builds**
  ([IGCIT #1614](https://github.com/IGCIT/Intel-GPU-Community-Issue-Tracker-IGCIT/issues/1614)). A fix
  is in progress.
- **16 GB shared-memory devices are below the comfortable floor.** Switch 2 titles use a 12 GiB guest
  memory layout.
- **`EDEN_NX2_GM=11` is measured in Ocarina of Time only.** If another game renders wrong with it,
  remove the switch and report the game.
