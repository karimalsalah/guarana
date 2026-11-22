# Frame-time hitches: Hyrule Field on the Arc 140T

Root-cause notes for the remaining hitches of build arc-v21 dev6 (`EDEN_NX2_FLUID=1 EXTRAPOLATE=1 EARLY_PAGES=256 EARLY_AFTER=1 EARLY_SUBMIT=1 GPL=1`), Ocarina of Time, Hyrule Field, Core Ultra 9 285H, Arc 140T. Written from the logs `log-stutter-trace-dev6.txt` (`EDEN_NX2_STUTTER_TRACE=1 EDEN_NX2_STUTTER_DETAIL_MS=80`) and `log-v21-profile-field.txt` (`EDEN_NX2_GUEST_THREAD_STATS=1`, `EDEN_NX2_GPU_PROFILE=1`) and from the code at this commit. Nothing here was built or run: every number is from those two logs, and what the logs cannot say is marked as a hypothesis with the run that settles it.

Paths are under `src/`. Line numbers are of this branch, `texture_cache.h` as edited here (the arc-v21 line is given where the code moved).

## Summary

1. The 60-110 ms "other" frames are not GPU, present, fence or vsync waits, and not loading work, finishes, allocation or read-back count. They are the game's own frame (guest main thread plus five workers) growing slowly over tens of seconds (a ramp from 0 to 22 slow frames a second in 131-141 s). The logs show why a guest frame is 25-30 ms against 3-11 ms of GPU thread work (serialised read-backs of GPU-written memory, each waiting for the host GPU: hypothesis H1) but **not what makes it grow**; the candidates are the machine (power or thermal, H4), the content of the area (H5) and core placement (H2), and the runs in "Next runs" separate them.
2. `Scheduler::Finish` and the texture collector are real costs but **do not explain the over-33 ms frames**: the window with 79 finishes had 9 over-33 ms frames, the window with 17 had 94, the window with none had 1.
3. In the windows where `texture_collect` is large, nearly all of it is the collector's own `Scheduler::Finish`: it downloads every GPU-modified image it evicts and waits for the GPU once per image (`texture_cache.h:189` at arc-v21, `:245` here).
4. The collector runs in its high-priority mode on this machine because an integrated GPU's device budget is capped at 4 GiB (6 GiB aggressive) whatever the RAM (`vulkan_device.cpp:2523`): 6144 MiB here, thresholds expected 4050 / critical 4818 MiB on a machine with 27 GiB of commit left. `EDEN_NX2_TC_*_MIB` cannot lift it (they are capped by the budget).
5. `query_fence` is not occlusion queries. It is the sum of the fence manager's `SignalFence` time and every wait for a GPU tick (`MasterSemaphore::Wait`), which includes the waits inside `Finish`. Making game queries resolve earlier is unsafe for Ocarina.

Implemented behind default-off switches (see [switches.md](switches.md)): `EDEN_NX2_HITCH_FINISH_TRACE`, `EDEN_NX2_HITCH_GC_BATCH`, `EDEN_NX2_HITCH_GC_CLEAN_FIRST`, `EDEN_NX2_HITCH_UMA_BUDGET_MIB`.

## 1. What the 60-110 ms "other" frames are

### What is excluded

| Candidate | Evidence | Verdict |
|---|---|---|
| GPU threads | In `Nx2Stutter` lines of cause `other`, busy is `front:3-4, back:8-11, worker:5-8` ms whether the interval is 36 ms or 114 ms (frame 9124: 95.6 ms, back 8.5; frame 9125: 43.7 ms, back 9.5). GPU work per frame is constant; the interval is not. | Not the cause. |
| Present pacing, vsync | `nx2:vi2 last 600 presents` (`core/hle/service/nx2_gsv.h:2171`) in the field: frame 29.7-43.4 ms (max 75-194 ms), blocked on the GPU fence 0 us (max 8-21 us), composite 34 us, vblank pacing 0 us, 600 of 600 presents ran a frame ahead. The present call never waits; the game sets its cadence upstream of it. | Not the cause. |
| Syncpoint/fence at present | Same lines: fence wait 0 us; `WaitForFrame ... waited 0 us`. | Not the cause. |
| Frame budget pacer | `EDEN_NX2_TARGET_FPS` is not in the dev6 environment; `video_core/frame_budget.cpp` only holds presents with it set. | Not active. |
| Fluid presenter | 487-589 of 600 slots shown on time, 12-113 missed "with no frame ready": it consumes what the guest makes, so it follows the hitches and does not make them. | Symptom. |
| Shader/pipeline builds, memory allocation | `gfx_miss`, `pipeline_wait`, `compute_miss` are 0.0 in every field window after 100 s; `memory_allocate` and `memory_free` show up only in the first windows after the scene loads. | Not the cause. |

