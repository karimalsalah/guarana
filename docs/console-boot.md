# Console boot: launch to gameplay in seconds, no shader hitch

The goal: the first play of a game on an Intel or AMD integrated GPU feels like a console. The game
is in play within seconds of the launch, and no frame visibly hitches because a shader or a pipeline
is being built, on the first run as well as later ones. This page is the plan and its status. Each
step is its own pull request behind a default-off `EDEN_NX2_*` switch with a row in
[switches.md](switches.md); nothing becomes a default without valid rows from the suite in
"Measure" below ([AGENTS.md](../AGENTS.md), rule 1). Roadmap items [X1 to X6](../ROADMAP.md#next)
point here.

Numbers come from a trace line or an upstream measurement, with the source next to them. "Estimate"
marks every number that does not.

## Done means

On the Arc 140T (Core Ultra 9 285H, driver 32.0.101.9175), Ocarina of Time, rows from
`tools/bench/suites/console-boot.json`:

- **Second play** (the cache the first session left), started directly (a shortcut, the launcher,
  `-g`) after a clean exit, as the suite starts it: first guest frame within 10 s of the process
  start. That is an estimate, see "The boot target" below; the suite's first rows give today's
  number on this kind of start ([#46](https://github.com/karimalsalah/guarana/issues/46)'s 24.7 s
  was a game-list start on which the startup check ran, which pays two more costs).
- **First play** (no emulator cache, no driver cache): no frame over 100 ms whose cause is a
  pipeline (`Nx2Stutter` causes `gfx_miss`, `compute_miss`, `pipeline_wait`) from the first frame
  to the scene, and the held-draw time to the scene (`boot_timing` `split`) under 1 s. Today's
  number is not measured; the suite's first rows give it.
- The image is unchanged wherever a draw is not skipped, and every skip is one the policy names
  (below, "What the image does").

## 1. Where the time goes today

One timeline, the only full one: [#46](https://github.com/karimalsalah/guarana/issues/46), Arc 140T,
warm caches, a game started from the game list, the `Boot timing:` marks of #3. Each second is
counted once.

| At (ms since process start) | Phase ends | Took (ms) | What can take it off |
|---:|---|---:|---|
| 4084 | Logging starts (the startup Vulkan check runs before it) | 4084 | Step B2 |
| 8108 | Frontend Vulkan instance created (the settings' device list) | 3642 | `EDEN_NX2_VULKAN_INSTANCE_ONCE` (step B1) |
| 12597 | Renderer Vulkan instance created | 3664 | `EDEN_NX2_VULKAN_INSTANCE_ONCE`, `EDEN_NX2_BOOT_ASYNC` (step B1) |
| 14201 | Shader cache log and driver cache read | 1790 | `EDEN_NX2_CACHE_MMAP`, `EDEN_NX2_BOOT_ASYNC` (step B3) |
| 20053 | Shader cache load done: the boot builds (6007 pipelines) | 5852 | `EDEN_NX2_BOOT_ASYNC` for graphics; compute still waits (step B4) |
| 20718 | RomFS patch done | 265 | Small |
| 24746 | First guest frame | 4028 | Step B5 |

What the table does not say, and where it comes from (code reading unless marked):

- The 4.08 s before logging is the startup Vulkan check (`lichii/startup_checks.cpp`): on Windows it
  starts a child copy of `guarana.exe` that creates its own Vulkan instance, and waits for it, unless
  `vulkan_check.txt` matches the driver key and the last session ended cleanly. A session that was
  killed (a crash, a lab script, `taskkill`) makes the next start run it again: about one more
  instance creation. Its Debug setting could not turn it off before step B2: `perform_vulkan_check`
  was read before the configuration loaded.
- A direct launch (`guarana.exe -g <game>`, a shortcut, the launcher, the bench) does not list the
  Vulkan devices at startup; #46 was a launch from the game list, which does. So the frontend's
  3.6 s instance is a game-list launch's only, and on a direct launch `EDEN_NX2_VULKAN_INSTANCE_ONCE`
  can only overlap the renderer's instance with the kernel and process set-up.
- Between the end of the load and the first frame the frontend runs nothing in sequence: the guest
  starts, opens its RomFS (the patch marks), reads its assets, and every pipeline its first frames
  need that the boot did not build is built then (section 4).
- `vkCreateInstance` takes about 3.6 s on this driver each time. [warm-up.md](warm-up.md) says the
  Intel driver reads its own shader cache (about 450 MB) at instance creation, so the time grows
  with that cache. A cold driver cache makes the instance faster: the suite records whether
  the driver's cache folder was cleared.
- Issue [#12](https://github.com/karimalsalah/guarana/issues/12)'s "gap of about 7 s before the
  RomFS patch" has the length of the two shader phases above (1.8 + 5.9 s), and the RomFS patch is
  reached only from the game's first RomFS open, after the shader load and the guest release. So
  the gap is the shader load, not the container: an inference from the marks and the code order,
  not a measurement. Parsing a NUP once per session came with #3.
- A second run of the same boot with a different build (warm-up.md, "Measuring boot and warm-up":
  guest released at 12.26 s, first frame at 19.56 s, instance 4.15 s) is not added to this table: its
  zero and its build differ.

## 2. What exists, and what it costs

Every boot and shader switch on `main`. None has a verified-scene row yet ("measured" says what
there is instead).

| Switch | What it takes off | What it costs or risks | Measured |
|---|---|---|---|
| `EDEN_NX2_VULKAN_INSTANCE_ONCE` | The second `vkCreateInstance` on a list launch: 3.6 s (#46). On a direct launch only the overlap with kernel and process set-up, 0.5 to 1 s (estimate, warm-up.md) | An instance made on another thread must match the renderer's window type and validation setting, else a second one is made as before | No |
| `EDEN_NX2_BOOT_ASYNC` | The graphics boot builds (most of the 5.85 s) move behind the first frame; the driver cache read overlaps the log read; the instance is made early | Draws that need a queued boot build wait for that build; the shader workers compete with the guest for the first seconds; compute programs still block | No |
| `EDEN_NX2_CACHE_MMAP` | The copy of the whole log into memory, and one-thread decoding (part of the 1.79 s; the share is not known) | A compaction by another process can fail while the log is mapped (retried at a later boot) | No |
| `EDEN_NX2_CACHE_SAFE` | Intel's driver crash when the cache is read during builds (IGCIT #1614) | A boot deadlock, not reproduced; opt-in until fixed | No |
| `EDEN_NX2_PIPELINE_BINARIES` | AMD: pipelines rebuilt every boot because the driver's cache drops some ([Lichii #1406](https://github.com/zortos293/Lichii/pull/1406): 867-876 s to 17-30 s from the second boot, RX 6650 XT) | The store's size (up to 4 GiB); Intel's driver does not offer the extension | Upstream only |
| `EDEN_NX2_GPL` (with `_FAST_LINK`, `_OPTIMIZE`, `_OPTIMIZE_THREADS`) | A new pipeline whose two shader stages were built before is a fast link, a fraction of a millisecond, instead of a 400-1000 ms build ([switches.md](switches.md)) | A new stage still costs a library build; fast-linked code runs until the optimized link replaces it; each stage gets its own descriptor set | No hitch rows. On by default for Ocarina of Time on Intel (the game's switch defaults, from the play configuration: "no failures, image unchanged") |
| `EDEN_NX2_GPL_OPTIMIZE_CACHED` | Pipelines loaded from the cache stay fast-linked all session ([#103](https://github.com/karimalsalah/guarana/issues/103)); this queues their optimized link | More background links after boot | No |
| `EDEN_NX2_UNOPT_FIRST` | A library's first build without driver optimization (Intel), the optimized one later ([#13](https://github.com/karimalsalah/guarana/issues/13)) | Every library is built twice. Not known whether the unoptimized build is cheaper on this driver at all (its plan's step 1) | No |
| `EDEN_NX2_GPL_PARALLEL_PARTS` | The two libraries of a pipeline built at once instead of one after the other (#136) | Needs a free driver worker | No |
| `EDEN_NX2_WAIT_BOOST` | A build a draw waits for runs at normal priority instead of below normal | None known | No |
| `EDEN_NX2_SHADER_REUSE` | Translation of stages already translated with the same inputs ([Lichii #1378](https://github.com/zortos293/Lichii/pull/1378); [Eglantine #825](https://github.com/Eglantine-Community/Eglantine/pull/825) counts 64% of a cold boot's stages as repeats) | A verify mode exists; never on AMD's Windows driver | No |
| `EDEN_NX2_ASYNC_TRANSLATE` | Translation on the GPU thread for a skippable native draw ([Lichii #1379](https://github.com/zortos293/Lichii/pull/1379)) | Needs `native_async_draws` Bounded or Skip; the draw is skipped past `native_pipeline_wait_ms` | No |
| `EDEN_NX2_PREBUILD_NO_INLINE` | A draw that finds its cached pipeline still queued builds it inline on the GPU thread (#103 finding 2); this skips the draw instead | The draw is missing until a worker built it | No |
| Settings `use_asynchronous_shaders` (default off), `native_async_draws` (default Wait; Bounded, Skip), `native_pipeline_wait_ms` (8), `EDEN_NX2_ASYNC_DISCARD` | Bounded: [Lichii #465](https://github.com/zortos293/Lichii/pull/465), draws held 14.7 s to 0.2 s per 10 s (Ocarina of Time, RX 6700 XT); Skip: [Lichii #224](https://github.com/zortos293/Lichii/pull/224), worst hold 816 ms to 0-8.7 ms (RTX 4070 SUPER) | A skipped draw is missing for a frame or more ("a flash", #465); a draw made once is lost for good | Upstream only |
| `guarana-cli --warm-pipelines`, the precache, the background prebuild, `UseOrder` | Driver compiles before play, from the title's records: 4515 pipelines and programs in 155 s on the Arc 140T ([warm-up.md](warm-up.md)); boot builds in first-use order | Nothing for a first run: there are no records | Warm-up measured; no hitch rows |

Upstream work we do not carry yet, from a survey of every Lichii and Eglantine pull request
(open, merged and closed) that touches these files:

| Pull request | What it does | Measured upstream | Use here |
|---|---|---|---|
| [Lichii #1838](https://github.com/zortos293/Lichii/pull/1838) (open) | Saves the driver pipeline cache at the end of the load on Intel's Windows driver again; today it is written only when the session ends | Arc B390: without it the cache is written at 270-721 s, after the session; the save takes 1-2 s | Step B6 |
| [Eglantine #874](https://github.com/Eglantine-Community/Eglantine/pull/874) (merged) | Skips depth-tested native draws into targets the game redraws while their pipeline builds | Mario Kart World, RX 6700S: 20-60 stalls of 50-330 ms per race before | Step F4 |
| [Lichii #1169](https://github.com/zortos293/Lichii/pull/1169) (closed) | `EDEN_NX2_HEAVY_SKIP`: skips draws of pipelines over 4,000 instructions while they build | RTX 4090, empty caches: p99 297 to 42 ms | Step F4, after E#874 |
| [Lichii #1719](https://github.com/zortos293/Lichii/pull/1719) (open) | Graphics warm-up from the game's files (#1183 rebased) | RX 6650 XT: 582 s of warm-up; library driver time 220.4 s to 0.32 s | Step F6 |
| [Eglantine #825](https://github.com/Eglantine-Community/Eglantine/pull/825), [Lichii #1815](https://github.com/zortos293/Lichii/pull/1815) | Native translation on workers, wait 2 ms then skip | Ocarina of Time cold boot: GPU-thread pipeline time 3.4 s to 0.9 s (E#825) | Compare with our #1379 carry first |
| [Lichii #1366](https://github.com/zortos293/Lichii/pull/1366) (draft) | Precompiled pipelines from the player's own play | RADV only; about 26 min cold | Not ported: revisit when merged |

[giangero1/Fritchii-ZeroStutter](https://github.com/giangero1/Fritchii-ZeroStutter) changes one shader
commit over Fritchii v5.8.4 (compute helpers not inlined, guessed values for incomplete graphics
records); its shipped answer to freezes is a pre-built cache download for one GPU and driver, and
its launcher turns graphics pipeline libraries off and skips draws. Nothing there is worth porting
as a default; the guessed values for incomplete records may be worth a switch once we count how many
Ocarina of Time records are dropped as incomplete.

## 3. First play and second play

A true first play has no records: nothing can be built ahead from the player's own cache, and the
driver's cache is empty too. Only these help it:

- graphics pipeline libraries, so a new pipeline of known stages is a link (on for Ocarina of Time
  on Intel already);
- building what a draw waits for sooner: libraries in parallel, the waited-for build at normal
  priority, translation off the GPU thread;
- a draw policy that does not stop the frame for a build, with an image rule (below);
- building ahead from the game's own files (step F6), which costs minutes, so it is a background
  task the player chooses, not part of a boot.

From the second play on, the records exist: the boot builds what the last sessions used in the
order they first used it, the rest is prebuilt in the background, and the steps B1-B7 shorten the
boot.

## 4. What the image does when a pipeline is not ready

Today's defaults: native draws Wait. Asynchronous shaders are off in code and seeded on in a new
configuration, but they only let a draw be skipped when it is not native, and every Switch 2
channel runs native, so for a Switch 2 game a draw whose pipeline is not built stops its frame
until the build ends: 400-1000 ms for a full build on Intel, a fraction of a millisecond for a fast
link of two built libraries. Bounded and Skip (`native_async_draws`) apply to native draws only
when depth is on.

| Case | Default | Bounded or Skip (`native_async_draws`), asynchronous shaders on | Image meanwhile |
|---|---|---|---|
| New pipeline, both libraries built (`EDEN_NX2_GPL`) | Fast link on the spot | Same | Correct; fast-linked code until the optimized link replaces it |
| New pipeline, a library to build | The frame waits for the build | Bounded: waits up to what is left of 8 ms this frame, then skips; Skip: skips at once | Skipped: the object is missing until it is built, one frame or more |
| Cached pipeline still queued in the background prebuild | The queued job is cancelled and the pipeline built from the live state, translated on the GPU thread; the draw waits. A job already running is waited for | Same; `EDEN_NX2_PREBUILD_NO_INLINE` skips the draw and raises the job instead | As above |
| New compute pipeline | The dispatch waits | Waits: a dispatch is never skipped | Correct, late |

No path draws with a substitute pipeline (code reading): a draw waits for its own pipeline or is
dropped. That is the rule a skip must respect: a draw into a target the game draws again next frame (scene geometry with depth) may be
skipped, it reappears once built; a draw that is made once (a menu, a loading screen's text, a
target that is written once and read later) must wait, because a skipped one is lost for the
session. Eglantine #874's test (depth-tested draws into targets that are redrawn) is that rule, so
it comes first; a heavier rule (#1169's instruction count) comes after it, measured.

## 5. Steps

Each is one pull request with its switch and row; "Saves" is the expected effect and its source.

| Step | What | Saves | Switch | Status |
|---|---|---|---|---|
| M1 | `tools/bench` boot matrix: cold, second and warm caches, the baseline against each switch, one command | Makes the rest measurable | none | Merged ([#222](https://github.com/karimalsalah/guarana/pull/222)) |
| B1 | One Vulkan instance per boot | 3.6 s on a list launch (#46); 0.5-1 s direct (estimate) | `EDEN_NX2_VULKAN_INSTANCE_ONCE` (exists) | Measure (M1) |
| B2 | The startup Vulkan check: its Debug setting now takes effect (it was read before the configuration loaded), and `EDEN_NX2_STARTUP_CHECK_ASYNC` runs its child while the frontend starts, waiting for the verdict only before the first use of Vulkan. The check stays on by default: after an unclean exit or a driver change it is what survives a crashing driver | The whole check (4.1 s in #46) for a player who turns it off; with the switch, the frontend's start overlaps it, under 1 s (estimate) | `EDEN_NX2_STARTUP_CHECK_ASYNC`, the existing setting | This pull request |
| B3 | Map the log, decode in parallel | Part of 1.79 s (#46) | `EDEN_NX2_CACHE_MMAP` (exists) | Measure (M1) |
| B4 | Boot builds behind the first frame, compute included | Up to 5.85 s (#46), less the builds the first frames wait for | `EDEN_NX2_BOOT_ASYNC` (exists); compute: new | Measure graphics; compute is the next code step |
| B5 | The 4.0 s from the load to the first frame: the guest's start plus the pipelines its first frames build | Not known: the suite's `at_first_frame` (held draws and builds at the first frame) says how much is pipelines | none | Measure (M1) |
| B6 | Save the driver pipeline cache at the end of the load on Intel (port of Lichii #1838) | What the boot load built survives a crash or a kill later in the session, so the next boot's boot builds hit the driver cache. Pipelines built in play still reach the file only at exit, and a first play builds nothing at load. Costs 1-2 s at load end (#1838) | `EDEN_NX2_LOAD_SAVE` | [#224](https://github.com/karimalsalah/guarana/pull/224) |
| B7 | Persisted SPIR-V ([X2](../ROADMAP.md#x2-persisted-spir-v)) | Translation of every cached pipeline at each boot; the share of the 5.85 s is not known (measure with `EDEN_NX2_PIPELINE_BUILD_TRACE` against the boot builds' span) | New | After B4 |
| F1 | First-play rows: GPL on and off, async shaders, Bounded, Skip | The first-play numbers this page lacks | Settings, `EDEN_NX2_GPL=0` | Measure (M1) |
| F2 | Library builds in parallel, waited-for builds raised, unoptimized first | Shorter stalls, not fewer | `EDEN_NX2_GPL_PARALLEL_PARTS`, `EDEN_NX2_WAIT_BOOST`, `EDEN_NX2_UNOPT_FIRST` (exist) | Measure (M1) |
| F3 | Translation off the GPU thread, translated stages reused | Up to 3.4 s to 0.9 s of GPU-thread time on a cold Ocarina of Time boot (E#825, NVIDIA) | `EDEN_NX2_ASYNC_TRANSLATE`, `EDEN_NX2_SHADER_REUSE` (exist) | Measure (M1) |
| F4 | Skip what may be skipped while it builds: port of Eglantine #874, a native draw into a target the game redraws every frame (then Lichii #1169) | Stalls of 50-330 ms on new pipelines (E#874, RX 6700S) | `EDEN_NX2_SKIP_REDRAWN` (#874's part) | This pull request |
| F5 | Build ahead from the player's own records: queue the pipelines an earlier session first used right after the one just drawn ([A5](../ROADMAP.md#a5-shader-need-prediction)) | Builds a later area needs before it is reached, in session 2 on; not known | New | After M1 rows |
| F6 | Build ahead from the game's files, as a background task the player starts (port of Lichii #1719) | Library driver time 220.4 s to 0.32 s after a 582 s warm-up (#1719, RX 6650 XT) | New, setting Ask/Never | Later |

**The boot target.** #46's 24.7 s is a start from the game list on which the startup check ran (the
last session did not end cleanly, or the driver had changed): it pays the startup check (4.1 s,
B2) and the device list's instance (3.6 s, B1). A direct start after a
clean exit, the kind the suite measures and the launcher makes, pays neither, so it starts near
24.7 - 7.7 = 17 s (an estimate from the same trace, not a measurement). From there, 10 s needs most
of B4 (5.9 s of boot builds), part of B3 (1.8 s of cache reads) and some of B5; the renderer's own
instance (3.7 s) stays unless `EDEN_NX2_BOOT_ASYNC`'s early instance overlaps it. All of this is an
estimate, to be replaced by the suite's rows; a game-list start after a crash has the B1 and B2
costs on top, and a first start ever pays the check (it always runs then).

## 6. Measure

`tools/bench/suites/console-boot.json` (step M1) runs, in one command on the owner's machine, every
variant with a cold, a second and a warm cache, three times, alternating the order, each with
`EDEN_NX2_STUTTER_TRACE=1`:

- **cold**: no emulator cache for the title; with `--driver-cache-dir`, no driver cache either;
- **second**: what the same variant's cold run left: the second play of a game;
- **warm**: a copy of the owner's own cache as the suite found it.

Each row has the first frame and the game-shown time since the process started, the boot phases,
the held draws and pipeline builds from the first frame to the scene (`boot_timing` `split`), the
`Nx2Stutter` frames and their causes from the log, and fps in Hyrule Field. A row is invalid when its
cache state is not what it claims (a cold run that loaded records, a boot the crash guard cut short,
a second run whose cold run did not save its driver cache). How to run it, and what each variant
tests: [tools/bench/README.md](../tools/bench/README.md#boot-suites).

A switch becomes a default only in a later pull request that cites valid rows from the Arc 140T and
one other device, every repeat showing the gain, with the image checked.
