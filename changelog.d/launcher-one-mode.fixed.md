- Launcher: the mode is set in one place. When the launcher applies a mode (at every Play, and
  Advanced > Apply), it removes the mode block the emulator's own Guarana settings page wrote into
  `nx2_switches.txt` and sets that page back to Custom. The two blocks used to mix: the page's Smooth
  added frame-pacing lines under the launcher's mode, its Battery added a second autopilot, and its
  `EDEN_NX2_EXTRAPOLATE=1` survived on Intel integrated GPUs where the launcher leaves that line to
  the emulator ([#119](https://github.com/karimalsalah/guarana/issues/119)). A hand-made profile
  (Custom in the launcher) leaves the page's block as it is.
