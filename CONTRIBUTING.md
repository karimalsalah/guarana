# Contributing to Guarana

Guarana makes the Lichii Switch 2 emulator run well on integrated GPUs, Intel Arc first and then AMD's handheld chips, and is honest about what it measured. People and AI agents are both welcome, and the rules are the same for both. [AGENTS.md](AGENTS.md) has the long form: build commands, the repository map, how to verify a change.

Open issues and pull requests here, in [karimalsalah/guarana](https://github.com/karimalsalah/guarana). Questions go to [Discussions](https://github.com/karimalsalah/guarana/discussions).

## Five-minute start

1. Fork the repository and clone your fork.
2. Pick something: an issue labelled `good first issue`, `help wanted` or `agent-ready`, or an issue labelled `needs-device-data` if you have that hardware. A device report needs no code at all.
3. Create a topic branch from `main` in your fork, for example `fix/oot-shadow-flicker` or `perf/foliage-mesh`.
4. Make the change and add or update a unit test in `src/tests` (or a Python test next to the tool you changed). Build and test with the one-line commands in [AGENTS.md](AGENTS.md#build).
5. Open a pull request into `main` and fill in the template. CI builds Linux and Windows, runs the tests, and comments a Windows test build on your pull request.

Do not have a compiler set up? Skip step 4's build: CI builds every pull request for you, and the test build it links is what testers run.

## The quality bar

1. **Ship only a measured gain on a verified scene.** A verified scene is one the benchmark confirmed the game was in: a `tools/bench` row with `"scene_ok": true`. A run in the wrong scene is not a result, whatever its fps.
2. **Measure in pairs.** Baseline and change back to back, in one session, on the same machine, plugged in. Show the baseline next to the result. Baselines drift between sessions.
3. **Experiments go behind a switch.** New behaviour without a verified gain is an `EDEN_NX2_*` environment switch, off by default, with a row in [docs/switches.md](docs/switches.md). Unset, the emulator behaves exactly as before.
4. **Drop what did not help.** A switch that showed no gain in a paired run is removed, or stays off with the result written in its row.
5. **Claim only the numbers you have.** "Measured on one laptop" is a fine thing to write; "faster" without a table is not.
6. **Correctness first.** A speed-up that can change the image needs a scene check and a known-issues note. A driver-bug fix needs a before and after screenshot or log.

## Picking an `agent-ready` issue

`agent-ready` issues come from the agent task form. Triage adds the label when the acceptance test is checkable, and a maintainer removes `needs-triage` once they have read it; take only issues without `needs-triage`. They name the goal, the files, an acceptance test, and the measurement if there is one. Humans can take them too.

- Comment on the issue to say you are taking it, so two people do not do the same work. If you stop, say so.
- Stay inside the issue's scope. Found something else? Open another issue.
- Done means the acceptance test passes, and failed before your change. Paste its output in the pull request, with `Closes #<number>`.
- If the issue says it needs a device run (debug server, `tools/bench`), say in the pull request what you measured and what still needs someone with the hardware; a maintainer adds `needs-device-data`.
- AI agents read [AGENTS.md](AGENTS.md) first. Never start or stop the emulator on someone's machine, never ask the owner for a build (CI makes one), and keep the debug server's etiquette.

## Free test builds from CI

- Every pull request that changes code gets `build-check`: Linux GCC and Windows clang-cl with warnings as errors, the unit tests and the Python tool tests. Pull requests that change only documentation, the website, issue forms, labels or benchmark rows skip the builds.
- The Windows build of your pull request is uploaded as `guarana-pr<N>-<commit>-windows-x86_64` and linked in a comment on the pull request within minutes of the run finishing. It is kept for 14 days; downloading needs a GitHub account. It is built without the release builds' ThinLTO and PGO, so compare its fps only with another test build.
- Want a build without opening a pull request? Turn on Actions in your fork and push a branch named `exp/<topic>`: the same workflow runs there and its run page has the build.
- The [`nightly` pre-release](https://github.com/karimalsalah/guarana/releases/tag/nightly) is `main` of the last day, built like a release.

## Submitting device data

Numbers from your hardware are as valuable as code, especially for GPUs the maintainers do not have.

- **Quick**: open a [Device report](https://github.com/karimalsalah/guarana/issues/new?template=device-report.yml). It asks for the device, CPU, GPU, driver, RAM, OS, power mode, build, scene, settings and your numbers, with a screenshot of the scene.
- **Best**: run `tools/bench` ([CONTRIBUTING-bench.md](tools/bench/CONTRIBUTING-bench.md)). It plays a scene the same way on every machine, checks the scene against your reference screenshot, and writes rows without user names or paths. Paste the rows into a device report, or open a pull request with your `tools/bench/results/<date>-<machine-id>.jsonl` and the regenerated `tools/bench/RESULTS.md`.
- Make a report comparable: plugged in, background programs closed, the driver version in full, the power mode, and the baseline measured the same way right before or after.

## Proposing a switch

Open a [Performance idea](https://github.com/karimalsalah/guarana/issues/new?template=perf-idea.yml) or a pull request with:

1. What it changes and what its values or bits mean.
2. The scene where it helps, with a paired A/B from the same session.
3. What can go wrong: when the output can differ from the default.
4. How to reproduce it on an integrated GPU, or that you only have other hardware.

Its row in [docs/switches.md](docs/switches.md) is part of the same pull request.

## Commits and pull requests

- Commit subjects are `area: change`, in the imperative: `vulkan: keep vertex input dynamic state on Intel 101.x drivers`. The body says why, and for a performance change the device, scene, before and after numbers, and how many runs.
- One topic per pull request, with its tests, its switch rows and a line in `CHANGELOG.md` under `[Unreleased]`.
- Lanes (`main`, `release/vX.Y`, `hotfix/*`, `exp/*`, `upstream`) and releases are in [docs/LANES.md](docs/LANES.md).

## Upstream first

Guarana is a fork of [zortos293/Lichii](https://github.com/zortos293/Lichii). Before writing a fix, look for one there (the `nx2` branch, open and closed pull requests, issues) and in Eglantine. Port an existing fix with its author credited (`Co-authored-by:`) instead of writing your own, and list carried pull requests in [CREDITS.md](CREDITS.md). A fix that is not specific to integrated GPUs belongs upstream too: open the pull request there as well and link it from ours.

## Legal and privacy

- Never attach or commit keys, firmware, game files, extracted game data, shader dumps or caches, saves, or logs with your user name or paths. Do not ask for them or say where to find them. See [LEGAL.md](LEGAL.md).
- Contributions are under GPL-3.0-or-later, the project's licence ([LICENSE.txt](LICENSE.txt)); keep the SPDX notices of the files you touch.
- Security problems go through [SECURITY.md](SECURITY.md), not public issues.

Everyone here follows the [Code of Conduct](CODE_OF_CONDUCT.md).
