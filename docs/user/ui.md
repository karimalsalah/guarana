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

Off by default. Switch it on with Ctrl + V or the View menu. The default **compact** overlay shows:

- the game's own frame rate, big;
- the frame rate that reaches the screen once generated frames are added ("shown");
- a line of the last 2 seconds of frame times: flat is even, a red stretch is a hitch;
- frame generation as `FG off`, `FG on` or `FG on +N` (N generated frames per second), and the current frame time.

The detailed overlay (CPU and GPU load, 5 second graph with min, median and max, upscaler tags) is still there: set `guarana_overlay_compact=false` in the config file. Drag the overlay to move it.

## The Guarana settings page

Settings > Guarana holds what players change most, in plain words. Every other setting stays where it was; the page reads and writes the same values. Changes made here are written when you press OK or Apply, after the Graphics page, so they win if both pages changed the same value in one visit. The resolution cannot be changed while a game runs.

| Control | What it does |
|---|---|
| Play mode: Smooth | The configuration measured smoothest in Ocarina of Time on an Arc 140T: the vblank-locked presenter with predicted frames (`EDEN_NX2_FLUID`, `EDEN_NX2_EXTRAPOLATE`), pipeline library links (`EDEN_NX2_GPL`) and early read-backs (`EDEN_NX2_EARLY_SUBMIT`, `EDEN_NX2_EARLY_PAGES=256`, `EDEN_NX2_EARLY_AFTER=1`). Sets Frame pacing to Auto. |
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
