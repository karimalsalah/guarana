# Handover

The living handover for any agent or person who takes over Guarana or works alongside it. It says
where things stand, how to pick up each lane, and how a change is measured. The rules are in
[AGENTS.md](AGENTS.md), and how an agent finds and claims work is in
[docs/agents/README.md](docs/agents/README.md).

**Last checked: 2026-11-13, 17:42 UTC**, from branch `feat/agent-magnet`. "Observed" below means
what `git` and `gh` showed at that time. Anything this file could not confirm is marked
**(unsure)**. If you change the state of a lane, update its row in the same pull request.

## Where things stand

- **`main` is v20.2.0.** `VERSION` says `20.2.0`, and the tag `v20.2.0` is on `main`'s head,
  `0c8587d3ee`. v20.2 brings the Guarana rebrand, the new window and play modes (Smooth is
  `EDEN_NX2_FLUID=2` with `EDEN_NX2_CADENCE=3`), Ocarina of Time smooth by default on Windows
  integrated GPUs, opt-in pipeline library fast links (`EDEN_NX2_GPL`), and an upstream merge. The
  details are in [CHANGELOG.md](CHANGELOG.md).
- **The v20.2.0 release is still building.** The `release-binaries` run for the tag started at
  17:08 UTC and was still running at 17:42 UTC. The newest published release then was v20.1.0.
  Whether the v20.2.0 run succeeded **(unsure)**: check with
  `gh run list -R karimalsalah/guarana --workflow release-binaries.yml`.
- **Open pull requests:**
  - **#58** `feat/pacing-switches`: frame pacing switches, all off by default:
    - `EDEN_NX2_QUEUE_LIMIT`, `EDEN_NX2_FLUID_OFFSET_US`, `EDEN_NX2_FLUID_SPLIT_SUBMIT`,
      `EDEN_NX2_FLUID_QUEUE=5` with `EDEN_NX2_FLUID_QUEUE_HOST_WAIT`, and `EDEN_NX2_BUDGET_HOLD_RES`;
    - an exact-size borderless fullscreen window;
    - upstream pull requests #1602 and #995.

    Its three CI checks passed, and GitHub reported it mergeable. Its description says a paired
    A/B of each switch in Hyrule Field on the Arc 140T is running, and that the rows go into the
    pull request and `docs/switches.md` before any switch becomes a default. Six more commits on
    the local branch were not pushed (FLUID at 120 Hz, `EDEN_NX2_CADENCE_DIVISOR`,
    `EDEN_NX2_FLUID_QUEUE` 6 and 7, and two cache fixes).
  - **#53** `docs/bug-form-device-fields` and **#54** `docs/switch-registry-nx2-13`: documentation.
  - **Drafts #1 to #5:**
    - #1 `exp/fluid-presenter`
    - #2 `exp/intel-cache-safety`
    - #3 `exp/boot-time`
    - #4 `exp/upload-batching`
    - #5 `exp/gm4-rebase`
