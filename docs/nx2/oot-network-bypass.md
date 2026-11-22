# Ocarina of Time: bypassing the game's anti-aliasing network

The Legend of Zelda: Ocarina of Time (`0400486024DEC000`) runs a tensor network on its tonemapped
image followed by an output pass that sharpens hard. The No sharpening option
(`oot_anti_aliasing`) shows the tonemapped image and can also skip the update's network passes.

## What the game does

The update (1.1.1) was inspected in Kokiri Forest, docked at 1x with the built-in Max Resolution
patch. Sampled frame dumps, constant-buffer traces and pixel statistics showed:

- The 3D renders at the output size (2560x1440 docked), with no jitter and no motion vectors.
- The scene draws into an HDR B10G11R11 image (values above 1). A full-screen tonemap pass reads
  it and renders the tonemapped B10G11R11 image (0 to 1). Its fragment shader varies with the scene
  (`bded5ce917183214` outdoors, `3ac011bd2d772994` outdoors at another time of day,
  `6cb06edf14be1d79` indoors, `96d6efdbfe52605c` in parts of adult Zora's Domain and Lon Lon
  Ranch, and possibly more).
- The game then runs, on that **tonemapped** image: a prepass (`8475d671e6967428`), a heavy
  dispatch (shader `d9ee412f00ce399a`), the network (`42593194bb0b2b32`) and an output pass
  (`da6df3e54c716df2`). The output pass samples one texture, the tonemapped image (texture handle
  at cb0+0xc0), and writes the B10G11R11 image the rest of the frame uses (texture handle at
  cb0+0xd0). Keys are native program keys.
- The tensor instructions are in the output pass: 192 `HMMA.16816` per invocation of
  `da6df3e54c716df2`, counted in a native dump. The prepass, the heavy dispatch and the pass called
  the network here have none, so No sharpening, which replaces the output pass, is what removes the
  tensor math ([design](../design/game-reconstruction-and-motion.md)).
  With no known tonemap shader seen yet, the latest draw of the output pass's input size is
  taken as the tonemap instead, whichever shader drew it (the candidate detection).
- The network keeps no history: it is a single-frame spatial smoother (mean |Laplacian| of green
  0.0042 -> 0.0037). The output pass then sharpens hard: 2.2x the input's edge contrast, with
  overshoot up to 1.72 on bright edges. That is the halo users see.
- Only the HUD and a plain copy for presentation follow the output pass.

In the base game (1.0.0), the same tonemap can run below the output resolution. A frame dump in the
Temple of Time showed a 1920x1080 tonemapped scene feeding a 2560x1440 output. The base game's
output shaders use the same constant-buffer handle offsets as the update.

## What No sharpening does

- On 1.1.1, `ShaderCache::ComputeShader` calls `KeplerCompute::BypassOotNetwork`. The output
  pass uses a same-size point copy. The prepass and network are skipped while the replacement works.
  The known intermediate program is also skipped between a skipped prepass and the output copy.
- On 1.0.0, `KeplerCompute::BypassOotBaseNetwork` recognizes the known output programs from
  `DispatchNativeCompute` before translation, which can reject their per-tap gathers. The game
  selects different programs as the internal resolution changes. Match the full native key for
  each candidate that shares the first word. The replacement uses a bilinear copy that can scale
  the tonemap up to the output resolution. The base game's other compute passes still run.
- Both paths use `AccelerateSurfaceCopy` and get the output format, size and block-linear layout
  from the output pass's texture descriptor at cb0+0xd0.

