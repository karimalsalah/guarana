# Autopilot (`EDEN_NX2_AUTOPILOT=1`)

Status: experimental, off by default, no verified-scene measurement (see
[switches.md](switches.md)). It claims no gain; the measurement plan below is how one is found or
refuted.

A closed loop that, every 2 s, reads how the frames were shown and nudges knobs the emulator
already has. It changes presentation and resolution only. It never touches guest state, saves,
input or timing the game can observe other than the GPU time scale the frame budget already
exposes.

## Where it lives

| Part | File |
|---|---|
| Decision logic, a pure class (inputs to decision, no settings, Vulkan or logging) | `video_core/autopilot_controller.{h,cpp}` |
| Taps from the present threads (shown-frame times, refresh, FLUID slot rate) | `video_core/autopilot_feed.{h,cpp}` |
| Session: gathers inputs, applies decisions, logs, restores | `core/nx2_autopilot_session.{h,cpp}`, started at game boot next to `Nx2PowerSession` |
| Debug server | `autopilot` method and `status.autopilot` ([nx2/debug-mcp.md](nx2/debug-mcp.md)) |
| Tests | `src/tests/video_core/autopilot_controller.cpp` |

## Inputs (per 2 s window)

- Real fps: guest presents (`g_nx2_present_count`, Switch 2 vi2 presents) over the window. Under
  5 fps (a loading screen, a stall) every frame generation and resolution decision is held.
- Shown frames: the intervals between frames that reached the swapchain, generated ones included
  (a tap in `PresentManager::NoteShownFrame` and the direct present path, independent of
  `EDEN_NX2_SHOWN_STATS`): p50, p95 and the share within 2 ms of the median.
- Display refresh: the present thread's measure when the FLUID presenter runs, else the primary
  display's mode. Unknown holds the frame generation target.
- GPU time per frame: `RendererBase::GetGpuBusyMs()` (sampled every 0.5 s, median of the window)
  over the real frame time. Unknown disables the resolution step.
- Power source: `GetSystemPowerStatus` AC line status (Windows); a full battery on AC counts as AC.

## Decisions and bounds

Each knob moves at most one step per window, with a dwell after a change and hysteresis.

Frame generation (only while `frame_gen` is on; the autopilot never turns it on or off, that is
the user's or a preset's):

- Candidates are the integer divisors of the refresh (rounded to a whole Hz, so 119.88 counts as
  120) from 20 to 75 fps: 120 Hz gives 60, 40, 30, 24, 20; 144 Hz gives 72, 48, 36, 24; 60 Hz gives
  60, 30, 20.
- A target needs real fps of at least target / 3. A higher target needs 10% more than that and 5
  windows since the last change; a kept one may fall to 90% of it and a lower one follows after 2
  windows. At 120 Hz: 60 from about 22 fps up, 40 below about 18. Flapping around 20 fps changes
  nothing.
- Target mode (`frame_gen_target_rate` not 0): steers the target, never between 0 and non-zero
  (the frame pool is sized for the mode at swapchain creation). The multiplier setting is inert in
  this mode (the pacer's ceiling is the maximum), so it is left alone.
- Fixed multiplier mode (target 0): steers the multiplier to round(target / real fps) with a
  0.6 band, one step at a time, between 2 and the user's own multiplier (never above it, for the
  same frame pool reason).
- Judder: shown frames within 2 ms of the median under 75% for 3 windows step the target down one
  candidate; that candidate is not stepped back up to for 60 s.
- FLUID: the presenter's slot divisor follows the target (`AutopilotFeed::SlotRate`) instead of
  `round(refresh / 60)`, so a 40 fps target on 120 Hz lands on every third vblank.

Resolution (the GPU time scale of the frame budget; a larger scale makes the game's own dynamic
resolution go lower; only while `EDEN_NX2_TARGET_FPS` / the cadence leave the frame budget without
a target, otherwise the existing controller owns it):

