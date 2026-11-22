# Design: the game's own reconstruction and motion on integrated GPUs

**This is a plan. The switches named below do not exist yet unless a line says "today" or
"exists".** Numbers are marked *measured* (with their source) or *estimate* (with how it was
worked out). An estimate is a reason to measure, never a reason to change a default: only rows
from `tools/bench` on a verified scene do that (AGENTS.md, rule 1).

Switch 2 games already compute what a generic upscaler or frame generator has to guess: a
reconstruction network tuned to the game, and in many games motion vectors, depth and jitter for
it. This design uses that work on integrated GPUs, in two parts that touch different files:

- **A. Reconstruction.** Run the game's own network fast on Intel's XMX and AMD's WMMA matrix
  units, so a game can render at 540p to 810p and output 1080p to 1440p through its own network at
  a cost an integrated GPU can carry.
- **B. Frame generation.** Generate frames by warping with the game's motion and filling what the
  camera reveals from depth, paced by the FLUID presenter. Target: 40 real frames shown at 120 on a
  15 W handheld, with input latency no worse than the game at 60 frames without generation.

Section 4 is the measurement plan: one command per switch for the owner, and the image checks.

## 1. What the games compute

**Ocarina of Time** (`0400486024DEC000`) has no motion vectors and no jitter. Its anti-aliasing
network is a per-pixel network held in registers, and it runs in the **output pass**: native
program `da6df3e54c716df2` (shader `6bd913e1f34e83eb`, the 1:1 pass at the scene size with the Max
Resolution add-on) issues 192 `HMMA.16816.F16` per invocation (measured: counted in a native dump
on the owner's PC). 144 of them start from a zero accumulator, and runs of up to eight share one
input operand (one input tile times eight weight tiles). The passes
[oot-network-bypass.md](../nx2/oot-network-bypass.md) calls the prepass (`8475d671e6967428`) and
"the network" (`42593194bb0b2b32`) have no tensor instructions, nor does the heavy dispatch (shader
`d9ee412f00ce399a`). So "No sharpening", which replaces the output pass with a copy, is what
removes the tensor math today. The output programs of the dynamic-resolution steps (Max Resolution
off) have not been dumped: their tensor shape is unknown.

Guarana rebuilds Ocarina's motion itself: the **motion guide** (half-size RGBA16F; xy the motion
to the previous frame in UV from the scene depth reprojected through the game's camera, plus Link's
pose with `EDEN_NX2_EXTRAPOLATE_ACTOR`; z the nearness `1 - depth`) exists today for frame
extrapolation ([frame-generation.md](../frame-generation.md)). It knows the camera and Link, not
other characters, particles or water.

**Games with a DLSS contract.** Fire Emblem: Fortune's Weave feeds its network B10G11R11 colour,
R16G16 motion, D32 depth and a jitter sample, validated in
[native-dlss-replacement.md](../native-dlss-replacement.md). Fast Fusion's inputs are known but not
proven. These games give full per-pixel motion, characters included. Neither has a bench scene, so
nothing in this design can be measured on them yet: every first step below is built so that
Ocarina of Time measures it, with the DLSS contract as a second provider of the same data.

## 2. Part A: the game's network at an integrated GPU's cost

### 2.1 How guest tensor instructions run today

`backend/spirv/emit_spirv_tensor.cpp`, per host:

| Host | Path | Per `HMMA.16816` |
|---|---|---|
| NVIDIA | 16x8x16 cooperative matrix in registers | one product, no memory traffic |
| AMD RDNA 3 (780M, 890M) | 16x16x16 cooperative matrix through shared memory | every lane stores A, B (columns 8-15 zero) and C to per-subgroup scratch, barrier, three cooperative loads, one product (K=16 rounded once), store, barrier, two loads back |
| Intel Xe2 (Arc 140V, B580) with `EDEN_NX2_INTEL_COOP_MATRIX=1` | 8x16x16 through shared memory | as AMD, but K=16 is split into two K=8 products per 8-row half so the fp16 accumulator rounds where the guest's does (Fritchii-Intel-Fixes 0008): four products, three barriers |
| Intel Xe-LPG+ (Arc 140T) | software, whatever the switch | its driver lists no shape the paths above take (below) |
| Intel by default | software | shuffles (or shared-memory gathers with `EDEN_NX2_SW_MATRIX_SHARED`) and per-lane fp32 FMAs, rounded to fp16 after each K=8 group |

