# Game-DLSS replacement

The Vulkan renderer can replace a recognized game-DLSS evaluation with NVIDIA DLSS on
Windows, then AMD FSR 3.1.4 if NVIDIA DLSS is disabled or unavailable. FSR uses AMD's
eight temporal-upscaling passes, including depth, motion, history and optional RCAS.
This is separate from the presentation filters that scale the finished window image.

Fire Emblem uses the jitter-only spatial workaround by default. Explicit `fsr` and `dlss`
comparison modes enable temporal replacement for its validated input contract. The upstream
temporal blend and RCAS fallback remain available if a temporal backend is unavailable.
Invalid consumed inputs leave the native dispatch enabled. An already-rejected native
tensor shader remains an explicit dispatch failure. General tensor emulation remains.

## Supported input contract

Selection currently recognizes Fire Emblem: Fortune's Weave's captured prepass and output
shader, under its exact program ID. Temporal mode requires matching B10G11R11 input/output,
R16G16 motion, D32 depth and a fresh jitter sample. Spatial mode requires only the matching
color/output and fresh jitter. The upstream temporal contract uses reversed depth,
normalized motion toward the previous frame, HDR color and jitter in render pixels.
Resolution scaling scales the render extent and jitter together. History resets on size
changes, first use and evaluation gaps longer than 250 ms. An explicit game camera-cut
reset is not yet extracted.

Fast Fusion's shaders still execute natively. Its captured shader identities are known,
but motion scale/direction, jitter units, depth convention, reset and resource roles have
not been proven for this LLE path. The RGBA16F menu/DLAA path also requires alpha handling
that the Fire Emblem contract does not provide. Turning on NVIDIA DLSS does not override
these safety checks and is not evidence of a Fast Fusion visual fix.

For a diagnostic run, `EDEN_NX2_DLSS_CONTRACT_TRACE=1` logs at most 24 matching Fast Fusion
dispatches per compute engine. `EDEN_NX2_DLSS_CONTRACT_SKIP=N` skips the first N matching
dispatches, capped at one million. Logs include exact shader identity, QMD launch dimensions,
bounded constant-buffer words and raw texture descriptors. They contain guest addresses
and should remain private. No shaders are skipped or game files changed by this capture.

## Ocarina of Time

The Legend of Zelda: Ocarina of Time (`0400486024DEC000`) has no DLSS of its own. Its
end-of-frame upscale pass can be replaced with FSR 3.1 on Linux or Windows, on any supported
Vulkan GPU, or NVIDIA DLSS on Windows. The **Use AMD FSR 3.1 upscaling in Ocarina of Time**
setting (`ocarina_fsr`) is off by default. NVIDIA DLSS takes precedence when its setting is
enabled and the runtime is available; FSR runs only when its own setting is on, so without it
the game keeps its own pass whenever DLSS is off, unavailable or fails to start.
`EDEN_NX2_DLSS_REPLACEMENT=fsr` explicitly enables FSR; `native` disables both replacements;
`dlss` asks for DLSS as its setting does.

- Max Resolution add-on off: the 3/4-scale pass (`e40f5efb5480e692`; `ef542df87cacfe9e` on the
  Linux RADV build, same contract) is replaced and FSR/DLSS
  writes the game's output size (1080p to 1440p docked, 810p to 1080p handheld).
- Max Resolution add-on on, docked: the full-size pass (`6bd913e1f34e83eb`) at 2560x1440 is
  replaced by a 3840x2160 temporal image. The game still finishes its 1440p frame. At presentation,
  the HUD is taken from the difference between that frame and a 1440p copy of the temporal scene,
  and composited over the 4K scene. The 4K image skips the presentation's post-processing and
  upscale filters.
- Max Resolution add-on on, handheld: the scene renders at 1920x1080 and the same output pass
  runs at 1:1 (`c033c3ddc9f0897b`, 1920x1080 to 1920x1080); the game's final pass then scales to
  its 2560x1440 output. FSR replaces it as native anti-aliasing (no upscale). DLSS leaves the
  game's pass there (DLAA through it is untested).
- The game has no motion vectors or jitter. Motion comes from the scene depth reprojected
  through this and the previous frame's camera (vertex constant buffer 3). Link's recognized
  skinned draw is replayed with both poses where his depth matches. Without his pose the frame
  keeps the camera motion. The input is tonemapped: both backends run in LDR without auto exposure.
- FSR's LDR profile uses unit exposure/pre-exposure (no auto exposure), finite standard
  device depth (near 0.1, far 25000), and vertical FOV recovered from the view-projection.
  Its shaders use `HDR_COLOR_INPUT=0` and `INVERTED_DEPTH=0`. Camera reconstruction alone
  converts the stored [0,1] depth to GL [-1,1] clip coordinates. Motion is previous minus
  current in render pixels, with screen-down Y, so FSR's scale is `(1,1)` before normalization.
