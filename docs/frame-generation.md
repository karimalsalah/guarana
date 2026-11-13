# Frame generation (extrapolation and the FLUID presenter)

Ocarina of Time only. Switches are in `docs/switches.md`.

## Pipeline

1. Per scene frame the rasterizer records a half-size guide: RGBA16F, xy the motion to the previous frame in UV (camera, plus Link's pose when `EDEN_NX2_EXTRAPOLATE_ACTOR` is on), z the nearness `1 - depth` of the nearest texel of each 2x2 block (`ocarina_motion_guide.comp` on bypassed frames, `ocarina_extrapolation_guide.comp` after the upscaler's motion pass). `IsCameraCut` marks a frame whose camera moved the far plane over a quarter of the screen, or whose previous camera is not the last frame's.
2. `RasterizerVulkan::OcarinaExtrapolationSource` pairs the guide with the HUD images: the scene before the HUD and the presented image after it. Without both there is no HUD mask (the log says why: `Frame extrapolation: no HUD mask for frame ...`).
3. `FrameGen::ProcessExtrapolated` decides per real frame whether it can be predicted (a source, no cut, a screen rectangle).
4. `RecordFluidFrames` (`renderer_vulkan.cpp`) records, inside the real frame's own submission, one `frame_extrapolation.comp` dispatch per slot from the first slot the frame can be done for until the slot the next real frame is expected for. A prediction moves each pixel p to `p - advance * motion(p)` (solved as a fixed point, two steps, with the nearer neighbour winning and a fallback for holes), where `advance` is the slot's time minus the frame's recording time in real frame intervals, at most 3. Pixels of the HUD are copied unmoved.
5. `PresentManager::FluidPresentThread` shows, at each slot, the newest real frame's frame for the slot whose GPU work is done. A frame that is late or whose copy is still queued moves to the next slot unless a later frame of its batch or a newer one is waiting; the frames of older real frames are dropped when a real frame without predictions (a cut, no motion, HDR) is shown.

## Known limits

- A prediction depends on the real frame, so it is done when the real frame is: a real frame whose GPU work finishes after its slot leaves that slot to an older prediction or nothing. Nothing can be recorded earlier.
- `EDEN_NX2_FLUID=1` predicts by the whole latency (recording to slot), so with the GPU latency of a GPU-bound iGPU every shown frame is extrapolated 0.5 to 2.5 frame intervals; `EDEN_NX2_FLUID=2` subtracts the latency a real frame could be shown at. Compare them. Real frames at least as fast as the slots are shown as drawn at their first slot (`EDEN_NX2_FLUID_PREDICT_ALL=1` for the old behaviour).
- Motion is the displacement between the last two game frames. Fixed-step motion (the game's 20 Hz logic) rendered at a varying rate is predicted with the wrong speed; a jump (animation restart, teleport at the same camera) is not a camera cut and is carried on.
- Parallax of surfaces the camera reveals has no source pixel: the shader fills it from the neighbour that moved away (red with the debug tint).
- Translucent effects, particles and shadows follow the surface behind them. Link's overlay is rasterized at half size, so his silhouette edge is a pixel or two off.
- The prediction is SDR 8-bit; with HDR output prediction is paused and real frames are shown.
- Below 10 frames per second the interval is clamped to 0.1 s and the advance is overstated.
- The pool is 7 frames: a real frame, the previous batch's predictions still waiting for their slots and the frame on screen can leave nothing for the new predictions (`cut short`).

## How to test

Run with `EDEN_NX2_EXTRAPOLATE=1 EDEN_NX2_FLUID=1 EDEN_NX2_SHOWN_STATS=1` in Hyrule Field at the same spot and time as earlier runs.

1. Screenshots during a camera pan, three times: normal, with `EDEN_NX2_EXTRAPOLATE_DEBUG=1`, with `=2`. Cyan is moved, red a hole, magenta too far, blue off the scene, yellow a scene pixel that would have dragged the HUD, green HUD. The bar on the top is the advance. The normal screenshots must show no red or blue fringe; if they do and the debug switch is off, the cause is not the tint.
2. The `Fluid presenter (10 s ...)` lines. First line: `no frame ready` is split into `scheduled but their GPU work not done` (the real frame was late for its slot: a latency problem) and `nothing scheduled` (the batch ended before the next real frame: raise `EDEN_NX2_FLUID_SPARE`). Third line: `not predictable` (cuts, menus, HDR), `fewer predictions than slots for lack of a free frame` (the pool), `ended by the prediction limit`, `shown a slot later` (frames saved by the roll), `predicted from an older real frame` (spares shown), `dropped as older than a real frame no prediction follows`.
3. `Shown frames` (`EDEN_NX2_SHOWN_STATS`): share within 2 ms of the median and p99.
4. `Frame extrapolation: ... HUD mask on N of M predictable frames` every 600 frames; M small against N means the camera or the guide is missing, N small against M means the HUD mask is off (the line before it says why).
5. Cut and pause checks: walk through a door, open the pause screen, return; with the debug switch off no frame of the old scene may flash after the new one.
6. A/B in one session through the environment: `EDEN_NX2_FLUID=1` against `=2`, `EDEN_NX2_FLUID_SPARE=0` against `2`, `EDEN_NX2_EXTRAPOLATE_HUD_DILATE=0` against default.
