# FAQ

- [The project](#the-project)
- [Performance and frame generation](#performance-and-frame-generation)
- [Playing on a PC, laptop or handheld](#playing-on-a-pc-laptop-or-handheld)
- [Install and data](#install-and-data)
- [Contributing](#contributing)

## The project

### What is Guarana?

An open-source Nintendo Switch 2 emulator tuned for integrated GPUs: Intel Arc iGPUs first, AMD
Radeon iGPU handhelds next. It is a fork of [Lichii](https://github.com/zortos293/Lichii) that
focuses on even frame pacing, frame generation, fast boot, and a contributor harness that humans and
AI agents can both use.

### How is it related to Lichii, Eglantine, Eden and yuzu?

Guarana forks Lichii's `nx2` branch. Lichii descends from Eglantine, which descends from Eden, which
descends from yuzu. Guarana keeps their copyright notices and license terms (the project as a whole
is GPL-3.0-or-later), tracks Lichii, and merges upstream fixes, sometimes before upstream does. It is an independent
project: not affiliated with Lichii or with Nintendo. Credits are in [CREDITS.md](../CREDITS.md).

### Is this legal?

Guarana ships no keys, firmware, games or system files, and the project does not accept them
anywhere: not in issues, pull requests or discussions. You must use your own, dumped from hardware
you own. Laws differ between countries, and nothing here is legal advice. The project's full
position is in [LEGAL.md](../LEGAL.md).

### Where do I get keys, firmware and games?

From a console and games you own, using dumping tools for your own hardware. Nobody here will help
you find them anywhere else, and such requests are closed without discussion.

### Which games work?

*The Legend of Zelda: Ocarina of Time* is the main test case and the only game with published
numbers. Other games run on the same emulation core as upstream Lichii, but how they run on an
integrated GPU is unknown. Try yours and [send a report](DEVICES.md#send-a-device-report).

### Is there an Android, Linux or macOS build?

v20 release builds are Windows x64 only. The source tree comes with upstream's other frontends and
build files ([docs/Build.md](Build.md)), but nothing outside Windows is tested here, and other
platforms are not on the [roadmap](../ROADMAP.md) yet.

### Does it work on NVIDIA or desktop AMD graphics cards?

Discrete GPUs are not this fork's target and are not tested here. Reports are still welcome.

## Performance and frame generation

### What does "game fps" mean?

The number of frames the game itself renders per second. Generated frames are not included. Every
fps figure Guarana publishes is game fps, because that is what the game's speed and input response
follow. No figure for frames shown per second is published yet.

### Does frame generation make the game faster?

No. It makes motion look smoother by showing extra frames between the game's own frames, so a 60 Hz
display gets a new image on every refresh. The game still runs, and reads your input, at its own
frame rate. [How it works](../README.md#how-the-frame-pacing-works).

### Does frame generation add input lag?

Interpolation, the method most frame generators use, has to hold each real frame back until the next
one exists. The extrapolation Guarana uses in Ocarina of Time (`EDEN_NX2_EXTRAPOLATE=1`) predicts
forward from the newest real frame instead, so it shows each real frame as soon as it is ready.
Neither method makes the game react faster. No latency measurement has been published yet.

### Why does it hitch the first time I enter an area?

The GPU driver is compiling the shaders that area needs: 0.4-1 s per pipeline on Intel's driver.
Compiled pipelines are kept in the caches, so later visits do not compile them again. Fewer and
shorter hitches are on the [roadmap](../ROADMAP.md#next).

### Why does motion stutter slightly with frame generation on?

Known issue. Generated frames are spaced across the time between two real frames, and at the
26.7-28 game fps v20 reads in Ocarina of Time that time is not a whole number of 60 Hz refreshes, so
shown frames cannot all stay up equally long. A presenter locked to the display's refresh is in
progress (`EDEN_NX2_FLUID=1`, experimental): see
[the roadmap](../ROADMAP.md#n1-vblank-locked-presenter-for-generated-frames).

### Which settings should I use?

On an Intel Arc iGPU, start with the play config in the [quick start](../README.md#get-it). On
anything else, nobody knows yet: measure with and without each switch
([BENCHMARKING.md](BENCHMARKING.md)) and share the result.

### Will it run on my device?

Only the Arc 140T has been tested. [docs/DEVICES.md](DEVICES.md) lists the devices we target, the
requirements, and third-party reports.

### I have 16 GB of RAM. Will it work?

It is below the comfortable floor: Switch 2 titles use a 12 GiB guest memory layout, and an
integrated GPU takes its memory from the same RAM. Close other programs and enlarge the page file
([guide](user/Troubleshoot.md#shaders-fail-with-out-of-host-memory-on-windows)). Reports from
16 GB devices help us decide what to do about it.

### What does the power policy change on my PC?

With `EDEN_NX2_POWER_POLICY=igpu` (Windows, integrated GPUs only), the emulator turns CPU boost off
and lowers the minimum processor state in your active power plan while a game runs, so more of the
shared power budget can go to the GPU. It saves your values first and writes them back when the game
stops or the emulator exits. If the emulator crashes, the next start puts them back. Without the
switch, nothing is changed. It is experimental, and its effect has not been measured yet.

## Playing on a PC, laptop or handheld

Short answers; each links the page with the detail and the numbers.

### How do I play the Switch 2 Ocarina of Time on a PC?

Download the Windows zip from [Releases](https://github.com/karimalsalah/guarana/releases/latest), extract it and run
`lichii.exe`. Install keys and firmware (*Tools*) and add the folder with your game, all dumped from hardware you own. For
this game the shader fix is on by default; turn on frame generation (multiplier 3, target 60). The steps:
[the 5-minute path](https://karimalsalah.github.io/guarana/play-ocarina-of-time-switch-2-on-pc.html).

### What is the best Switch 2 emulator?

There is no finished one. Guarana, Lichii, Eglantine and Eden are one family (Guarana forks Lichii), and all are
experimental for Switch 2. Guarana is the one tuned and measured for laptop and handheld integrated GPUs; on a large
discrete GPU we have no measurements. [Comparison](https://karimalsalah.github.io/guarana/play-ocarina-of-time-switch-2-on-pc.html#compare).

### Emulator or recompilation (for example Hookshot)?

An emulator runs the game as dumped and also other Switch 2 games. A recompilation turns one game's code into a native
Windows program, a separate project per game that we do not run or measure. Both need a dump from your own hardware.

### What speed can I expect on a laptop?

On the one tested laptop (Core Ultra 9 285H, Arc 140T), Hyrule Field went from 14.8 to 22.8 game fps with frame generation
off, from one shader fix. Other machines are untested: send a [device report](DEVICES.md#send-a-device-report).

### Why does it stutter, and what helps?

Two causes. Entering a new area compiles shaders once (0.4-1 s per pipeline on Intel). Uneven frame times are pacing: the
vblank-locked presenter put 100% of frames within 2 ms of the median on the title screen and 75% in Hyrule Field, against
33-54% before, and ships in the next release. A development build (v21) measured 19 stutters per 30 s in a scene that
had 37-92 on the old setup. One laptop, one scene. [Detail](https://karimalsalah.github.io/guarana/fix-stutter-switch-2-emulator.html).

### I get a black screen or a crash

Update the graphics driver, enlarge the page file, and read [Troubleshooting](TROUBLESHOOTING.md). On Intel there is a
known pipeline-cache driver crash with a fix in progress. Report with the `Host crash:` line of the log, never with keys,
firmware, game files or logs that show your user name.

### Does it run on a ROG Ally, Legion Go, MSI Claw or Steam Deck?

Not measured by us yet. Third-party reports exist for the ROG Xbox Ally X (about 30 fps) and the MSI Claw 8 AI+ (22-24 fps,
another build). Everything else is untested. [Handhelds](https://karimalsalah.github.io/guarana/switch-2-emulator-handheld.html).

### What about Intel Arc?

Arc is the first target. The Arc 140T is tested; the 140V, Meteor Lake and B390 are not, or have only third-party reports.
[Intel Arc](https://karimalsalah.github.io/guarana/switch-2-emulator-intel-arc.html).

### Which controllers work, and how do I set one up?

*Emulation → Configure… → Controls*, pick the pad, map it, save a profile. DualSense, Xbox, Switch Pro and 8BitDo pads have
no published test yet; the page lists what to try. [Controller setup](https://karimalsalah.github.io/guarana/controller-setup.html).

### The emulator says a key or firmware is missing

Install your own keys and firmware from *Tools → Install Decryption Keys* and *Tools → Install Firmware*, dumped from
hardware you own at the version your game needs. Nobody here will point you to downloads.

### Can I use Lossless Scaling?

Optionally. Guarana only loads a `Lossless.dll` that you supply from your own Lossless Scaling purchase and distributes
none of it. The built-in extrapolating frame generation needs no extra files.

## Install and data

### Where are my settings, saves and logs?

In `%APPDATA%\lichii`: v20 keeps the upstream folder name. The log is `log\eden_log.txt` in that
folder (**File → Open Lichii Folders → Log Folder**). A `user` folder next to `lichii.exe` makes the
install portable and is used instead.

### Can I use Guarana and upstream Lichii side by side?

Yes, but they share `%APPDATA%\lichii`: settings, saves, caches, and the switches in
`%APPDATA%\lichii\nx2_switches.txt` apply to both. Back the folder up before switching. A portable
`user` folder next to one of them keeps its settings, saves and caches apart; both still read the
switches in `%APPDATA%\lichii` ([roadmap L3](../ROADMAP.md#l3-portable-mode-out-of-the-box)).

### It closes as soon as I start it

Two common causes are a CPU without AVX2 and a missing Visual C++ runtime. See
[Troubleshooting](TROUBLESHOOTING.md#it-closes-at-once-or-does-not-start).

### How do I update?

Download the new release and extract it into a new folder; your data in `%APPDATA%\lichii` stays
where it is. Automatic updates are on the [roadmap](../ROADMAP.md#l4-auto-update).

## Contributing

### I'm not a programmer. How can I help?

Measure. Every device except the Arc 140T is untested, so a device report or a set of bench rows is
worth more right now than most code. Start with [BENCHMARKING.md](BENCHMARKING.md) or
[send a device report](DEVICES.md#send-a-device-report).

### Can I use AI tools to contribute?

Yes. Disclose them with an `Assisted-by: <tool>` trailer, stay the human who owns the pull request,
and back performance claims with measured rows. The rules are in [AI_POLICY.md](AI_POLICY.md).

### How do I get a performance change merged?

Bring a paired A/B from one session: baseline and change run back to back with `tools/bench` on the
same machine and scene, both rows valid with the scene check passing, as described in
[BENCHMARKING.md](BENCHMARKING.md#evidence-for-a-pull-request). A change that only looks faster, or
that was only reasoned about, is not enough.

### Where do I ask questions?

In the [Discord](https://discord.gg/urz7vMdRM) or in [Discussions](https://github.com/karimalsalah/guarana/discussions). Use
[issues](https://github.com/karimalsalah/guarana/issues/new/choose) for bugs and device reports.
