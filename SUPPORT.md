# Getting help

## Where to ask

- **Chat, setup help and quick questions**: the Guarana Discord, [discord.gg/urz7vMdRM](https://discord.gg/urz7vMdRM). Contributors answer in real time. Same rules as here: no keys, firmware or games, ever.
- **Questions that deserve a lasting answer**: [Discussions](https://github.com/karimalsalah/guarana/discussions). Search first; someone may have had the same driver or game problem.
- **Bugs, device results, performance ideas, tasks** stay on GitHub, not on Discord: [issues](https://github.com/karimalsalah/guarana/issues/new/choose), through the form that fits. The forms ask for what we need to act on a report.
- **Security problems**: privately, as [SECURITY.md](SECURITY.md) explains.
- **Documentation**: [docs/](docs), and for the `EDEN_NX2_*` switches [docs/switches.md](docs/switches.md).

Anyone can claim to be staff in a chat: nobody from the project will ask you for a password, a key, or a download outside this repository.

Download Guarana only from this repository's [releases page](https://github.com/karimalsalah/guarana/releases). Builds offered anywhere else are not ours.

## What we cannot help with

- Getting keys, firmware or games. Dump your own from hardware you own; nobody here will tell you where to find them, and such requests are closed. See [LEGAL.md](LEGAL.md).
- Builds of other forks, or of upstream Lichii: ask their projects.
- Hardware or drivers we have no way to test. A device report with numbers is the way to get such hardware looked at.

## A report we can act on

- The exact build: Help > About, or the first line of the log.
- CPU, GPU and the full driver version, RAM, the OS and its build, and whether the machine was plugged in.
- The game and where in it, the settings you changed, and every `EDEN_NX2_*` switch you set.
- A screenshot, and the log of the session (`eden_log.txt` in the `log` folder of the emulator's data directory, `%APPDATA%\lichii\log` on Windows). Open it first and remove your user name and any folder paths you do not want public.

Never attach keys, firmware, game files, saves or shader caches.