- Goal real fps: half the frame generation target, else 30.
- GPU bound (GPU busy over 85% of the frame, real fps under 95% of the goal): one lower-resolution
  step, scale x1.25, at most 2x the scale the session began with and the frame budget's own limit,
  3 windows between steps.
- No effect: if two steps in a row do not raise real fps by 4%, the run is undone in one step to the
  scale it started from and not retried for 60 s.
- Headroom (real fps at the goal and GPU busy under 70% for 5 windows = 10 s): one higher-resolution
  step, x1/1.25, never above the starting scale. Each further step needs another 10 s.
- A scale set by hand or a frame budget target ends the autopilot's hold on it.

Power policy (Windows, integrated GPU, and `EDEN_NX2_POWER_POLICY` not set, which means
`Nx2PowerSession` owns the scheme): two windows on battery apply the `igpu` policy (boost off,
minimum state 5%), two windows on AC restore it. A failed apply is not retried.

Restore at game stop: only values that still equal what the autopilot set are put back (target,
multiplier, GPU time scale, power scheme). Its target and multiplier are marked as held values, so
a config save leaves them out.

## Logging

One line per change at Info, all starting `Autopilot:`, with what, from to, why and the numbers:

```
Autopilot: frame generation target 60 -> 40 fps (real fps is too low for the target; real 15.2 fps, shown p50 25.0 p95 41.7 ms, 62% within 2 ms, refresh 120.00 Hz (present thread))
Autopilot: resolution lower (GPU time scale 1.000 -> 1.250; GPU bound below the real fps goal; ..., goal 30.0 fps, GPU busy 97%)
Autopilot: power policy igpu applied (on battery; ...)
Autopilot: 10 s: <the same numbers>, GPU busy 91%, power ac, frame generation on target 60 multiplier 2, GPU time scale 1.250 (base 1.000), power policy off, 3 changes
```

## How to measure

No claim is made until paired runs show it. Same scene, same save, same build, same session, plugged
in or on battery as the test says:

1. Baseline: `EDEN_NX2_SHOWN_STATS=1 EDEN_NX2_FLUID=1` (frame generation configured as the user
   would, for example target 60) and no autopilot. Let it run 60 s after the scene settles.
2. Autopilot: the same plus `EDEN_NX2_AUTOPILOT=1`. Discard the first 30 s (the loop converges).
3. Read from the log: `Shown frames:` lines (p50, p95, `% within 2 ms of the median`, frames per
   second), `Fluid presenter` (shown on slot, missed, held refreshes) and `Autopilot:` changes. The
   debug server's `autopilot` method shows the state live; `power` the package watts on battery.
4. Compare shown p95 and within-2-ms share (smoothness), real fps (the cost of any resolution
   step), and for the power policy, package watts and fps on battery.
5. Repeat at least three pairs, alternating order. A change that moves p95 or the even share inside
   the pair-to-pair spread is no result. Record it in the switch row either way.

Also check that the autopilot is quiet: a stable scene should show no `Autopilot:` change lines
after convergence.

## Risks

- Settings are written from the autopilot thread while the present threads read them. They are
  plain u32 reads of a value that changes rarely; the frame generation pacer already reads them
  every frame and the status bar writes them live.
- A target change under FLUID moves the slot divisor while frames are in flight; a few frames may
  land on the wrong slot once. Not measured.
- Resolution steps depend on the game scaling its resolution from the GPU time (Ocarina of Time
  without the Max Resolution add-on, for example). Elsewhere the no-effect guard undoes them after
  three windows each way.
- On the Ocarina of Time Intel iGPU preset `frame_gen` is off, so only the resolution and power
  parts apply there.
- The refresh fallback is the primary display's mode: on another monitor without FLUID the target
  may be chosen against the wrong refresh. Use FLUID, or read the `refresh_source` field.
- Real fps counts Switch 2 vi2 presents only; Switch 1 titles read as no data and nothing is
  steered.
- The power policy needs the same rights as `EDEN_NX2_POWER_POLICY`; a host that refuses leaves the
  scheme alone.
