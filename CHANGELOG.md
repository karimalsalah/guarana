# Changelog

All notable changes to Guarana. The release notes of a version are its `## [X.Y.Z]` section, as
`tools/ci/release-notes.sh` extracts it: keep links inline (link definitions at the bottom of this
file do not travel with a section), group entries under `###` headings, and never put another
`## ` line inside a version. New entries are files in `changelog.d/` (see its README), so pull requests do not conflict here; a
line under `## [Unreleased]` is still accepted until the next release. `tools/ci/bump-version.sh`
folds the fragments in, moves the entries into a version section and dates it at release.

Upstream pull request numbers refer to [Lichii](https://github.com/zortos293/Lichii).

## [Unreleased]

## [20.7.1] - 2026-11-20

### Fixed

- Windows ARM64 release builds link again: the Switch 2 BLE WinRT code now compiles against the
  exact C++/WinRT version the Qt arm64 package was built with (2.0.250303.1, fetched from NuGet),
  instead of the Windows SDK's newer 2.0.250303.5 that lld-link's `/failifmismatch` rejects.
- macOS release builds no longer fail when the runner's freetype bottle still links
  `libhvf.dylib` (which macOS 15.0-15.3 lack): freetype is rebuilt from source on the runner.

## [20.7.0] - 2026-11-20

### Added

- `EDEN_NX2_ASYNC_GUARD` (experimental, off): with asynchronous shaders, draws that write memory,
  use transform feedback, rasterize nothing or render into a small target wait for their pipeline
  instead of being skipped, so effects later draws depend on are not lost until a restart. From
  Eden ([6a767ac43a](https://git.eden-emu.dev/eden-emu/eden/commit/6a767ac43a089fe988526bf7f8477ccc43277fb1)).
- `EDEN_NX2_F2I_ABS_NEG` (experimental, off): F2I's absolute value and negation modifiers are
  decoded the right way round, a shader fix Eden made for Disco Elysium
  ([Eden 6e24dcb4b5](https://git.eden-emu.dev/eden-emu/eden/commit/6e24dcb4b5)).

### Changed

- Bump the launcher cargo group: sysinfo 0.40.1 to 0.40.3 and zip 9.4.1 to 9.5.1.
- Bump actions/checkout from 5 to 7 in the bench-rows and switch-registry workflows.
- Windows: the launcher and a game it starts are one app to Windows. In a launcher session the
  emulator takes the launcher's AppUserModelID (the one on its Start menu shortcut), and the
  launcher sets it for itself also when it starts from a game file or a link. They share one taskbar
  button, and pinning the game's button pins the launcher instead of `guarana.exe`.
- Bump rfd from 0.17.2 to 0.18.0 in the launcher.
- Bump actions/setup-java from 4 to 5 in the Android workflow.
- Upstream sync (zortos293/Lichii nx2 to 6708d2476c, 31 commits): Ocarina of Time FPS Unlock
  carry fixes — cloth below 60 fps, Epona's fence jump, fractional day/night seconds and Deku
  web steps; nvdrv small-GPFIFO-submit allocation churn; texture cache recovery from failed
  host scratch allocations and bounded descriptor lookups; Vulkan borrows a cached sampler
  when the driver refuses allocation; core_timing drops due events whose owner expired;
  Windows zero-page unlocks capped at 1 MiB; sysmix purges output streams too (Xenoblade DE
  docked/handheld crash); Yoshi 2.0.0 full-scene-resolution FXAA option; macOS bundle ships
  nx2_switches.txt in Resources; scratch HOME profile state; ARM64 NCE validation and
  Dynarmic defaults. Text speed stays the shipped JSON pack; Guarana's shutdown telemetry,
  portable switch loading and built-in metadata are kept across the new upstream code.
- Bump vite from 8.3.6 to 8.3.8 in the launcher UI.

### Fixed

- Android nightlies install beside the stable app instead of updating it. The APK build checks the packaged app ID before uploading it, and the upstream update checker stays off on both channels.
- A game the launcher starts no longer opens the debug console (when "Show Log in Console" is on in
  Configure > Debug) or the What's New dialog after an update: the game is the one window, and the
  launcher's Updates screen has the notes. Creating a folder link on Windows (the Link choice of the
  data import, Link to Ryujinx) no longer flashes a console window.
- Audio: while Windows' microphone prompt is unanswered, capture streams wait for the one open
  already waiting instead of starting one each, and the SDL backend opens the microphone on the
  calling thread again, so a stuck open cannot hold SDL's next playback open
  ([#199](https://github.com/karimalsalah/guarana/issues/199)).
- Ocarina of Time with No sharpening, the default on Intel integrated GPUs: the picture is no
  longer hazier and flatter than in v20.4.0. Since v20.5.0's upstream merge the bypass copied the
  image the network's output pass samples, which on Hyrule Field is not the tonemapped picture
  (26 dB from the network's own output, against 32 dB). It copies the tonemap drawn that frame
  again, and the sampled image only when no known tonemap was drawn (Lichii #1688's case).


## [20.6.0] - 2026-11-18

### Added

- Android: release and nightly builds include `guarana-<version>-android-arm64.apk` (Android 13 or
  newer): the app under Guarana's own id, `io.github.karimalsalah.guarana`, which reads the player's
  `nx2_switches.txt` from its user folder as the desktop builds do. Until the release key is set up
  ([docs/SIGNING.md](docs/SIGNING.md), "Android") it is a preview build,
  `io.github.karimalsalah.guarana.preview`. Nobody on the project has booted it on a device yet.
- [docs/ARM64.md](docs/ARM64.md): Guarana on Snapdragon X laptops and Android handhelds: what runs
  where, how CI builds the Windows ARM64 and Android packages, what the renderer needs from Adreno
  drivers (mesh shaders above all), the expected gains, and the call for testers.
- Issue forms for ARM64 testers: Snapdragon X laptops (Adreno X1 / X2) and Android devices (Adreno
  6xx to 8xx, other Android GPUs) in every "Graphics card" list, "Windows 11 on ARM" and "Android" in
  the operating system lists, and "Phone or tablet" as a device report form factor. New issues get the
  `QUALCOMM`, `arm64` and `android` labels, and the benchmarks page filters reports by Qualcomm GPUs
  and by phones and tablets.
- `EDEN_NX2_ASYNC_DISCARD=2` (experimental, off): with asynchronous shaders, a native draw whose
  pipeline is still building is skipped only when it draws into render targets the game clears
  and redraws every frame, and waits otherwise, so title text, chat bubbles and lighting drawn
  once are not lost on an empty shader cache. Ported from Fritchii (`a962f20533`).
- Benchmark: boot suites. `tools/bench/suites/console-boot.json` measures launch to gameplay and
  first-visit hitches in one command, every boot and shader switch against the baseline with a
  cold, a second and a warm shader cache (and the GPU driver's cache with `--driver-cache-dir`),
  three times; each row has the boot phases, the held draws and builds to the scene and the
  stutter frames, and is invalid when its cache was not in the state it claims
  ([docs/console-boot.md](docs/console-boot.md)).
- Device reports: each report gets the engine's GPU family (Arrow Lake-H, Lunar Lake, Radeon 780M,
  Steam Deck LCD and the others) when it names its GPU exactly, and the new
  `site/data/community-evidence.json` counts reports and devices per family, game and build, with
  the effects that paired `tools/bench` rows measured. Nothing reads it to change a default yet
  ([#24](https://github.com/karimalsalah/guarana/issues/24)).
- Experimental `EDEN_NX2_DLSS_SKIP_PASSES` (ports of zortos293/Lichii#864 and #1985): a game's own
  DLSS network passes are skipped while a replacement upscale draws over their output (`drew`) or
  while the network cannot be shown (`blocked`). Off by default; upstream measured The Duskbloods
  on a Steam Deck at 56.6 to 40.4 ms of GPU time a frame.
- Experimental `EDEN_NX2_FG_GAME_MOTION`: with `EDEN_NX2_FLUID_INTERP`, in-between frames are
  warped from the game's motion (Ocarina of Time's motion guide: the camera from the scene depth,
  and Link) instead of optical flow, and what one real frame no longer shows is filled from the
  other by depth. Off by default and not measured yet; `tools/bench/suites/game-motion-fg.json`
  measures it against the optical-flow interpolation the game ships with at 120 Hz.
- Design for the game's own reconstruction network and motion on integrated GPUs
  (`docs/design/game-reconstruction-and-motion.md`), and the bench suite of its first step:
  `tools/bench/suites/game-network.json` measures Ocarina of Time's anti-aliasing network with
  each existing way of running its tensor instructions (software, the shared-memory gather,
  Intel's 8x16x16 cooperative matrices), with the game presets off so the network actually runs.
- Experimental `EDEN_NX2_INTEL_COOP_MATRIX_8X8`: the game's tensor networks on the Arc 140T's own
  cooperative matrices (8x8x16 with fp32 accumulators, the only fp16 shape its driver lists), two
  products per guest HMMA through shared memory, rounded to fp16 once. Off by default, not measured
  and not yet run on the hardware; `tools/bench/suites/game-network-a1b.json` measures it.
- Experimental switches for the game's own tensor networks on matrix units that go through shared
  memory (AMD's 16x16x16, Intel's 8x16x16): `EDEN_NX2_HMMA_ZERO_C` starts an HMMA whose accumulator
  is zero from a zero matrix instead of a stored C tile (bit-exact), and `EDEN_NX2_HMMA_K16_ONCE`
  runs a K=16 HMMA as one 8x16x16 product per row half, rounding once. The diagnostic
  `EDEN_NX2_TENSOR_LOG` logs each tensor program's HMMA counts and the path they take, and
  `tools/bench/suites/game-network-a1.json` measures the two switches on Ocarina of Time. All off by
  default and not measured yet.
- Debug server `input_latency`: holds a right-stick step and times, from when the game first
  reads it, the first shown frame whose picture moved (generated frames included) and the first
  real one, so frame generation's input latency can be measured against no generation.
- `EDEN_NX2_INTEL_COMPUTE_SIMD16` (experimental, off): on Intel GPUs, compute shaders without warp
  operations can be built at 16 lanes, which built Ocarina of Time's large global-memory programs
  about 2.6 times faster on an Arc B580 at the cost of some frame rate. From Fritchii-Intel-Fixes
  (patch 0011).
- `EDEN_NX2_LOAD_SAVE=1` (experimental, off by default): on Intel's Windows driver the boot load
  saves the driver pipeline cache once its builds have ended and starts the background prebuild
  after it, so a crash or kill later in the session no longer loses that boot's driver builds
  (port of [Lichii #1838](https://github.com/zortos293/Lichii/pull/1838); step B6 of
  [docs/console-boot.md](docs/console-boot.md)).
- `EDEN_NX2_IR_POOL_CAP_MIB` (experimental, off): the shader recompiler's object pools free
  their memory after a shader once it passes the cap instead of keeping their largest shader's
  worth, which can return host RAM an integrated GPU shares. Ported from Fritchii (`c99790b878`).
- `EDEN_NX2_NCE_REPORT=1` (diagnostic): one log line per module a game loads, on any host, with
  what native code execution (NCE) on ARM64 hosts would rewrite and which system registers it
  would leave to the host CPU, so Switch 2 titles can be checked for NCE without an ARM64 device.
  [docs/NCE.md](docs/NCE.md) has the plan for NCE on Linux ARM64, Android, Windows on ARM and macOS.
- Performance overlay: a memory row with the RAM Guarana holds and the RAM the PC has free, and the
  GPU memory it uses with the room the driver still gives it, amber when either runs low. On an
  integrated GPU the GPU memory is marked shared and its room is bounded by the free RAM. Ported
  from Fritchii (Frizti/Fritchii `58f55cb7d9`, `8627ff4233`).
- Built-in patches can be data: a patch pack (`dist/patch-packs/*.json`, see
  [docs/patch-packs.md](docs/patch-packs.md)) adds built-in add-ons for any game without C++.
  They are checked as the compiled-in ones are: build id, the game's original bytes (by
  SHA-256), code caves in executable padding, zero-filled state, user mods first. Ocarina of
  Time's three Text Speed add-ons are the first pack and work as before. The packs ship in a
  `patch-packs` folder next to the program and are compiled into it as well, so Android and a
  program without the folder keep them; a file in the folder replaces the compiled-in copy of its
  name. The unit tests reject a pack with a field the format does not have, a patch without a
  description, or bytes another built-in writes.
- Presenter telemetry for pacing A/Bs ([#8](https://github.com/karimalsalah/guarana/issues/8)):
  a `Present setup` line at each swapchain creation names the window's display and its refresh
  rate, the present mode and image count, `EDEN_NX2_FLUID` and Windows' hardware-accelerated GPU
  scheduling; `EDEN_NX2_SHOWN_STATS` also counts the intervals between presents in refreshes
  (under one, one, two, three or more) and the refreshes missed against the median; the
  diagnostic `EDEN_NX2_PRESENT_MODE=immediate|fifo|fifo_relaxed|mailbox` asks for one present
  mode, falling back with a warning when the surface lacks it; and
  `tools/bench/suites/presenter-ab.json` runs FIFO against IMMEDIATE with frame generation off
  and on.
- GPU trace bench: on a driver that does not replay bit for bit (Intel's Windows driver), the
  baseline's own run-to-run difference becomes a noise floor, and a variant within it is reported
  as within noise instead of failing the comparison (`trace_replay.py bench --noise-margin`).
- Device reports name the GPU's PCI ID: `tools/bench` rows record it (`hardware.gpus[].pci`, from
  Windows' PNP device ID or `lspci -nn`), and the launcher's "Share my numbers" writes it after the
  GPU's name. The report bot uses it for the engine's GPU family, so Meteor Lake and Panther Lake
  GPUs that their drivers name only "Intel Graphics" count for their family
  ([#24](https://github.com/karimalsalah/guarana/issues/24)).
- The boot `Resolution:` line and the debug server's `status.resolution` cite the community's paired
  `tools/bench` measurements (`site/data/community-evidence.json`, which each build now carries) where
  a preset or switch default sets a value measured on the player's GPU family and game. No value
  changes ([#24](https://github.com/karimalsalah/guarana/issues/24)).
- `EDEN_NX2_SKIP_REDRAWN=1` (experimental, off by default): with asynchronous shaders on, a Switch 2
  draw into a target the game redraws every frame is skipped while its pipeline builds instead of
  stopping the frame (port of [Eglantine #874](https://github.com/Eglantine-Community/Eglantine/pull/874);
  step F4 of [docs/console-boot.md](docs/console-boot.md)).
- `EDEN_NX2_STARTUP_CHECK_ASYNC=1` (experimental, off by default): the startup Vulkan check's child
  process runs while the frontend starts, and the frontend waits for its verdict only before it
  first uses Vulkan (step B2 of [docs/console-boot.md](docs/console-boot.md)).
- Status bar: in Ocarina of Time a chip shows which upscaler replaces the game's own (FSR 3.1,
  FSR 4 or DLSS) with the render and output sizes, for example `FSR 3.1 720p → 1080p`, or why
  the game's own upscaler draws (`OFF`, `RES 1X`, `NO CAMERA`, `NO DEPTH`, `WAITING`,
  `UNAVAILABLE`); between games a click turns Ocarina FSR on or off. Until now only NVIDIA DLSS
  showed its state. Ported from CheezyEmu (CheezerWheezer/CheezyEmu `76f03c3c03`, `51eb67792f`).
- Windows on ARM: release and nightly builds include `guarana-<version>-windows-arm64.zip`, a
  native ARM64 build for Snapdragon X laptops (clang-cl for ARM64 with ThinLTO, dynarmic's ARM64
  backend, Qt linked statically). Nobody on the project has booted it on a device yet: device
  reports are welcome.

### Changed

- The game network suites now log each tensor program's path (`EDEN_NX2_TENSOR_LOG` in
  `game-network.json` too) and say which GPUs their matrix arms apply to: the Arc 140T's driver lists
  only 8x8x16 shapes with fp32 accumulators and 8x8x32 8-bit shapes, so `EDEN_NX2_INTEL_COOP_MATRIX`
  and the A1 switches do nothing there (they apply to Xe2 and RDNA 3). The design gains step A1b, an
  8x8x16 path for the 140T, and says to read only each suite's second pass (every arm compiles cold).
- Add-Ons: the page says whether each built-in patch is made for the installed version of the
  game before the game has ever been started. It reads the build id of the game's main program
  from the version that would start (the enabled update and mods included) without loading it.
  A built-in made for another version gets the warning sign and "Cannot be applied: it is made
  for another version of the game", ticked or not; one made for this version says so until the
  first start tells whether it was applied. The other reasons (a mod changes the same part of the
  game, the game's code is not what it expects) still come from the last start. The loader call
  is `AppLoader::ReadMainBuildId`, ported from Lichii #1358.
- CI: only pushes save the build's dependency and compiler cache; pull requests restore the
  newest one of their base. A day of pull requests had filled the repository's 10 GB of Actions
  cache with caches only their own pull request could read, which evicted `main`'s and made its
  next Windows build start cold (65 minutes instead of about 9).
- Legal hardening for the public release: PRIVACY.md documents exactly what device reports
  collect and how to opt out; SPDX headers completed on the room stub and the texture-rect test;
  CREDITS.md names ProfessorTee's render scales (Lichii #903). The Nintendo-unaffiliated, BYO
  keys/firmware/games stance is unchanged and verified across README and forms.
- Windows: lz4 1.11.1, up from 1.10.0, for compressed game files and NSO decompression. It no
  longer crashes or overflows on invalid arguments. Linux uses the system's lz4.
- NX converter: nxformat 1.0.4, up from 1.0.3. It imports Switch 2 Edition and multi-program
  games better, and handles incomplete sparse native bases with their matching updates. Loading
  `.nx` games is unchanged.
- Archives: QuaZip 1.7.2, the release, instead of a December 2025 commit. It reads zip64
  archive comments correctly.
- `release-binaries.yml` runs dispatched on a branch other than `main` save no caches: they could
  serve only that branch, and the repository's 10 GB cache quota would evict `main`'s to hold them.
- Merged [zortos293/Lichii](https://github.com/zortos293/Lichii) `nx2` up to `a2af87d7de`
  (226 commits): native HOME isolation and NCA signature policy, texture-cache write-back fixes,
  the built-in patch system with Ocarina of Time text speeds (port of NotRexed/Hookshot#402) and
  FPS unlock frame-rate flag, present ids and present waits for unlocked-frame-rate games
  (host vsync), frame generation that never waits for a present frame, game-frame pacing targets,
  mesh vertex-input lending, color-feedback-loop checks, RADV hang-report wait, and the
  Switch 2 Joy-Con 2 IR mouse support. Guarana's fluid presenter, locked cadence, graphics
  pipeline libraries, draw templates, prepass split, async-discard modes 1/2 and skip-redrawn
  all kept; the pending-draw policy now also applies upstream's color-write-history heuristic
  (Eglantine #874) and the bounded wait budget. Resolutions listed in the merge pull request.
- Upstream: merged zortos293/Lichii `nx2` from `99e89096cb` to `7c7f723b68` (50 commits).
  Among them: Nextendo online play no longer needs a special build and follows Configure >
  Network, still off by default (Lichii #1979); with it on, a game's signed NPLN patch is checked
  before boot ("Checking online patches") and applied after mods (Lichii #1358), with Nextendo's
  network rules and signed game catalogue (Lichii #1984) and Splatoon 3's network requests
  (Lichii #1988). Ocarina of Time's No sharpening copies the image the output pass samples, which
  ends a frozen 3D under a live HUD (Lichii #1875), and skips the unused heavy dispatch while the
  output is replaced (Lichii #1941). A lost Vulkan device ends the process with a log line instead
  of an unhandled exception (Lichii #1938); a buffer without memory frees staging memory and
  retries (Lichii #1963, #1931); padded depth targets are drawn as the square they are (Lichii
  #1868); terminated threads and a stopped game's kernel objects close safely (Lichii #1957,
  #1932); and bounds fixes in the NSO, KIP, NPDM, NCZ, ticket and XTS readers. Help > Check for
  Updates stays hidden in our releases, which are built without the update checker. With
  `EDEN_NX2_OOT_BYPASS_ANY` the
  bypass still copies the tonemap the rasterizer tracks ([docs/switches.md](docs/switches.md)).

### Removed

- Four frame pacing experiments that measured no gain, or worse, are removed with their code:
  `EDEN_NX2_QUEUE_LIMIT`, `EDEN_NX2_FLUID_OFFSET_US`, `EDEN_NX2_FLUID_SPLIT_SUBMIT` and
  `EDEN_NX2_FLUID_HOST_WAIT_US`. Setting one now changes nothing; the presenter keeps the defaults
  they replaced. Their measurements are in
  [docs/switches.md](docs/switches.md#removed-experiments).
- Two tool helpers that nothing called are removed: `person_label` in `tools/ci/legacy.py` and
  `empty_document` in `tools/ingest/ingest.py`.
- Seven `gpu_perf` scope counters and the autopilot's 5th percentile of shown frame times are gone:
  they were counted but never reported anywhere. Nothing a player or the debug server sees changes.

### Fixed

- Android builds cache Gradle's resolved dependencies and wrapper between runs: Maven Central
  rate-limits the runners (HTTP 429), and a cold resolution used to fail the whole APK.
- Linux and macOS: Guarana installs and announces itself as `io.github.karimalsalah.guarana`
  instead of upstream Lichii's `io.github.zortos293.lichii`: the desktop entry, AppStream metadata,
  MIME package and icon, the Wayland app id, the About dialog's theme icon and the macOS bundle id.
  A Guarana package and a Lichii package no longer install the same files or share one app id
  ([#107](https://github.com/karimalsalah/guarana/issues/107)).
- Built-in console news shows Guarana's own GitHub releases, credited to Guarana with an "Open
  GitHub" link, instead of Lichii's releases labelled as Eden news. The update checker keeps its
  source ([#107](https://github.com/karimalsalah/guarana/issues/107)).
- Benchmark boot suites: a second run on a driver that keeps its own pipeline cache (NVIDIA, AMD)
  is no longer marked invalid for a driver cache its cold run never saves, and the caches of cold
  runs that no second run copies are deleted at once instead of filling the work folder; the
  README says to record the reference with `bench.py run` and to smoke-test with two runs first.
- Experimental `EDEN_NX2_FG_GAME_MOTION`: in-between frames no longer drag a copy of the HUD into
  the scene beside it during a pan. A source on the HUD gives way to the other real frame's, as
  extrapolation's rule has it, and `EDEN_NX2_EXTRAPOLATE_HUD_FILL` applies.
- Device reports: a bench run's boolean settings are recorded as `true` and `false`, as the ini has
  them, so the resolver can cite community evidence for a boolean setting.
- Launcher: every version runs from one path, `%LOCALAPPDATA%\Guarana\app\guarana.exe`, instead
  of its own `versions\<version>\` folder. Windows keys the microphone permission, the GPU choice in
  Settings > System > Display > Graphics, firewall rules and Game Bar's game detection by that path,
  so an update or rollback no longer resets them, and the microphone question that held up a boot
  comes once, not after every update
  ([#199](https://github.com/karimalsalah/guarana/issues/199)). The launcher fills `app\` with
  copies of the version's programs and hard links to its other files, and swaps it whole with two
  renames, never while Guarana or a shader warm-up runs: an update made during a game switches at
  the next Play.
- Audio: a Switch 2 game opens the host microphone only when it starts capturing, and no
  game thread waits for that open. On Windows the first start from a new folder (so after every
  launcher update) asks for microphone access, and Ocarina of Time waited on that prompt before
  its first frame, though it never captures
  ([#199](https://github.com/karimalsalah/guarana/issues/199)).
- Mod import on Windows refuses a mod name such as `D:`, or one that ends in a period or a space.
  `D:` made the folder the import clears the current folder of drive D, usually its root, so a
  mod whose type had to be picked by hand could delete files on another drive. An import that was
  cut off no longer loses the mod it had moved aside to replace.
- Merge gate: `tools/ci/pr_gate.py` now requires the "Check the changelog entry" job, so a pull
  request whose changelog check failed or has not reported is no longer called ready. `main` is
  still judged by the three jobs every push runs, and a `resolve/pr-N` commit takes the changelog
  verdict of its pull request's head ([#147](https://github.com/karimalsalah/guarana/issues/147)).
- Stop: each teardown step is logged with its time (`Shutdown step N took X ms: <step>`), the last
  line names the slowest, and the stop watchdog's lines name the last step done, so a slow stop
  says where its time went ([#239](https://github.com/karimalsalah/guarana/issues/239)).
- Stop: a teardown busy on the CPU is no longer taken for a hung one, for up to a minute without
  a step done. After a long session the watchdog could end the program during a step that works
  without noting progress, and a Stop from the menu then closed the whole app instead of going
  back to the game list. Compute pipelines are destroyed with progress, like the graphics ones
  ([#239](https://github.com/karimalsalah/guarana/issues/239)).
- Stop: the Windows trim of all-zero guest memory pages stops when a Switch 2 game stops, and a
  pass in progress ends at its next check: a stop could wait for a whole pass over memory it was
  about to release ([#239](https://github.com/karimalsalah/guarana/issues/239)).
- The Debug setting "Perform Vulkan check" turns the startup Vulkan check off again: it was read
  before the settings were loaded, so the check always ran.
- CI: the Windows jobs try the Vulkan SDK download up to four times before failing, so a
  flaky download server no longer fails a build.

### Security

- Android: only release builds (tags, the nightly and runs on `main`) are signed with the release key.
  Pull requests and runs dispatched by hand build preview APKs with the debug key, and the decoded
  keystore is deleted even when the build fails.
- Windows: zlib 1.4, up from 1.3.2. It fixes an overflow in non-blocking `gzwrite()`
  (CVE-2026-85091), an `inflateGetDictionary()` write past the caller's buffer present since
  1.2.8, and other inflate, deflate and minizip memory-safety bugs. Linux and macOS builds use the
  system's zlib.

## [20.5.0]

### Added

- Tests: the built-in patch tests fail when two built-in patches that can be applied together
  write, check or keep their state in the same bytes of a game's module: code caves, hooks and
  state blocks. A patch and the one it replaces, and the alternatives of one group, may share
  bytes, because only one of them is applied. The table is read through
  `FileSys::BuiltinPatchRanges`.
- Ocarina of Time: three new built-in add-ons, "Text Speed 2x", "Text Speed 4x" and "Text Speed
  Instant", speed up the letters and pauses of dialogue, the fairy's hints and signs. They are
  unchecked by default (Properties > Add-Ons), and with more than one checked the fastest
  applies. A built-in add-on can now be off until it is checked: `[DisabledAddOns]` then holds
  `+<name>` for it (Lichii #1422).

### Changed

- Add-Ons: a built-in patch is labelled "Built-in" in the Version column (it was "IPS"), with
  "recommended" or "experimental" where that applies, and listed without "(built-in)" after its
  name. A box under the list says in plain words what the selected built-in does and, once the
  game has been started, whether it was applied. A ticked built-in that was not applied gets a
  warning sign and its reason: made for another version of the game, a mod changes the same part
  of the game, or the game's code is not what it expects. The game list shows a built-in by its
  name alone and leaves out built-in options that nobody ticked.
- Settings: the Mario Kart World frame-rate options (30 FPS mode, 60 FPS mode, drop to 30 FPS
  when slow, dynamic FPS) and the Hyrule Warriors: Age of Imprisonment ones (dynamic FPS, 30 FPS
  mode) are listed only in their own game's properties, under Adv. Graphics. They were listed in
  Configure > Graphics > Advanced and in every other game's properties, where they do nothing. A
  value set globally before still applies, and shows on the game's page until it is changed there.
  The list of such settings is Eglantine's `game_tweaks` (Eglantine #837 and #844).
- Performance overlay: the trace scrolls in real time. Each bar is one present, as wide as the
  gap since the one before and as tall as that gap, coloured against the 120, 60 and 30 Hz
  budgets. A 1% low sits beside the frame rate, the bars under the trace show how the gaps
  spread, and a strip shows each guest core and the GPU threads. Readouts ease on a spring, and
  the border flashes when a present runs past 1.2 times the median. Right-click the overlay for
  the detailed trace (5 seconds, named cores, min, median and max fps);
  `guarana_overlay_compact=false` is the same setting.
- Add-Ons: importing a mod asks before it replaces a mod of the same name, which it did without
  a word. The new mod is copied completely before the installed one goes, so a copy that fails
  leaves the installed mod as it was. Importing from a folder no longer renames that folder, or
  deletes a folder next to it that has the name chosen for the mod.
- Add-Ons: each imported mod gets a `mod.json` with its name, the zip or folder it was imported
  from and the date, shown under the list when the mod is selected. The game never reads the
  file, and a mod that brings a `mod.json` of its own keeps it.
- Add-Ons: an "Open Mods Folder" button opens the game's mod folder, and makes it for a game
  that has no mods yet.
- Add-Ons: a warning under the list names the game files that more than one ticked mod replaces
  and the mod whose file the game gets, which is the first one in name order.

### Fixed

- GPU traces: `guarana-cli --replay` refuses a trace that holds no presents (`the trace has no
  frame to replay`, exit status 1) instead of writing an empty `frames.json` and exiting 0. The
  recorder logs an error and sets `last_error` when a recording ends before its first present,
  which closing the game during the snapshot does, and logs a warning when it ends short of the
  presents asked for. `trace_status` gains `last_presents`. `trace_replay.py check` runs corpus
  entries the replay must refuse (`expect_failure`), and CI replays the sample trace written
  without its presents (`--gpu-trace-sample <trace> --no-presents`).

## [20.4.0] - 2026-11-16

### Added

- Issue forms: the crash, gameplay bug and visual bug forms now ask for the GPU driver (required, with where to find it on Intel, AMD and Linux), and the UI bug form asks for it as an optional field, as the device report form already did (issue #42).
- `EDEN_NX2_FLUID_COMPOSE=1` (experiment, off by default): with `EDEN_NX2_FLUID` and the screen copy on a compute-capable queue family, the swapchain images are also created with STORAGE usage (the sRGB view format is left out of their format list), and the slot's frame is composed into the image by a compute pass on that queue (one dispatch that also draws the letterbox bars) instead of `vkCmdCopyImage`. Frames, frame generation and interpolation are drawn at the game's internal size (the largest guest size seen) and the pass scales them bilinear into the swapchain image with the letterbox, so the configured scaling filter (FSR 3 and the others) does not run; a window much smaller than the guest returns to the classic path, as do the following cases. `EDEN_NX2_FLUID_COMPOSE_NATIVE=1` (diagnostic) keeps the frames at the window's size and the pass copies them 1:1, to measure the compute store alone. Back to the classic path on HDR output, extrapolation, DLSS Frame Generation, a surface without storage images (or a driver that refuses them), the watchdog's fallback, the debug server's shown_frames pictures and frame generation without `EDEN_NX2_FLUID_INTERP`. Not measured; every other composite stays as it was.
- `EDEN_NX2_MESH_BEFORE_PASS=1` (experiment, off by default): on drivers without `VK_EXT_mesh_shader` (AMD), the compute dispatches of an emulated task or mesh draw run right before the render pass they draw in begins, instead of ending the pass and beginning it again for every such draw (about 70 cuts a frame in Hyrule Field on a Radeon 890M). It applies only when no draw in the pass wrote storage and the draw does not sample its own render targets; ported from the patch in zortos293/Lichii issue 1431 by ronaldpena007-ops, not measured here (issue #22).
- `EDEN_NX2_MESH_BEFORE_PASS=1` (experiment, off by default): on drivers without `VK_EXT_mesh_shader` (AMD), the compute dispatches of an emulated task or mesh draw run right before the render pass they draw in begins, instead of ending the pass and beginning it again for every such draw (about 70 cuts a frame in Hyrule Field on a Radeon 890M). It applies only when no draw in the pass wrote storage, the current draw writes no storage and does not sample its own render targets; ported from the patch in zortos293/Lichii issue 1431 by ronaldpena007-ops, not measured here (issue #22).
- Launcher Doctor: four rows that look at Guarana's own settings and only report (issue #125). "Switch file" says when the launcher's block in `nx2_switches.txt` is not what this launcher writes (an `EDEN_NX2_EXTRAPOLATE=1` from a launcher before #122 on an Intel integrated GPU), when its `EDEN_NX2_FLUID=1` wins over Ocarina of Time's measured value, when two blocks set the same switch, when a line is read wrongly (a byte-order mark, a ` # note` after the value), and when an `EDEN_NX2_` variable is set in Windows. "Settings" says when a shader cache setting is off. "Shader cache files" says when a cache log is of another format than the installed Guarana's (once a build says which it writes) and when Prepare shaders marked a game done but the driver's cache file is missing on an Intel GPU (#106). "Settings folder" says when settings were left behind in `%APPDATA%\lichii` (#107). Nothing is changed, and keys, firmware, saves and games are never read.
- `gpu_costs` times the frame generation passes: three new `emu` rows, `frame_gen_process` (once per interpolated real frame), `frame_gen_interpolate` and `frame_gen_extrapolate` (one per generated frame), recorded only in the frames the existing cost sampling times (`EDEN_NX2_COST_SAMPLING`, the debug server's `gpu_costs` `enable`, `EDEN_NX2_VRS_AUTO`, `EDEN_DKB_FRAME_TRACE`); their GPU time was in no row before ([docs/nx2/debug-mcp.md](docs/nx2/debug-mcp.md)).
- `EDEN_NX2_FLUID_MAILBOX=1` (experiment, off by default): with `EDEN_NX2_FLUID`, the swapchain presents with MAILBOX instead of FIFO when the surface offers it, so a present that misses its vblank no longer queues ahead of the later ones (a later present that arrives before it is shown replaces it); one log line per swapchain says which mode it got. Image counts are unchanged. Not measured; plan in [docs/switches.md](docs/switches.md).
- `EDEN_NX2_FLUID_IMMEDIATE=1` (experiment, off by default): with `EDEN_NX2_FLUID`, the swapchain presents with IMMEDIATE instead of FIFO when the surface offers it (the Intel driver on the reference laptop lists IMMEDIATE and FIFO, no MAILBOX), so a present is shown as soon as its copy is done and nothing queues behind a late one; MAILBOX still wins when `EDEN_NX2_FLUID_MAILBOX` is on and listed. A present lands during scan-out (not at a vblank), so a tear line can show, at a height that follows the copy's finish; one log line per swapchain says which mode it got. Not measured; plan in [docs/switches.md](docs/switches.md).
- `EDEN_NX2_OOT_BYPASS_STEP_GUARD=1` and `EDEN_NX2_OOT_BYPASS_SHAPE=1` (experiments, off by default, read only with `EDEN_NX2_OOT_BYPASS_ANY=1`): Ocarina of Time's No sharpening bypass skips the anti-aliasing network only while the tonemap is the image and drawn area it last replaced, so a resolution step whose output pass it does not know runs the network instead of leaving the scene black ([#98](https://github.com/karimalsalah/guarana/issues/98)); and it can know the output pass of any step by its shape. Not measured; plan in [docs/switches.md](docs/switches.md).
- `EDEN_NX2_OOT_RENDER_SCALE=75` or `=50` (experiment, off by default): Ocarina of Time with the Max Resolution add-on draws its scene at 75% or 50% of the output resolution without NVIDIA DLSS, by applying the render-scale records that applied only with DLSS on a Streamline build; the applied render scale stays unset, so FSR stays off and the network bypass stays on. Emulator-side resolution handling only, ignored for other values or when DLSS chooses the scale. Not measured; plan in [docs/switches.md](docs/switches.md).
- `EDEN_NX2_FLUID_MODE_PERIOD=1` (experiment, off by default): with `EDEN_NX2_FLUID`, the display mode's refresh period is kept when the present thread's wakes slip, instead of the median of the last wake intervals replacing it whenever they differ by more than 10%. A present thread that sleeps through every other vblank at 144 Hz no longer halves the clock to 72 Hz and doubles the copy deadline; the vblanks it slept through are counted at the mode's rate, and whole slots slept through show as overslept. Windows only, where the display mode's period is known. A median that is not a whole multiple of the mode period, with a lower quartile that differs too (a display whose refresh really changed), is still followed (issue [#136](https://github.com/karimalsalah/guarana/issues/136)). Not measured; plan in [docs/switches.md](docs/switches.md).
- `EDEN_NX2_FG_PINGPONG=1` (experiment, off by default): the FSR3 frame generation chain copies one full-size frame per real frame instead of two. Its two images swap roles each real frame and the new frame goes into the one that held the older frame, where today the current image is first copied into history ([#136](https://github.com/karimalsalah/guarana/issues/136)). Not measured; plan in [docs/switches.md](docs/switches.md).
- `EDEN_NX2_SEED_FILTER=1` (experiment, off by default): a new global config is written without the Fsr3 scaling-filter seed, so the filter stays at its default, ScaleForce, and a game preset could choose one (none does yet); Fsr3 costs 3.4 to 3.7 ms of GPU time per shown frame on the Panther Lake laptop against 1.7 ms for ScaleForce. Not measured; part of #136.
- Debug server: `boot_timing` returns, next to the boot phases, the cumulative draw time held for unbuilt pipelines, the pipelines built by kind and the guest file read time, sampled at the first presented frame and at each `{"mark": "<label>"}` a client takes, to split boot to gameplay ([#136](https://github.com/karimalsalah/guarana/issues/136)). Counted only when `EDEN_NX2_DEBUG_SERVER` is set.
- `EDEN_NX2_GPL_PARALLEL_PARTS=1` (experiment, off by default): with `EDEN_NX2_GPL=1`, a pipeline builds its fragment shader library as a job of its own on the shader job workers while its building thread builds the pre-rasterization library, and runs the job itself when no worker is free; not measured yet (#136).
- `EDEN_NX2_FG_MEDIAN_ONCE=1` (experiment, off by default): the FSR3 frame generation chain computes the 3x3 median of the motion field once per real frame, at flow resolution, in one extra pass after the optical flow, and the interpolation shader reads it with one fetch, where today each of the up to five interpolation dispatches recomputes it for every output pixel ([#136](https://github.com/karimalsalah/guarana/issues/136)). Not measured; plan in [docs/switches.md](docs/switches.md).
- `EDEN_NX2_INTEL_COOP_MATRIX=1` (experiment, off by default): on an Intel device whose driver lists an 8x16x16 fp16 cooperative matrix shape, guest HMMA runs on those matrices through shared memory instead of shuffles, a K=16 HMMA as two products so the fp16 accumulator rounds after each K=8 group as the guest does (Fritchii-Intel-Fixes 0007 and 0008 on top of Lichii#779; the author's figure is 13.5 ms of GPU time per frame saved on an Arc B580, not measured here). Not measured on the Arc 140T or Panther Lake.
- `EDEN_NX2_GPL_OPTIMIZE_CACHED=1` (experiment, off by default): with `EDEN_NX2_GPL=1`, pipelines loaded from the shader cache get the optimized link in the background at their first draw, as pipelines built during play do, instead of drawing fast-linked until they are evicted and rebuilt (issue #103). The `Nx2Perf gpl` line now counts fast-linked draws with no optimized link coming and the links queued at a first draw. Not measured; plan in [docs/switches.md](docs/switches.md).
- `EDEN_NX2_PREBUILD_NO_INLINE=1` (experiment, off by default): with asynchronous shaders, a draw whose cached pipeline is still queued or building in the background prebuild is skipped, as one with a new pipeline is, instead of translating and compiling it on the GPU thread; the entry is raised to NeedNow and a later draw takes it (issue #103). Not measured; plan in [docs/switches.md](docs/switches.md).
- `tools/bench/suites/presenter-and-locks.json`: one command measures the default-off pacing, pipeline link, lock and thread placement switches (`EDEN_NX2_PRESENT_SPIN`, `EDEN_NX2_GPL_OPTIMIZE_CACHED`, `EDEN_NX2_SCHED_PAUSE`, `EDEN_NX2_SYNCPOINT_SPLIT`, `EDEN_NX2_HYBRID`, `EDEN_NX2_IDLE_SPIN_US=0`) against the baseline, fullscreen with the pacing counters, in the Kokiri Forest scene or any scene given with `--scene`.
- Welcome: a person's first merged pull request and first device report get one thank-you comment with one next step ([welcome.yml](.github/workflows/welcome.yml)).
- Agent kit: [docs/agents/README.md](docs/agents/README.md#what-costs-nothing-today) lists the agent tiers that cost nothing today, with the vendors' stated limits.
- Pull request template: a note for a first pull request, a tests item for contributors who cannot build (CI runs the tests), and an `Assisted-by:` trailer item; CONTRIBUTING.md says why a first-time contributor's checks may wait for approval.
- Docs: [docs/lifecycle.md](docs/lifecycle.md) lists what Guarana and its launcher store, from switch files and settings to shader caches and markers, and when each is flushed, loaded and preloaded, with the code that does it; [docs/design/settings-hygiene.md](docs/design/settings-hygiene.md) is the proposal in [#125](https://github.com/karimalsalah/guarana/issues/125) (not yet built) to record who wrote each value, reset what the player did not choose, and let the doctor run on its own.
- `EDEN_NX2_VULKAN_INSTANCE_ONCE=1` (experiment, off by default, issue #46): the Vulkan instance is created once per boot. A game started from the game list no longer pays for a second `vkCreateInstance` (about 3.6 s on the Arc 140T): the frontend's device list borrows the instance made on another thread and the renderer takes the same one. Not measured.
- `EDEN_NX2_WAIT_BOOST=1` (experiment, off by default): when a draw blocks on a shader or pipeline build job that is already running, the worker thread running it goes from below-normal to normal priority until the job ends, on Windows and Linux (a no-op where priorities cannot be set). Not measured yet; the plan is in [docs/switches.md](docs/switches.md).
- `EDEN_NX2_VBLANK_RECHECK_ASYNC=1` (experiment, off by default, Windows): the FLUID presenter's 2 s display-mode recheck (`QueryDisplayConfig`) runs on a helper thread that publishes the period, instead of on the present thread just before its vblank wait; the helper logs what the queries cost. Not measured yet.
- `EDEN_NX2_SHOWN_GOVERNOR=1` (experiment, off by default): with `EDEN_NX2_FLUID`, every 2 s the shown rate steps through the divisors of the display's refresh rate (144, 72, 48 Hz) from the share of slots shown on time, real frames against the cadence's rate and the GPU's busy time, with a dwell and a 60 s block after a step up that failed; the cadence divisor scales with it (6, 3, 2) so real frames keep their rate and only the in-between frames change (issue #136). Not measured; plan in [docs/switches.md](docs/switches.md).
- `tools/ci/pr_gate.py NUMBER` answers whether a pull request is safe to merge (issue #147): `ready` or `not-ready` with the reasons, exit status 0 or 1, `--json` for the evidence. Not ready: a draft; a required `build-check` job failing or still running on the head; `main` red while the pull request lacks the new `fixes-main` label; a conflict outside `CHANGELOG.md`, `docs/switches.md` and `dist/nx2_switches.txt`. It only reads GitHub. `build-check` also runs on `merge_group`, so GitHub's merge queue can be turned on later; what runs on pull requests and pushes is unchanged. See [docs/LANES.md](docs/LANES.md#merging).
- Launcher: `EDEN_NX2_LAUNCHER_PAD_IDLE=1` (experiment, off by default, read from the launcher's own environment): the launcher stops polling controllers while a game it started runs, its 250 ms loop and gilrs' own polling thread, and resumes when the game exits (not measured; [launcher/CONTRACT.md](launcher/CONTRACT.md#pads-and-the-log-while-a-game-runs)).
- Launcher: `launcher.log` lines carry milliseconds, and every start writes a launch timeline (Play pressed, settings ready, command built, process spawned, each boot phase, the emulator's window, the exit), each with the milliseconds since Play ([launcher/CONTRACT.md](launcher/CONTRACT.md#pads-and-the-log-while-a-game-runs)).
- `EDEN_NX2_PIPELINE_BINARIES=0` or `1` (experiment, unset keeps the built-in choice; issue [#20](https://github.com/karimalsalah/guarana/issues/20)): overrides whether the `VK_KHR_pipeline_binary` store runs. Unset, it runs on drivers with no pipeline cache of their own and on AMD's proprietary driver, as before. `0` turns it off (an escape hatch for a driver bug); `1` turns it on wherever the driver offers the extension, the `pipelineBinaries` feature and `VK_KHR_maintenance5`, also where the driver keeps a cache of its own (Mesa RADV, NVIDIA: NVIDIA stored 2.3 GB in one session) or prefers its internal cache. One log line at device creation says the decision and why. The reference Intel driver does not offer the extension, so this is for AMD and Mesa testers. Not measured; plan in [docs/switches.md](docs/switches.md).
- Game resolver: one pure function (`Core::GamePresets::Resolve`) now decides a game's settings, add-ons and pacing switches from the machine, the title and what the player set, and gives each a value, a reason and its evidence (a measured row in [docs/switches.md](docs/switches.md), or not measured); `Apply` and `ApplySwitchDefaults` take their decisions from it, the boot log gets one `Resolution:` line and the debug server's `status` a `resolution` field. It also records, and does not yet use, the pixels of the display the window is on and the GPU's compute-unit or SM count where the driver reports it (AMD and NVIDIA; Intel's Windows driver does not). No default changes: a test pins today's values. Part of #101.
- `EDEN_NX2_FRAME_TARGET_HOTKEYS=1` (experiment, off by default): four hotkeys that set the frame generation target without opening Configure: "Frame target 30", "Frame target 40", "Frame target 60" (no default keys) and "Cycle frame target" (controller Home + Dpad Left). All can be rebound; each shows the new target over the game window for a moment, or "Frame generation is off". Targets are published atomically for the current session and do not change saved configuration (issue #35).
- `EDEN_NX2_HMMA_FULL_SUBGROUPS=1` (experiment, off by default): a compute pipeline whose HMMA runs on cooperative matrices is created with full subgroups also when its SPIR-V is emitted by a deferred shader job, as the synchronous route already does; the flag was set only by emission, after the pipeline had copied the shader info. The SPIR-V is unchanged, and no wrong output was shown without it ([#105](https://github.com/karimalsalah/guarana/issues/105)). Not measured; plan in [docs/switches.md](docs/switches.md).
- `EDEN_NX2_ASYNC_TRANSLATE=1` (experiment, off by default): with asynchronous shaders on and `native_async_draws` Bounded or Skip, a new native graphics pipeline whose draw may be skipped is translated on a shader job worker from environments detached from guest memory, and its draw waits within `native_pipeline_wait_ms` instead of the GPU thread translating it; from upstream pull request [#1379](https://github.com/zortos293/Lichii/pull/1379), on top of #1378. Snapshot memory is reserved before copying, unexpected worker failures retry synchronously, and stale prepass generations are invalidated. The size estimate uses `bucket_count` because `boost::unordered_flat_map` has no `capacity`. `EDEN_NX2_PREBUILD_NO_INLINE` still skips a draw whose prebuild entry is queued, without caching a null pipeline.
- Vulkan: add an off-by-default ASTC upload flush-batching experiment for texture streaming (#136).
- Benchmark: `tools/bench/suites/slot-compositor.json` measures the slot compositor (`EDEN_NX2_FLUID_COMPOSE`) against the baseline and its diagnostic control, then `EDEN_NX2_FG_PINGPONG`, `EDEN_NX2_FG_MEDIAN_ONCE`, `EDEN_NX2_SHOWN_GOVERNOR` and `EDEN_NX2_OOT_RENDER_SCALE=75` one at a time on top of it, fullscreen with the pacing counters. Rows from it are how these switches get a measured result ([#136](https://github.com/karimalsalah/guarana/issues/136)).
- Changelog fragments ([#118](https://github.com/karimalsalah/guarana/issues/118)): a pull request adds `changelog.d/<slug>.<section>.md` instead of a line in `CHANGELOG.md`, so merging one pull request no longer makes every other one conflict there. `tools/ci/changelog_fragments.py` has `collect` (the `[Unreleased]` section with the fragments folded in, used by `changelog-section.sh` and the nightly notes), `check` (build-check: a pull request that changes code needs a fragment or a line under `[Unreleased]`) and `release` (`bump-version.sh` folds the fragments into the new version and deletes them). Lines already under `[Unreleased]` stay, and both forms are accepted until the next release. See [changelog.d/README.md](changelog.d/README.md).
- Drivers: at boot the engine compares the graphics driver's version with the versions it
  knows something about and logs the result after the `Device profile:` line. Known today:
  NVIDIA's driver before 580.119.02 and Mesa RADV before 26.2.5, which the renderer works
  around (the window names such a driver once per run), and Intel's Windows driver before
  32.0.101.9175 on the Core Ultra and Arc B-series integrated families, the driver the Intel
  numbers were measured on (logged only). Every other driver gets no verdict
  (`Common::AssessDriver`, `docs/DEVICES.md`).
- Added experimental slot-compositor bicubic and light sharpen filters behind
  `EDEN_NX2_FLUID_COMPOSE_FILTER` (default off), in the existing single pass.
  Not measured; part of #136.
- Vulkan: add default-off `EDEN_NX2_FLUID_INTERP_PHASE` for the FSR3 chain only (other frame generation types use main's path), to map interpolated frames to their display slots using unrounded source intervals (#136, #104).
- Vulkan: add an opt-in bounded presenter event history with `EDEN_NX2_FLUID_RECREATE_TRACE` for swapchain and compositor failure diagnosis (#136).
- Benchmark: add explicit GM=11, GM=0 and GM=3 variants and traced twins to the iGPU switch suite.
- Tools: `tools/nx2/trace_replay.py bench` runs a paired A/B on a recorded GPU trace: each
  variant (switches, a build, or both) replays the same frames in turn, and the report says
  whether its frames are the baseline's bit for bit and what a frame cost to replay. No game,
  save or reference screenshot is needed, so the scene cannot be the wrong one
  (`docs/nx2/gpu-trace.md`). `guarana-cli --replay` gains `--no-images` and writes `replay_ms`
  per frame, the frame's own cost without the PNG encoding of the one before it.

### Changed

- `docs/switches.md` lists the known issues of the image-changing experiments merged on 14 November (`EDEN_NX2_FG_PINGPONG`, `EDEN_NX2_OOT_BYPASS_STEP_GUARD`, `EDEN_NX2_OOT_BYPASS_SHAPE`): none has had its scene check yet, so they stay off outside paired runs. `EDEN_NX2_FLUID_COMPOSE` is on that list too: it draws at the game's internal size and scales with a bilinear pass, so the configured scaling filter does not run.
- Docs: the `EDEN_NX2_PRESENT_SPIN` row now says it only changes the non-FLUID present thread's sleep, not the FLUID presenter's, and records that on the Panther Lake laptop it fell to 16.9 real frames a second with a p99 of 132 ms (issue #136).
- The launcher build pins Node 22 (`actions/setup-node`), so a runner image change cannot move Node outside what the locked vite accepts.
- Upstream Lichii sync (through `7a4fe8e0`): solid NSZ files play, with a bounded NCZ decoder and a private disk cache (#1675, #1734) and idle readers releasing their decoder for the memory budget (#1737); Mario Kart World gets a 60 FPS mode at normal game speed with a 30 FPS fallback (#1704, two new Advanced settings); command list pages are copied again after DMA copies and clears (#1736); on Intel only divergent barriers lose their execution barrier, uniform ones are kept (Mario Kart World ANV hang, #1377, #1677).
- Upstream Lichii sync (through `2834e34c`): the texture cache's garbage collector no longer spends its deletion budget on images it cannot evict — non-evictable CostlyLoad (ASTC) images at the oldest end of the LRU no longer starve the pass, candidates are filtered before the budget is spent, the `EDEN_NX2_TC_OLD_AGE` pass drains the emptiest VMA blocks first, and deleted images keep the block fill ratios accurate (#1731). Guarana's batched eviction downloads (`EDEN_NX2_HITCH_GC_BATCH`) and clean-first ordering are kept on top of it, with the same block accounting for batch deletions.
- The FSR 3 scaling filter no longer allocates and clears an output-size RGBA16F history image that no pass reads (no descriptor, shader or draw ever bound it), so it saves one image of memory per filter instance and one clear at the first frame; the rendered result is unchanged (#136).
- Launcher: a new interface. The fruit is now an eye that follows your focus and opens into the game when you press Play, and Home shows your last measured session: the share of shown frames on time, a pacing strip and the fps readouts ([#188](https://github.com/karimalsalah/guarana/issues/188)).
- The performance overlay eases the fps, frame time, and CPU/GPU readouts toward the measured values, draws CPU and GPU meters, fills the frame-time trace, and marks a hitch when the newest frame is slower than 1.2× the median. The footer shows guest-to-wall speed and how many shaders are building. The motion timer stops while the overlay is hidden.
- The FLUID presenter and the frame-generation cadence parse `EDEN_NX2_SHOWN_RATE` once per process instead of calling `getenv` on every vblank. The frame-generation setting is still read each time, so a UI change applies when the environment did not override it. The rate is unchanged.
- Upstream: merged zortos293/Lichii `nx2` from `642eace1b7` to `99e89096cb` (67 commits).
  Among them: Loading Shaders waits only for the pipelines the last two sessions used and
  builds older ones in the background (Lichii #1803); generated-frame spacing counts from the
  deadline, which ends Ocarina of Time locking at 11 to 17 game fps at 240 Hz with 4x frame
  generation (Lichii #1779); images newer ones overwrote are collected at the collector's own
  age, which ends the 16 GB growth in Ocarina of Time's Skulltula House (Lichii #1845); global
  memory accesses are routed among their own candidate buffers (Lichii #1807, #1848, #1853);
  large ASTC arrays decode in batches on integrated GPUs (Lichii #1823); a staging buffer
  without memory degrades instead of aborting (Lichii #1870); Ocarina of Time's upscale passes
  are recognised by native program key, so a lowering change no longer turns FSR and DLSS off
  (Lichii #1871); Mario Kart World keeps GPU tick timestamps (Lichii #1817); SDL gyro survives
  a controller reconnect (Lichii #1805); and bounds fixes in the NCZ, KIP, NPDM, IPSwitch and
  cheat parsers.
- `EDEN_NX2_GM`: narrowed and rebased loads (bits 2 and 4) now sit on top of upstream's
  per-access routes: a load takes its narrowed list when it has one and its route otherwise.
  Buffer base hoisting (bit 1) applies to both.
- Frame trace: Guarana's per-pass GPU cost timing (`DKB gpu_pass renderer_frame=...`, the
  debug server's `gpu_costs`) runs next to upstream's new pass trace (`DKB gpu_passes`, one
  `DKB gpu_pass <ms>` line per pass); `tools/bench` reads the summary line only.
- Shader cache: the log line and marker that count the pipelines admitted for the background
  prebuild report the planned count again; it had read 0 since the v20.3 merge, which also
  kept the "Background prebuild done" line from being logged.
- CI: the Windows build uses ccache 4.14.3 (response-file escaping and false cache hits).

### Fixed

- CI is green on `main` again after the upstream sync and the doctor checks: the buffer cache tests' fake runtime answers `MergesListPageRuns` (upstream #1736 made `Nx2CopyListPages` ask for it), and the doctor's hygiene tests name documented switches, with its prefix filter listed in the switch registry check.
- Windows clang-cl builds no longer hang on the resource step: the `.rc` files are compiled
  with the SDK `rc.exe` directly instead of CMake's `cmake_llvm_rc` wrapper, whose clang
  line-marker conversion left SDK-header markers unconverted, flooding `rc.exe` with RC2019
  errors until its output pipe filled and the build deadlocked (CMake #20198).
- The end of the boot shader load (`LoadPipelinesV2`) ran its last block twice: the async-boot
  completion, the boot-timing mark and the warm-only final save were duplicated by an earlier
  merge. The second `AsyncBoot::Done()` counted the load as finished one job early, so the
  async boot's driver-cache save could run while a pipeline build was still in flight (the
  crash upstream #1685 guards against), and warm-only runs saved and reported twice.
- Fix unit-test linking when the OpenGL shader backends are disabled.
- The ingest shallow-checkout test clones with `--no-local` and a plain path instead of a `file://` URI, so it runs on Windows git (which rejects `file:///C:/...`) as well as Linux; the clone is still shallow.
- Ocarina of Time on a Windows integrated GPU now gets its early read-back switch defaults (`EDEN_NX2_EARLY_SUBMIT=1`, `EDEN_NX2_EARLY_PAGES=256`, `EDEN_NX2_EARLY_AFTER=1`) from the first game started in a process. The read-back read them when the GPU was created, before the renderer put the game's defaults in the environment, so a first start ran with the read-back's built-in values (0, 64, 2), and a later start in the same process with whatever the previous game's defaults left, even for another game or after automatic presets were turned off. Every start from the launcher, which starts the emulator for each game, and from `guarana-cli` is a first start. The renderer now reads them again right after the defaults, and the boot log says which values are in effect (`Read-back switches in effect`). A value you set in the environment or in your own `nx2_switches.txt`, the Smooth mode's included, was already in effect and stays. This changes what the default preset does on a first start; not measured on a device yet (setting the three switches by hand on an older build gives the same). Found while mapping settings hygiene ([#125](https://github.com/karimalsalah/guarana/issues/125)). Unit tests only; not compiled here.
- Ocarina of Time on an Intel integrated GPU with a 120 Hz or 144 Hz display gets its locked set out of the box, with interpolated in-between frames ([#119](https://github.com/karimalsalah/guarana/issues/119)). The shipped `nx2_switches.txt` sets `EDEN_NX2_EXTRAPOLATE=1`, which turned `EDEN_NX2_FLUID_INTERP` off. A game's switch defaults now replace a value that only the shipped `nx2_switches.txt` set; the real environment and your own `nx2_switches.txt` still win. The 120 Hz and 144 Hz sets name `EDEN_NX2_EXTRAPOLATE=0`, the only value of any set that differs from the shipped file's. A value that a game's defaults or a switch file put in the environment no longer passes to the new version started after an in-app update, where it would count as yours, nor to a second game started in the same process. The `Switch defaults for this game and GPU` log line now lists the values it set, the ones it set over the shipped file (with that file's value) and the ones the player set. Not measured after the change; the 144 Hz set's own rows were measured with `EDEN_NX2_EXTRAPOLATE=0` ([docs/switches.md](docs/switches.md#defaults-for-ocarina-of-time-on-a-windows-integrated-gpu)).
- Launcher: with an Intel integrated GPU, the launcher no longer writes `EDEN_NX2_EXTRAPOLATE=1` into your own `nx2_switches.txt` ([#119](https://github.com/karimalsalah/guarana/issues/119)). At 115-125 or 140-148 Hz that line won over the emulator's locked 120 and 144 Hz sets for Ocarina of Time, so the presenter predicted frames instead of interpolating one for every refresh (one 144 Hz Panther Lake laptop, single runs at the same cadence divisor of 7: 15.5 real and 58.5 shown frames a second with the line, 20.6 and 84.1 without it). With "Ocarina of Time: predict frames forward" on (the default), the line is now left out at every refresh rate, primary GPU or not, and the emulator decides at each start: its 60 Hz Intel set and the shipped file still predict at other rates. Off still writes 0, and machines without an Intel integrated GPU get 1 as before. In Custom mode the launcher's block (only that, not `qt-config.ini`) is now rewritten at every start, so a line an older launcher wrote goes too. The toggle's help says so. Takes effect with the emulator change that makes the 120 and 144 Hz sets name `EDEN_NX2_EXTRAPOLATE=0` (#119); with an older emulator build the shipped file's 1 still applies. Unit tests only; not compiled or run on a device here.
- The switch registry check passes on main again: `tools/ci/switch-registry-upstream.txt` is regenerated from upstream Lichii's `57b75fec`, the commit the upstream integration merged, which adds upstream's `EDEN_NX2_GPU_TRACE` and `EDEN_NX2_GPU_TRACE_PRESENTS` (upstream [#1655](https://github.com/zortos293/Lichii/pull/1655)) and drops `EDEN_NX2_LIST_HLE_VERIFY`, which nothing uses any more. The `upstream` mirror still stops at the previous merge, so CI's `--upstream-ref` did not have them.
- The switch registry's checked-in upstream list is regenerated from upstream Lichii `7a4fe8e078`, the commit main last merged, so a run without the upstream branch (`python -I tools/ci/switch_registry.py` on a clone) no longer reports `EDEN_NX2_VULKAN_CACHE_FLUSH` (upstream [#1712](https://github.com/zortos293/Lichii/pull/1712)) and `EDEN_NX2_JIT_SYMBOLS` as missing a row. CI was unaffected: it reads upstream's tip.
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
- CI: a failed scheduled `upstream-sync` run now opens one "upstream-sync is blocked" issue with the run and the fix (it comments again only when upstream moved, and the next good run closes it); the `tools/ci` and `tools/ingest` tests start on every file they read, including `README.md` and `AGENTS.md`; a new `tools-nx2` workflow runs the six `tools/nx2` test suites no workflow ran, and `scripts/run-tests` runs them with the GPU trace replay and offline RADV tests; and the Linux-only probe tests (`test_thread_worker_wake.py`, `test_switches.py`) skip on Windows instead of failing.
- One device profile before the device ([#101](https://github.com/karimalsalah/guarana/issues/101)): the game's pacing set (Ocarina of Time on an Intel integrated GPU) is chosen from the refresh rate of the display the game's window is on, not the primary display's, and the GPU vendor is read once, by PCI vendor ID, for the settings presets and the switch defaults alike (they read it by driver ID and by PCI ID before). With one display nothing changes. The boot log's switch-defaults line names the display (`display \\.\DISPLAY2 (primary) at 144 Hz`). Not measured on a machine with two displays.
- Debug adapter (`tools/nx2/debug_mcp.py`): with no emulator listening it says to start Guarana (guarana.exe), not Lichii, and names itself `guarana-debug`; `--call status` (a method name alone) works from PowerShell, which can drop the inner quotes of `--call '{"method":"status"}'` (Windows PowerShell 5.1, or any PowerShell through a `.cmd` wrapper), and a JSON error now says so. AGENTS.md's list of debug methods gains the nine it lacked (`sample`, `speed_limit`, `host_timing`, `autopilot`, `read_memory`, `search_memory`, `read_gpu_memory`, `input`, `input_sequence`), `docs/nx2/debug-mcp.md` gains the `trace_*` ones, and a test keeps both in step with the adapter.
- ASTC decoding and the OpenGL unswizzle shaders no longer store texels outside the image: a block at the right or bottom edge of an image whose size is not a multiple of the block size, and workgroups rounded past the image, wrote out of range, which can corrupt other images' memory or fault the GPU on some drivers (AMD in particular). Stores inside the image are unchanged. Ported from Nolar-Hack/Lichii `eb518660ec` (Frizti), which took it from CheezerWheezer/Lichii `132cc46c9e`; not yet checked on a device.
- Shader: the global increment and decrement loops take the storage element
  pointer again, after the NVIDIA buffer-select merge passed the whole type.
- Vulkan: drain background GPL optimization builds before the teardown driver pipeline cache save
  in `~PipelineCache`, and add unit-tested shared/exclusive gating, reload drain, and unchanged
  save skipping behind opt-in `EDEN_NX2_CACHE_SAFE=1` on Intel Windows drivers
  ([#7](https://github.com/karimalsalah/guarana/issues/7)).
- Frame target hotkeys: read the experiment switch once after loading the switch file instead of on every target read (#35).
- Tests: `game_presets` binds the `SwitchDefaults` span to a named local before its range-for, so GCC 13's `-Wdangling-reference` no longer reads the default `DisplayInfo{}` argument as a destroyed temporary. The span points at the static switch arrays; behaviour is unchanged.
- Switch registry: document the Intel 8x16x16 cooperative path and its shared-memory scratch requirement for full HMMA subgroups (#105).
- Launcher: on an Intel integrated GPU the profile block no longer writes
  `EDEN_NX2_FLUID=1`. That line won over Ocarina of Time's measured `FLUID=2`,
  the same way a leftover `EXTRAPOLATE=1` turned interpolation off (#119).
- Launcher: contain game and shader warm-up workers and their descendants in Windows jobs,
  ending them when the launcher exits or crashes ([#172](https://github.com/karimalsalah/guarana/issues/172)).
- Launcher: assign workers to their Windows job immediately after normal startup, document the brief pre-assignment containment gap, and fix Windows paths in the containment tests.
- Build: MSVC can fill a solid NCZ prefix and search storage-buffer addresses,
  and GCC can compile the query-report lambda and the tests that shadowed
  Catch2, dynarmic, and ring-buffer names (Lichii #1768, #1763, #1762, #1761, #1760).
- Fire Emblem 1.1.1's captured prepass opens the same replacement chain as the
  original prepass (Lichii #1752).
- Dump RomFS reads a loose .nup from its loader instead of looking for an
  installed NCA (Lichii #1764).
- Ocarina of Time: the 3D view no longer comes out white or in false colours after a load.
  A stream buffer wrap that was refused kept the start position, so the next uploads were
  written over bytes the GPU had not read yet (Lichii #1835).
- Resolution scaling: images are not scaled past the device's 2D image and framebuffer limits.
  Ocarina of Time's sun shadow map at 1.5x and 2x went over 16384 and lost the device on AMD
  (Lichii #1890).
- Texture cache: the retention limit follows the lowest driver budget read lately, so a budget
  that Windows moves down and up no longer takes the process over it every few seconds
  (Lichii #1860).
- Memory: native compute launches clear the pending upload list, which grew for the whole
  session in Ocarina of Time, 768 MiB after 90 minutes (Lichii #1908).
- Moving the window during play no longer aborts with VK_INCOMPLETE: the swapchain image,
  surface format and present mode queries are repeated (Eglantine c40fca7da4).
- Status bar: "Building: N shader(s)" returns to zero when a compute pipeline is dropped before
  its build ran (Lichii #1799).
- A render target that gets no memory is left out of the framebuffer instead of being bound as the null view, which handed the driver a null attachment (an access violation on AMD's Windows driver with a small UMA carve-out). Port of [zortos293/Lichii#1766](https://github.com/zortos293/Lichii/pull/1766).
- The pipeline prebuild's failed-translation counter takes its lock as `failed_lock`, so GCC's `-Wshadow` does not stop the build (port of [zortos293/Lichii#1759](https://github.com/zortos293/Lichii/pull/1759)).
- Log: the file log flushes once a second, so a hang or a kill keeps the last
  second of lines below Error (Lichii #1771).
- DLSS panel: installing super resolution also copies `sl.common.dll` and
  `sl.dlss.dll` (Lichii #1753).
- GameChat: answer the camera request over CMIF and return `chat:u`'s objects
  (Lichii #1750).
- Ocarina FSR: on AMD the jitter offset stays non-negative so the integer viewport
  scissor does not drop the last column and row (Lichii #1744).
- NVIDIA: routed global memory picks its storage buffer with OpSelect instead of
  one branch per buffer, which hung the driver (Lichii #1757, #619). Other
  drivers keep the per-buffer chain.
- Vulkan: report an ignored pipeline-binary override even when the driver does not offer the extension (#20).
- Vulkan: defer zero-extent swapchains, handle failed image acquisition explicitly, and flush fatal presentation-thread errors before exiting ([#136](https://github.com/karimalsalah/guarana/issues/136)).
- Vulkan: avoid submitting a frame twice after a failed present, and exclude queue activity
  during the blit screen's device idle wait, including pending copy-ahead presents (#136).
- Build: the frame-target toast test can include `qt_config.h`, so SimpleIni's
  headers travel with `qt_common` instead of being private to that library.
- Build: `video_core.h` includes `core/game_presets.h`, so `Core::GamePresets::DisplayInfo` is a complete type where `ApplyGameSwitchDefaults` gives it a default argument. `main` did not compile on GCC or clang-cl after the resolver changes landed ([#101](https://github.com/karimalsalah/guarana/issues/101)).

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
- Docs: [docs/DEVICES.md](docs/DEVICES.md#intel-graphics-4-xe3-cores-panther-lake) lists the 4-core Panther Lake GPU (`IntelGraphicsPantherLake`) in the support matrix, with what its driver reports and the single runs made on it; no default changes. Tests: a preset listed for a GPU family is found before a broader one of the same title, and no shipped preset hides behind an earlier one ([#24](https://github.com/karimalsalah/guarana/issues/24)).
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