- Jitter (FSR only, on by default): the game has no subpixel sample sequence, so the host adds
  one. While FSR replaced the previous frame's upscale, every draw that depth-tests against the
  scene depth at the scene size has its viewport offset by a Halton (2,3) sub-pixel amount
  (AMD's phase count, 8 x (output / render width)^2: 18 for 1440p -> 4K, 8 for native AA), and
  FSR gets the same offset as the frame's jitter. Shadow maps, half-size passes, full-screen
  passes and frames the game upscales itself are not offset. A full-screen pass with depth
  writes on but the test off writes no depth (Vulkan disables writes with the test), so the
  depth-write flag alone doesn't count. DLSS gets no jitter.
- Motion with jitter: FSR is told the motion vectors carry no jitter (no jitter-cancellation
  flag). Camera motion comes from the unjittered matrices, reconstructed at each depth sample's
  unjittered position (the pixel centre minus the jitter). The replay of Link's skinned draw is
  offset by the same jitter, so its scene-depth check compares the same samples the game drew,
  and its motion is taken from the unjittered position. Without that offset the jitter made the
  check fail on most of the fragments it otherwise accepts.
- Texture LOD bias (FSR only): when the session's upscaler is FSR, mipmapped samplers get
  AMD's recommended bias log2(render / output) - 1 = -1.58 in docked mode (none in handheld).
  Samplers pick the biased or unbiased variant at each bind, so a console mode switch takes
  effect in-session. Images the GPU wrote (render targets: bloom, blur, reflections, fog) keep
  the game's own bias.
- FSR 4 (Windows, AMD's driver, off by default): with "Use AMD FSR 4 in place of FSR 3.1" on,
  the same inputs, jitter and RCAS setting go to AMD's FSR 4 through AMD's FidelityFX runtime
  (FidelityFX SDK 2.4.0, FSR Upscaling 4.1.2). Nothing of AMD's ships with Lichii: the setting's
  panel downloads `amd_fidelityfx_loader_vk.dll` and `amd_fidelityfx_upscaler_vk.dll` from
  the SDK's GitHub repository at the release commit into the user folder's `fidelityfx` folder,
  after showing AMD's licence. Each file must match its pinned size and SHA-256
  (`vulkan_common/ffx_files.h`) and carry AMD's Authenticode signature, checked after the
  download and again before loading. AMD's runtime picks the model (FP8 on RDNA 4, INT8 on RDNA 3
  and RDNA 2); the setting is only shown for those GPUs. `VK_EXT_shader_float8` and the
  `shaderFloat16` device feature are enabled for it only while the setting is on. FSR 3.1
  stays created and takes over if the files are missing or don't check out, AMD's runtime offers
  no FSR 4 for the GPU, the context can't be created or a dispatch fails. The log says which, and
  shows `Ocarina FSR 4 evaluated` while it runs.
- FSR 4 on Linux (RADV, off by default): "Use FSR 4 (fsr4-vk community Vulkan port) in place of
  FSR 3.1" feeds the same inputs to [fsr4-vk](https://github.com/Astyyyyy/fsr4-vk) (GPL-3.0), a
  community Vulkan port of FSR 4.1.1 built from AMD's weights and shaders. It is not AMD's
  official FSR 4. The release workflow builds it with `tools/fsr4-vk/build.sh`, which pins the
  fsr4-vk commit and the SHA-256 of AMD's signed `amd_fidelityfx_upscaler_dx12.dll` of FidelityFX
  SDK 2.3.0 it extracts them from (listed under MIT in the SDK's `license.md`). Both x86-64 Linux
  packages carry `usr/lib/amd_fidelityfx_upscaler_vk.so` and, in `usr/share/licenses/fsr4-vk`,
  fsr4-vk's licences and AMD's SDK licence. The aarch64 packages don't: fsr4-vk's shader
  conversion fails its own asset check on arm64 hosts. A build without it loads the library from the user
  folder's `fidelityfx` folder. The setting is shown for RDNA 2 and newer Radeons on RADV
  (`VideoCommon::IsFsr4PortGpu`). At boot `VideoCommon::CheckFsr4Port` checks what fsr4-vk's
  shaders use on the created device (descriptor buffers, synchronization2, 32-lane compute
  subgroups, 8/16-bit arithmetic with integer dot products, linear compute derivatives) and picks
  the network: FP8 with FP16/FP8 cooperative matrices, `shaderFloat8` and the matrix conversions
  of `VK_NV_cooperative_matrix2` (RADV exposes it only with its `radv_cooperative_matrix2_nv`
  option), INT8 otherwise. Lichii asks fsr4-vk for that model by version. The extra device
  features are enabled only while the setting is on with RADV. Anything missing, a missing
  library or a failed context or dispatch leaves FSR 3.1 running, and the log names the active
  upscaler and why. Ported from #915's loader; untested on AMD hardware so far.
- #644 (`82494c4820`, `74a2d20c90`, `4352c06bbf`) supplies the effects capture, used by FSR only
  (DLSS gets no bias mask and runs as before). FSR uses its R16F current/history bias as both
  reactive and transparency/composition masks; missing capture uses zero masks, not stale
  history. The DLSS preset changes were not imported.
- RCAS uses `fsr_sharpening_slider` with the existing inverted 0–200 mapping (stops × 100;
  stored 200 disables sharpening). HUD handling
  and the Max Resolution 1440p-to-4K presentation composition are shared with #606.
- FXAA and SMAA are skipped on frames either temporal backend drew.
- Any unexpected input, a busy frame slot, a rescaled image (resolution scale other than 1x) or
  a failed evaluation leaves the game's pass to run. The log names the reason as
  `Ocarina temporal: the game's upscale draws frame N (reason)`.
  `EDEN_NX2_DLSS_CONTRACT_TRACE=1` logs every FSR evaluation and scheduler writeback;
  otherwise periodic evaluation/writeback messages avoid a per-frame logging cost.

### Every resolution step: `EDEN_NX2_OOT_FSR_ANY=1` (experiment, not compiled or run yet)

Without the Max Resolution add-on the scene follows the game's GPU-time controller: 1440x810,
1280x720, 1024x576 and 854x480 (drawn into 864x480 images) for the 1920x1080 output, each step
with its own upscale program. The replacement above takes only the 3/4-scale program, so at the
lower steps the game's network runs again. With `EDEN_NX2_OOT_FSR_ANY=1` and FSR as the upscaler
(`ocarina_fsr`, or `EDEN_NX2_DLSS_REPLACEMENT=fsr`; not DLSS):

- The output pass is known as `EDEN_NX2_OOT_BYPASS_ANY` knows it
  (`KeplerCompute::IsOotOutputPass`): the shader hashes of `oot_network.h`
  (`OotNetwork::IsOutputShader`), `EDEN_NX2_OOT_BYPASS_OUTPUT=<hex,...>`, and otherwise any
  native program that reads one E5B9G9R9 image and stores one B10G11R11 image no smaller. The
  log names a program found by its shape once (`output pass known by its shape`).
- Any scale is taken when the output is not smaller, at most 4x, and of the scene's shape to
  within a render pixel (`OcarinaUpscaleFits`). The scene is the render area of the scene's
  draws (the surface clip), not their images: 854x480 of 864x480.
- FSR 3.1 keeps its render-size images at the largest step (3/4 of the output, more if a larger
  scene comes) and runs smaller steps in their top-left part, as its SDK does for dynamic
  resolution (`Fsr3Upscaler::Inputs::max_render_size`). A step neither reallocates them nor
  resets the history; the history resets on a camera cut, a frame the game upscaled, or a frame
  over 250 ms, as before. The motion and colour copies, the effects mask and the extrapolation
  guide are remade at the new size (the last two wait for the GPU once per step).
- Jitter: the Halton length is 8 x (output / render width)^2 of the last replaced frame (14 at
  810p, 40 at 480p). The draws offset are those into a depth of the last replaced frame's scene
  size, so the first frame of another step is drawn without an offset, all of it.
- Texture LOD bias: log2(render / output) - 1 of the last replaced frame, in quarter LODs
  (-1.5 at 810p and 720p, -2 at 576p, -2.25 at 480p), in either console mode.
- A second depth reader (`3456eac1b79ed5b8`, same handle at +0x128) gives the scene depth in
  frames the first leaves without one.
- Frame extrapolation (`EDEN_NX2_EXTRAPOLATE`) gets its guide at every step, and the game's
  output image as the scene before the HUD.
- The log has `Ocarina temporal: FSR replaces pass ... WxH of WxH -> WxH` at Info for each new
  pair of sizes, and `Ocarina temporal: the game's upscale draws frame N (reason): pass ...`
  as a warning when it declines (when the reason changes and every 120th repeat). The reason
  `no dispatch known as the game's upscale` is a step whose program is neither listed nor of
  the shape above; its shader hash (the frame's slowest `s6:` entry of `DKB gpu_work`) goes in
  `EDEN_NX2_OOT_BYPASS_OUTPUT`.

`EDEN_NX2_GPU_TIME_SCALE=<float>` (experiment) multiplies the GPU time a Switch 2 title measures
with report timestamps, on top of GPU Clocks: the time since the last present is divided by the
clock's 1, 4 or 16 and then multiplied (`AmpereTimeSincePresent` in `gpu_timestamp.h`). It never
exceeds the real time, so with GPU Clocks at Normal the useful range is 0 to 1. Ocarina sizes
its dynamic resolution from that time: a larger value gives a lower step. Which value gives
which step is not known. The scale is a runtime value (`VideoCore::FrameBudget::GpuTimeScale`),
latched with each present into the anchor `GPU::Impl::GetTicks` reads, so it changes between
frames only; the variable is its starting value.

`EDEN_NX2_TARGET_FPS=<fps>` (experiment, not compiled or run yet) lets a controller move that
scale to hold a frame rate: guest presents are held to the target's interval, and the scale
goes up when frames take longer than that and slowly down when they have headroom, so the game
settles on the highest step that fits. The time "since the last present" is the time since the
renderer composed the previous guest frame, not since a host vsync: at 30 fps it runs over the
whole 33 ms frame, the wait for the pacer included, and GPU work measured between two reports
of one frame is scaled as a whole. The Max Resolution add-on must be off (with it the game does
not follow GPU time), and one of the two frame-rate add-ons on so the game keeps its speed
below 60 fps. See `frame_budget` in `docs/nx2/debug-mcp.md`.

Shader hashes, buffer offsets and Link's mesh shape are taken from the game's current build.
A game update that changes them disables the replacement and leaves the game's pass in place.
The "Ocarina of Time anti-aliasing" option (No sharpening, docs/nx2/oot-network-bypass.md)
bypasses the same network on other GPUs. It does nothing while this replacement is enabled:
temporal upscaling takes precedence because it replaces the same output pass.

## Settings and comparison runs

"Use NVIDIA DLSS in place of a game's DLSS" defaults to off and takes effect when the game
starts. Disabled means no NVIDIA runtime is loaded; an explicitly selected temporal
replacement uses FSR 3.1. Missing DLLs and unsupported GPUs fall back to FSR. Recording is
asynchronous. A failed DLSS recording clears that frame's output, restores input layouts,
and makes later evaluations fall back; it does not wait for the scheduler worker.

`EDEN_NX2_DLSS_REPLACEMENT=fsr` selects temporal replacement and forces FSR for comparison.
`dlss` selects temporal replacement with NVIDIA preferred when enabled and available.
`native` preserves the guest evaluation. With no override, Fire Emblem uses spatial de-jitter.
The old `EDEN_NX2_DLSS_STANDIN` variable is not read. The internal `standin` backend override
does not select Fire Emblem's intercepted dispatch. The RCAS sharpness setting controls FSR
and fallback sharpening. NVIDIA model-selection overrides
from the upstream implementation remain available for diagnostic comparisons.

## Build and distribution

FSR needs no external runtime. Keep `externals/FidelityFX-SDK/LICENSE.txt` and its attribution
with distributions. The vendored files are unmodified AMD MIT sources from FidelityFX SDK
v1.1.4, commit `c6efa6bf7f2027b3ec94f28578bb5965eabb9e55`.

For the optional Windows NVIDIA path, obtain only the MIT Streamline headers and license
from NVIDIA-RTX/Streamline tag `v2.14.16`, commit
`625db07d4bca5abc67d781c42af7e7fb0761f1b6`. The SDK directory must contain `include/sl.h`,
`include/sl_dlss.h`, `include/sl_helpers_vk.h`, `include/sl_security.h` and their companion
headers. MSVC x86_64 builds enable `ENABLE_NVIDIA_DLSS` by default and fetch the headers;
`-DEGLANTINE_STREAMLINE_SDK=<header-root>` selects a local copy. This adds Windows
`version`, `wintrust` and `crypt32` libraries, not NVIDIA import libraries. Retain Streamline's
MIT `license.txt` in the packaged notices.

Do not bundle NVIDIA's proprietary DLLs. The Graphics panel obtains missing files from
NVIDIA's Streamline release only after the user reviews and accepts the license texts.
The runtime searches `EDEN_STREAMLINE_DIR`, the user `streamline` directory, then the
executable's `streamline` directory. It verifies the interposer signature and supports the
NVIDIA app's signed DLSS model. The UI and Windows loading path require Windows validation.

## Provenance

This backport is AI-assisted and requires human review and testing before distribution.
It carries upstream work by Duck912 from Eglantine commits `08dbcc7e3b3d`, `11cebf56014a`
and PR #434 through `f9d0c802e8e4`, plus Jonas's warning fix `537c1f4e173a`. The empty-extension requirement
fix `1889602c7a0d` is a separately included follow-up. Existing SPDX notices and AMD/NVIDIA
license text are retained. The upstream nx2 merge also carries newer asynchronous recording
and fallback changes while retaining the fork's signature checks and input validation.
