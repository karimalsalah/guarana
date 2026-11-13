# Frame generation

Notes on how frames reach the display when frame generation is on. Switch rows are in [switches.md](switches.md).

## The FLUID presenter

`EDEN_NX2_FLUID=1` makes the present thread show one frame on every slot of the display (a slot is `divisor` vblanks, 2 on a 120 Hz display for 60 Hz slots). Between real frames the frames shown are extrapolated from the last real one. `RecordFluidFrames` (`video_core/renderer_vulkan/renderer_vulkan.cpp`) records a prediction for each slot from the real frame's first slot through the one the next real frame is expected for, when the real frame is recorded: the motion inputs are rewritten with every frame, so predictions cannot be recorded later. `FluidPresentSlot` (`vk_present_manager.cpp`) then shows, for each slot, the newest real frame's frame whose GPU work is done. A slot with none is a miss: `N with no frame ready` in the `Fluid presenter (10 s, ...)` line.

With `EDEN_NX2_SHOWN_STATS=1` two lines every 10 s say how it went:

- `Fluid presenter (10 s, ...)`: slots, slots shown on their slot, slots missed (and why: no frame ready, last copy still queued, slept through).
- `Shown frames: ...`: the intervals between frames that reached the swapchain, generated ones included, and `% within 2 ms of the median`.

## The frame predictor (`EDEN_NX2_PREDICT`, experimental)

The schedule above covers the slots up to the one where the next real frame is expected, using the smoothed real-frame interval. Real-frame intervals in Ocarina of Time on an Arc 140T (about 22 game frames per second) run from 25 to 110 ms, so the next real frame often comes after the last prediction, and slots miss.

`video_core/nx2_frame_predictor.{h,cpp}` is a model that learns the interval online:

- **Target**: the interval between two real frames being recorded, in ms. The model predicts the next one after each frame.
- **Features**, all known when a frame is recorded: the last 6 intervals relative to their EWMA, the GPU latency the presenter measured (record to GPU work done) and its change, the guest's read-back wait since the last frame and the number of waits (`VideoCore::AsyncReadback::g_counters`), GPU ticks submitted and not finished, where in its slot the frame was recorded, and the EWMA level.
- **Two predictors run on every frame**: the EWMA baseline (weight 0.25) and recursive least squares (forgetting factor 0.99, 14 weights) on the residual over that EWMA, so a model that has learned nothing is the baseline. The one with the lower mean absolute error over about the last 16 frames is used: the model takes over when it is 10% lower and gives back when it is 5% higher (at least 24 scored intervals first). A frame costs about 0.8k multiplications.
- **Safeguards**: updates are Huber-clipped (a spike moves the weights by a few typical errors), the weights are within +-4 and every covariance entry is bounded (no windup when a feature never changes), a gap over 500 ms restarts the stream without discarding the weights, and a non-finite value resets the model (`resets` in the log).
- **p90**: each predictor keeps an offset to its prediction, moved by stochastic descent of the pinball loss for the 0.9 quantile of its error. About 90% of the intervals should come at or before the prediction plus this offset (`p90 covers` in the log).

A linear model is the choice because it converges in tens of frames (a 10 s window at 22 fps has 220), is deterministic, needs no learning rate and has the same worst case as the baseline. The lags give away alternating patterns (33 and 50 ms), and the read-back wait, latency and ticks in flight can give away spikes. A 2-layer MLP trained by SGD would add what a linear model cannot (a spike that needs two signals together) at the cost of tuning, slower convergence and no guarantee of the baseline's worst case; it is not built.

### What it changes

| Value | Predictions | A real frame nearly done | Log and `status` |
|---|---|---|---|
| `1` | Recorded through the slot the next real frame is expected for at the p90 (`Cover`), at most 2 slots past the old reach and never more than `FLUID_MAX_PREDICTIONS`; fewer when the model expects the frame sooner | The present thread waits up to 1 ms (adapts between 0.3 and 2 ms) for a scheduled real frame, or its predictions, whose GPU work is expected done by then, instead of showing an older batch's prediction | yes |
| `2` | As `1` | Not waited for | yes |
| `3` | As without the switch | Not waited for | yes: counts the misses the predictor's schedule would have avoided |

Unset, or `0`, leaves all of it out: no call changes what the presenter does.

### Reading it

Every 10 s (no other switch needed):

