# Contributing to Guarana

Guarana, a fork of Lichii, makes the Switch 2 emulator run well on integrated GPUs, Intel Arc first and then AMD's handheld chips, and is honest about what it measured. People and AI agents are both welcome, and the rules are the same for both. [AGENTS.md](AGENTS.md) has the long form: build commands, the repository map, how to verify a change.

Open issues and pull requests here, in [karimalsalah/guarana](https://github.com/karimalsalah/guarana). Chat, setup help and "is anyone on this?" go to the Discord, [discord.gg/urz7vMdRM](https://discord.gg/urz7vMdRM); questions that deserve a lasting answer go to [Discussions](https://github.com/karimalsalah/guarana/discussions). Bugs and device reports stay on GitHub so they can be tracked.

Using an AI tool? [docs/AI-QUICKSTART.md](docs/AI-QUICKSTART.md) gets Claude Code, Codex, Gemini CLI, Cursor, Copilot, Windsurf, Aider, Cline or a plain web chat from zero to a pull request, one command each.

## Five-minute start

1. Fork the repository and clone your fork.
2. Pick something: an issue labelled `good first issue`, `help wanted` or `agent-ready`, or an issue labelled `needs-device-data` if you have that hardware. A device report needs no code at all. Before you start on a code issue, comment `/claim` on it ([how](#picking-an-agent-ready-issue)). Device reports and the call for testers (#55) need no claim.
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

- Comment `/claim` on the issue before you start, so two people do not do the same work: the `agent-claims` workflow assigns it to you and labels it `claimed`. If you stop, comment `/unclaim`. A claim with no open pull request linked to it is released after 72 hours ([docs/agents/README.md](docs/agents/README.md#2-claim-it)). This works for `good first issue`s too.
- From a terminal, `echo /claim | gh issue comment <n> -R karimalsalah/guarana --body-file -` works in every shell. Not `--body "/claim"` from Git Bash on Windows: it rewrites the argument into a path inside the Git install folder, such as `.../Program Files/Git/claim`, which claims nothing. In Git Bash, `MSYS_NO_PATHCONV=1 gh issue comment <n> --body /claim` works too.
- Stay inside the issue's scope. Found something else? Open another issue.
- Done means the acceptance test passes, and failed before your change. Paste its output in the pull request, with `Closes #<number>`.
- If the issue says it needs a device run (debug server, `tools/bench`), say in the pull request what you measured and what still needs someone with the hardware; a maintainer adds `needs-device-data`.
- AI agents read [AGENTS.md](AGENTS.md) first. Never start or stop the emulator on someone's machine, never ask the owner for a build (CI makes one), and keep the debug server's etiquette.

## Free test builds from CI

- Every pull request that changes code gets `build-check`: Linux GCC and Windows clang-cl with warnings as errors, the unit tests and the Python tool tests. Pull requests that change only documentation, the website, issue forms, labels or benchmark rows skip the builds.
- The Windows build of your pull request is uploaded as `guarana-pr<N>-<commit>-windows-x86_64` and linked in a comment on the pull request within minutes of the run finishing. It is kept for 14 days; downloading needs a GitHub account. It is built without the release builds' ThinLTO and PGO, so compare its fps only with another test build.
- Your first pull request here may show no checks at first: GitHub can hold the workflow runs of a first-time contributor until a maintainer approves them. That is a repository setting, not a problem with your pull request.
- Want a build without opening a pull request? Turn on Actions in your fork and push a branch named `exp/<topic>`: the same workflow runs there and its run page has the build.
- The [`nightly` pre-release](https://github.com/karimalsalah/guarana/releases/tag/nightly) is `main` of the last day, built like a release.

## Submitting device data

Numbers from your hardware are as valuable as code, especially for GPUs the maintainers do not have.

- **Ten minutes, no code**: follow [docs/TESTING.md](docs/TESTING.md): install, stand in Hyrule Field, press Share in the launcher (or fill in the form) and submit.
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

## Credit

Every merged pull request is credited for good: your name goes on the [Hall of fame](https://karimalsalah.github.io/guarana/legacy.html) and in [CONTRIBUTORS.md](CONTRIBUTORS.md), rebuilt daily from the git history. Your first merged pull request and your first device report each get one thank-you comment from the `welcome` workflow, with a next step; your name appears once a maintainer merges the next Hall of fame update. Authors of `Co-authored-by:` trailers count too, and so do ported fixes (credit the original author). AI tools you used are listed as tools, not as people; the person who owns the pull request is the contributor.

Everyone here follows the [Code of Conduct](CODE_OF_CONDUCT.md), on Discord too.