The images are learned at run time, not hard-coded. The copy writes the image the output pass
would write (cb0+0xd0). It reads render target 0 of the latest B10G11R11 draw since the previous
output pass that has the size of the image the output pass samples (cb0+0xc0) and is not the
output (`RasterizerVulkan::TrackOotTonemappedImage`). The scene draws come first, so that is the
tonemap pass whichever shader draws it. The sizes and the output come from the previous output
pass, so the first output pass runs as usual. Builds up to 228 copied render target 0 of the
latest draw with one of three known tonemap fragment shaders. Where the game used another tonemap
shader, that image was no longer drawn, so the 3D froze under a live HUD (#1688). #1875 then
copied the sampled image itself, which is the scene before the tonemap: dark scenes came out
brighter, bright ones darker and with less colour. The prepass and
the network are skipped only while the output pass is being replaced. The update also has output
programs this does not replace (for example the scaled field upscale `e4cda162ff6009e2`, see
docs/native-dlss-replacement.md). A frame that ends in one of them would read the skipped
network's stale result, so a skipped prepass that no replacement followed brings the network back
from the next frame. The log names the first program after the network the first time that
happens. The replacement requires two
B10G11R11 block-linear images, and the tonemapped image must fit the output dimensions (an exact
match on 1.1.1). If no tonemapped image was drawn or the copy fails, dispatch falls back to the
game's shader. The state lives in the rasterizer (`OotNetwork::BypassState`) because the tonemap
pass draws on the 3D engine and the network's passes do not all run on the same compute engine.

NVIDIA DLSS takes precedence: while Ocarina DLSS is enabled ("Use NVIDIA DLSS in place of a
game's DLSS" on, with DLSS available from Streamline), `RasterizerVulkan::OotNetworkBypass` returns
null and No sharpening does nothing, because DLSS replaces the same output pass
(docs/native-dlss-replacement.md). The log says
`Ocarina of Time: No sharpening is off while NVIDIA DLSS is on` once. Once DLSS turns itself off
for the session or its evaluation fails, No sharpening takes over from the next frame.

The game's constants (shader hashes, program keys, the cb0 offset) are in
`src/video_core/oot_network.h`.

## Other modes

The keys above were captured docked with the Max Resolution add-on. Without it the scene renders
at a dynamic resolution (864x480 to 1440x810 in handheld), the output pass is another program
that writes 1920x1080, and the bypass does nothing. What the log says at `*:Info`:

- `Ocarina of Time: dispatch not bypassed (<reason>): key <native key> hash <shader hash> grid ...
  block ..., tonemapped WxH drawn WxH, reads ..., stores ...` (`tonemapped image not read` before
  the bypass has read one): once per program and reason, every
  native compute dispatch left to the game, with the images its cb0 handles read (TEX, TLD, TLD4,
  TXD) and store (SUST) as `+offset=address WxH format`. The output pass is the one that stores a
  B10G11R11 (format 0x21) image of the output size.
- `Ocarina of Time: unexpected network images; not replaced (<reason>: ...)` and
  `Ocarina of Time: anti-aliasing network bypassed (...)`: when the output pass's outcome changes.

`EDEN_NX2_OOT_BYPASS_ANY=1` (experiment, off by default) makes the bypass cover those modes:

- The output pass is also known by shader hash: the passes the temporal replacement knows
  (docs/native-dlss-replacement.md), and `9302bc0d6d1d9bc1` (from 1024x576) and `03bf0ffc7d2d0bed`
  (from 854x480), measured writing the 1920x1080 output in handheld. Each resolution step has its
  own program; the 1280x720 one is not measured. Its image is the program's one image store when
  it has exactly one, +0xd0 otherwise.
- `EDEN_NX2_OOT_BYPASS_OUTPUT=<hex,...>` and `EDEN_NX2_OOT_BYPASS_SKIP=<hex,...>` add output and
  skipped passes, as native program keys or shader hashes from the log lines above.
- The output may be larger than the tonemapped image: up to 4x, same shape within 1/32. The copy
  is then bilinear. Its source is what the tonemap draw could reach of its target (the surface
  clip cut by viewport 0 and its scissor), not the whole target, which dynamic resolution may
  leave partly unused.
- With the switch the copy still reads render target 0 of the latest draw with one of the three
  tonemap shaders the rasterizer tracks (`OotNetwork::IsTonemapShader`), as builds before #1875
  did, not the image the output pass samples: where the game tonemaps with another shader (#1688)
  it copies a stale image. Without the switch the bypass reads what the output pass samples.

### Resolution steps

With `EDEN_NX2_OOT_BYPASS_ANY` and without Max Resolution the scene can go black at a dynamic
resolution step (issue #98). The prepass and the network are skipped while `output_replaced` is
set, and only an output pass the bypass takes writes that flag. An output pass it does not know
is left to the game without clearing it, so at a step whose output program is new the network
stays skipped and the game's upscale reads an image nobody wrote. Two switches, read only with
`EDEN_NX2_OOT_BYPASS_ANY` and off by default, address it:

- `EDEN_NX2_OOT_BYPASS_STEP_GUARD=1`: each replacement records the tonemapped image's address and
  drawn area, and the network is skipped only while the current tonemap matches them
  (`OotNetwork::SkipAllowed`). The first frame at a new step runs the whole network, and its output
  pass decides: replaced, or left to the game on the network's real output. `Ocarina of Time: step
  changed to WxH: the network runs this frame` is logged once per drawn area and tonemapped image,
  with the addresses of both images, so a tonemap that moves to another image at the same step
  shows up too. A pass the bypass does not take that has the output pass's shape (below) also
  clears the flag.
- `EDEN_NX2_OOT_BYPASS_SHAPE=1`: an output pass is also known by its shape
  (`OotNetwork::IsOutputShape`, the test `EDEN_NX2_OOT_FSR_ANY` already uses): it reads one
  E5B9G9R9 image and stores one B10G11R11 image elsewhere, no smaller. The image it reads must
  also be the size of what the tonemap drew, within 1/32 in each axis
  (`OotNetwork::MatchesDrawnArea`; the issue's 1195x672 against a 1200x672 step). Logged once per
  shader as `output pass known by its shape (bypass)`. A pass named in `EDEN_NX2_OOT_BYPASS_SKIP`
  is skipped before this test and is never taken by its shape.

With the guard, a step change costs at least one frame of the full network. At a step whose
output pass the bypass does not take, the network runs every frame, as without the bypass. With
`EDEN_NX2_OOT_BYPASS_SHAPE` too, an output pass that matches the drawn area is taken at the step's
first frame, and the network is skipped again from its second. Neither switch is measured.

## Results (Ryzen 7 9800X3D, RTX 5090, Kokiri Forest, 1x)

| Mode | fps | Image |
|---|---|---|
| Game | ~41 | sharpened, halos |
| No sharpening | ~50 | clean; edges aliased as rendered, pairs with resolution scaling |

## Caveats

- The known shader keys cover base 1.0.0 (main build ID
  `33FA6C2C23848282A77E8B88A0A93F4887C55920`) and update 1.1.1
  (`E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348`). A different revision needs its shaders checked.
- B10G11R11 has little precision in dark tones: without the network's smoothing some banding
  shows in deep shadows.
- Vulkan only: other backends offer no bypass state, so the setting has no effect there.