```
Frame predictor (10 s, EDEN_NX2_PREDICT=1): rls in use (2 switches, 0 resets), 213 intervals, mean 45.1 ms, next 41.0 ms (p90 52.3); interval error, in use / rls / ewma: mean 4.10 / 4.10 / 9.30 ms, p90 9.80 / 9.80 / 18.20 ms; p90 covers 89%
Frame predictor (10 s): 1200 slots, 12 with no frame ready; 31 slots shown by a prediction past the baseline's reach, 4 misses the predictor's reach would have avoided; 4 holds for a nearly done real frame, 3 landed, 1 timed out, 4.2 ms waited
```

(Numbers are an example of the format, not a measurement.) The first line is the model: which predictor is in use, how often it switched, the mean and the 90th percentile of the absolute error of the interval prediction for the model in use, the RLS model and the EWMA baseline, and the share of intervals within the p90 prediction. The second is the schedule: `with no frame ready` is the same count as in the `Fluid presenter` line; `shown by a prediction past the baseline's reach` are slots the old schedule would have missed that a prediction filled (mode 1 or 2); `misses ... would have avoided` are misses that fall in slots the predictor's reach included and the old one's did not (mode 3 shows what 1 or 2 would do; in mode 1 or 2 it counts misses that remain despite the longer reach, which needs a free frame to predict into).

The debug server's `status` (`docs/nx2/debug-mcp.md`) has the same under `frame_predictor`: `mode`, `model`, `switches`, `next_interval_ms`, `next_interval_p90_ms`, and `total` (since the game started, no p90s) and `last_window` with the interval errors, `slots_no_frame`, `slots_avoided`, `misses_avoidable` and the hold counts.

### Measuring it

Nothing here has been run. The predictor is judged by the slots it saves without costing real frames.

1. Setup as for the `EDEN_NX2_GPL` plan in [switches.md](switches.md): Arc 140T, plugged in, nothing else running, same save and route (ride into Hyrule Field and stand in the scene that gave 55 to 141 misses per 10 s). `EDEN_NX2_FLUID=1`, frame generation with extrapolation as configured for FLUID, `EDEN_NX2_SHOWN_STATS=1`. The debug server on to read `status`.
2. Runs A (no `EDEN_NX2_PREDICT`), B (`EDEN_NX2_PREDICT=1`) and C (`EDEN_NX2_PREDICT=3`), alternating A, B, C, A, B, C, A, B, C, each 150 s, with the first 30 s dropped (the model needs a few hundred frames). Paired: the same route within a pair.
3. Per run, over the lines after the first 30 s, take:
   - `Fluid presenter` lines: slots missed with no frame ready per 10 s, and the whole missed count; `frames received` (real frames per 10 s: the extra predictions cost GPU time, and a lower figure in B than in A is a cost); `real frame age at display`.
   - `Shown frames` lines: the `% within 2 ms of the median`, p95 and p99.
   - `Frame predictor` lines (B and C): interval error of the model against the baseline, `p90 covers`, `switches`.
4. B beats A when its `no frame ready` is lower in most pairs (paired difference, not only the means) and the within-2-ms share is higher, with `frames received` and the game's fps not lower by more than run to run noise (A against A, the first and the second). C against A should show no difference in `no frame ready`; its `would have avoided` is the most B can save, and B's `shown by a prediction past the baseline's reach` should come near it.
5. If the interval error of the model is not lower than the baseline's in a game that does not show its pattern, `rls in use` should stay off or flip rarely; a count of `switches` in the tens per 10 s means the two are level and costs nothing.
6. Failure signs: misses lower but `frames received` lower too (the extra predictions slowed the real frames: use `2` or lower `FLUID_MAX_PREDICTIONS`), `late` copies up in the `Fluid presenter` line with `1` against `2` (the hold waits too long), `resets` above 0.

### Tests

`src/tests/video_core/nx2_frame_predictor.cpp` runs the model on synthetic interval streams: steady (no worse than the baseline), alternating 33 and 50 ms and a 3 frame cycle (far better), unpredictable spikes (no worse), spikes that follow a read-back wait (far better), a level change (followed in 30 frames), garbage (NaN, infinities, negative, huge: finite, weights and covariance bounded, useful again afterwards), a pause, and the planning, slot counts, ten second window and hold arithmetic of `FramePredictor` on plain nanoseconds.

### Limits

- Intervals are measured where the real frame is recorded, so a stall before the recording (the guest waiting) shows up as a long interval the model can only learn from its signals.
- Predicting further out costs GPU time on an integrated GPU; each prediction not shown is wasted. The reach is limited to 2 slots past the old one for that reason.
- The hold is a busy wait of the present thread of up to 2 ms, and a copy queued later can come after its vblank (`copies done after their vblank`).