- **The work queue:** 18 open `agent-ready` issues, #6 to #46, none assigned
  ([the list](https://github.com/karimalsalah/guarana/issues?q=is%3Aissue%20is%3Aopen%20label%3Aagent-ready%20-label%3Aneeds-triage%20no%3Aassignee)).

## Lanes in progress

Each lane is a branch from `main` at `0c8587d3ee`. At 17:42 UTC none of these was pushed to
`karimalsalah/guarana` except `feat/pacing-switches`. Their purpose comes from their names and
commit subjects only. Whoever runs a lane owns the details **(unsure)**.

| Branch | Observed | Resume |
|---|---|---|
| `feat/pacing-switches` | Pull request #58 plus six local commits, see above | Finish the paired A/B, put the rows in #58 and `docs/switches.md`, then push and merge. A switch becomes a default only after a measured gain |
| `feat/readme-v203` | 3 commits: the README for v20.3, an index of `docs/` in `docs/README.md`, README art in SVG | This lane owns `README.md`; link the agent docs from it, not the other way round |
| `feat/game-booster` | No commits yet | Ask the owner what it is for **(unsure)** |
| `feat/sign-and-ship` | No commits yet | Ask the owner; the name suggests signing and shipping the binaries **(unsure)** |
| `feat/device-profile` | 1 commit: "common: record the device profile and its class at boot" | Related to issue #24, per-device presets **(unsure)** |
| `feat/windows-integrations` | No commits yet | Ask the owner what it is for **(unsure)** |
| `chore/brand-sweep` | 3 commits: Guarana names in the UI, the installer and metainfo links, the README warm-up row | Rebase on `main` before review; it touches many files, so merge it early or late, not in the middle |
| `feat/agent-magnet` | This branch: the agent front door, the claim workflow, optional agent workflows, the MCP registry entry, this file | Merge to `main`: the claim and agent workflows only work from the default branch |

To pick up a lane:

1. `git fetch origin`, then `git worktree add ../wt-<lane> <branch>`. Use one worktree per lane.
2. Read its commits (`git log main..<branch>`) and its pull request, if it has one.
3. Rebase on `main`.
4. Continue, keep the pull request in draft until it is measured, and update the lane's row here.

A branch with no commits has its goal only with whoever started it. Ask rather than guess.

## How a change is measured

The full rules are in [docs/BENCHMARKING.md](docs/BENCHMARKING.md) and
[tools/bench/README.md](tools/bench/README.md).

- **Use a verified scene.** A `tools/bench` row counts only with `"valid": true` and
  `"scene_ok": true`: the benchmark checked the scene against a reference screenshot. The scene of
  record is Ocarina of Time, Hyrule Field (`tools/bench/scenes/oot-hyrule-field.json`).
- **Measure in pairs.** Run the baseline and the change back to back, in one session, on the same
  machine, plugged in:
  `python -I tools/bench/bench.py run ... --variant tools/bench/variants/baseline.json --variant <yours>.json`.
  Then run `bench.py compare` on the two result files.
- **Report every number.** Put the device, driver, power mode, the before and after numbers and
  the number of paired runs in the pull request's table and in the switch's row in
  [docs/switches.md](docs/switches.md). Report losses too.
- **The measurement machine** for the published numbers is one laptop: Core Ultra 9 285H, Arc 140T,
  Intel driver 32.0.101.9175. Builds, benchmarks or the emulator never start on it unless the owner
  asks, because a compile spoils a run.
- **Live readings are not results.** Use the debug server (`frame_times`, `gpu_costs`) to find a
  cause; only bench rows prove a gain.

## Where things are

| What | Where |
|---|---|
| Rules, repository map, build and test commands | [AGENTS.md](AGENTS.md) |
| Finding work, `/claim`, CI gates, optional agents, MCP registry steps | [docs/agents/README.md](docs/agents/README.md) |
| Which agents to court and what each needs from the owner | [docs/agents/RESEARCH-2026-11.md](docs/agents/RESEARCH-2026-11.md) |
| Lanes, releases, hotfixes, repository settings | [docs/LANES.md](docs/LANES.md) |
| Every `EDEN_NX2_*` switch, its status and measured effect | [docs/switches.md](docs/switches.md) |
| Benchmark harness and evidence rules | [tools/bench/README.md](tools/bench/README.md), [docs/BENCHMARKING.md](docs/BENCHMARKING.md) |
| Debug MCP server: methods and etiquette | [docs/nx2/debug-mcp.md](docs/nx2/debug-mcp.md), `tools/nx2/debug_mcp.py`, [.mcp.json](.mcp.json) |
| Frame generation and pacing | [docs/frame-generation.md](docs/frame-generation.md) |
| Reports, issues, test builds and how they connect | [docs/FEEDBACK.md](docs/FEEDBACK.md) |
| Plans | [ROADMAP.md](ROADMAP.md) |
| AI rules: disclosure, ownership, spam | [docs/AI_POLICY.md](docs/AI_POLICY.md) |
| Claude Code skills (bench, debug-server, perf-change, release, upstream-port) | `.claude/skills/` |

## If you are an agent starting now: do these five things first

1. **Read [AGENTS.md](AGENTS.md)**, then this file and [docs/agents/README.md](docs/agents/README.md).
2. **See what moved since this file was written:** `git fetch origin`,
   `gh pr list -R karimalsalah/guarana` and `gh run list -R karimalsalah/guarana --limit 10`. If
   this file is stale, fixing it is your first commit.
3. **Take exactly one thing:** a lane above, or an `agent-ready` issue that you `/claim`. Work in
   your own worktree, on a branch that carries the issue number.
4. **Run the Python tests** before and after your change (section 4 of
   [docs/agents/README.md](docs/agents/README.md)). Never start a build, a benchmark or the
   emulator on the owner's machine unless asked.
5. **Open a draft pull request** with the template filled in, an `Assisted-by:` trailer on each
   commit, a changelog fragment (`changelog.d/`), and your lane's row here updated.

## Not confirmed when this was written

- Whether the v20.2.0 release run finished and published its assets.
- What `feat/game-booster`, `feat/sign-and-ship` and `feat/windows-integrations` are for, and
  whether `feat/device-profile` is issue #24.
- Whether the A/B for #58 has finished, and its numbers.
