# Readback annihilation sprint — 2026-11-21

Port classification + evidence for the measured readback levers. Worktree
`C:/lb/readback-sprint`, branch `feat/readback-annihilation` from `origin/main`
(2ba5a0d564). Companion: PR #285 head `port/eglantine-hot-pages` un-staled by
merging `origin/main` (merge 95c90602c3, clean, 0 conflicts).

## Measured bottleneck (why these levers)

`C:/lb/bottleneck-1121.json` — vision-gated Lon Lon Ranch gameplay, NATIVE
Switch 2 OoT remake (0400486024DEC000), installed build 58afd454f6, tuned crisp
(CADENCE=3, SHOWN_RATE=120, FLUID_INTERP=0), Arc 140T:

- frame 45.2 ms = CPU JIT+kernel-HLE 26.77 ms (59.2%) + readback-sync
  reader-waits 18.4 ms (40.7%) + presenter 0.029 ms (0.06%)
- real 22.12 fps, p99 53.36 ms, 0 stutters; GPU fence 0 us, gpu_side_waits 0
  => GPU idles on the CPU
- readbacks RPC: 1479 reader waits / 60 s, mean wait 16.5 ms, early downloads
  17.5 MB/s; OnCPURead log counter cross-checks exactly
- BUFFER_DOWNLOADS sweep: bd0 21.39 fps, bd1 19.58 fps — both == or worse than
  baseline; the downloads switch is NOT the lever (0 = Switch 2 native tiling
  default). The 18.4 ms is structural reactive-flush/read-trap design.

## Per-commit classification

Method: line-set presence of each commit's added lines against `origin/main`
(normalized whitespace), plus symbol/function-level verification of every
"missing" line. Ratios in `C:/lb/rb-classify.json`.

### Lichii #1028 — async on-demand readback (zortos293/Lichii, MERGED)

Sibling measured +17% OoT (28.2 -> 32.9 fps, p90 -5 ms) on a pre-port build.
**All 22 non-merge commits ALREADY-COVERED in Guarana main** — the port landed
earlier and Guarana evolved past it (superset):

| commit | subject (abridged) | class | evidence |
|---|---|---|---|
| fb1845900c | reading thread waits for its downloads | ALREADY-COVERED | `FinishReadDownloads` (5 files), `reader_waits` counters |
| d402148081 | READ_CHECK vs synchronous flush | ALREADY-COVERED | evolved into `ReadCheck` struct + `BufferCache::CheckReadDownload` (buffer_cache.h:1279), `take_out(write_ranges)` at 721-723 |
| 1c2f37dd7a | land read downloads under query results | ALREADY-COVERED | 0.92 presence; query-cache landing present |
| df7e7da097 | read check: kept bytes as landed | ALREADY-COVERED | CheckReadDownload landed-bytes form |
| 1a3e8bcbe0 | downloads right behind GPU writes | ALREADY-COVERED | `RecordEarlyDownloads`, `early_downloads`, EARLY_SUBMIT/PAGES/AFTER tuning (Guarana-only superset) |
| bdaf4ae1c5 | read check for early downloads | ALREADY-COVERED | 0.93 presence |
| df0b5f6bde | guest writes wait for landing under them | ALREADY-COVERED | 0.95; `applying_writes` in gpu.cpp |
| ac740d6ae8 / 122b049db5 / d020b62d91 | read-check refinements | ALREADY-COVERED | folded into CheckReadDownload |
| 027514b2c3 | applying_writes cleanup on every exit | ALREADY-COVERED | 0.86; gpu.cpp applying_writes |
| a846b27fc3 / b41488e16e | ASYNC_READBACK test switch | ALREADY-COVERED | switch added then REMOVED by 30eefa7dc6 in the same PR; Guarana has 0 ASYNC_READBACK refs = final always-async state |
| 55601ecbbe / f99baf1336 | macro DMA narrow wait / DMA_READ_VERIFY | ALREADY-COVERED | `Maxwell3D::FrontDmaRead`, `GpuSplit::WaitForWrites`, DMA_READ_VERIFY (3 files) |
| 30eefa7dc6 | always async, drop the switch | ALREADY-COVERED | 1.00 reverse-apply of 4c43a2f9ad; no switch in tree |
| d06c15a457 | drop [[likely]]/[[unlikely]] | ALREADY-COVERED | explains the attribute-line "misses" in earlier commits |
| 4c43a2f9ad | keep nx2.h out of download area header | ALREADY-COVERED | clean REVERSE apply (only commit that applies both ways) |
| 8d096867b4 | land downloads before writes to unheld memory | ALREADY-COVERED | 0.94 |
| b3047aff8a / fff4de3caf | query GPU-modified / end kept results | ALREADY-COVERED | 0.93 / 0.85, query_cache present |
| 17e3ad1e07 | ClearDownload landings, PrepareRegionWrite no wait | ALREADY-COVERED | `clear_landings`/`clear_waits` counters, PrepareRegionWrite (6 files) |

Guarana-only additions beyond #1028: `EDEN_NX2_READ_FAST` read lane
(`GpuSplit::RequestReadLane`), `FrontRunOnBack` back-thread macros,
EARLY_* tuning, back_dma/back_wait telemetry. `nx2_async_readback.h` is a
strict superset of the #1028 tip header (445 vs 215 lines).

### Eglantine read-traps / preflush (Eglantine-Community/Eglantine nx2)

