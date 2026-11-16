- Launcher: every version runs from one path, `%LOCALAPPDATA%\Guarana\app\guarana.exe`, instead
  of its own `versions\<version>\` folder. Windows keys the microphone permission, the GPU choice in
  Settings > System > Display > Graphics, firewall rules and Game Bar's game detection by that path,
  so an update or rollback no longer resets them, and the microphone question that held up a boot
  comes once, not after every update
  ([#199](https://github.com/karimalsalah/guarana/issues/199)). The launcher fills `app\` with
  copies of the version's programs and hard links to its other files, and swaps it whole with two
  renames, never while Guarana or a shader warm-up runs: an update made during a game switches at
  the next Play.
