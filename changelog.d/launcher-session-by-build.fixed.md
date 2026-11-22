- Launcher: nightlies and release candidates now start games in a launcher session too, as stable
  releases from 20.3.0 do: the emulator's own window and game list no longer appear around the
  game, the launch screen follows the boot, and the game returns to the launcher when it stops. The
  launcher now reads whether a build knows `--launcher-session` from the program itself; before, it
  went by the version label and counted every nightly and pre-release as too old.
