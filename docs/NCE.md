# Native code execution (NCE) on ARM64 hosts

The Switch 2's CPU runs ARM64 code, and so do Snapdragon X laptops, Android handhelds and phones, ARM64 Linux machines and Apple Silicon Macs. On such a host the game's code can run on the host CPU directly instead of through dynarmic's JIT, which translates every block first. This page says what runs where today, what NCE needs on each operating system, the gain to expect, and the plan, starting with `EDEN_NX2_NCE_REPORT`.

Nobody on the project owns an ARM64 device yet: everything below that says "runs" was compiled by CI and read in the code, not booted. Device reports turn it into facts ([ARM64.md](ARM64.md) has the call for testers).

## What Guarana carries

yuzu's NCE, which Eden and Lichii kept: [`src/core/arm/nce/`](../src/core/arm/nce/). yuzu introduced it in November 2023, adapting Skyline's way of running game code natively on ARM devices ([yuzu progress report, November 2023](https://yuzu-mirror.github.io/entry/yuzu-progress-report-nov-2023)).

- **The patcher** (`patcher.cpp`) rewrites, at load time, the instructions that must reach the emulator into branches to trampolines: supervisor calls (`SVC`), the thread pointer (`MRS` of `TPIDR_EL0` and `TPIDRRO_EL0`, `MSR TPIDR_EL0`) and the counter (`MRS CNTPCT_EL0`, `CNTFRQ_EL0`). It makes exclusive loads and stores ordered. Everything else runs as it is.
- **The runtime** (`arm_nce.cpp`, `arm_nce.s`) swaps between host and guest context on the same thread and uses Linux signals: faults on guest memory, an interpreter for loads and stores the host cannot do (`interpreter_visitor.cpp`), and a signal to stop a running guest thread.
- **The gate** (`CMakeLists.txt`): `HAS_NCE` is set for ARM64 Linux and Android only. There `Settings::values.cpu_backend` defaults to NCE (`src/common/settings.h`), and `Settings::SetNceEnabled` (`settings.cpp`) keeps it only with fastmem and a 39-bit address space. A 36-bit or 32-bit program falls back to dynarmic, as in yuzu.
- **Switch 2 titles reach it.** `loader/nup.cpp` and `loader/nx.cpp` hand the ExeFS to `AppLoader_DeconstructedRomDirectory`, which calls `SetNceEnabled` with the title's NPDM address-space type and runs the patcher on every module (`deconstructed_rom_directory.cpp`, `nso.cpp`).
- **Page size.** Fastmem refuses a host whose pages are not 4 KiB while NCE is selected (`common/host_memory.cpp`, "does not support fastmem with NCE"). Android 15 and later run on some 16 KiB-page devices ([Android developers](https://android-developers.googleblog.com/2024/08/adding-16-kb-page-size-to-android.html)), and Asahi Linux uses 16 KiB pages.

## What runs today

| Host | Build | Guest code runs on | Status |
|---|---|---|---|
| Windows and Linux, x86-64 | `windows-x86_64`, `linux-x86_64` | dynarmic, x64 backend | The fork's target; measured |
| Linux ARM64 | `linux-aarch64` AppImage and tar.gz | NCE by default for a 64-bit title with a 39-bit address space, else dynarmic's ARM64 backend | Never booted by the project |
| Android | `android-arm64` APK, once `android.yml` lands (#227) | Same as Linux ARM64 | Never booted by the project |
| Windows on ARM (Snapdragon X) | `windows-arm64` zip, once its release row lands (#228) | dynarmic, ARM64 backend; fastmem through the Windows exception handler | Never booted by the project. The x64 build should also run there under Prism, which emulates AVX2 since the October 2025 update of Windows 11 24H2 ([Windows Latest](https://www.windowslatest.com/2025/10/21/windows-11-on-arm-is-finally-ready-for-gaming-with-avx-avx2-support-now-rolling-out)), at the cost of translating dynarmic's x64 output once more |
| macOS, Apple Silicon | `macos-arm64` | dynarmic, ARM64 backend | Upstream's build, not tested here |

## Expected gain

On Android, yuzu measured NCE at 20 to 100% more frames per second than dynarmic in Switch 1 games, "on a game-by-game basis, as well as on a device-by-device basis", and more with more power and thermal headroom ([yuzu progress report, November 2023](https://yuzu-mirror.github.io/entry/yuzu-progress-report-nov-2023)). Press reports went up to 200%. That is the ceiling for a scene bound by the game's own CPU work. A scene bound by the GPU, or by the emulator's own threads (GPU command processing, shader compiles, read-backs of GPU data), gains little; on the Arc 140T much of an Ocarina of Time frame is the latter ([NX2_NOTES.md](../NX2_NOTES.md), rows 98 and 108). For Switch 2 titles there is no number yet; a gain becomes a claim only with a paired run on a verified scene (AGENTS.md, rule 1).

## Plan

### 1. Know what Switch 2 code needs (`EDEN_NX2_NCE_REPORT`, this change)

With `EDEN_NX2_NCE_REPORT=1`, every module a game loads gets one log line, on any host, x86-64 included:

```
NCE report for main: 2381206 instructions; patched: 412 SVC, 37 thread pointer, 21 counter; 1904 exclusive (made ordered); run natively, reads: FPCR x96 CNTVCT_EL0 x3 ...; run natively, writes: FPCR x88
```

(The counts above are made up to show the format.) It decodes the module with the patcher's own decoders (`instructions.h`): what NCE would rewrite, and every other system register access, which NCE would leave to the host CPU. Registers the host answers differently from the console are the work NCE needs before a title runs right on it:

- `CNTVCT_EL0`, `CNTVCTSS_EL0`, `CNTPCTSS_EL0`: counters the patcher does not rewrite. Natively they read the host's counter at the host's rate, while the emulator gives the guest 19.2 MHz (`CNTFREQ`, `core/hardware_properties.h`); many hosts count at another rate.
- `MIDR_EL1`, `ID_AA64*`: identification registers. EL0 code cannot read them directly; Linux emulates the read and answers with the host's values.
- Anything unnamed (`S3_...`): look it up before trusting NCE with that title.

The owner, or anyone with Switch 2 games, runs each title once with the switch on any machine and adds a row to the table below. No ARM64 device is needed.

| Title | Build | Modules | Native registers that need work | Date |
|---|---|---|---|---|
| (no row yet) | | | | |

### 2. Close what the report finds (Linux ARM64 and Android)

Each register in that table that matters gets patched like `CNTPCT_EL0` (a trampoline that returns the emulated value), behind a default-off `EDEN_NX2_*` switch until a tester's paired run shows the title right. The same goes for an instruction the host lacks: the Switch 2's Cortex-A78C class cores and the hosts players use (Snapdragon 8 Gen 2 and newer, Snapdragon X) are newer, so a missing extension is unlikely, but the report's register list is where it would show first (`ID_AA64ISAR*` reads).

### 3. Testers prove it (Linux ARM64 and Android)

For each title: boot with the default backend (NCE), then with `cpu_backend = Dynarmic` (Settings > CPU on Android, the config file elsewhere). Report both in one device report: fps, p99 frame time, the scene, whether the image and audio are right. Watch for the log line `Program does not utilize 39-bit address space` (the title fell back to dynarmic) and `does not support fastmem` (a 16 KiB-page device).

### 4. Windows on ARM: a port, later

No emulator in this line runs guest code natively on Windows ARM64. Eden's CI does not even build for it: its Windows ARM64 row is commented out, next to a note asking whether ARM works ([Eden-CI/Workflow `.ci/matrix/msvc.sh`](https://github.com/Eden-CI/Workflow/blob/HEAD/.ci/matrix/msvc.sh)). What a port needs:

- **x18.** On Windows ARM64, x18 is the platform register: in user mode it points to the thread's TEB ([Microsoft, ARM64 ABI conventions](https://learn.microsoft.com/cpp/build/arm64-windows-abi-conventions)). Guest code may use it as an ordinary register. Whether Windows tolerates that while guest code runs (exceptions, APCs) is not documented; a test program on a tester's laptop decides it. A next increment of the report can count x18 writes per title.
- **Faults and preemption.** Linux signals become a vectored exception handler (dynarmic's `exception_handler_windows.cpp` already has an ARM64 path for fastmem) and `SuspendThread`/`SetThreadContext` to stop a running guest thread. `arm_nce.s` is GNU assembly with Linux system calls: it needs a Windows port.
- **Memory** is ready: fastmem on Windows uses placeholder mappings (`common/host_memory.cpp`), and Windows ARM64 has 4 KiB pages.
- **Alternative:** running the guest in a virtual machine (Windows Hypervisor Platform), as Ryujinx did with Apple's Hypervisor.framework on macOS. That avoids x18 and SVC patching, if the platform offers it on ARM64 laptops.

Weeks of work, and only worth it once steps 1 to 3 show Switch 2 titles gaining on Linux ARM64 or Android.

### 5. macOS

Apple Silicon uses 16 KiB pages, which this NCE cannot map 4 KiB guest pages on; the hypervisor route above would be the way. Not planned.

## Sources

- yuzu, [Progress Report November 2023](https://yuzu-mirror.github.io/entry/yuzu-progress-report-nov-2023): NCE introduced, 20-100% on Android, 39-bit titles only.
- Microsoft, [Overview of ARM64 ABI conventions](https://learn.microsoft.com/cpp/build/arm64-windows-abi-conventions): x18 reserved, the TEB in user mode.
- Android developers, [Adding 16 KB page size to Android](https://android-developers.googleblog.com/2024/08/adding-16-kb-page-size-to-android.html).
- Windows Latest, [Windows 11 on Arm gets AVX and AVX2 in Prism](https://www.windowslatest.com/2025/10/21/windows-11-on-arm-is-finally-ready-for-gaming-with-avx-avx2-support-now-rolling-out).
- Eden-CI, [`.ci/matrix/msvc.sh`](https://github.com/Eden-CI/Workflow/blob/HEAD/.ci/matrix/msvc.sh): the Windows ARM64 row commented out.
