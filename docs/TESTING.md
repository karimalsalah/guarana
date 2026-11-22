# Testing Guarana: the 10-minute protocol

You do not need to code, run a debugger or read a log. Ten minutes of play and one form give the
project what it cannot get any other way: how Guarana runs on **your** device. Every GPU except one
laptop's Arc 140T is untested, so a single honest report moves the project forward.

What you need: Windows 11 x64, Guarana, and your own copy of the game, dumped from hardware you own.
Guarana never provides games, keys or firmware, and nobody here can help you get them.

If you would rather measure with scripts, use [tools/bench](../tools/bench/CONTRIBUTING-bench.md). This
page is the path that needs nothing but the game and a browser.

## Before you start (2 minutes)

1. **Install.** From v20.1, run **Guarana Setup** from [Releases](https://github.com/karimalsalah/guarana/releases/latest):
   one click, no admin rights. Before v20.1, unzip the release and run `lichii.exe`; the report works the
   same way.
2. **Plug in** the charger and close browsers, game launchers and overlays. A report on battery is welcome
   too, but say so (the form asks for the power source).
3. **Leave the settings alone.** Use the defaults the launcher picked (mode **Auto**). Do not set any
   `EDEN_NX2_*` switch. If you changed something earlier, undo it for this test, or list it in the report.
4. Note your **driver version** in full. The launcher fills it in for you; by hand it is in Device Manager
   or your GPU vendor's control panel.

## The scene (5 minutes)

The test scene is **Hyrule Field in The Legend of Zelda: Ocarina of Time**, the one scene every report
is compared on.

1. Start the game from the launcher (or the emulator) and load a save that stands in Hyrule Field. Any
   save works, but one made in the field itself, in daylight, is easiest to compare. Use the same save
   every time you test.
2. Stand still. No camera movement, no enemies, nothing on screen but the field and the HUD.
3. Let it settle for 30 seconds. The first visit hitches while shaders compile; that is real and it is
   not the number we want.
4. Stay for **one more minute**. The longer the stand, the better the numbers.
5. Take **one screenshot** of the scene (Windows: Win+Shift+S). Reports without a screenshot cannot be
   checked.

Different game or scene? Fine: say so in the report. Numbers from other games help too.

## What to send (3 minutes)

### Easiest: the launcher (v20.1)

After the game, if you played at least a minute, **Home** shows a card: *"Your session: 25 game fps, 93%
of frames on time. Share it with the community?"* Press **Share**.

- Your browser opens GitHub's device report with the device, CPU, driver, RAM, OS, build, mode,
  profile and numbers already filled in. Nothing is sent until you press **Submit new issue**.
- Fill in the **Scene** ("Hyrule Field, standing still, day"), the choices the launcher leaves to you
  (form factor, graphics card, game, power source) and attach the **screenshot**.
- Your user name, folder paths and PC name are removed before the form opens. Read it anyway.
- If the form would be too long for a link, the launcher copies the text to your clipboard and opens the
  empty form: paste the text in.
- Not interested? **Not now** closes the card, **Don't ask again** stops it (Home > Advanced turns it
  back on).

The numbers on the card are read from the emulator's own log: *game fps* counts frames the game renders
(generated frames never count), and *on time* is the share of shown frames within 2 ms of the median
interval, a measure of how even the pacing was. They are good for comparing devices, but they are not a
`tools/bench` row, and the form says so.

### By hand

Open the [device report form](https://github.com/karimalsalah/guarana/issues/new?template=device-report.yml)
and fill in:

| Field | What to write |
|---|---|
| Device | Maker and model: "ASUS ROG Ally X", "a desktop" |
| CPU, graphics card, driver, RAM, OS | The exact names; the driver in full (for example 32.0.101.9175) |
| Power source and mode | Plugged in or battery; Windows power mode; the TDP if you set one |
| Build | Help > About in the emulator, or the version on the launcher's setup card |
| Scene | "Hyrule Field, standing still, daylight, docked mode, fullscreen 1080p" |
| Results | Game fps and how you got it (see below), plus how it *felt*: smooth, stuttering, crashing |
| Screenshot | The scene you measured |

**Getting an fps number without tools.** The launcher's *Share my numbers* card is the way. Without it, write
what you saw and say how you counted: a feeling alone ("smooth", "stutters in the grass") still helps,
but say if the number is the game's own rate or a rate with frame generation on, because only the game's own
rate counts as game fps.

**"It does not run" is a result.** Send it with the log: Report a problem in the launcher makes a zip with
your user name and paths removed. A crash or a black screen on a GPU nobody has tried is exactly what we
are looking for.

## After you submit

- A bot replies within minutes with the row it parsed, or what to fix. Answer it; that is the whole job.
- A bot parses your report and shows the row. A report like this goes on the community list on the
  [benchmarks page](https://karimalsalah.github.io/guarana/benchmarks.html#community).
- The **records board** ranks only `tools/bench` runs whose scene check passed ("valid, `scene_ok`"), because
  they are measured the same way on every machine. A 10-minute report cannot set a record. If you want one, run
  [tools/bench](../tools/bench/CONTRIBUTING-bench.md) (about an hour, mostly unattended) and put your GitHub
  handle in "Credit me as"; leave it empty to stay anonymous.
- Credit: reports that are used are named in the release notes by the maintainers. Merged work (code, tests,
  ports) goes on the [Hall of fame](https://karimalsalah.github.io/guarana/legacy.html) and in the launcher's
  Credits screen automatically; the hall of fame is built from the git history, so a report alone does not
  appear there.
- Want to do more? Run the same scene with another power mode, or after a driver update, and send a
  second report. Two reports from one device in two power modes are worth more than two devices.

## The debug-free path, in short

Install, plug in, leave defaults, stand in Hyrule Field for a minute and a half, screenshot, press Share
(or fill the form), press Submit. No flags, no environment variables, no log files, no debug server.

## What we do not want

- Keys, firmware, game files, saves, shader caches or logs with your name or paths in them.
- Requests for games or system files. Nobody here can help, and such comments are removed.
- Numbers from a modified build without saying so, or generated-frame rates passed off as game fps.

Questions while testing: the [Discord](https://discord.gg/urz7vMdRM). Bugs and reports stay on GitHub so
they can be tracked. How a report becomes a fix: [FEEDBACK.md](FEEDBACK.md). Supported and untested
devices: [DEVICES.md](DEVICES.md).
