# Credits

Guarana is built on years of other people's work. Thank you.

## Upstream projects

| Project | What we owe it |
|---|---|
| [Lichii](https://github.com/zortos293/Lichii) (zortos293 and [its contributors](https://github.com/zortos293/Lichii/graphs/contributors)) | The emulator this fork is: its `nx2` branch, with Switch 2 support, the Vulkan renderer work, the debug server and the PGO profiles. |
| [Eglantine](https://github.com/Eglantine-Community/Eglantine) | Lichii's base. Its original developer is Camille LaVey. |
| Eden | The emulator Eglantine forked, and the Switch emulation work of its team. |
| yuzu | Where this line of emulators began. |
| Sudachi | Credited by Lichii among the projects it builds on. |

Their copyright notices stay in the source files, and the history in git keeps every author.

## Intel fixes

[Fritchii-Intel-Fixes](https://github.com/LuckyLuckLucky/Fritchii-Intel-Fixes): patches 0001 (dynamic primitive topology follows the pipeline's tessellation stages), 0003 (shuffles stay in bounds on host subgroups narrower than the guest warp), 0007 (guest HMMA on 8x16x16 cooperative matrices) and 0008 (Intel's 8x16x16 HMMA rounds after each K=8 half, as the guest), both behind `EDEN_NX2_INTEL_COOP_MATRIX` and built on the Lichii pull request #779 below, and 0010 (native float32 storage atomic add with `VK_EXT_shader_atomic_float`), ported to Lichii `nx2` in this fork, and the experiment EXP56 (a cleared depth target larger than its colour targets follows the clear instead of an upload: `EDEN_NX2_DEPTH_CLEAR_COVERS`). The patches name their author only as "Fritchii Intel fixes"; their authors tested them on an Arc B580 and an Arc 140V.

## Upstream pull requests we carry

Pull requests to zortos293/Lichii that were not merged upstream when we took them. Names are the commit authors as recorded in git.

| Pull request | Author | Change |
|---|---|---|
| [#1173](https://github.com/zortos293/Lichii/pull/1173) | [PantACRO4life](https://github.com/PantACRO4life) | Graphics pipelines linked from per-stage `VK_EXT_graphics_pipeline_library` parts (`EDEN_NX2_GPL`), with the create state gathered in one struct |
| [#1177](https://github.com/zortos293/Lichii/pull/1177) | [PantACRO4life](https://github.com/PantACRO4life) | Optimized links of the libraries built in the background and swapped in at a later draw (`EDEN_NX2_GPL_OPTIMIZE`); libraries released with the pipelines that used them; libraries only with independent interpolation decoration and five bound descriptor sets |
| [#1185](https://github.com/zortos293/Lichii/pull/1185) | [justin-wq1337](https://github.com/justin-wq1337), [PantACRO4life](https://github.com/PantACRO4life) | Libraries linked optimized on AMD's Windows driver, where fast links draw white fringes (`EDEN_NX2_GPL_FAST_LINK`). Closed upstream without being merged; we keep it. |
| [#864](https://github.com/zortos293/Lichii/pull/864) | [cerroverb](https://github.com/cerroverb) | A game's DLSS network passes skipped while the replacement upscale draws over their output (`EDEN_NX2_DLSS_SKIP_PASSES=drew`); ported from its net diff, behind a default-off switch |
| [#1985](https://github.com/zortos293/Lichii/pull/1985) | [the-cashman](https://github.com/the-cashman) | The same passes skipped while the network cannot be shown, which also spares the replacement's inputs from the NO_TENSOR reject path (`EDEN_NX2_DLSS_SKIP_PASSES=blocked`); behind a default-off switch |
| [#948](https://github.com/zortos293/Lichii/pull/948) | [danieljesus](https://github.com/danieljesus) | ASTC textures with uneven mips stay in BC on integrated GPUs and cards under 6 GiB; the threshold compares against the heap size, not the budget. Merged upstream as `c9ca6734f6`: `main` has had the upstream version since the upstream merge in #197. |
| [#933](https://github.com/zortos293/Lichii/pull/933) | mstampfli | A suspended list copy is polled without a kernel call each time |
| [#1155](https://github.com/zortos293/Lichii/pull/1155) | [PantACRO4life](https://github.com/PantACRO4life) | Texture descriptors are flushed one by one when the pool runs past the GPU address space. Merged upstream as `1f579f1334`: `main` has had the upstream version since the upstream merge in #197. |
| [#1218](https://github.com/zortos293/Lichii/pull/1218) | [PantACRO4life](https://github.com/PantACRO4life), [Nanoc6](https://github.com/Nanoc6) | GPU memory registry and address-space listeners are called outside their locks, so the GPU memory registry no longer deadlocks. Merged upstream as `edee104245`: `main` has had the upstream version since the upstream merge in #197. |
| [#408](https://github.com/zortos293/Lichii/pull/408) | ZeroOneTV | Pipeline cache: a masked key keeps its graphics record, used pipelines are relinked; kernel object lifetime and HostMemory move tests. Its kernel object lifetime part (the general channel's event and a failed load's process closed before the kernel shuts down) is upstream's own fix now, zortos293/Lichii#1932 (`a3b4397afd`): the upstream merge to `7c7f723b68` replaced our copy with it. The rest is still open upstream. |
| [#1568](https://github.com/zortos293/Lichii/pull/1568) | [cerroverb](https://github.com/cerroverb) | Device memory clamps the cold hint size and skips cold hints for placeholder addresses. Merged upstream as `555e326e2a`: `main` has had the upstream version since the upstream merge in #197. |
| [#1417](https://github.com/zortos293/Lichii/pull/1417) | [PantACRO4life](https://github.com/PantACRO4life) | Clean shutdown: AM releases an application process it never took, services stop without the server lock held. Merged upstream as `f3c341f6cd`: `main` has had the upstream version since the upstream merge in #197. |
| [#1564](https://github.com/zortos293/Lichii/pull/1564) | Hugo | The compute shader precache marker follows the file's contents and the shader lowering. Closed upstream without being merged; we keep it. |
| [#1447](https://github.com/zortos293/Lichii/pull/1447) | [zortos293](https://github.com/zortos293) | Draw templates: each draw's sampled textures and image descriptors are kept across frames (`EDEN_NX2_DRAW_TEMPLATE_VERIFY` checks them) |
| [#1571](https://github.com/zortos293/Lichii/pull/1571) | [jpkto](https://github.com/jpkto) | The texture cache's bounded retire scan resumes where it stopped; overwritten render targets are deleted after ten unused frames |
| [#1602](https://github.com/zortos293/Lichii/pull/1602) | [cerroverb](https://github.com/cerroverb) | A guest write that finds the caches busy waits for the next gather instead of blocking the game thread. Closed upstream without being merged; we keep it. |
| [#995](https://github.com/zortos293/Lichii/pull/995) | [VirusEx](https://github.com/VirusEx) | Pointer-slot compute rebuilds compile on a worker; the Hyrule Field grass-culling dispatch keeps its old translation instead of freezing for 2.5-3.4 s. Merged upstream as `af5b4e2a73`: `main` has had the upstream version since the upstream merge in #197. |
| [#1584](https://github.com/zortos293/Lichii/pull/1584) | [rushi806](https://github.com/rushi806) | Packed R16 shadow dispatches may read global memory, so Ocarina of Time's prop shadows render (upstream issue #1343) |
| [#1483](https://github.com/zortos293/Lichii/pull/1483) | [cerroverb](https://github.com/cerroverb) | The virtual range cache remembers linear spans |
| [#1099](https://github.com/zortos293/Lichii/pull/1099) | [tibzejoker](https://github.com/tibzejoker) (#874), rebased by [zortos293](https://github.com/zortos293) | Fence-flagged payload releases stay synchronized at low GPU accuracy with default fences (hand port onto our counter reports) |
| [#1378](https://github.com/zortos293/Lichii/pull/1378) | Krim | Graphics pipelines reuse the translated shaders of an earlier pipeline when their translation inputs and the guest memory they read match (`EDEN_NX2_SHADER_REUSE=1`) |
| [#1379](https://github.com/zortos293/Lichii/pull/1379) | Krim | Skippable native graphics pipelines are translated as shader jobs on workers, from environments detached from guest memory (`EDEN_NX2_ASYNC_TRANSLATE=1`); builds on #1378 |
| [#1838](https://github.com/zortos293/Lichii/pull/1838) | [zortos293](https://github.com/zortos293) | Where reading the driver pipeline cache during builds crashes (Intel's Windows driver), the boot load saves the cache once its builds have ended and starts the background prebuild after that save (`EDEN_NX2_LOAD_SAVE=1`; upstream makes it the behaviour there) |
| [#1596](https://github.com/zortos293/Lichii/pull/1596) | [sanchodecuba11-del](https://github.com/sanchodecuba11-del) | The selected user index is cast to the launch parameter type (MSVC C4244). Merged upstream as `eeeb349746`: `main` has had the upstream version since the upstream merge in #197. |
| [#1475](https://github.com/zortos293/Lichii/pull/1475) | [RomainROCH](https://github.com/RomainROCH) | Logging no longer calls std::terminate when formatting fails while the host is out of memory |
| [#1558](https://github.com/zortos293/Lichii/pull/1558) | [BrunoSilvestre00](https://github.com/BrunoSilvestre00) | The loading screen no longer freezes in borderless fullscreen on Windows. Merged upstream as `ffac7a7478`: `main` has had the upstream version since the upstream merge in #197. |
| [#779](https://github.com/zortos293/Lichii/pull/779) | [dylan-hachmann](https://github.com/dylan-hachmann) | Guest HMMA on Intel's 8x16x16 cooperative matrices as two 8-row products through shared memory (closed unmerged upstream; carried behind `EDEN_NX2_INTEL_COOP_MATRIX`, with Fritchii-Intel-Fixes 0007 and 0008) |
| [issue #1431](https://github.com/zortos293/Lichii/issues/1431) (a patch in an issue, not a pull request) | [ronaldpena007-ops](https://github.com/ronaldpena007-ops) | The mesh emulation's compute dispatches of a draw run before the render pass they draw in, instead of cutting the pass (`EDEN_NX2_MESH_BEFORE_PASS`) |
| [#1835](https://github.com/zortos293/Lichii/pull/1835) | [anthonyloukinas](https://github.com/anthonyloukinas) | A refused stream buffer wrap keeps its position, so the first region is not handed out while the GPU still reads it (Ocarina of Time: a white or false-colour 3D view after a load) |
| [#1890](https://github.com/zortos293/Lichii/pull/1890) | [zortos293](https://github.com/zortos293) | Images are not scaled past the device's 2D image and framebuffer limits (Ocarina of Time's 12288x12288 sun shadow map at 1.5x and 2x lost the device on AMD) |
| [#1860](https://github.com/zortos293/Lichii/pull/1860) | [zortos293](https://github.com/zortos293) | The texture cache's retention limit follows the lowest driver budget read lately instead of the last one |
| [#1799](https://github.com/zortos293/Lichii/pull/1799) | [Reiixan](https://github.com/Reiixan) | A compute pipeline dropped before its build ran counts as done, so "Building: N shader(s)" returns to zero |
| [#1908](https://github.com/zortos293/Lichii/pull/1908) | [lethevimlet](https://github.com/lethevimlet) | Native compute launches clear the pending upload list, which grew for the whole session in Ocarina of Time (768 MiB after 90 minutes). Merged upstream as `2dcf679e15`, the commit we had cherry-picked: the upstream merge to `7c7f723b68` brings it as upstream's, with no second copy. |
| [Eglantine c40fca7da4](https://github.com/Eglantine-Community/Eglantine/commit/c40fca7da4) | kraibse | Swapchain image, surface format and present mode queries are repeated on VK_INCOMPLETE instead of aborting (moving the window during play) |
| [#1422](https://github.com/zortos293/Lichii/pull/1422) | [BrunoSilvestre00](https://github.com/BrunoSilvestre00) | Ocarina of Time: the "Text Speed 2x", "4x" and "Instant" add-ons (its author ported them from NotRexed/Hookshot#402); built-in add-ons that are off until checked, stored as `+<name>` in `[DisabledAddOns]`; groups of built-ins that patch the same place, where the first one applied wins |
| [Eglantine #837](https://github.com/Eglantine-Community/Eglantine/pull/837) and [#844](https://github.com/Eglantine-Community/Eglantine/pull/844) (merged in Eglantine, not yet in Lichii; a part) | [kraibse](https://github.com/kraibse), [rdenno](https://github.com/rdenno) | The `game_tweaks` map of settings that only affect one game, with its lookups and its test: Mario Kart World's and Hyrule Warriors' frame-rate options leave Graphics > Advanced. Eglantine's Tweaks tab is not taken: the game's own Adv. Graphics page lists them |
| [Eglantine #874](https://github.com/Eglantine-Community/Eglantine/pull/874) (merged in Eglantine, not in Lichii) | Rhys Denno; the write frame tracking: AManWithAPlan | A depth-tested native draw into a target first written in an earlier frame is skipped while its pipeline builds (`EDEN_NX2_SKIP_REDRAWN=1`), with the texture cache's per-image first write frame from Eglantine's stale target skip (c41d23692f) |
| [Eglantine f27522bfb9](https://github.com/Eglantine-Community/Eglantine/commit/f27522bfb9) (in Eglantine, not in Lichii) | [kraibse](https://github.com/kraibse) | Guest pages the CPU rewrites every frame stay unprotected and are uploaded at most once a frame, instead of being write-protected after each upload (`EDEN_NX2_HOT_PAGES=1`; on by default in Eglantine) |

When one of these is merged upstream, the next upstream sync brings the upstream version, and this table says so.

## Other Lichii forks

Work taken from other forks of Lichii. Their authors as recorded in git; each fork is GPL-3.0.

| Fork | Commits | Author | Change |
|---|---|---|---|
| [Fritchii](https://github.com/Frizti/Fritchii) (v5.8.4) | [58f55cb7d9](https://github.com/Frizti/Fritchii/commit/58f55cb7d9ee912931ce060ea8683694796e6ac3), [8627ff4233](https://github.com/Frizti/Fritchii/commit/8627ff4233323977fd5ead274867e4dd2ce66d5c) | [Frizti](https://github.com/Frizti) | The performance overlay's RAM and VRAM readouts: what the emulator holds and the room left, amber under 2 GB of RAM or 1 GB of video memory (hand port onto our overlay; the room of an integrated GPU's shared memory is also bounded by the free RAM) |
| [Fritchii](https://github.com/Frizti/Fritchii) (v5.8.4) | [c99790b878](https://github.com/Frizti/Fritchii/commit/c99790b8786cb6a5abc6a564a7115e2b8d15d937) | [Frizti](https://github.com/Frizti) | `EDEN_NX2_IR_POOL_CAP_MIB`: the shader recompiler's object pools free memory above a cap instead of keeping their largest shader's worth |
| [Fritchii](https://github.com/Frizti/Fritchii) (v5.8.4) | [a962f20533](https://github.com/Frizti/Fritchii/commit/a962f20533a1733375cadd0ec59ecf6f82d8ddaf) | [Frizti](https://github.com/Frizti) | `EDEN_NX2_ASYNC_DISCARD=2`: native draws with an unbuilt pipeline are skipped only into render targets cleared every frame; the others wait (hand port onto our pending-draw policy and texture cache) |

## Artwork

The Lichii logo, icon and library artwork that the packages still use are by YOB (yobslab), from upstream Lichii (`dist/branding`).

## Third-party components

The libraries the emulator uses keep their own licences: see [LICENSES](LICENSES), the notices in `externals/`, and the licence files inside the release packages (the Linux x86_64 packages ship fsr4-vk with its own).

## Guarana

The fork's own changes are listed in [CHANGELOG.md](CHANGELOG.md) and in git history, with their authors.
