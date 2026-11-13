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

[Fritchii-Intel-Fixes](https://github.com/LuckyLuckLucky/Fritchii-Intel-Fixes): patches 0001 (dynamic primitive topology follows the pipeline's tessellation stages), 0003 (shuffles stay in bounds on host subgroups narrower than the guest warp) and 0010 (native float32 storage atomic add with `VK_EXT_shader_atomic_float`), ported to Lichii `nx2` in this fork. The patches name their author only as "Fritchii Intel fixes"; their authors tested them on an Arc B580 and an Arc 140V.

## Upstream pull requests we carry

Pull requests to zortos293/Lichii that were not merged upstream when we took them. Names are the commit authors as recorded in git.

| Pull request | Author | Change |
|---|---|---|
| [#948](https://github.com/zortos293/Lichii/pull/948) | [danieljesus](https://github.com/danieljesus) | ASTC textures with uneven mips stay in BC on integrated GPUs and cards under 6 GiB; the threshold compares against the heap size, not the budget |
| [#933](https://github.com/zortos293/Lichii/pull/933) | mstampfli | A suspended list copy is polled without a kernel call each time |
| [#1155](https://github.com/zortos293/Lichii/pull/1155) | [PantACRO4life](https://github.com/PantACRO4life) | Texture descriptors are flushed one by one when the pool runs past the GPU address space |
| [#1218](https://github.com/zortos293/Lichii/pull/1218) | [PantACRO4life](https://github.com/PantACRO4life), [Nanoc6](https://github.com/Nanoc6) | GPU memory registry and address-space listeners are called outside their locks, so the GPU memory registry no longer deadlocks |
| [#408](https://github.com/zortos293/Lichii/pull/408) | ZeroOneTV | Pipeline cache: a masked key keeps its graphics record, used pipelines are relinked; kernel object lifetime and HostMemory move tests |
| [#1568](https://github.com/zortos293/Lichii/pull/1568) | [cerroverb](https://github.com/cerroverb) | Device memory clamps the cold hint size and skips cold hints for placeholder addresses |
| [#1417](https://github.com/zortos293/Lichii/pull/1417) | [PantACRO4life](https://github.com/PantACRO4life) | Clean shutdown: AM releases an application process it never took, services stop without the server lock held |
| [#1564](https://github.com/zortos293/Lichii/pull/1564) | Hugo | The compute shader precache marker follows the file's contents and the shader lowering |
| [#1447](https://github.com/zortos293/Lichii/pull/1447) | [zortos293](https://github.com/zortos293) | Draw templates: each draw's sampled textures and image descriptors are kept across frames (`EDEN_NX2_DRAW_TEMPLATE_VERIFY` checks them) |
| [#1571](https://github.com/zortos293/Lichii/pull/1571) | [jpkto](https://github.com/jpkto) | The texture cache's bounded retire scan resumes where it stopped; overwritten render targets are deleted after ten unused frames |
| [#1378](https://github.com/zortos293/Lichii/pull/1378) | Krim | Graphics pipelines reuse the translated shaders of an earlier pipeline when their translation inputs and the guest memory they read match (`EDEN_NX2_SHADER_REUSE=1`) |

When one of these is merged upstream, the next upstream sync brings the upstream version, and this table says so.

## Artwork

The Lichii logo, icon and library artwork that the packages still use are by YOB (yobslab), from upstream Lichii (`dist/branding`).

## Third-party components

The libraries the emulator uses keep their own licences: see [LICENSES](LICENSES), the notices in `externals/`, and the licence files inside the release packages (the Linux x86_64 packages ship fsr4-vk with its own).

## Guarana

The fork's own changes are listed in [CHANGELOG.md](CHANGELOG.md) and in git history, with their authors.
