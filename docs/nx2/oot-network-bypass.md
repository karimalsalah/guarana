# Ocarina of Time: bypassing the game's anti-aliasing network

The Legend of Zelda: Ocarina of Time (`0400486024DEC000`) runs a tensor network on its tonemapped
image followed by an output pass that sharpens hard. The "Ocarina of Time anti-aliasing" setting
(`oot_anti_aliasing`: Game / No sharpening) can skip both and show the tonemapped image instead.

## What the game does

Found with sampled frame dumps (`capture_frame` with `sampled`), a trace of the texture handles in
the compute passes' constant buffer 0, and pixel statistics, in Kokiri Forest, docked, at 1x with
the built-in Max Resolution patch.

- The 3D renders at the output size (2560x1440 docked), with no jitter and no motion vectors.
- The tonemap pass (fragment shader `bded5ce917183214` outdoors, `3ac011bd2d772994` outdoors at
  another time of day, `6cb06edf14be1d79` indoors) renders a B10G11R11 image.
- The game then runs, on that **tonemapped** image: a prepass (`8475d671e6967428`), a heavy
  dispatch (shader `d9ee412f00ce399a`), the network (`42593194bb0b2b32`) and an output pass
  (`da6df3e54c716df2`) that writes the B10G11R11 image the rest of the frame uses (texture handle
  at cb0+0xd0). Keys are native program keys.
- The network keeps no history: it is a single-frame spatial smoother (mean |Laplacian| of green
  0.0042 -> 0.0037). The output pass then sharpens hard: 2.2x the input's edge contrast, with
  overshoot up to 1.72 on bright edges. That is the halo users see.
- Only the HUD and a plain copy for presentation follow the output pass.

## What No sharpening does

`ShaderCache::GetComputePipeline` asks `KeplerCompute::BypassOotNetwork` about every native compute
dispatch (`DispatchReplaced` skips it):

- the prepass and the network are skipped;
- the output pass is replaced by an `AccelerateSurfaceCopy` of the tonemapped image into the output
  pass's own target (point filter, same size; format, size and block-linear layout from the output
  pass's texture descriptor at cb0+0xd0).

The images are learned at run time, not hard-coded: the tonemapped image is render target 0 of the
latest draw whose pipeline has a tonemap fragment shader
(`RasterizerVulkan::TrackOotTonemappedImage`), and the output is whatever the output pass's handle
selects. Nothing is replaced until the tonemapped image has been seen. The prepass and the network
are skipped only while the output pass is being replaced: if the tonemapped image or the output
descriptor is not a B10G11R11 block-linear image, their sizes differ, or the copy fails, the next
frame runs the whole network natively again. The copy's source takes its block layout from the
tonemap draw's render target. The state lives in the rasterizer (`OotNetwork::BypassState`) because the tonemap
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
that writes 1920x1080, and the bypass does nothing. What the log says at `*:Info`, while a
tonemapped image is known:

- `Ocarina of Time: dispatch not bypassed (<reason>): key <native key> hash <shader hash> grid ...
  block ..., tonemapped WxH drawn WxH, reads ..., stores ...`: once per program and reason, every
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

## Results (Ryzen 7 9800X3D, RTX 5090, Kokiri Forest, 1x)

| Mode | fps | Image |
|---|---|---|
| Game | ~41 | sharpened, halos |
| No sharpening | ~50 | clean; edges aliased as rendered, pairs with resolution scaling |

## Caveats

- Hashes, keys and the offset are specific to the game's main with build id
  `E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348`; a game update needs them re-checked. With other
  keys the bypass does nothing.
- B10G11R11 has little precision in dark tones: without the network's smoothing some banding
  shows in deep shadows.
- The heavy dispatch between the prepass and the network still runs; its result goes unused.
- Vulkan only: other backends never learn the tonemapped image, so the setting has no effect there.