### What remains: the guest's own frame

The distribution says the same. In the cleaner field windows the frame histogram is a tight peak at 25-30 ms (121 s window: `25:12,26:55,27:84,28:96,29:70,30:23`, 1 frame over 33 ms). In the 131-141 s window it is smeared (`27:45,28:44,29:25,30:32,31:22,32:17,33:12,34:10,35:13,36:10,37:10,38:8`, 94 over 33 ms, tail to 109 ms) with the same GPU work per frame. So a second term adds to the guest frame in some windows and not in others. (The windows from 77 to 101 s follow the scene load: 565-1611 images created per 10 s, 199-206 frames over 33 ms; they are a separate, loading hitch.)

The profile run (`log-v21-profile-field.txt` from line 6419; 13 fps, because the profile itself slows the game) gives the structure of that frame, in ms per frame:

- main thread t88 on core 0: 45.1 on a core, of which **counter polling 27.65 (27.45 "tight")** and **GPU-cached memory 19.75 in 204 028 calls**, of them 0.9 read-backs costing 11.79;
- five workers t155-t159, prio 44, one per core: 11.4-13.7 on a core each, **9-10 ms of it in GPU-cached memory read-backs** (0.7-0.9 per frame, 8-10 ms each), waiting 58-60 ms on addresses (the main thread's job signals);
- the present thread t95: asleep (it only paces).

The main thread did 2.64 million counter reads per second on core 0 (`Guest core 0 ... 2642572 counter reads/s`) alongside 204 028 slow-path reads of GPU-cached memory a frame (`core/memory.cpp:298`, `RasterizerCachedMemory`): it spins on a word of GPU-cached memory with a counter timeout, waiting for the workers. Each worker waits for a read-back of GPU-written memory (`OnCPURead`, `video_core/gpu.cpp:804`; the log prints every 64th read, which is about 47 a second in the field, 1.3 per frame).

A read-back waits until the host GPU has run everything recorded before the copy. The fluid presenter measures that at **15.5-17.7 ms** ("GPU work done after recording", dev6 log at 136.4 s and 146.4 s); the profile run's async read-back lines (795-815 s) say readers waited on their fence 54-57 times a second, 280-340 ms, longest 22-32 ms. So:

> guest frame time ≈ game work + (read-backs in the dependent chain) × (GPU latency of a read-back).

A chain of 3-4 gives 50-70 ms and would explain why a guest frame is 25-30 ms while the GPU threads work 3-11 ms. A read-back that an early download already landed costs nothing; one that did not costs the latency. This is **hypothesis H1**, and it explains the baseline, **not the smear**: the read rate is the same in clean and smeared windows (from the `(N so far)` timestamps: about 51 a second at 111-131 s, 53 at 131-135 s, 40 at 137-139 s, 32 at 143-145 s), and the fluid presenter's GPU latency is 15.5 ms in the 131 s window and 17.7 ms in the 141 s one. For the smear, something else must grow: the latency per read (a deeper GPU queue at that moment) or guest time that is not reads.

### What does and does not track the smear (checked in the dev6 log)

- Slow frames (over 33 ms) per second, 131 to 141 s: 0, 4, 5, 11, 5, 15, 9, 5, 15, 22, 4, with maxima 34, 37, 61, 110, 96, 83 ms; all 95 have cause `other`. The window before (111-121 s) has one slow frame. It is a ramp that keeps going to the end of the log (142-147 s: 33-114 ms frames), not a burst.
- Not guest memory trimming: `TrimZeroPages` ran at 39.1, 58.6 and 66.3 s only. Not ASTC decoding: the last `ASTC GPU decodes` line is at 98 s. Not pipeline builds: `gfx_miss`, `compute_miss`, `pipeline_wait` are 0.0. Not image or buffer creation: the smeared window made 21 images (2.9 ms), the clean 131 s window none. Not finishes (section 2). Not the read rate. Not GPU thread time per frame (`busy` 3-4 / 8-11 / 5-8 ms in 36 ms and 114 ms frames alike).
- So the cause varies slowly. **H4, the machine**: package power or temperature (the 285H under a 28 W cap after the turbo budget runs out) slows every guest thread; the signature is that the GPU threads' CPU time per frame rises too, which the log cannot show for clean frames because `busy` is printed for slow frames only. **H5, the content**: Link walks into an area with more actors, grass or streaming, and the game's own CPU work grows; the signature is that `game code` ms of the workers and the main thread grows while `ready` does not. Both are open: neither log has the data.

**H2: the guest threads wait for a core, not for the GPU.** The workers show "ready 1.5-4.8 ms" a frame in the profile run (`ready` is runnable without a core); five worker cores and the GPU front, back, worker, present, fence and vsync threads share a 16-thread package under a power cap. A 10 s window in which Windows puts a worker on an E core looks like a smeared window. `EDEN_NX2_HYBRID=1` (see switches.md) is the existing test.

**H3: late HostTiming events**: a timed wait or timer event that fires 5-20 ms late stretches a guest sleep (the workers show "sleep 0.8-1.1 in 4-6 calls" a frame). Not measured in either log.

### Next runs

Compare a clean window with a smeared one; that difference is the first-order question.

1. Cheap, nothing that slows the game: dev6's environment plus `EDEN_NX2_STUTTER_TRACE=1 EDEN_NX2_STUTTER_DETAIL_MS=80 EDEN_NX2_HITCH_FINISH_TRACE=1 EDEN_NX2_HOSTTIMING_STATS=1 EDEN_NX2_PRESENT_FENCE_TRACE=1 EDEN_NX2_FS_READ_TRACE=1 EDEN_NX2_TC_LOG=1 EDEN_NX2_MEM_TRACE=1`, and in a second terminal `typeperf "\Processor Information(_Total)\% Processor Performance" "\Processor Information(_Total)\Processor Frequency" -si 1 -o cpu.csv` (H4; or HWiNFO's package power and temperature). Read `HostTiming wait` (lateness p99 and max per second: H3), `Guest storage reads` and `Guest file reads` (loading or streaming: H5), the `Hitch finish trace` callers (section 2), `Texture cache: device memory ... (expected, critical)` and `Texture cache last 600 frames: ... downloaded` (section 3). Note where in the field Link is when the slow frames ramp up.
2. `EDEN_NX2_GUEST_THREAD_STATS=1` with `EDEN_NX2_STUTTER_TRACE=1` and dev6's environment (add `EDEN_NX2_GPU_PROFILE=1` only if the game still holds 20 fps with it): the main thread's and the workers' `game code`, `ready`, `waiting` by kind and "read-back N in M" in a clean and in a smeared window, and the `Async read-back per second` line (`readers waited on their fence N times, X ms (longest Y ms)`). H1 predicts longer read waits in the smeared window with the same GPU time; H2 predicts more `ready`; H4 predicts every guest and GPU thread's CPU time per frame rising together (and the typeperf frequency falling); H5 predicts `game code` rising alone.
3. If H1 holds: the same with `EDEN_NX2_READ_FAST=1`, and with `EDEN_NX2_EARLY_PAGES=1024`.
4. If H2 holds: `EDEN_NX2_HYBRID=1`, three runs against three.