Three facts shape everything after:

- **The Arc 140T has no 8x16x16 shape.** Asked directly (`vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR`, driver 101.9175, the owner's laptop, 2026-11-16), it lists three shapes: 8x8x16 with fp16 inputs and **fp32** accumulator and result, and 8x8x32 with signed or unsigned 8-bit inputs and 32-bit results. No fp16 accumulator and no 16-column shape, so `EDEN_NX2_INTEL_COOP_MATRIX` (8x16x16 fp16) and the A1 switches below do nothing there; they apply to Xe2 (Arc 140V, B580) and AMD RDNA 3. The 140T needs its own path (A1b). Its 8x8x16 has the guest's own N=8, so no zero columns, and its 8x8x32 int8 shape is what A3's INT8 would use.

- **The Intel preset hides the network.** `ApplyGamePresets` (`renderer_vulkan.cpp`) counts a GPU
  as "without matrix cores" unless it has the 16x8x16 or 16x16x16 path; the 8x16x16 path does not
  count. So on an Arc iGPU the Ocarina preset applies No sharpening even with
  `EDEN_NX2_INTEL_COOP_MATRIX=1`, and the output pass never runs. No row exists of the game's
  network on the Arc 140T.
- **The cost is the traffic around each product, not the product.** On the shared-memory paths
  each `HMMA` stores and reloads its operands; the network chains them in registers, so most of that
  traffic carries values another `HMMA` produced or will reuse.

### 2.2 Costs

| Pass and path | Cost | Kind |
|---|---|---|
| Output pass `6bd913e1f34e83eb`, B580 (discrete Xe2), software | 15.1 ms | measured (Fritchii-Intel-Fixes 0007, offline) |
| Same, 8x16x16 | 1.25 ms (75,795 -> 14,098 instructions) | measured (0007) |
| A hot 64x36 tensor pass, 8x16x16 without and with the K=8 split | 1.4 -> 2.0 ms | measured (0008) |
| Ocarina, Panther Lake 4 Xe3 cores, network at the 1440x810 step (Max Resolution off) | about 20 ms of 39.6 ms of game passes | measured, one run ([#98][98]) |
| Ocarina, Arc 140T, output pass, software, 2560x1440 | 30 to 45 ms | estimate: 15.1 ms times the B580's fp32 rate over the 140T's (about 13.7 / 4.8 TFLOPS) |
| Same, 1440x810 step | 8 to 12 ms | estimate: #98's 20 ms times the 4-core Xe3's fp32 rate over the 140T's (about 2.6 / 4.8) |
| Same, on its 8x8x16 matrices (A1b) | 2.5 to 4 ms (1440p), about 1 ms (810p step) | estimate: 1.25 ms times the B580's XMX rate over the 140T's (about 3x), and the 0007 ratio for the step; it assumes A1b costs per product what 8x16x16 does |
| Xe2 or RDNA 3, after the steps of 2.3 A1 | 30 to 50% below their 8x16x16 or 16x16x16 figure | estimate: 0008's split costs +43%; the zero accumulators and shared operands remove about half of the remaining scratch traffic |
| Same, host network (A3) | 1 ms or less at 1440p | goal, not an estimate: the network's arithmetic at a third of XMX peak |

Device rates are vendor peak figures, so every estimate above carries a factor of two either way.
The first measurement (A0) replaces the 140T rows.

### 2.3 Steps

Each step is a default-off switch with its own pull request, tests, `docs/switches.md` row and
bench suite. A step that does not win in a paired run stays off and says so.

**A0. Measure what exists, and say which path each program takes.** No new arithmetic.

- Suite `tools/bench/suites/game-network.json` (with this design): the network on (`oot_anti_aliasing`
  Game, presets off, frame generation off) in software, with `EDEN_NX2_SW_MATRIX_SHARED`, and with
  `EDEN_NX2_INTEL_COOP_MATRIX=1`, against No sharpening as the floor.
- `EDEN_NX2_TENSOR_LOG=1` (diagnostic, in A1's pull request): once per program with tensor
  instructions, its shader hash, the counts by depth, how many start from zero and how many reuse
  the previous operand, the path taken and why not a faster one (the shared-memory limit, the
  workgroup width, coherent memory, the device). A row whose log has no such line for
  `6bd913e1f34e83eb` did not run the network.
- If 8x16x16 wins with a clean image on the 140T, the preset question follows: count 8x16x16 as
  matrix cores in `ApplyGamePresets`, which turns No sharpening, Max Resolution and frame generation
  off on those GPUs; that is a separate pull request with its own rows.

**A1. Less traffic per product (`emit_spirv_tensor.cpp`).** Bit-exact against the same path unless
stated.

| Switch | What | Image |
|---|---|---|
| `EDEN_NX2_HMMA_ZERO_C=1` | An `HMMA` whose accumulator is the zero register starts from a zero matrix: no C stores, no C load | bit-exact |
| `EDEN_NX2_HMMA_K16_ONCE=1` | Intel 8x16x16: one product per K=16, rounding once as the 16x16x16 path does | changes rounding (2.4) |
| `EDEN_NX2_HMMA_SHARE_A=1` | Consecutive `HMMA`s with the same A operand and no barrier, store or branch between them load A once | bit-exact |
| `EDEN_NX2_HMMA_PAIR_N=1` | Two such `HMMA`s with different B fill the 16 columns of one product instead of zero padding: half the products for the paired ones | bit-exact |

These help every game's network on AMD and on Intel with the 8x16x16 path, not only Ocarina. Tests
in `src/tests/video_core/spirv_software_matrix.cpp` check the emitted SPIR-V (stores, loads,
barriers and products per `HMMA`), as the existing 8x16x16 tests do, and a CPU model of the
arithmetic checks the rounding (2.4). The first pull request (#226) built `EDEN_NX2_TENSOR_LOG`,
`EDEN_NX2_HMMA_ZERO_C` and `EDEN_NX2_HMMA_K16_ONCE`; `SHARE_A` and `PAIR_N` are the second.

**A1b. The Arc 140T's own path (`EDEN_NX2_INTEL_COOP_MATRIX_8X8`).** The guest's 16x8x16 HMMA as two 8x8x16 products (one per 8-row half) with fp32 accumulators: the B tile is the guest's 16x8 exactly, no zero columns, and C goes to shared memory as fp32. Each lane rounds its D values to fp16 once per HMMA: as the guest for K=8, once instead of twice for K=16 (`K16_ONCE`'s rounding, which the CPU model bounds). Exact midpoint rounding would need an fp16 accumulator type the driver does not list. `EDEN_NX2_HMMA_ZERO_C` applies; a shared A is the next step, as for A1. Built with SPIR-V tests; unverified on the hardware.

**A2. Fewer passes (ports).** Lichii #864 and #1985 skip a game's network passes whose output only
feeds a pass a replacement overwrites (measured upstream: Duskbloods on a Steam Deck 56.6 -> 40.4 ms
a frame; the 890M's network passes 7.6 ms of a 24 ms frame). Lichii #1941 skips Ocarina's unused
intermediate dispatch once No sharpening's copy works, which [#98][98] asked for. Ported, not
rewritten, each behind the switch its upstream pull request uses or a default-off one.

**A3. The network as one host kernel (`EDEN_NX2_OOT_NETWORK_HOST`).** The output pass is a stack of
per-pixel layers. A host compute kernel recognises the program by native key (as
`FindOcarinaUpscalePass` does), reads the weights from the guest memory the pass binds, at run time
and never stored or shipped, and runs the layers back to back on cooperative matrices with the
activations kept on chip, the layers fused and one pass instead of the game's chain. Values: `fp16`
first; `int8` (XMX and WMMA both have it) only if the per-layer scales keep the error inside the
budget of 2.4. Before any code, the layer shapes are decoded from the owner's dump into a local note
(not into the repository: it is game code), and a CPU reference of the kernel is checked against
the guest-exact model on synthetic weights. This step is per game: Ocarina first, the DLSS-contract
games' networks after.

**A4. Render low.** With Max Resolution off the game chooses a step between 854x480 and 1440x810
from the GPU time it measures, and runs a step's own output program. The decisive comparison for
"render low, output through the game's network" is: Max Resolution off, the network on, the best
path from A0 to A3, against today's preset (Max Resolution on, No sharpening). It needs the step
programs dumped first (`EDEN_NX2_SHADER_DUMP=<dir>`, a few seconds at each step) to know their
tensor shape, and a pinned step in both arms (`EDEN_NX2_GPU_TIME_SCALE` or `EDEN_NX2_TARGET_FPS`,
[native-dlss-replacement.md](../native-dlss-replacement.md)) so the arms compare the same
resolution. [#98][98]'s black scene comes from the bypass at those steps; with the network on, the
bypass is off.

### 2.4 The error budget

The guest network is already fp16 with a rounding after each K=8 group. The reference is therefore
the guest's own arithmetic, modelled on the CPU, and the budget is how far a host path may stray
from it compared with how far the guest itself strays from exact arithmetic.

- **CPU model** (`src/tests/video_core/hmma_numerics.cpp`, with A1): `HMMA.16816.F16` as the guest
  computes it (fp16 operands, products summed in fp32, rounded to fp16 after each K=8 group), the
  K16-once variant, an fp32-accumulator variant, and a double-precision reference. Synthetic stacks
  shaped like the output pass (layers of 16 and 64 channels, seeded weights and inputs, ReLU between
  layers) give each variant's largest and mean error against the reference. The test fails when
  K16-once strays further from the reference than the guest-exact model by more than one fp16 unit
  in the last place on any output.
- **INT8 (A3)**: per-layer scales chosen on the CPU from the weights; accepted only within the same
  bound on the same synthetic stacks, and then within the live check below.
- **Live**: `EDEN_NX2_DLSS_CONTRACT_TRACE` logs descriptors, not tensors, so live checks compare
  pictures. Two runs of the same arm give the noise floor (Guarana's Intel replays already differ
  from run to run, PSNR 37 to 45 dB); a switch passes when its picture is within that floor of the
  baseline's, and its edges overshoot no more than the baseline's (the output pass sharpens hard,
  so a wrong rounding shows first as halos or speckles).

## 3. Part B: frames from the game's motion

### 3.1 Providers

The warp takes one input format whatever the game: the half-size guide (xy motion to the previous
frame in UV, z nearness). Providers:

1. **Ocarina of Time**: the existing guide (camera and Link). It is produced today only with
   `EDEN_NX2_EXTRAPOLATE`; B1 asks for it without turning extrapolation on.
2. **The DLSS contract** (B4): the game's own motion vectors and depth, written into the same
   format by one small pass (motion scaled from render pixels to UV, jitter removed, depth to
   nearness). Characters, particles with motion vectors and animated surfaces then move right.

### 3.2 The warp and the fill

Notation: `m(p)` the guide's motion at `p` (to the previous real frame), `z(p)` its nearness, `q` an
output pixel, `t` the in-between frame's position from the previous real frame (`0`) to the newest
(`1`), `advance` a prediction's distance past the newest frame in real-frame intervals.

- **One solve for both directions.** A surface point at `p` in the newest frame is at
  `p + (1 - t) m(p)` at time `t` and at `p - advance m(p)` after it. Both are today's fixed-point
  solve in `frame_extrapolation.comp` (`p = q + a m(p)`), with `a = -(1 - t)` to interpolate and
  `a = advance` to extrapolate.
- **Interpolation (B1, as built in `present/game_motion_warp.h`).** Two solves per pixel: from the
  newest frame with `a = -(1 - t)` on its guide, and from the previous frame with `a = t` on the
  previous guide, its own motion reversed standing for its forward motion (constant velocity).
  - The newest frame's solve converged, and the previous one did not or found the same surface
    (nearness within 10%) or a farther one: the newest frame's point, mixed by `t` with the same
    point in the previous frame, `p + m(p)` (exact by the guide's definition), where that frame's
    nearness there says it saw the point; else the newest frame's point alone.
  - Both converged on different surfaces and the previous frame's is nearer: it is in front at time
    `t` (an occluder's leading edge) and the previous frame supplies the pixel alone.
- **Disocclusion fill from depth (B1, roadmap A2).** Only the previous frame's solve converged: an
  occluder moved over `q` in the newest frame, and the previous frame, which saw behind it,
  supplies the pixel. Neither converged: the farther of the two solves' last estimates (the
  background) wins, as today's `behind` rule picks.
- **Later: the exact forward motion.** The previous frame's forward motion is its own depth
  reprojected through the inverse of the newest frame's camera step (camera motion only; exact for
  static surfaces), which needs the camera matrices beside the guide. B1's constant velocity is
  right for steady camera motion and wrong where it turns or stops within one real frame interval.
- **Extrapolation (B2).** Ahead of the newest frame nothing has seen what the camera will reveal,
  except where the camera's own translation reveals what the previous frame showed (parallax): the
  same reprojection fills those pixels; the rest keep today's background rule.
- **HUD.** Today's mask (`hud_reference` against `hud_composite`): HUD pixels come from the newest
  frame, unmoved. Scene pixels must not drag it along, as extrapolation's rule says: a source on
  the mask (the newest frame's, standing for both: the HUD stays put) gives way to the other
  frame's; where every source a pixel uses lies on it, the scene under the HUD
  (`EDEN_NX2_EXTRAPOLATE_HUD_FILL`) or the newest frame's colour there, unmoved.

The arithmetic lives in a header shared by the test (C++ on plain arrays: pans, a foreground over a
moving background, a disocclusion in each direction, a cut) and mirrored line by line in the shader;
the test checks positions to a tenth of a pixel, that no pixel is left without a source when either
frame saw it, and that the background, never the occluder, fills a disocclusion.

### 3.3 Pacing

The FLUID presenter as today: `EDEN_NX2_FLUID_INTERP` puts in-between frames on the vblank slots
before the newest real frame (`RecordFluidInterpolated`), `EDEN_NX2_FLUID=2` shows a real frame at
its first slot and predictions after it, and `EDEN_NX2_CADENCE` holds real frames to an even share
of the shown rate (40 for 120). B1 changes what fills a slot, not when.

### 3.4 Costs

Measured on the Arc 140T at 2560x1440 and 120 Hz with `EDEN_NX2_FLUID_INTERP` (the play
configuration, Hyrule Field, 2026-11-16, `gpu_costs` rows in the lab logs):

| Row | Cost | Kind |
|---|---|---|
| `frame_gen_process` (history copies and optical flow), per real frame | 0.64 to 0.88 ms | measured |
| `frame_gen_interpolate`, per in-between frame | 0.86 to 0.99 ms | measured |
| The game's submissions, per real frame | about 43 ms | measured |
| Guide pass, per real frame | 0.1 to 0.2 ms | estimate: half size, four depth taps a texel |
| B1 warp, per in-between frame | 0.6 to 1.0 ms | estimate: 15 to 20 texture taps a pixel, against the measured interpolation's 9-tap median plus warp |
| `frame_gen_extrapolate` | not measured | |

Optical flow is under 1 ms of a 43 ms real frame, so B is not a GPU-time saving: at 40 real frames
and two in-between frames each, B1 should land within half a millisecond a real frame of today
(estimate). Its case is the picture (the camera's exact motion instead of a flow estimate on
foliage and the HUD; disocclusions filled from the frame that saw them) and, through B2, latency.

### 3.5 Latency

The target compares 40 real frames shown at 120 with 60 real frames shown at 60, both from the
input being read to the first shown frame that changes (onset). Arithmetic (estimate; real frame
time and the vblank wait only, the game's own queue the same in both):

| Configuration | Onset | Against 60 without generation |
|---|---|---|
| 60 real, no generation, 60 Hz | 16.7 ms frame + 8.3 ms average vblank wait = 25.0 ms | |
| 40 real, extrapolation (`FLUID=2`), 120 Hz | 25.0 + 4.2 = 29.2 ms | +4.2 ms |
| 40 real, interpolation, 120 Hz | the first in-between frame already carries a third of the change: 29.2 ms onset, the full change 2 slots later (45.8 ms) | +4.2 ms onset, +20.8 ms full |

So the target is reachable only within about 4 ms by position in the pipeline, with extrapolation
or with interpolation's onset; meeting it outright needs a shorter queue than the 60 frame baseline
(the cadence's phase lock, which keeps real frames done about 3 ms before their slot; a cap on
the GPU queue, `EDEN_NX2_QUEUE_LIMIT`, showed no gain and was removed). The presenter's part is
`real frame age at display` in the `Fluid presenter` line. The whole of it, from when the game
reads the input to the first shown frame whose picture moved, is B3's probe, the debug server's
`input_latency`; section 4 says how to measure both arms of the target with it.

### 3.6 Steps

| Step | Switch | What |
|---|---|---|
| B1 | `EDEN_NX2_FG_GAME_MOTION=1` | With `EDEN_NX2_FLUID_INTERP`: in-between frames from the guide and the depth fill (3.2) instead of the optical-flow chain; the guide is made without `EDEN_NX2_EXTRAPOLATE`; each real frame's guide is copied. As built, the optical flow still runs as the fallback for frames without two guides; skipping it when a batch needs none is the next step |
| B2 | `EDEN_NX2_FG_GAME_MOTION=2` | B1, and extrapolated frames (`EDEN_NX2_FLUID` without interpolation) take the parallax fill |
| B3 | debug server `input_latency` | the onset probe of 3.5: built (a right-stick step, then the first shown frame whose picture moved, timed from when the game read the step) |
| B4 | `EDEN_NX2_FG_GAME_MOTION` with a DLSS-contract game | the second provider (3.1); measured once such a game has a bench scene |
| B5 | ports | from the HUD passes of CheezerWheezer's Lichii fork (`f0ff1554ba`, `57dc6d8d0c`), which feed AMD frame generation (not in Guarana): their rule that the HUD carries no motion, as B1's (scene pixels no longer drag the HUD along, 3.2; #258). Open: the case they were written for, a scene smaller than the output. Guarana's guide path keeps the mask only when the presented image and the scene copy match in size; otherwise it logs `Frame extrapolation: no HUD mask for frame N (...): the presented image and the scene differ in size`, and the HUD moves with the scene in generated frames. That would happen with a scene below the output size (Max Resolution off, A4); not verified. The shaders already read both images by UV, so the fix would keep the mask at any size: look for that line in the B suite's and A4's logs first. Still to port: Lichii #1493 (generated frames in HDR) and #1945 (one set of intermediates, about 70 MiB less at 1440p) |

## 4. Measurement plan

For the owner, on the Arc 140T laptop, plugged in, nothing else running (no compiler, no other
session's build: check for `cargo`, `ninja`, `clang` first). The build is the pull request's CI
artifact (`guarana-pr<N>-<commit>-windows-x86_64`), unpacked into a folder that has started once
already: a new folder triggers the Windows microphone prompt and its first boot can hang
([#199][199]). Set `@common` as in [BENCHMARKING.md](../BENCHMARKING.md) (Hyrule Field, the third
file-select entry), then per suite a dry run and the run:

```powershell
python -I tools/bench/bench.py suite tools/bench/suites/game-network.json @common --dry-run
python -I tools/bench/bench.py suite tools/bench/suites/game-network.json @common
```

| Suite | Arrives with | Compares |
|---|---|---|
| `game-network.json` | this design (A0) | network on: software, shared-memory software, 8x16x16 (a no-op on the Arc 140T: its row equals software, and its log line says why); No sharpening as the floor |
| `game-network-a1b.json` | A1b | the Arc 140T's own shape: software against 8x8x16 with fp32 accumulators (`EDEN_NX2_INTEL_COOP_MATRIX_8X8`), alone and with `EDEN_NX2_HMMA_ZERO_C` |
| `game-network-a1.json` | A1 | 8x16x16 alone, then with each A1 switch, then all. For a GPU that lists 8x16x16 fp16 (Xe2) or 16x16x16 (RDNA 3, where only `ZERO_C` applies), not the Arc 140T |
| `game-network-render-low.json` | A4 | Max Resolution off at a pinned step with the network on, against the preset |
| `game-motion-fg.json` | B1 | `FLUID_INTERP` with optical flow against B1, and extrapolation as the latency reference; the 120 Hz set's frame generation pinned, fullscreen, pacing counters on (B2 adds its arm) |

Every arm of an A suite emits new SPIR-V for the network, so its first boot compiles cold: run each suite twice in one session and read only the second pass. On the Arc 140T the 8x16x16 arms are no-ops: its A0 rows compare software with the shared-memory gather and say what the network costs on this GPU at all, and `game-network-a1b.json` measures its 8x8x16 path.

What counts:

- A row counts when it is valid with `scene_ok: true`, and, for the A suites, when its log has the
  `EDEN_NX2_TENSOR_LOG` line for the output pass and no `anti-aliasing network bypassed` line.
- Each suite runs its arms back to back; run it twice in one session (A, B, A, B) and report the
  paired differences with both baselines. A difference smaller than the gap between the two baseline
  rows is no result.
- A: the output pass's GPU time from the row's `gpu_costs` (`work`, shader `6bd913e1f34e83eb`), the
  game's fps and p99. B: `Fluid presenter` slots on time and misses, `Shown frames` within 2 ms and
  p99, `frames received`, `real frame age at display`, and the `frame_gen_*` rows.
- Nothing becomes a default from this plan alone: a default needs these rows plus the image checks.

Onset latency (B3, the target of 3.5), on the same build after the B suite. Start Ocarina of Time
once per arm, with `EDEN_NX2_DEBUG_SERVER=127.0.0.1:6545` and the arm's switches from
`game-motion-fg.json` (its base `env` plus the arm's own, and frame generation on with FSR3 in the
graphics settings). Then start it once more without frame generation and without the FLUID switches,
on a 60 Hz display, as the reference. In Hyrule Field, standing still with nothing moving nearby and
the camera free to turn right, run this five times per arm, a few seconds apart:

```powershell
python -I tools/nx2/debug_mcp.py --server 127.0.0.1:6545 --call input_latency
```

A call counts when `input_read` is true, `onset_ms` is positive (a frame shown before the game read
the step cannot show it) and the frames before the onset stay under half the `threshold`; otherwise
stand somewhere stiller or raise the threshold. Report each arm's
median `onset_ms` and `real_onset_ms`, and their differences from the reference's. The target is an
onset within about 4 ms of the reference's at 40 real frames shown at 120 (3.5). Times are taken
when the present call returns, so the display's own lag comes on top in every arm alike.

Image checks (screenshots the suite saves, plus `EDEN_NX2_FRAME_DUMP` where named):

1. **A, any switch**: the suite's second screenshots of baseline and switch side by side at 200%:
   edges of the HUD hearts and text, Link's outline, the fence and grass against the sky. A halo,
   speckle or one-pixel column that the baseline does not have fails the switch.
2. **A, rounding** (`HMMA_K16_ONCE`, A3): the Ocarina title movie for a pink line or speckled
   columns (the symptom of Fritchii-Intel-Fixes 0009), and a dark interior (banding).
3. **A, picture distance**: two baseline screenshots give the noise floor; the switch's screenshot
   against the baseline must be within it (`scene_diff` in the rows).
4. **B, motion**: a slow camera pan and a fast one in Hyrule Field with `EDEN_NX2_EXTRAPOLATE_DEBUG=1`
   (red = hole, blue = off the scene): fewer red pixels at the edges of trees and Link than the
   optical-flow arm, no tint with the debug switch off.
5. **B, what the guide does not know**: an enemy, a bird or the water moving while the camera holds:
   ghosting or doubling there is the known limit of the Ocarina provider (3.1), recorded, not a
   failure of the warp.
6. **B, cuts and HUD**: through a door and into the pause screen and back: no frame of the old scene
   after the new one; HUD text steady during a pan,
   and no copy of the hearts or buttons beside them (`EDEN_NX2_EXTRAPOLATE_DEBUG=1`: yellow where the
   HUD rule acted).

## 5. Ports and what is not ported

Already in Guarana: Lichii #779 and Fritchii-Intel-Fixes 0007 and 0008 (the 8x16x16 path, opt-in),
#1283 (native fp16 rounding in software), #1694, #1698, the native-key pass recognition (#1871) and
the guide. To port, in this order: Lichii #864 and #1985 (A2), #1941 (A2), #1243 (a tensor bench
in `tools/shader-sweep` that measures the paths without the game, with a CPU reference: the best
home for A1's numbers between owner runs), Fritchii-Intel-Fixes 0009 if the title movie shows its
symptom with 8x16x16, and B5's two. Not ported: NVIDIA DLSS frame generation and Reflex, NVIDIA
optical flow (NVIDIA only), Lossless Scaling (image only, no game data). The survey that found these
is summarised in the pull request that adds this page.

## 6. Order and lanes

A (shader recompiler, `video_core/oot_*`, `src/tests/video_core/spirv_software_matrix.cpp`) and B
(`renderer_vulkan/present/`, `host_shaders/`, the rasterizer's guide) touch different files and run
in parallel. Both add rows to `docs/switches.md` and tests to `src/tests/CMakeLists.txt`: the
second to merge rebases. Order inside A: A0 rows, A1, A2, then A3 and A4 on the rows A0 and A1
give. Inside B: B1, B3, B2, then B4 and B5.

[98]: https://github.com/karimalsalah/guarana/issues/98
[199]: https://github.com/karimalsalah/guarana/issues/199
