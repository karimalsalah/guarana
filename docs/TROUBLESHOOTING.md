# Troubleshooting

Find your symptom below. Each entry says what you see, why it happens and what to do. If nothing
matches, [report it](#reporting-a-problem). Upstream's handbook covers more general cases:
[docs/user/Troubleshoot.md](user/Troubleshoot.md).

| You see | Go to |
|---|---|
| The program closes at once, shows nothing, or reports a Vulkan error | [It closes at once or does not start](#it-closes-at-once-or-does-not-start) |
| A message about missing keys or firmware | [Keys or firmware missing](#keys-or-firmware-missing) |
| A short freeze the first time you enter an area | [Hitches on first visits](#hitches-on-first-visits) |
| A crash while shaders are being built, on Intel | [Crash while shaders build on Intel](#crash-while-shaders-build-on-intel) |
| Slightly uneven motion with frame generation on | [Uneven motion with frame generation](#uneven-motion-with-frame-generation) |
| Wrong colours, smearing or ghosting in generated frames | [Artifacts in generated frames](#artifacts-in-generated-frames) |
| Your `nx2_switches.txt` lines seem to do nothing | [The play config does not apply](#the-play-config-does-not-apply) |
| CPU boost stayed off after the emulator crashed | [Power settings after a crash](#power-settings-after-a-crash) |
| Missing graphics or "out of host memory" on a 16 GB machine | [Out of memory on 16 GB](#out-of-memory-on-16-gb) |
| Lower frame rate than you expected | [Low frame rate](#low-frame-rate) |
| A game other than Ocarina of Time draws wrong | [A game renders wrong](#a-game-renders-wrong) |

## First: find the log

Open **File → Open Guarana Folders → Log Folder**. The current session writes `eden_log.txt`; the
session before it is kept as `eden_log.txt.old.txt`. The first line names the build. On Windows the
folder is `%APPDATA%\guarana\log`, or `user\log` next to
`guarana.exe` in a portable install. The log contains paths from your machine: read it before you
share it.

## It closes at once or does not start

- **CPU without AVX2.** Release builds use `x86-64-v3` code, which needs AVX2. On an older CPU the
  program exits at once, often before it writes a log; Windows Event Viewer (**Windows Logs →
  Application**) then shows exception code `0xc000001d`, an illegal instruction. Use a CPU with
  AVX2, or build from source without `x86-64-v3` ([docs/Build.md](Build.md)).
- **Missing Visual C++ runtime, or damaged system files.** Follow upstream's
  [fails to launch](user/Troubleshoot.md#eden-fails-to-launch-and-does-not-leave-any-logs) guide.
- **Started from inside the zip.** Extract the whole zip into a folder you can write to, then run
  `guarana.exe` from there.
- **Vulkan initialization error.** Update your GPU driver; see upstream's
  [Vulkan initialization error](user/Troubleshoot.md#vulkan-initialization-error) steps.

## Keys or firmware missing

Install your own keys with **Tools → Install Decryption Keys** and your firmware with
**Tools → Install Firmware** (**From Folder** or **From ZIP**). Both must be dumped from a console
you own. The project cannot help you obtain them, and requests for them are closed.

## Hitches on first visits

**Expected, for now.** The first time an area needs a shader, the emulator translates it and the
GPU driver compiles a pipeline: 0.4-1 s per pipeline on Intel's driver. Compiled pipelines are kept,
so later visits do not compile them again.

- Leave **Use persistent pipeline cache** on, and do not delete the shader cache unless a maintainer
  asks you to.
- To confirm a hitch is a shader build, add `EDEN_NX2_STUTTER_TRACE=1` to your `nx2_switches.txt`:
  the log then gets an `Nx2Stutter` line for each slow frame, naming its top cause (for example a
  pipeline built on the GPU thread). Remove the line when you are done.
- Fewer and shorter hitches are on the [roadmap](../ROADMAP.md#next).

## Crash while shaders build on Intel

**Known driver issue, fix in progress.** Intel's Vulkan driver can crash when the pipeline cache is
saved while other threads are still building pipelines into it
([IGCIT #1614](https://github.com/IGCIT/Intel-GPU-Community-Issue-Tracker-IGCIT/issues/1614)).

- The reporter of that driver issue says that turning off **Use Vulkan pipeline cache** (advanced
  graphics settings) avoids the crash. We have not verified this, and it may make shader loading
  slower. Try it only if the crash keeps coming back, and turn it on again once the fix ships
  ([roadmap N2](../ROADMAP.md#n2-intel-driver-crash-when-the-pipeline-cache-is-saved-during-builds)).
- When you report a crash, include the `Host crash:` line from the log (exception code, thread and
  stack) and your driver version. A crash usually also leaves `eden_crash.dmp` next to the log: do
  not post it publicly, because a memory dump can contain data from the game and your system. Keep
  it until a maintainer asks for it.

## Uneven motion with frame generation

**Known issue, fix in progress.** Generated frames are spaced across the time between two real
frames. At the 26.7-28 game fps v20 reads in Ocarina of Time, that time is more than two 60 Hz
refreshes and less than three, so shown frames cannot all stay up equally long: a slight
micro-stutter. A presenter locked to the display's refresh is in progress (`EDEN_NX2_FLUID=1`,
experimental); see [roadmap N1](../ROADMAP.md#n1-vblank-locked-presenter-for-generated-frames).

- To tell it apart from other stutter, turn **Enable Frame Generation** off. If motion is then even
  (at the game's own rate), you are seeing generated-frame pacing.
- With HDR output, predicted frames are paused, because the prediction pass writes SDR.
- Reports with your display's refresh rate and your frame generation settings help.

## Artifacts in generated frames

- **Red and blue swapped.** Fixed in v20: the prediction pass no longer declares an output format
  that swapped the channels on some swapchains. If you still see it on v20 or later, report it with
  your GPU and driver version.
- **Smearing or ghosting at edges.** A predicted frame is an estimate. Where movement uncovers
  something the real frame did not show, the pass has to guess, and fast motion makes that visible.
- **The HUD smears or moves.** The HUD should stay where the game drew it. If it does not, report it
  with a screenshot.

## The play config does not apply

1. The file must be `%APPDATA%\guarana\nx2_switches.txt`. Windows can hide extensions: make sure it
   is not `nx2_switches.txt.txt`.
2. One `NAME=VALUE` per line; lines starting with `#` are ignored. Restart the emulator after every
   edit.
3. A variable already set in your environment wins over the file.
4. **Enable Frame Generation** must be on for `EDEN_NX2_EXTRAPOLATE=1` to do anything, and it only
   applies to Ocarina of Time.
5. Check the log for these lines:

| Switch | Log line when it is active |
|---|---|
| `EDEN_NX2_GM=11` | `EDEN_NX2_GM=11: global memory routing bits 11` |
| `EDEN_NX2_POWER_POLICY=igpu` | `Power policy igpu: scheme ... changed`. `... is not an integrated GPU, nothing is changed` means it did not apply to your GPU |
| `EDEN_NX2_EXTRAPOLATE=1` | `Frame extrapolation: 600 frames, ...`, repeated while frames are predicted |

## Power settings after a crash

`EDEN_NX2_POWER_POLICY=igpu` turns CPU boost off and lowers the minimum processor state in your
active power plan while a game runs, and writes your saved values back when the game stops or the
emulator exits. If the emulator crashed, **start it once and close it**: the start restores the saved
values (the log says `Power policy: restored scheme ... after an unclean exit`). The values wait in
`nx2_power_restore.txt` in the emulator's config folder until then, so do not delete that file.

## Out of memory on 16 GB

16 GB of shared memory is below the comfortable floor: Switch 2 titles use a 12 GiB guest memory
layout, and an integrated GPU takes its memory from the same RAM. Symptoms are missing graphics,
pipelines that fail with `VK_ERROR_OUT_OF_HOST_MEMORY`, or a crash during the first shader builds.

- Enlarge the page file:
  [Shaders fail with "out of host memory"](user/Troubleshoot.md#shaders-fail-with-out-of-host-memory-on-windows).
- Close browsers, launchers and overlays while you play.
- A [device report](DEVICES.md#send-a-device-report) from a 16 GB machine helps us decide what to do
  about it.

## Low frame rate

- **Plug in.** On battery, laptops and handhelds usually run at a lower power limit.
- **Close heavy background programs.** An integrated GPU shares memory and power with everything
  else. `python -I tools/bench/play.py --exe <guarana.exe> --game <game> --dry-run` lists the
  processes using the most CPU and GPU memory, and changes nothing.
- **Name your driver.** The published numbers use Intel driver 32.0.101.9175; other versions may
  behave differently.
- **Compare like with like.** Published figures are game fps in one scene on an Arc 140T. Generated
  frames are not counted, and other scenes are heavier or lighter.
- **Send numbers, not impressions.** [BENCHMARKING.md](BENCHMARKING.md) shows how to measure so your
  result can be compared with others.

## A game renders wrong

1. If `EDEN_NX2_GM=11` is in your switches, remove it and restart. It has only been measured in
   Ocarina of Time; if removing it fixes the game, report that.
2. Check whether upstream already knows the problem:
   [Lichii issues](https://github.com/zortos293/Lichii/issues). On AMD, also read
   [docs/nx2/amd.md](nx2/amd.md).
3. Otherwise, [report it](#reporting-a-problem).

## Reporting a problem

Open an issue from the [templates](https://github.com/karimalsalah/guarana/issues/new/choose) and give:

- the build (first line of the log), device, CPU, GPU, driver version and RAM;
- your switches (`nx2_switches.txt`) and the settings you changed, frame generation included;
- the steps that lead to the problem, and whether it happens every time;
- the log of the affected session, after reading it for personal information, and a screenshot of
  the problem.

Never attach keys, firmware or game files. Questions that are not bugs go to
[Discussions](https://github.com/karimalsalah/guarana/discussions).