## 2. Why `Scheduler::Finish` runs 17-79 times per 10 s

`Scheduler::Finish` (`renderer_vulkan/vk_scheduler.cpp`) submits and waits for the GPU. The stutter trace's `scheduler_finish` site (`nx2_stutter_trace.h:97`) counts every call on every thread. Per-caller attribution did not exist: both runtimes' `Finish()` (`vk_texture_cache.cpp`, `vk_buffer_cache.cpp`) forwarded to `scheduler.Finish()` with the wrapper's own `std::source_location`, so every texture or buffer cache caller looked the same. They now pass the caller's location through, and `EDEN_NX2_HITCH_FINISH_TRACE=1` prints the callers.

What the dev6 log already shows per 10 s window of the field (`Nx2StutterSummary` sites, ms):

| Window end | frames | over 33 ms | image_delete | texture_collect | scheduler_finish |
|---|---|---|---|---|---|
| 81.1 s | 253 | 199 | 1201 | 593.7 | 281 x 568.2 |
| 101.1 s | 213 | 206 | 919 | 224.8 | 12 x 215.7 |
| 111.2 s | 290 | 87 | 33 | 10.4 | 5 x 38.4 |
| 121.2 s | 354 | 1 | 0 | 12.7 | none |
| 131.2 s | 349 | 9 | 244 | 160.1 | 79 x 143.0 |
| 141.2 s | 300 | 94 | 55 | 62.6 | 17 x 52.7 |