**All 3 commits ALREADY-COVERED (evolved superset).** fastmem-only semantics
and the default-off bypass path are intact; downloads=1 MKW-glitch warning
still applies (keep default).

| commit | subject | class | evidence |
|---|---|---|---|
| a128874344 | NO_REACTIVE opt-in bypass | ALREADY-COVERED | memory.cpp:606-615, opt-in `EDEN_NX2_NO_REACTIVE=1` + `CurrentProgramIsSwitch2()` gate, adapted to `Settings::ReactiveFlushing()` |
| 06e211e0d0 | rearm clean read areas on GPU write | ALREADY-COVERED | `read_trap_generation.fetch_add` in RasterizerArmReadTraps (memory.cpp:705), non-core reader path (1019-1023), conditional `MarkRegionAsPreflushable` + `read_trapped_ranges.Subtract` (buffer_cache.h:381-420, evolved with in_flight/early-download integration), `InvalidateGPUCache()` before the read flush (gpu.cpp:463, evolved `FlushRegionForRead`) |
| 5d742e369e | download only Preflushable parts at fences | ALREADY-COVERED | `DownloadsAtFence` (=IsRangePreemtive, +AsyncBufferDownloads), `HasGpuWritesNotDownloadedAtFence` (=HasNonPreemptiveGuestData), `QueueFenceDownload` partial queuing (=ForEachPreflushableIn), memory_tracker `IsRegionPreflushable`/`ForEachPreflushableRange`/`UnmarkRegionAsPreflushable` (buffer_cache_base.h:1020-1095) |

### Eglantine hot-pages f27522bfb9 — ADDITIVE (the one real port)

Presence ratio 0.01 vs main: genuinely absent. Ported by our draft #285
(15082269fe, 0.94 line match; delta = docs/changelog/CREDITS + Guarana
`MarkWrittenTracked` engine-write re-mark adaptation). Sibling A/B: OoT Kokiri
spawn 25.9 -> 24.7 ms, protection calls -71%, protected pages -94%, luma stable
over 800 frames.

**Deliberate semantic difference:** upstream defaults ON (`=0` disables);
Guarana #285 is opt-in `EDEN_NX2_HOT_PAGES=1` (unset/0 = old behavior),
documented in docs/switches.md with the flicker risk note. Preserved.

This sprint: cherry-picked 15082269fe onto feat/readback-annihilation
(clean; delta byte-identical to the source commit — phantom scan
IDENTICAL-DELTA), and merged origin/main into #285 head (95c90602c3, clean).
Both trees verified byte-identical for all 5 touched files.

### Eglantine gpu_split follower drains a0d6443f67 / 354d1347e8 — BLOCKED

SHAs resolve in NONE of Eglantine-Community/Eglantine, Frizti/Fritchii,
zortos293/Lichii (GitHub commits API 422; not in any fetched ref) — likely
force-pushed-away PR heads. Dependency noted: upstream follower opt-in
cd5745dadb (`EDEN_NX2_RENDER_FOLLOWER=1`, Lichii #2350) lands via Guarana
PR #312 (merge/upstream-20261121); do not block on it. Re-source the drain
SHAs from the sibling's current PR heads before porting.

Related already-covered: Fritchii 065e04e87b (skip unbuilt pipelines into
per-frame targets) = Guarana #245/#247, both MERGED.

## Scans (merge-discipline gates)

- conflict markers: 0 in all touched files
- phantom scan: cherry-pick delta vs origin/main byte-identical to 15082269fe
  source delta (IDENTICAL-DELTA); #285 main-merge clean, no manual seams
- brace/signature: no union seams (no conflicts occurred); hot-page symbols
  verified present (HotPagesEnabled, g_hot_page_epoch, PrepareForHotTracking,
  hot_epoch/hot_frame, Type::Hot/Flip/NoHot/HotClean)
- changelog gate: `python -I tools/ci/changelog_fragments.py check --base HEAD~1`
  -> `changelog: ok` (fragment changelog.d/hot-pages.added.md, first line '- ')
- tuning semantics preserved: FLUID_INTERP=0 iGPU default, CADENCE pacing,
  NO_REACTIVE default-off, HOT_PAGES opt-in; platform fixes (ARM64 cppwinrt,
  macOS freetype) untouched (diff confined to 5 files)

## Build

`C:/lb/build-rb.ps1` (build-1120.ps1 preset pattern: VS2022 BuildTools vcvars64,
clang-cl/lld-link C:/lb/llvm, Ninja, Qt 6.8.3 msvc2022_64, CPM cache
C:/lb/v20/.cache/cpm, -march=x86-64-v3 -mtune=arrowlake, LICHII_TESTS=ON,
LICHII_CMD=ON). Source C:/lb/readback-sprint, build C:/lb/build-rb.
Status: see C:/lb/readback-sprint-result.json.

## Measure (vision-gated A/B)

Baseline: 22.12 real fps / 45.2 ms frame / p99 53.36 ms (bottleneck-1121.json,
installed 20.7.0 build, Lon Lon autosave 11/17/2026 12:22, tuned crisp env).
A/B lever: `EDEN_NX2_HOT_PAGES=1` vs unset, same save/spot/env, harness
C:/lb/auto_tune.py (vision-gated, id-matched .req gate protocol, post-sample
gate). Exact command in the result JSON.
