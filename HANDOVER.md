# Handover

The living handover for any agent or person who takes over Guarana or works alongside it. It says
where things stand, how to pick up each lane, and how a change is measured. The rules are in
[AGENTS.md](AGENTS.md), and how an agent finds and claims work is in
[docs/agents/README.md](docs/agents/README.md).

**Last checked: 2026-11-16, 19:20 UTC**, from branch `merge/upstream-20261116c` (the upstream
merge to `0a15f2ce07`; the measurements below are older and say when they were taken). "Observed" below means
what `git` and `gh` showed at that time. Anything this file could not confirm is marked
**(unsure)**. If you change the state of a lane, update its row in the same pull request.

## Where things stand

- **v20.4.0 is released.** The tag `v20.4.0` is on `4fabf93048`, the commit `v20.4.0-rc1` names:
  the release candidate became the release unchanged. Next to the 417 commits `main` gained
  after v20.3.0, it holds:
  - the merge of zortos293/Lichii `nx2` up to `99e89096cb` (67 commits, pull request #197; the
    resolutions are listed there);
  - six fixes ported from open upstream pull requests and Eglantine, the replay bench
    (`tools/nx2/trace_replay.py bench`) and the driver verdict at boot (pull request #198).

  The details are in [CHANGELOG.md](CHANGELOG.md).
- **v20.5.0-rc1 is tagged** on `7b713872dd` (pull request #211), a pre-release: the Ocarina of
  Time text speed add-ons with the off-by-default built-in patches (#202, Lichii #1422), what the
  add-ons page says about a built-in patch (#204), the test for two built-ins that touch the same
  bytes (#205), the safer mod import (#206), a game's own frame-rate options on its own page (#203,
  a part of Eglantine #837), the overlay's live trace (#209) and the replay's refusal of a trace
  without presents (#207). None of them has been tried in the game yet. The upstream merge below
  came after the tag and goes into the next candidate.
- **`main` holds zortos293/Lichii `nx2` up to `0a15f2ce07`**: 50 commits after `99e89096cb` came
  with #220 (to `7c7f723b68`), 9 more with the merge from `merge/upstream-20261116c`; each pull
  request lists its resolutions. The `upstream` mirror is at `0d98e37f9a`, behind: that merge took
  `nx2`'s tip. GitHub deletes a pull request's head branch when it merges it, so merging the
  sync's own pull request (head `upstream`) deletes the mirror and the next sync fails (#201,
  closed by the run of 16 November 13:55 UTC after the branch was pushed again by hand). Never
  merge the sync's pull request: merge upstream on a `merge/upstream-<date>` branch, then close
  the sync's pull request as superseded, as #210 was ([docs/LANES.md](docs/LANES.md), "Upstream").
- **What the merges changed for our own code**, for whoever merges upstream next:
  - To `99e89096cb`: upstream routes each global memory access among its own candidate buffers,
    and `EDEN_NX2_GM` bits 2 and 4 sit on top (a load takes its narrowed list when it has one, its
    route otherwise). Upstream's pass trace took the names of our per-pass GPU cost timing, which
    is now `cost_pass_*`, `CostPass` and `ReportCostPassTimes` in `vk_scheduler`. A lookup drops a
    prebuilt pipeline that is still queued and the draw builds it; the async boot counts such a
    build as ended.
  - To `7c7f723b68`, Ocarina of Time's No sharpening: upstream copies the image the output pass
    samples (cb0+0xc0) instead of the tonemap draw it tracked, brings the network back after a
    skip that no replacement followed (`output_pending`, `after_network`; Lichii #1875) and skips
    the unused heavy dispatch in that window (Lichii #1941). `KeplerCompute::BypassOotNetwork`
    does exactly that without our switches, and keeps our logs and the `OotNetworkBypassed` call
    the extrapolation guide needs. `EDEN_NX2_OOT_BYPASS_ANY` and the switches read with it still
    copy the tonemap the rasterizer tracks (`TrackOotTonemappedImage`, `IsTonemapShader`,
    `DrawArea`, which upstream deleted and we keep), so with them a tonemap drawn by a fourth
    shader is still copied stale (Lichii #1688). Moving the switch path to the sampled image is
    open.
  - To `7c7f723b68`, elsewhere: `Device::ReportLoss()` is `[[noreturn]]` and ends the process,
    so no `[[fallthrough]]` follows it. A buffer is made through `CreateAfterRecovery`, which
    frees staging memory and retries; the `EDEN_NX2_UMA` host-mapped choice sits inside it. The
    general channel's event and a failed load's process are closed before the kernel shuts down
    by upstream's code (Lichii #1932), which replaced ours from carried #408. Nextendo online play
    is a setting, not a build option (`OnlinePlayAvailable` is gone). Upstream's
    `nsight_aftermath` input of `release-binaries.yml` is refused only for a build that would
    publish. New user-visible "Lichii" strings were renamed Guarana; the User-Agent sent to
    nextendo.network stays "Lichii", as it was.
  - To `0a15f2ce07`: upstream merged Lichii #1890, which we carried; the merge left one copy.
    The staging pool's `ReleaseMemory` waits only for submitted ticks instead of calling
    `Scheduler::Finish` mid-draw (Lichii #1997), and our `RetainUntilCurrentTick` still treats a
    buffer in its level's deferred map as pinned. A present fenced by a service thread (the nx2
    video decoder, the CDMA pusher) is composited on the scheduler owner, in order (Lichii
    #1996). Nextendo adds friends, friend requests and presence (Lichii #1983) and an opt-in
    profile name and picture sync (Lichii #1990), all only with online play on; Switch 1 add-ons
    show on Switch 2 Edition titles in the game list (Lichii #2001).
- **Upstream's signed NPLN patches (Lichii #1358) and our built-in patches** do not go through the
  same code. Ours live in `PatchManager` (all ten are Ocarina of Time's), show in Add-Ons, are
  turned off as `+<name>` in `[DisabledAddOns]` and apply in `PatchNSO`. An NPLN patch is a
  certificate fix that nextendo.network signs for one exact build: only while online play is on
  (Configure > Network, off by default), `PrepareForGame` reads the main build id before the boot
  (`AppLoader::ReadMainBuildIdHex`, from the Qt `BootGame` and `guarana-cli`) and fetches or
  re-checks the build's patch, and the NSO loader applies it after the mods and our built-ins,
  keyed on title id, build id and module, only when its guards (the original bytes) still match.
  A built-in or a mod that changed those bytes makes the loader refuse it ("original instructions
  do not match"), and the game cannot reach Nextendo. Add-Ons does not list it, `[DisabledAddOns]`
  cannot turn it off, and the #205 overlap test does not see it. No built-in patches an NPLN title
  today, so nothing collides yet; patch packs that patch online games should check the NPLN
  guards first.
- **Open pull requests:** `gh pr list -R karimalsalah/guarana`. A sync pull request (head
  `upstream`) is never merged by hand: see the `main` bullet above.
- **Shown-frame pacing was settled on 16 November: the upstream merge cost none.** In Hyrule
  Field, counting only runs with no compiler running in any 5 s sample. The baseline is `main`
  just before the merge (`7ba919b7a8`, 417 commits after v20.3.0), so the runs say what the
  merge cost, not what changed since the v20.3.0 release:

  | Build | Runs | Shown frames a second | Within 2 ms of the median | Slots missed of 1200 per 10 s |
  |---|---|---|---|---|
  | Before the merge | 1 | 119.8 to 119.9 | 99% | 0 to 3 |
  | v20.4.0 (two builds of the merged code, one of them the tagged commit's) | 3 | 119.6 to 120.0 | 98 to 100% | 0 to 4 |
  | Before the merge, with `EDEN_NX2_FLUID=2`, interpolation and a shown rate of 120 | 1 | 119.6 to 119.9 | 98 to 99% | 1 to 3 |
  | v20.4.0 with the same switches | 1 | 119.9 to 120.0 | 99 to 100% | 0 to 1 |

  The host GPU's busy time per sampled frame was 41.6 to 44.7 ms before the merge and 42.1 to
  43.5 ms on v20.4.0, so no gain is claimed either. The 102 to 110 frames a second this file reported
  before came from another agent session compiling on the machine: with a compiler in 14 of its
  34 samples, the build from before the merge itself read 102.6 to 106.7 and missed up to 170
  slots. These were lab runs,
  not `tools/bench` rows; `tools/bench` marks such a row invalid (`background_cpu`). Before a
  pacing run, look for `cargo`, `rustc`, `ninja` and `clang-cl` from every session on the
  machine, not only your own.
- **Known problems:**
  - #199, fixed after v20.4.0: a boot that never showed a frame (2 of 22 boots of this version
    on the measurement machine). Windows asks "Allow Guarana to access your microphone?" the
    first time a program at a new path opens the microphone, and cubeb's open waits for the
    answer. The prompt does not take the foreground, so it is easy to miss. Ocarina of Time
    opens two capture streams while it boots, though it never captures, so its first frame
    waited on the prompt. The answer is kept per path, and the launcher installs each version
    in a folder of its own, so every update asked again. The prompt was seen on screen in the
    lab on 16 November; the evidence is in the issue. Since the fix a sysmix capture stream
    opens the host microphone only when the game starts capturing, on a helper thread, and the
    game hears silence until the open returns.
  - The replay bench has no real rows yet. `guarana-cli --replay` starts on Intel's Windows
    driver, but the Hyrule Field trace recorded for it holds no presents: the lab script judged
    the recording finished by the file's size and closed the game during the 3 GB snapshot. Since
    #207 the replay refuses such a trace and the recorder reports it. A new trace has to be
    recorded: keep the game open until `trace_status` shows `requested: false`.
- **The work queue:** the open `agent-ready` issues
  ([the list](https://github.com/karimalsalah/guarana/issues?q=is%3Aissue%20is%3Aopen%20label%3Aagent-ready%20-label%3Aneeds-triage%20no%3Aassignee)).

## Candidates from the 16 November survey

Read from the open pull requests of zortos293/Lichii, Eglantine's branches and about 480 forks with
commits of their own. None of these is measured on an integrated GPU; each needs a paired run before
it becomes a default.

| Candidate | What it is | State |
|---|---|---|
| `EDEN_NX2_OOT_BYPASS_SKIP=d9ee412f00ce399a` | Also skip the network's heavy dispatch while the output pass is bypassed (raphaeltorquat0/Lichii `c91d646a52`) | Superseded: upstream does it by default since Lichii #1941 (merged to `7c7f723b68`), between a skipped prepass and the output copy. Not measured on an integrated GPU |
| zortos293/Lichii#1903 | Host-visible buffer cache on integrated GPUs (15 to 25 fps on Adreno 830) | Read only; touches the buffer cache |
| zortos293/Lichii#1897, #1810, #1797, #1816 | Collector waits once per pass; semaphore acquires without a drain; ASTC decode image reuse | Behind a switch, then A/B |
| zortos293/Lichii#1832 | Marks routed stores' buffers written | Wait: it runs after `EDEN_NX2_GM` has built its narrowed lists from the read-only buffers |
| zortos293/Lichii#1874 | Formatless image loads on Vulkan 1.3 | No effect on Intel 32.0.101.9175, which reports the feature; no evidence yet where it changes something |
| Eglantine `662af0bfaf` (part) | `Scheduler::WaitSubmitted` woken by a counter instead of 50 us sleeps | Behind a switch, then A/B |
| Eglantine `d1195b7d49` | Translated stages reused across pipelines that differ only in fixed state | Second wave: it sits in the shader cache |
| Deathnetworks/Lichii `059e931f66` | Retry a failed graphics pipeline build on a later draw | Needs a thread-safe counter |

## Lanes in progress

No lane is open on the public repository. To start one:

1. `git fetch origin`, then `git worktree add ../wt-<lane> -b <branch> origin/main`. Use one
   worktree per lane.
2. Keep the pull request in draft until it is compiled and, for a performance change, measured.
3. Update this file's rows in the same pull request.

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

- That a build with the #199 fix starts from a new folder without the prompt: the unit tests
  cover the open waiting for capture and an open that blocks, but no first start from a fresh
  folder has been watched on such a build yet.
