# User Handbook - Window, play mode and shortcuts

How Guarana's window behaves, what it shows while a game runs, and the keys and controller buttons that go with it. Everything here has a setting; the defaults are the ones a controller player wants.

## The window

- The title is `Guarana`, with the game's name added while it plays. Release builds show no build hash in the title; Help > About still shows the build. Development builds keep the long title.
- The window icon is Guarana's.
- Size and position are remembered for each monitor. Close Guarana on your TV, open it on your desk monitor, and each opens where you left it. The monitor you used last is used again when it is connected. Turn this off in Settings > General: **Remember window size and position per monitor**.
- A game started with `-f` (the launcher does this) opens in borderless fullscreen; `guarana -f -g <game>` from the command line does the same. Without `-f` a game opens in a window, as before. Settings > Graphics > **Fullscreen mode** picks borderless or exclusive; borderless is the default on Windows.

## While a game runs in fullscreen

- The menu bar and the status bar are hidden.
- The mouse cursor hides after 2 seconds without movement and comes back when you move it (Settings > General: **Hide mouse on inactivity**).
- A short hint shows for a few seconds when fullscreen starts, naming the key and controller buttons that bring the menu back. It reads your hotkey settings, so it stays right if you change them (Settings > General: **Show a hint when a game goes fullscreen**).

## Shortcuts

All of them can be changed in Settings > General > Hotkeys.

| What | Keyboard | Controller (hold Home) |
|---|---|---|
| Leave fullscreen and show the menu | Esc | Home + B (toggles fullscreen) |
| Toggle fullscreen | F11 | Home + B |
| Pause or continue | F4 | Home + Plus |
| Stop the game | F5 | L + Plus + Minus |
| Restart the game | F6 | R + Plus + Minus |
| Show or hide the performance overlay | Ctrl + V | not bound |
| Toggle the status bar | Ctrl + S | not bound |
| Volume up, down, mute | =, -, Ctrl + M | Home + D-pad up, down, right |
| Screenshot | Ctrl + P | Screenshot button |

## The performance overlay

Off by default. Switch it on with Ctrl + V or the View menu. Drag it to move it. Right-click it and check **Detailed trace** for the larger one (the same switch as `guarana_overlay_compact=false` in the config file).

Both sizes show:

- the game's own frame rate, big, easing toward the measured value;
- the frame rate that reaches the screen once generated frames are added ("shown");
- the 1% low of the trace: the slowest 1% of presents, written as a frame rate;
- frame time, guest-to-wall speed, and a short read of the limit: steady, cpu bound, gpu bound, uneven, or compiling;
- the presents themselves. Each bar is one present. Its width is the time since the previous present, and its height is that same time. The color runs from mint (120 Hz or faster) through ice (60 Hz) and amber (45 Hz) to red (30 Hz or slower). A faint band is the 16.7 ms budget. The lines that drift left are one-second marks, so a flat 60 Hz ribbon still moves;
- how those presents spread across the same budgets (the row of bars labeled in Hz);
- host threads as a heat strip, and how busy the GPU was during the frame. The detailed trace names the threads: C0–C5, SC (single core), GC (GPU command), GB (GPU backend), VK (Vulkan worker);
- memory, read every two seconds: the RAM Guarana holds and the RAM the PC has free, then the GPU memory Guarana uses and the room the driver still gives it. The room turns amber under 2 GB of RAM or 1 GB of GPU memory: past that the PC pages or the GPU moves its data into RAM, and both show as stutter. On an integrated GPU the GPU memory is part of the RAM: the detailed trace says "shared", and its room is never more than the RAM the PC has free;
- frame generation (`FG off`, `FG on`, `FG on +N`), shaders being built, and the upscaler, when they are on.

The border flashes when the newest present is slower than 1.2× the median. The detailed trace also lists the minimum, median and maximum game frame rate over the last few samples. The overlay stops drawing when it is hidden.

## The Guarana settings page

Settings > Guarana holds what players change most, in plain words. Every other setting stays where it was; the page reads and writes the same values. Changes made here are written when you press OK or Apply, after the Graphics page, so they win if both pages changed the same value in one visit. The resolution cannot be changed while a game runs.

| Control | What it does |
|---|---|
| Play mode: Smooth | The pacing measured best in Ocarina of Time on an Arc 140T: the vblank-locked presenter predicting from when a frame can be shown (`EDEN_NX2_FLUID=2`), real frames held to 20 per second, one per game tick (`EDEN_NX2_CADENCE=3`), predicted frames (`EDEN_NX2_EXTRAPOLATE`), pipeline library links (`EDEN_NX2_GPL`) and early read-backs (`EDEN_NX2_EARLY_SUBMIT`, `EDEN_NX2_EARLY_PAGES=256`, `EDEN_NX2_EARLY_AFTER=1`). In Hyrule Field, slots shown on time went from 72% to 89% and evenly paced frames from 62% to 88% (one paired session). Sets Frame pacing to Auto. |
| Play mode: Quality | Nothing steers or holds the graphics back; removes the switches the other modes wrote. Pair it with a higher resolution. |
| Play mode: Battery | Turns processor boost off while a game runs so the graphics get the power, quiets background workers, steers frame timing (`EDEN_NX2_POWER_POLICY=igpu`, `EDEN_NX2_ECO_BACKGROUND`, `EDEN_NX2_AUTOPILOT`). |
| Frame generation | The same switch as the status bar button and Graphics > Frame generation. |
| Resolution | The same setting as Graphics > Resolution, with the common values. |
| Controller | Opens the Controls page. |
| Report a problem | The same as Help > Report a problem. Guarana sends nothing; it builds a zip and opens the issue form. |

A play mode is written to a marked block at the top of `nx2_switches.txt` in your Guarana folder (`%APPDATA%\guarana` on Windows), and takes effect the next time you start Guarana. A line you add below the block wins over it, and a real environment variable wins over the file. Choosing no mode removes the block. Nothing outside the block is touched.

## First run

With no games folder yet, the game list shows a panel with **Add your games folder**, **Install keys** and **Install firmware** (from a console you own; Guarana ships with neither), and links to the guide, the controller setup page and the Discord.

## Theme

New installs use **Guarana Dark**: flat navy surfaces, 8 px corners, an amber accent for focus and selection, and a 2 px amber ring on whatever has keyboard or controller focus. Change it in Settings > General > Game List > Theme. The other themes are unchanged.
