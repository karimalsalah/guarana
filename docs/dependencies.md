# Dependencies

Every third-party dependency Guarana builds with, the version it pins, the newest stable release,
what our upstreams pin, and what we do about it. Surveyed on 2026-11-16 from each project's GitHub
releases and tags, the security advisories GitHub publishes for it, and the configure logs of
`build-check` on `main` (f18523c4bd).

How to read the tables:

- **Lichii** is zortos293/Lichii `nx2` and **Eglantine** is Eglantine-Community/Eglantine `nx2`.
  Both pin exactly what we pin in `cpmfile.json`, except where a row says otherwise.
- **Built by** is the CI job that compiles or links the pin: `both` (Linux GCC and Windows clang-cl
  in `build-check`), `Windows`, `Linux`, `release` (only `release-binaries.yml`: Windows PGO,
  Linux x86_64 and arm64, macOS), or `none`. Linux takes some packages from Ubuntu 24.04 instead
  of the pin (the version is in brackets). A bump only `release` or `none` builds is not merged
  without a build that compiles it.
- **Risk** is that of moving to the newest release: low (no API or behaviour change for us),
  medium (API or build changes, or behaviour CI cannot see), high (needs code changes or a
  measurement).
- **Plan**: `upgrade` (a pull request of its own), `keep` (nothing newer, or a reason not to), or
  `proposal` (needs a decision or a build CI does not have; listed under [Proposals](#proposals)).

## CPM packages (`cpmfile.json`)

Downloaded and checked by SHA-512 by `CMakeModules/CPMUtil.cmake`; patches are in `.patch/<name>/`.

| Package | Pinned | Newest stable | Upstream | Built by | Risk | Plan and notes |
|---|---|---|---|---|---|---|
| httplib (yhirose/cpp-httplib) | v0.46.0 | v0.66.0 (2026-11-10) | same | both | medium | **upgrade first (security).** 18 of the 25 releases since 0.46.0 list security fixes, among them the `Cookie` header kept on a cross-origin redirect (GHSA-22mf-w2v3-r2jv, fixed in 0.51.0; 0.46.0 has the same code) and request parameters sent on to another origin on a redirect (fixed in 0.65.0). Our updater, DLSS and FSR4 runtime downloads, icon search and the Nextendo login use its client. Breaking: `Headers`/`Params` became insertion-ordered vectors (0.52), braced `Get(path, {{...}})` is ambiguous (0.50), certificates are matched on subjectAltName only (0.64). |
| zlib (madler/zlib) | v1.3.2 | v1.4 (2026-11-05) | same | Windows (Linux: system 1.3) | medium | **upgrade (security).** Fixes CVE-2026-85091 (gzwrite overflow), an `inflateGetDictionary()` overflow present since 1.2.8, and several inflate/deflate and minizip memory-safety bugs. Breaking: CMake 3.15 minimum, MSVC library names changed, `z_off_t` back to `long`. |
| fmt (fmtlib/fmt) | 12.1.0 | 12.2.1 (2026-11-07) | same | both | low | proposal: GHSA-vp42-wr8v-475c (heap overflow) affects 32-bit targets only, which we do not ship. Every file includes fmt, so the bump is a near-cold build on the pull request and again on `main`. |
| Catch2 | v3.13.0 | v3.16.1 (2026-10-08) | same | both (tests only) | low | upgrade. Test framework only; 3.15.1 fixes an out-of-bounds read in line breaking. Patch `0001-solaris-isnan-fix.patch` must still apply. |
| QuaZip (stachenov/quazip) | 2e95c9001b | v1.7.2 (2026-06-30) | same | both | low | upgrade. v1.7.2 is 24 commits ahead of our pin on the same branch (a release tag instead of a commit). |
| lz4 | ebb370ca83 (v1.10.0) | v1.11.1 (2026-11-10) | same | Windows (Linux: system 1.9.4) | low | upgrade. Functions deprecated since 1.7/1.8 left the headers in 1.11.0; we must not use them. |
| sirit | v1.0.5 | v1.0.7 (2026-09-01) | same | Linux (Windows uses sirit-ci v1.0.7) | - | keep: 1.0.6 and 1.0.7 change only sirit's CI packaging, not its source. |
| SimpleIni (brofield/simpleini) | v4.25 | v4.27 (2026-09-01) | same | both | medium | keep: 4.26 reformats the whole header and replaces its UTF-8 conversion, and neither release fixes anything we hit. It parses every settings file, so move only with a settings round-trip test. |
| xxHash | v0.8.3 | v0.8.4 (2026-09-19) | same (Lichii only; Eglantine has no xxhash) | both | low | upgrade. No API change; new vector paths (NEON preferred over SVE on aarch64) and command-line work. |
| libusb | v1.0.29 | v1.0.30 (2026-05-17) | same | Windows (Linux: system 1.0.27) | medium | proposal: 1.0.30 adds hotplug on Windows, which changes the path our adapter code takes there, and a new `os/windows_hotplug.c` that our own source list in `externals/libusb/CMakeLists.txt` must add. Its descriptor parsing memory-safety work is the reason to do it. |
| Vulkan-Headers, Vulkan-Utility-Libraries | v1.4.345 | v1.4.368 | same | both | medium | upgrade last, together (they must match). New enumerants can trip `-Werror=switch`; a near-cold build of `video_core`. |
| SPIRV-Headers | vulkan-sdk-1.4.341.0 | vulkan-sdk-1.4.363.0 | same | Windows (Linux: system 1.5.5) | low | upgrade with the Vulkan SDK proposal, or alone. |
| VulkanMemoryAllocator | v3.3.0 | v3.5.0 (2026-10-26) | same | both | high | proposal: allocation behaviour on an iGPU is a performance question; needs a paired `tools/bench` run. Patch `0001-macos-clang.patch` is for macOS clang, which only the release build compiles. |
| xbyak | v7.40.1 | v7.44 (2026-10-22) | same | both | medium | proposal: the x86-64 JIT emitter of dynarmic and the macro JIT. Bump with a game boot check. |
| cubeb (mozilla/cubeb) | fa02160712 | no releases; 91 commits since the pin | same | both | medium | proposal: audio backend; changes are audible, not testable in CI. |
| Boost (eden-emulator/ext-boost) | 1.92.0 | 1.92.0 (1.93 is in beta) | same | both | - | keep. |
| boost_headers | boost-1.91.0 | boost-1.92.0 | same | none | low | keep: only used when the Boost package has no `Boost::headers`, which neither CI job hits. |
| nlohmann_json | v3.12.0 | v3.12.0 | same | both | - | keep. |
| zstd | 33618c8 | v1.5.7 is older than the pin | same | both | high | keep: the pin is a `dev` commit past 1.5.7 and no newer release exists. NUP archives are byte-identical across platforms only when every build compresses with the same zstd (root `CMakeLists.txt`), so any change needs a NUP round-trip check. |
| opus | a3f0ec02b3 | v1.6.1 is older than the pin | same | both | - | keep: the pin is 6 commits past v1.6.1. |
| enet | v1.3.18 | v1.3.18 | same | both | - | keep (17 unreleased commits). |
| frozen | 61dce5ae18 | 1.2.0 is older than the pin | same | both | - | keep: 13 commits past 1.2.0. |
| cpp-jwt | 7f24eb4c32 | v1.5.1 is older than the pin | same | both | - | keep: 3 commits past v1.5.1. |
| gamemode | ce6fe122f3 | 1.8.2 is older than the pin | same | both (Linux code) | - | keep: 17 commits past 1.8.2. |
| reshade | v6.8.0 | v6.8.0 | same | both | - | keep. |
| streamline (NVIDIA-RTX) | v2.14.16 | v2.14.16 | same | Windows | - | keep. `src/lichii/configuration/nvidia_dlss_panel.cpp` downloads the same version's runtime: bump both together. |
| discord-rpc | 76616d8675 | no newer commit | same | none (`USE_DISCORD_PRESENCE=OFF` in CI) | - | keep. |
| zbic | 11b08f2712 | no newer commit | same | both | - | keep. |
| nx_tzdb (eden-emu/tzdb_to_nx) | 230326 | 230326 | same | both | - | keep (git.eden-emu.dev). |
| llvm-mingw runtime (eden-emu) | 20250828 | 20250828 | same | Windows | - | keep (git.eden-emu.dev). Revisit with an LLVM major change. |
| MoltenVK (V380-Ori/Ryujinx.MoltenVK) | v1.4.1-ryujinx | v1.4.1-ryujinx | same; Lichii#1970 (open) moves to a sparse-binding fork | release (macOS) | - | keep; follow Lichii#1970. |
| SDL3 source | release-3.4.8 | release-3.4.20 (2026-11-03) | same | none (CI uses sdl3-ci) | low | keep until a build uses it; see the SDL3 proposal. |
| OpenSSL source, openssl-cmake | openssl-3.6.2, 3.6.2 | openssl-3.6.5; openssl-cmake 4.1.0 | same | none (Linux, macOS: system; Windows: openssl-ci) | low | keep until a build uses it; see the OpenSSL proposal. |
| FFmpeg source | c7b5f1537d | - | same | none (only `LICHII_USE_EXTERNAL_FFMPEG`, Solaris) | - | keep. |
| oaknut | v2.0.3 | v2.0.3 | same | release (arm64 only) | - | keep. |
| biscuit | v0.19.0 | v0.20.0 | same | none (RISC-V only) | - | keep until a RISC-V build exists. |
| lagoon | 1.0.0 | 1.0.0 | same | none (LoongArch only) | - | keep. |
| libadrenotools, linkernsbypass, Vulkan-ValidationLayers | 8ba23b42d7, aa3975893d, vulkan-sdk-1.4.341.0 | -, -, vulkan-sdk-1.4.363.1 | same | none (Android only) | - | keep until the Android build is in CI. |
| breakpad | f80f288803 (in `externals/CMakeLists.txt`) | v2024.02.16 tag, 118 commits since | same | none (`LICHII_CRASH_DUMPS` off) | - | keep. |

## Prebuilt packages

Binary packages: `ci` entries in `cpmfile.json` take their SHA-512 from the release's `.sha512sum`
file, and `CMakeModules/NxFormat.cmake` pins SHA-256 per platform.

| Package | Pinned | Newest | Upstream | Built by | Plan and notes |
|---|---|---|---|---|---|
| openssl-ci (crueter-ci/OpenSSL) | 4.0.1-1788234794 | 4.0.1-1789774337 (a rebuild); OpenSSL itself is at 4.0.3 and 4.1.0 | same | Windows | **proposal (security).** The Windows build ships OpenSSL 4.0.1. 4.0.2 (moderate) and 4.0.3 (high) are security releases, and no prebuilt package carries them. |
| sdl3-ci (crueter-ci/SDL3) | 3.4.14-1788231389 | 3.4.14 (SDL itself: 3.4.20) | same | both | proposal: no newer package. SDL 3.4.16 to 3.4.20 fix Joy-Con stick calibration and Switch controllers failing on missing stick and IMU calibration, among other things. |
| ffmpeg-ci-av1 (Takoroka/ffmpeg-av1) | 9.0.2-av1-1 | 9.0.2-av1-1 | same | Windows | keep. |
| ffmpeg-ci-av1-mingw (Djihads80/Eglantine) | 9.0.2-av1-mingw-1 | same | same | none (MinGW) | keep. |
| ffmpeg-ci (crueter-ci/FFmpeg) | 9.0.2-1790726865 | same | same | none in CI | keep. |
| ffmpeg-ci-av1-linux (Eglantine-Community/FFmpeg) | - | 9.0.2-1794581964 | Eglantine only (Eglantine#856, 2026-11-13) | - | proposal: static FFmpeg 9.0.2 with dav1d for Linux x86_64 and arm64. Eglantine's reason (a tar.gz that needed the system's FFmpeg 6) does not apply here: our tar.gz and AppImage are bundled by linuxdeploy. Only `release-binaries.yml` would build it. |
| sirit-ci (eden-emulator/sirit) | v1.0.7 | v1.0.7 | same | Windows | keep. |
| nxformat (fab918/nxformat) | 1.0.3 | 1.0.4 (2026-11-15) | Lichii 1.0.3; Eglantine has none | both | upgrade: better Switch 2 Edition and multi-program imports in the converter; reader API and C ABI unchanged. |

## Vendored code

Copied into the tree; upgraded by hand, if ever.

| Code | Version | Newest | Notes |
|---|---|---|---|
| `src/dynarmic` | Eden's fork, in tree | - | Follows upstream merges. |
| `externals/stb` | stb_image 2.28, stb_image_write 1.16, stb_image_resize 0.97, stb_dxt 1.12 | nothings/stb has no releases | keep. |
| `externals/glad` | generated, GL 3.3 core | glad v2.0.8 | keep. |
| `externals/renderdoc/renderdoc_app.h` | API 1.7.0 | RenderDoc v1.47 | keep. |
| `externals/FidelityFX-FSR`, `FidelityFX-SDK`, `ffx-api` | FSR 1, SDK subset, upscaler API 4.x | FidelityFX SDK v2.4.0 | keep; `tools/fsr4-vk/build.sh` pins its SDK files to commit 60f4ea8190. |
| `externals/demangle` | LLVM's ItaniumDemangle | - | keep. |
| `externals/nxformat` | header-only C++17 reader, 1.0.3 | 1.0.4 | keep: loads `.nx` games in `core`. 1.0.4's reader changes are opt-in and the wire format is unchanged. The converter library (above) is a separate pin; no file includes both readers. |
| `externals/bc_decoder`, `getopt`, `tz` | - | - | keep. |

## Toolchain and CI

| Item | Pinned | Newest | Upstream | Plan and notes |
|---|---|---|---|---|
| LLVM (clang-cl, `tools/windows/install-llvm.ps1`) | 22.1.8 | 22.1.8 is the last 22.x; 23.1.5 (2026-11-03) | Lichii 22.1.8 | proposal (major change, the owner's call): also regenerates the Windows PGO profile (`lichii-windows.profdata`) and may need a newer llvm-mingw runtime. |
| clang for Linux PGO (`apt.llvm.org`) | 23 | 23 | Eglantine 23 | keep. |
| Qt (`install-qt-action`) | 6.8.3 | 6.8.3 is the last open-source 6.8; 6.12.0, 6.11.3 | same 6.8.3; bundled Qt (`AddQt`) 6.11.1 | proposal (minor change, the owner's call). |
| Vulkan SDK (`tools/windows/install-vulkan-sdk.ps1`) | 1.4.341.1 | 1.4.363.0 | same | proposal: changes the glslang that compiles our host shaders in Windows CI and releases. Needs a scene check. |
| GCC (Linux `build-check`) | 13 (Ubuntu 24.04) | - | same | keep. |
| CMake (Linux `build-check`, devcontainer) | 4.4.4 | 4.4.5, 4.5.0 | - | upgrade to 4.4.5 (patch) with the next CI change. |
| ccache (Windows) | 4.14.3 | 4.14.3 | same | keep. |
| sccache (Windows release) | 0.19.0 | 0.19.0 | same | keep. |
| linuxdeploy and its Qt plugin | `continuous` | - | same | keep; not pinned. |
| Homebrew (macOS release) | not pinned | - | same | keep. |
| Runners | ubuntu-24.04, windows-2022, macos-15; launcher on windows-latest | windows-2025 | Eglantine: windows-2025 | keep. |
| GitHub Actions | pinned by SHA or tag | - | Eglantine also uses Dependabot | Dependabot (`.github/dependabot.yml`) proposes these weekly. All are on their newest release except `actions/checkout` v5 in `bench-rows.yml` and `switch-registry.yml`. |
| Node.js (launcher, promo) | 22 | 24 LTS | - | keep (22 is maintained until April 2027). |
| nixpkgs (`shell.nix`) | ac588ef1c9 | - | - | keep. |

## Launcher (`launcher/`)

Built and tested by `launcher.yml` on Windows. Dependabot proposes the Cargo and npm updates weekly,
once a release is a week old. OSV (2026-11-16) lists no advisory for a locked package the Windows
build compiles. The lockfile's Linux-only part has glib 0.18 (RUSTSEC-2024-0429, unsound iterator)
and four crates marked unmaintained; the launcher is not built for Linux.

| Package | Locked | Newest | Plan and notes |
|---|---|---|---|
| tauri, tauri-build, tauri plugins; `@tauri-apps/cli`, `@tauri-apps/api` | 2.12.5, 2.7.4; CLI 2.12.4, API 2.12.4 | 2.12.6, 2.7.5 (2026-11-16); CLI 2.12.5, API 2.12.5 | Dependabot. |
| tauri-plugin-deep-link, tauri-plugin-notification | =2.6.3, =2.8.1 (exact) | 2.6.3, 2.8.1 | keep. |
| reqwest, rfd | 0.12.28, 0.17.2 | 0.13.5, 0.18.0 | Dependabot; breaking (0.x) releases, so check its pull request's build. |
| zip, sysinfo, vite | 9.4.1, 0.40.1, 8.3.6 | 9.6.0, 0.40.4, 8.3.8 | Dependabot. |
| typescript, `@resvg/resvg-js` | 7.0.2, 2.6.2 | 7.0.2, 2.6.2 | keep. |
| Rust | `rust-version = 1.95`; runner's stable | 1.100 | keep. |

## Android (`src/android`, not built in CI)

Gradle 8.13, Android Gradle Plugin 8.9.1, Kotlin 1.9.20, NDK 28.2.13676358, compile and target SDK
36, and the AndroidX libraries in `app/build.gradle.kts`. Not surveyed further: no workflow builds
the APK yet, so no bump could be tested.

## Proposals

These need the owner's decision or a build CI does not have:

1. **OpenSSL 4.0.3 for Windows.** The shipped OpenSSL 4.0.1 misses two security releases. No
   prebuilt package has them. Options: publish our own package from a fork of crueter-ci/OpenSSL,
   as Eglantine did for FFmpeg (Eglantine-Community/FFmpeg); or build `openssl` and
   `openssl-cmake` 4.0.3 from source on Windows, which adds minutes to every cold build.
2. **SDL 3.4.20.** Same situation as OpenSSL: build from the `sdl3` source pin (switch
   `LICHII_USE_BUNDLED_SDL3` off in CI), or publish a package. Controller changes need a player's
   check.
3. **Qt 6.8.3 to 6.11 or 6.12.** 6.8.3 gets no more open-source patches. Upstream's bundled Qt is
   6.11.1.
4. **LLVM 23.** 22.1.8 is the last 22.x. Needs a new Windows PGO profile.
5. **Vulkan SDK 1.4.363.0** in Windows CI and releases, with a scene check of host shaders.
6. **VulkanMemoryAllocator 3.5.0**, **xbyak 7.44**, **cubeb**: each needs a paired benchmark or a
   play check that CI cannot give.
7. **fmt 12.2.1**: low value for 64-bit builds; costs a full rebuild.
8. **libusb 1.0.30**: Windows hotplug is a behaviour change for the adapter code.
9. **Eglantine#856** (static FFmpeg on Linux): adopt when Lichii merges it, or when a test of
   `release-binaries.yml` on a branch shows it builds on both Linux architectures.

## Upgrading a CPM package

1. Change `version` in `cpmfile.json` and set `hash` to the SHA-512 of the archive CPMUtil
   downloads: `https://github.com/<repo>/archive/<version>.tar.gz`, or the release `artifact`
   (`tools/cpmutil.sh package version <name> <version>` and `tools/cpmutil.sh package hash <name>`
   do both).
2. Check that every patch in `.patch/<name>/` still applies to the new tree (`git apply --check`),
   and that the CMake targets and options we set in `cpmfile.json` and `externals/CMakeLists.txt`
   still exist.
3. One dependency per pull request, with the release notes' relevant lines in its description and
   a `changelog.d` fragment. Update this file's row.
