# Guarana on ARM64: Snapdragon X laptops and Android handhelds

Guarana is for integrated GPUs, and many of them sit next to ARM64 CPUs: the Adreno in Snapdragon X laptops, and the Adreno in Android handhelds and phones (the AYN Odin 2 has a Snapdragon 8 Gen 2 and the Retroid Pocket 5 a Snapdragon 865, [Notebookcheck](https://www.notebookcheck.net/Retroid-Pocket-5-Specs-release-date-and-launch-pricing-confirmed-for-new-retro-gaming-handheld.885414.0.html), [Handheld Index](https://handheldindex.pages.dev/comparisons/ayn-odin-2-vs-retroid-pocket-5/)). The Switch 2's CPU is ARM64 too, so these devices can also run game code without a translator in between ([NCE.md](NCE.md)).

This page is the design note: what runs where, how CI builds it, what the GPU drivers have to provide, and what testers should report. Nobody on the project owns one of these devices. Everything here was built by CI and read in the code, not booted: device reports are the next step ([call for testers](#call-for-testers)).

## What runs where

| Device | Download | CPU emulation | Status |
|---|---|---|---|
| Snapdragon X laptop (Windows 11 on ARM) | `guarana-<version>-windows-arm64.zip` (from #228) | dynarmic's ARM64 JIT, fastmem | Never booted by the project |
| | `guarana-<version>-windows-x86_64.zip` | dynarmic's x64 JIT, run by Windows' Prism emulator | Should run since Prism emulates AVX2 (Windows 11 24H2, October 2025 update), which the x64 builds need; slower, two translators in a row. Never booted by the project |
| Android 13 or newer, arm64 | `guarana-<version>-android-arm64.apk` (from #227) | Native code execution (NCE) for titles with a 39-bit address space, else dynarmic's ARM64 JIT | Never booted by the project |
| Linux on ARM64 | `guarana-<version>-linux-aarch64.AppImage` / `.tar.gz` | Same as Android | Published with the releases, never booted by the project |
| Apple Silicon Mac | `guarana-<version>-macos-arm64.zip` | dynarmic's ARM64 JIT, MoltenVK | Upstream's build |

Neither upstream builds the two new ones: zortos293/Lichii and Eglantine publish Windows x64, Linux and macOS only, and Eden's CI has its Windows ARM64 row commented out. The Android app in the tree is Eden's; nobody had compiled it against the Switch 2 code before Guarana's CI did.

## How CI builds them

**Windows ARM64** (`release-binaries.yml`, row `windows-arm64`, on GitHub's `windows-11-arm` runner):

- clang-cl from LLVM 22.1.8's native ARM64 build (`tools/windows/install-llvm-woa64.ps1`, pinned by checksum).
- Qt 6.11.1 from Eden-CI's static `windows-arm64` bundle (`LICHII_USE_BUNDLED_QT`), and SDL3, FFmpeg and OpenSSL from crueter-ci's `windows-aarch64` packages. The FFmpeg has no AV1 decoder: the AV1 build the x64 packages use has no ARM64 variant, so a Switch 2 title's AV1 movies may not play.
- ThinLTO, no PGO: the profile is x64's.
- Boost.Context assembled from its GNU-syntax PE sources, which armasm64 cannot take.
- The host-thread sampler (`nx2_sampler.cpp`, x64 registers) is stubbed, as on other hosts.
- It runs in every release and the nightly, not in pull requests.

**Android** (`android.yml`):

- Eden's own `.ci/android/build.sh`: the mainline flavor's arm64-v8a APK, built with NDK r29 (r28's libc++ lacks `std::atomic_ref`, which Lichii's ARM64 code uses) and CMake 3.31.5.
- It runs on pull requests that change the app, and in every release and the nightly. It is not a required check.
- The app is `io.github.karimalsalah.guarana`, beside Eden. Until the owner adds a release key it is a preview build, `io.github.karimalsalah.guarana.preview` ([SIGNING.md](SIGNING.md), "Android").
- The `EDEN_NX2_*` switches are read from `nx2_switches.txt` in the app's user folder (Open Guarana folder), as the desktop builds read theirs.

Both keep to the cache budget: their compiler caches are saved only by tags, the nightly and `main`.

## What the GPU has to provide

The renderer is the same on every platform. What decides whether an Adreno runs it, from `video_core/vulkan_common/vulkan_device.h`:

- **Mandatory:** Vulkan 1.1; the extensions `VK_EXT_vertex_attribute_divisor`, `VK_KHR_driver_properties`, `VK_KHR_sampler_mirror_clamp_to_edge` and `VK_KHR_shader_float_controls`; and core features including geometry and tessellation shaders, dual-source blending, logic ops, wide lines, cull distances, storage image writes without a format, and variable pointers. A device that lacks one is refused, and the log says which: `Missing required extension ...`.
- **Mesh shaders** (`VK_EXT_mesh_shader`) are optional, and the most important gap to watch. Switch 2 games draw foliage and props with task and mesh shaders. Without the extension, each of those draws is emulated with compute dispatches that end the render pass first (`vk_rasterizer.cpp`), and indirect mesh draws are skipped.
  - Ending a pass costs a tile-based GPU like Adreno more than a desktop GPU, because it writes the tiles to memory and reads them back.
  - `EDEN_NX2_MESH_BEFORE_PASS=1` moves the dispatches before the pass where it can ([switches.md](switches.md)). It is unmeasured, and an Adreno report would be its first.
  - The log's device feature list says whether the driver has mesh shaders (`mesh_shader=`).
- **Texture formats:** Adreno decodes ASTC in hardware. Where an Adreno vendor driver lacks the BCn formats, the Android app patches the driver to expose them (`libadrenotools`, `vulkan_device.cpp`); otherwise the renderer decodes BCn itself (`texture_cache/decode_bc.cpp`).
- **Drivers on Android:** the vendor driver, or a Mesa Turnip build loaded through the app's GPU driver manager (`libadrenotools`, which the app links). Reports must say which one ran.
- **Snapdragon X** uses Qualcomm's Windows Vulkan driver; the report form asks for its version.

None of the iGPU defaults engage on Adreno: the game presets and Ocarina of Time's pacing switches match Intel and AMD GPUs only (`core/game_presets.cpp`). The FLUID presenter, frame budget and frame generation are in the shared code and can be turned on through the switch file. An Adreno default comes only with a measured row (AGENTS.md, rule 1).

## Expected gain

| Change | Expected | Source |
|---|---|---|
| Native Windows ARM64 build instead of x64 under Prism | Less CPU time: dynarmic's output runs directly instead of being translated again. No number yet | To be measured: the same scene with both zips on one laptop |
| NCE on Android and Linux ARM64 | 20-100% fps in CPU-bound Switch 1 games, by game and device | [yuzu, November 2023](https://yuzu-mirror.github.io/entry/yuzu-progress-report-nov-2023); Switch 2 unmeasured |
| Mesh shaders on Adreno | Decides whether foliage-heavy scenes run well at all | The drivers' feature lists in device reports |

## Call for testers

Wanted: Snapdragon X laptops (X Elite, X Plus, X2), and Android devices with a Snapdragon 8 Gen 2 or newer, a G3x Gen 2, or older chips for a baseline. Handhelds such as the AYN Odin 2 family and Odin 3, the AYANEO Pocket S and the Retroid Pocket 5 and Mini, and Snapdragon phones: name your chip in the report.

1. Download from the [nightly pre-release](https://github.com/karimalsalah/guarana/releases/tag/nightly), or from a release once one carries them:
   - Windows on ARM: `windows-arm64.zip`, plus `windows-x86_64.zip` to compare.
   - Android: open `android-arm64.apk` on the device to install it.
2. Boot a game you dumped from your own console. The emulator needs your own keys and firmware; none are provided. On Android, if a game crashes at boot or right after, set the CPU backend to Dynarmic (Settings > CPU) and try again: NCE, the default, has never run Switch 2 code. Report both results.
3. Open a [device report](https://github.com/karimalsalah/guarana/issues/new?template=device-report.yml). Pick your Adreno in "Graphics card"; on Android pick "Phone or tablet" or "Handheld". Include:
   - the GPU driver: the vendor's, or Turnip and its version;
   - fps, p99 frame time and the scene;
   - whether the image looks right;
   - the log's lines starting `Vulkan`, `Missing required` and `mesh_shader`, with your user name removed from paths.
4. Paired runs are worth the most:
   - Windows: the ARM64 zip against the x64 zip.
   - Android: CPU backend NCE (the default) against Dynarmic.
   - Both: `EDEN_NX2_MESH_BEFORE_PASS=1` against unset, in a scene with grass.

A crash or a game that will not boot goes in a crash or game bug report instead, with the same GPU and driver answers.
