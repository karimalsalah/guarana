# Credits and attribution

Guarana is free software under the **GPL-3.0-or-later** ([LICENSE.txt](../LICENSE.txt)), and it stands on
years of other people's work. This page is the docs-facing attribution record; the canonical credits live in
[CREDITS.md](../CREDITS.md) at the repository root, and everyone who committed here is in
[CONTRIBUTORS.md](../CONTRIBUTORS.md). Git history keeps every author; upstream copyright notices stay in the
source files.

## The lineage

```
yuzu → Eden → Eglantine → Lichii → Guarana
```

| Project | What Guarana owes it |
|---|---|
| **yuzu** | Where this line of Switch emulators began. |
| **[Eden](https://git.eden-emu.dev/eden-emu/eden)** | The emulator Eglantine forked, and the Switch emulation work of its team. |
| **[Eglantine](https://github.com/Eglantine-Community/Eglantine)** | Lichii's base. Its original developer is **Camille LaVey**. |
| **Sudachi** | Credited by Lichii among the projects it builds on. |
| **[Lichii](https://github.com/zortos293/Lichii)** (zortos293 and [its contributors](https://github.com/zortos293/Lichii/graphs/contributors)) | The emulator this fork is: its `nx2` branch with Switch 2 support, the Vulkan renderer work, the debug server and the PGO profiles. Guarana tracks `nx2` through regular upstream syncs ([CHANGELOG.md](../CHANGELOG.md)). |

Upstream pull requests Guarana carries — including ones upstream had not merged when we took them — are listed
with their authors in [CREDITS.md](../CREDITS.md#upstream-pull-requests-we-carry).

## Ocarina of Time built-in add-ons

The showcase title is the **native Nintendo Switch 2 release of Ocarina of Time** (title `0400486024DEC000`,
update 1.1.1, build `E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348`) — not the N64 original via the NSO app.
Its built-in add-ons carry named work:

| Add-on / mechanism | Credit | Source |
|---|---|---|
| **Dynamic FPS** routine | **Dark** (Eglantine developer), who gave permission to bundle it; **Frizti**'s `oot-gather-fix` commit `765659af4ccb` preserved the call and routine and supplied the built-in integration; **CTGL**'s [Lichii #299](https://github.com/zortos293/Lichii/pull/299) set the delta cap to `CNTFRQ / 10` | [docs/nx2/ocarina-dynamic-fps.md](nx2/ocarina-dynamic-fps.md) |
| **FPS Unlock** two-block design, carry fixes (cloth, Epona jump, day/night clock, Deku web) | Lichii `nx2` fixes ported from Frizti/Fritchii's `oot-gather-fix` (`5e47afa889`), following [Hookshot](https://github.com/NotRexed/Hookshot) (`db0dd7140b`) and HeyListen (`4383cd71e`); upstream Lichii [#2313](https://github.com/zortos293/Lichii/pull/2313)/[#2314](https://github.com/zortos293/Lichii/pull/2314)/[#2317](https://github.com/zortos293/Lichii/pull/2317)/[#2318](https://github.com/zortos293/Lichii/pull/2318), carried into Guarana by the upstream sync ([PR #304](https://github.com/karimalsalah/guarana/pull/304)) | [docs/nx2/ocarina-dynamic-fps.md](nx2/ocarina-dynamic-fps.md), [changelog.d](../changelog.d) |
| **Text Speed 2x / 4x / Instant** | **NotRexed** ([Hookshot #402](https://github.com/NotRexed/Hookshot/pull/402)), by way of [Lichii #1422](https://github.com/zortos293/Lichii/pull/1422); shipped as the JSON pack `dist/patch-packs/0400486024DEC000-text-speed.json` | [docs/patch-packs.md](patch-packs.md) |
| Whole-tick counter sites (ocarina song, scarecrow's song) | **Hookshot** found both sites (`nx2/oot_patches.py`, `WHOLE_SITES`, GPL-3.0); the stubs in Guarana are its own | [docs/nx2/ocarina-dynamic-fps.md](nx2/ocarina-dynamic-fps.md) |
| Code-cave placement clear of mods and cheats | The **KaeGae registry** ([Kabosune/oot-remake-modding](https://github.com/Kabosune/oot-remake-modding), `registry/e0f3ccfa.toml`) | [docs/nx2/ocarina-dynamic-fps.md](nx2/ocarina-dynamic-fps.md) |
| **Texture Pool x2** | **RsMan-Dev**'s texture pool patch | `src/core/file_sys/patch_manager.cpp` |
| **Max Resolution** fixed render scales (DLSS Balanced/Performance/Ultra Performance) | **ProfessorTee**'s OoT render scales ([Lichii #903](https://github.com/zortos293/Lichii/pull/903)) | `src/core/file_sys/patch_manager.cpp`, [docs/user/DLSS.md](user/DLSS.md) |
| **No sharpening** (the game's tensor-network bypass) | Guarana original work on the remake's anti-aliasing network; upstream context Lichii [#1688](https://github.com/zortos293/Lichii/issues/1688)/[#1875](https://github.com/zortos293/Lichii/pull/1875) | [docs/nx2/oot-network-bypass.md](nx2/oot-network-bypass.md) |
| DLSS Native DLAA sizing | **myshadowww** ([Lichii #840](https://github.com/zortos293/Lichii/pull/840)), adapted | [docs/user/DLSS.md](user/DLSS.md) |

## Intel fixes

[Fritchii-Intel-Fixes](https://github.com/LuckyLuckLucky/Fritchii-Intel-Fixes): the cooperative-matrix,
shuffle-bounds, HMMA rounding and native atomic-add patches behind `EDEN_NX2_INTEL_COOP_MATRIX` and friends,
tested by their authors on an Arc B580 and an Arc 140V. Details and patch numbers:
[CREDITS.md](../CREDITS.md#intel-fixes).

## Branding and third-party components

- The Guarana name and readme art are the fork's own; the icon set the packages still use is by **YOB (yobslab)**,
  from upstream Lichii (`dist/branding`).
- The libraries the emulator uses keep their own licences: [LICENSES](../LICENSES), the notices in `externals/`,
  and the licence files inside the release packages (the Linux x86_64 packages ship fsr4-vk with its own).

## Guarana's own changes

Listed in [CHANGELOG.md](../CHANGELOG.md) and in git history, with their authors. Performance claims follow the
project's rule: every number names its machine, and a default changes only with a paired measurement
([AGENTS.md](../AGENTS.md), [docs/BENCHMARKING.md](BENCHMARKING.md)).

Built by the Guarana project — [github.com/karimalsalah/guarana](https://github.com/karimalsalah/guarana).