`texture_collect` and `scheduler_finish` match to within about 15% in four of the five windows with finishes (the collector's time is its finishes); the 111 s window's five finishes come from elsewhere. A quiet collector costs 0.03-0.04 ms a call (12.7 ms in 354 calls). Neither correlates with over-33 frames: 79 finishes with 9 slow frames, 17 with 94, none with 1.

Callers, from the code, for Hyrule Field with these settings (`texture_download` and `buffer_download` appear in no summary, so the two sited download paths are not it):

| Caller | Per-frame? | Can it become a tick wait or go away? |
|---|---|---|
| Texture collector, a GPU-modified image it evicts: `texture_cache.h:245` (`RunGarbageCollector`, `Cleanup`; `:189` at arc-v21) | Whenever the cache is over its expected threshold; one per evicted image | Batched by `EDEN_NX2_HITCH_GC_BATCH` (one wait per pass or per 64 MiB). Fewer with `GC_CLEAN_FIRST` and with a larger budget (section 3). Replacing the batch's `Finish` by a wait for the batch's own tick: proposal P1. |
| Guest read of GPU-modified texture memory: `texture_cache.h:1714` (`DownloadMemory`, already one wait per call; `:1651` at arc-v21) | Per read of an image no early download covers | Wait for the tick of the recorded copies only: proposal P2. |
| Guest read of GPU-modified buffer memory. Host-mapped buffers (the usual case on an integrated GPU, `Buffer::Mapped()`): `FinishForHostRead` at `buffer_cache.h:554` and `:567` (`DownloadRangesMemory`) and `:4220` (`DownloadBufferMemory`). Unmapped buffers: `runtime.Finish()` at `:569` (`DownloadRangesMemory`), `:4158` and `:4240` (the last two sited `buffer_download`) | When no early download or read-fast lane serves the read | These are the read-backs of section 1; the early lanes already replace most of them. Proposal P2. `FinishForHostRead` used to hide its caller behind `vk_buffer_cache.cpp`; it names it now. |
| `DMACopy`, `buffer_cache.h:1537` | Only with GPU fence behaviour Balanced or Accurate (`should_sync`) | Default behaviour does not run it; check the setting. |
| `FrameGen::ProcessExtrapolated`, `Rebuild`, the frame-gen readback, camera motion resize: `present/frame_gen.cpp:292,376,708,750`, `vk_ocarina_camera_motion.cpp:865,960,1293` | No: only on a mode or size change | Leave. |
| `InlineKeptWords`, `vk_rasterizer.cpp:4578` | Only with the list verifier on | Leave. |

The finish trace settles which of these ran: it names the file and line.

## 3. Why `texture_collect` runs every frame, and the thresholds

`RunGarbageCollector` (`texture_cache.h:148`) runs every frame from `TickFrame`. In a quiet frame it costs 0.03-0.04 ms: it builds a candidate list from the LRU and, every 60 frames, re-sums the blocks' bytes over all images. That is not a hitch and not worth a cadence change. The 0.2-0.5 ms mean and the long tail are the finishes of section 2.

Why it evicts at all, with thresholds `expected 3686 -> 4352` at boot, then `expected 4050, critical 4818` from 72.9 s (`Memory margins: base 768 MiB, out of memory 0 MiB, block slack 557 MiB`; `Host memory: 27840 MiB of commit left, the device budget bounds the thresholds`):

- `critical = budget - min(margin + pressure + slack, budget/2)` (`texture_cache.h:691`) with `margin = min(budget/8, 1 GiB) = 768` gives a budget of exactly **6144 MiB**: the cap of `Device::CollectPhysicalMemoryInfo` for an integrated GPU (`vulkan_device.cpp:2523`: 4 GiB, 6 GiB with aggressive VRAM use), applied again in `Device::GetDeviceMemoryBudget` (`vulkan_device.cpp:2466`). The cap exists because a Radeon 780M reported a 21 GB budget and the cache filled that much RAM. Here RAM is 31.4 GiB with 27.8 GiB of commit free.
- The block slack climbs from 16 MiB (boot) to 557 MiB as the collector deletes images and leaves holes in blocks, which lowers `critical` from 5120 to 4818 MiB and `expected` from 4352 to 4050 MiB. Below the retention limit (`budget - slack`, 5587 MiB) the collector's candidates are images unused for at least 150 frames (`eviction_age.h:16`); from `expected` (4050 MiB) on it is in high-priority mode and also downloads and evicts GPU-modified ones.
- `total_used_memory` is the whole device-local heap's usage as the driver counts it (`GetDeviceMemoryUsage`, `vulkan_device.cpp:2411`): on an integrated GPU that includes the buffer cache, staging, upscaler and frame-generation images, not only textures. The dev6 log has no `Texture cache: device memory` line (it needs `EDEN_NX2_TC_LOG=1`), so how far over `expected` the usage sits cannot be read from it. 244 deletes in the 131 s window against 0 in the 121 s window says it sits at the edge and crosses it.

Should the thresholds or the cadence change on UMA? The cadence: no. The cap: probably yes, as a measured option. `EDEN_NX2_HITCH_UMA_BUDGET_MIB=<n>` lifts the integrated cap and the thresholds follow (critical is capped at 5/8 of the budget past 5 GiB: 5760 MiB for 9216, 7680 MiB for 12288; values above 16384 are taken as 16384 so ASTC is not left uncompressed). The cost is more system memory held as device memory; the host check (`HostCriticalMemory`, the `Host memory:` line) still bounds it. If this removes the evictions, the collector's finishes go with them and it is the simplest fix; if textures were never the pressure, it changes nothing, and the run says so.

## 4. `query_fence` and the game's queries

`query_fence` is a stutter category, not a query count (`nx2_profile.h:122-125`): `Zone::Fence` (the body of `FenceManager::SignalFence`, `fence_manager.h:86`), `ListSubmitWait` and `ListGpuWait` (`vk_buffer_cache.cpp:617,623`), and `GpuWait` (`MasterSemaphore::Wait`, `vk_master_semaphore.cpp:91,113`). In a hitch frame it equals the finish: frame 7782 had `back:GPU wait 38.3/5` and `scheduler_finish:5x38.4`; frame 5363 `query_fence=34.0` with back 34.2. The steady 140-239 ms per 10 s is every GPU tick wait of the three GPU threads plus fence creation, 1.4-2.4% of wall time summed over threads: a cost, not a hitch.

Are the game's occlusion queries waiting synchronously? Not on the GPU threads. A query report is registered as an operation that runs when its fence is reached (`query_cache.h:399` signals a fence with `g_signaling_query_report_fence`; the fence thread writes the report to guest memory and wakes waiters with `Nx2SemaphoreWaiters::OnWrite`). `fence_manager.h:91` makes that fence a delayed one on purpose, and its comment records that running the report early made Ocarina read a wrong value (white flashes on leaves and walls). So earlier or asynchronous resolution is not safe for Ocarina, and the path is already asynchronous everywhere else. The guest sees a query word that changes when the GPU got there, and it polls it: that is the "cached memory" polling of section 1, and it ends when the GPU is done. The log's warning `Samples query ran 1026 slots with no reset or report, dropped its count` (`vk_query_cache.cpp:237`) says the game leaves a samples counter open; it does not mean waits.

## What was implemented

All off unless set; documented in [switches.md](switches.md) with their measurement plans.

| Switch | Change |
|---|---|
| `EDEN_NX2_HITCH_FINISH_TRACE=1` | Diagnostic. `Scheduler::Finish` timed per caller (thread, file:line), table logged every 10 s as `Hitch finish trace`. The texture and buffer cache runtimes' `Finish` and `FinishForHostRead` take the caller's `std::source_location`. |
| `EDEN_NX2_HITCH_GC_BATCH=1` | The collector records each download it needs for an eviction, up to 64 MiB, and waits once per batch. The image is deleted after its batch lands, with the same marks as before; an image already queued is skipped before it uses an iteration. A mode downgrade inside a pass sees the memory later, so a pass may evict a few more images than it would have. |
| `EDEN_NX2_HITCH_GC_CLEAN_FIRST=1` | Among the oldest images (8 per allowed deletion) the ones that need no download go first. A skipped image still uses an iteration, so a pass can delete more clean images than before. |
| `EDEN_NX2_HITCH_UMA_BUDGET_MIB=<n>` | The integrated device budget cap, instead of 4/6 GiB. |

Not compiled: this work was done without building. The edits are small and mirror existing code (`DownloadMemory`'s batch, `std::source_location` already on `Scheduler::Finish`), but the lead's build is the first compile.

## Proposals, not implemented

- **P1, collector without a drain.** Record the eviction downloads, then wait for the submit's tick (`scheduler.Flush()` returns it; `Wait(tick)`) instead of `Finish`, which also allocates a new context and resets the flush state, and hand the images to the `gpu_sentenced_images` queue so their memory is freed after the tick. Saves the context reset and lets the back thread keep recording. Risk: the image must not be reused, re-registered or read by a guest read-back in between; `CollectStaleReferences` and `MarkStaleTransfer` exist for that but need a test with the verifiers on.
- **P2, read-backs wait for their tick, not a drain.** `DownloadMemory` (texture) and the buffer read paths call `Finish`, which waits for everything recorded before and after the copy. A read needs only its copy's tick: `Flush()` after recording the copy and `Wait(tick)`. The early lanes (`EARLY_*`, `READ_FAST`) already do this for pages they learned; P2 extends it to the pages they did not. Risk: ordering with the outside-render-pass context and the host-read barrier (`FinishForHostRead`); `EDEN_NX2_READ_CHECK=1` verifies the bytes.
- **P3, make read-backs not serial.** If H1 holds, the cheapest large gain is covering more pages with early downloads (`EARLY_AFTER=1`, a larger table) and fetching the pages the five workers read each frame in one batched download right after the dispatch that writes them (`EARLY_SUBMIT` does this per page). A table of the pages read by thread in a frame (a counter in `GPU::OnCPURead`, logged every 10 s with the page, the reading thread and whether an early download covered it) would show which reads are left. Not added: it needs a hook in the guest memory slow path and should wait for the traces above.
- **P4, thresholds that follow the machine.** If the UMA budget test works, make the integrated cap a fraction of the host's RAM and commit (a quarter of RAM, with `HostCriticalMemory` as the guard) instead of 4/6 GiB, and stop the block slack from lowering `critical` on an integrated GPU, where a hole in a block is system memory the process may use anyway.
- **Not worth doing:** a slower collector cadence (a quiet frame costs 0.03 ms), and earlier query resolution (unsafe for Ocarina, section 4).

## Measurement plan

Same save and route for every run: boot Ocarina of Time, load the Hyrule Field save, ride the first screen for 3 minutes after a 60 s settle; three runs per variant, alternating; plugged in, 1280x765. Base environment: dev6's, plus `EDEN_NX2_STUTTER_TRACE=1 EDEN_NX2_STUTTER_DETAIL_MS=80 EDEN_NX2_HITCH_FINISH_TRACE=1 EDEN_NX2_TC_LOG=1`.

| Variant | Adds | Read |
|---|---|---|
| A | nothing | `Hitch finish trace` callers; `texture_collect` ms and `scheduler_finish` count per 10 s |
| B | `EDEN_NX2_HITCH_GC_BATCH=1` | `scheduler_finish` count down by the images-per-pass factor; `texture_collect` ms; downloaded-eviction count unchanged |
| C | B and `EDEN_NX2_HITCH_GC_CLEAN_FIRST=1` | downloaded evictions down; no pop-in |
| D | `EDEN_NX2_HITCH_UMA_BUDGET_MIB=9216`, then `12288` | `Texture cache thresholds` up; `image_delete` and `image_create` per 10 s down; `Host memory: ... commit left` above 8 GiB |
| E | the runs of section 1 (H1 to H5) | the guest threads' `game code`, `ready` and waits, and package frequency, in clean and smeared windows |

Success is judged on over-33 ms and over-100 ms frames per 10 s window and on the p99 of the present interval (`nx2:vi2 last 600 presents`, `Shown frames`) over three runs, not on one window: windows of the same build differ from 1 to 94 over-33 frames. Images: `capture_frame` at the same three route points with and without each switch; any difference is a bug. Unprofiled fps and p99 only count from runs without `GUEST_THREAD_STATS` and `GPU_PROFILE`.
