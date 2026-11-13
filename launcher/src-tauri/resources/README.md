Files bundled into the installer.

CI puts the current Windows build here before `tauri build`: `emulator.zip` (the release's
`guarana-<version>-windows-x86_64-pgo.zip`) and `emulator.json`
(`{ "version", "channel", "sha256", "asset" }`). On first run, when no version is installed, the
launcher installs `emulator.zip` into `%LOCALAPPDATA%\Guarana\versions\<version>\`, so setup works
offline. Neither file is committed.
