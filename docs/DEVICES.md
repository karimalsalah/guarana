# Devices

Guarana is built for integrated GPUs: Intel Arc first, AMD Radeon handhelds next. One device has
been fully tested so far (scene-gated gameplay); a second has sent a title-screen report
([#282](https://github.com/karimalsalah/guarana/issues/282)). Every other row below is waiting for a
report, and a report from your device is one of the most useful contributions you can make.

**Tested on one machine, built for thousands.** Sending a report takes minutes and no code:
[fill the device-report form](https://github.com/karimalsalah/guarana/issues/new?template=device-report.yml) →
`tools/ingest` parses it → CI validates the row and updates the
[community leaderboard](https://karimalsalah.github.io/guarana/benchmarks.html) pull request; verified reports
earn a row in the matrix below. The standing call is
[#55 — Call for testers: every device counts](https://github.com/karimalsalah/guarana/issues/55).

- **Tested:** the game ran and game fps was measured in a known scene, with the numbers published.
- **Reported:** a device report exists, but the full scene-gated gameplay bar is not met yet.
- **Untested:** nobody has sent a report yet. It may run well, badly or not at all.

## Requirements (v20)

| | Requirement | Tested with |
|---|---|---|
| Operating system | Stable packages: Windows x86_64/ARM64, Linux x86_64/aarch64 and Android ARM64. The nightly also publishes macOS arm64. Only Windows 11 x64 has been gameplay-tested ([stable releases](https://github.com/karimalsalah/guarana/releases/latest); [nightly](https://github.com/karimalsalah/guarana/releases/tag/nightly)) | Windows 11 x64 |
| CPU | x86-64 with AVX2: release builds use `x86-64-v3` code | Core Ultra 9 285H |
| GPU | A Vulkan driver; an integrated GPU is the target | Arc 140T, Intel driver 32.0.101.9175 |
| Memory | 16 GB of shared memory is below the comfortable floor: Switch 2 titles use a 12 GiB guest memory layout, and the iGPU takes its memory from the same RAM | 32 GB |
| Your files | Keys, firmware and games dumped from hardware you own | |

With 16 GB, enlarge the Windows page file before your first run:
[Shaders fail with "out of host memory"](user/Troubleshoot.md#shaders-fail-with-out-of-host-memory-on-windows).

## Support matrix

| GPU | Platform | Example devices | Status |
|---|---|---|---|
| Intel Arc 140T / 130T | Arrow Lake-H | Laptops | **Tested: Arc 140T.** The 130T is untested: send a report |
| Intel Arc 140V / 130V | Lunar Lake | MSI Claw 8 AI+, MSI Claw 7 AI+ | Untested: send a report |
| Intel Arc | Meteor Lake | MSI Claw A1M | Untested: send a report |
| Intel Arc B370 / B390 | Panther Lake | MSI Claw 8 EX AI+, other Arc G3 handhelds | Untested: send a report |
| Intel Graphics, 4 Xe3 cores | Panther Lake | Laptops | Untested: single-session readings only on a second laptop; no verified bench rows ([notes](#intel-graphics-4-xe3-cores-panther-lake)) |
| AMD Radeon 780M | | ROG Ally, ROG Ally X, Legion Go | Untested: send a report |
| AMD Radeon 880M / 890M | | ROG Xbox Ally X, Legion Go 2, MSI Claw A8 | Untested: send a report |
| AMD Radeon 8060S | Strix Halo | | Untested: send a report |
| Steam Deck, Ryzen Z2 A | RDNA2 | | Untested; likely too weak |

Discrete GPUs are not a target of this fork and are not tested here; reports are still welcome.

### Discrete GPU reports

| GPU | Device | Build | Scene | Real game fps | Status |
|---|---|---|---|---|---|
| NVIDIA RTX 5060 Ti | ASUS desktop, i5-14400F, 32 GB, Win11, 1440p@180 Hz | 20.5.0-rc1 | Title screen (vision-classified) | 60.0 (shown p99 18.8 ms) | **Reported** — [#282](https://github.com/karimalsalah/guarana/issues/282); gameplay sample requested |

## What has been measured

The verified `tools/bench` results below come from one laptop and one game: Core Ultra 9 285H, Arc
140T, Intel driver 32.0.101.9175, 32 GB RAM, *The Legend of Zelda: Ocarina of Time*. The switch A/B
was measured at the Hyrule Field overlook. Separate single-session readings from a second Panther
Lake laptop appear in the notes below and the [v20.3.0 changelog](../CHANGELOG.md); they have no
verified bench rows and do not make that GPU tested. **Game fps** counts the frames the game renders;
generated frames are not included.

| Configuration | Game fps |
|---|---:|
| `EDEN_NX2_GM` off, frame generation off | 14.75 |
| `EDEN_NX2_GM=11`, frame generation off (same spot) | 22.83 |
| v20 play config, frame generation 3x toward 60: live readings in Castle Town / in the field | 26.7 / 28 |

The play-config readings are not a same-spot A/B. The README has the
[full table and caveats](../README.md#measured-results).

## Third-party data points

Reported elsewhere, with other builds, settings and scenes. They are **not verified by us** and are
not directly comparable with the numbers above.

| GPU | Report | Source |
|---|---|---|
| AMD Radeon 890M | About 30 fps in Ocarina of Time | Upstream Lichii, [issue #1433](https://github.com/zortos293/Lichii/issues/1433) |
| Intel Arc 140V | 22-24 fps in gameplay | A third-party build by AlfaDev23 |

Turning one of these into a Guarana row, measured as in [BENCHMARKING.md](BENCHMARKING.md), is a
great first contribution.

## Notes by vendor

### Intel Arc

- v20 includes three Arc driver fixes ported from
  [Fritchii-Intel-Fixes](https://github.com/LuckyLuckLucky/Fritchii-Intel-Fixes): broken light beams,
  flashing lights and NaN bloom, and native float atomics.
- Ocarina of Time already defaults to `EDEN_NX2_GM=11`; leave it unset unless you need to override
  it. The Windows Intel iGPU preset chooses frame-generation settings for the display. On 120/144 Hz
  displays, leave `EDEN_NX2_EXTRAPOLATE` unset: enabling it overrides the shipped locked interpolation
  sets ([#119](https://github.com/karimalsalah/guarana/issues/119)).
- Intel's driver takes 0.4-1 s to compile one pipeline, so first visits hitch. The pipeline-cache
  crash reported in [IGCIT #1614](https://github.com/IGCIT/Intel-GPU-Community-Issue-Tracker-IGCIT/issues/1614)
  has an experimental, opt-in mitigation behind `EDEN_NX2_CACHE_SAFE=1` ([#7](https://github.com/karimalsalah/guarana/issues/7)).
  It remains off by default while its boot deadlock is unresolved.
- Results depend on the driver build: name yours in every report.

### Intel Graphics, 4 Xe3 cores (Panther Lake)

The GPU family `IntelGraphicsPantherLake` (8086:B0A0 and B090, which Intel lists with 4, 3 or 2 Xe3
cores). Only a 4-core B0A0 has been seen; the notes below are that one machine, and the 3- and 2-core
parts are in the family without evidence of their own.

- **Device:** a Panther Lake laptop, Intel driver 32.0.101.8724 (Windows driver 101.8724), a
  2880x1800 panel at 144 Hz.
- **What the driver lists** (read from the driver): `VK_KHR_cooperative_matrix` (cooperative
  matrices in compute shaders), `VK_EXT_vertex_input_dynamic_state`,
  `VK_EXT_extended_dynamic_state3`, `VK_EXT_graphics_pipeline_library` and `VK_EXT_host_image_copy`;
  subgroup size 32 (16 to 32, size control on); no native ASTC; present modes IMMEDIATE and FIFO
  only (no MAILBOX).
- **What ran:** in a first session the preset for Intel integrated GPUs without matrix cores
  applied. It is chosen when the emulator finds no cooperative matrix shape it can use for the
  game's tensor operations, whatever extensions the driver lists. The `Device profile:` line and
  the preset line in your log say what yours chose.
- **Measured:** single runs of Ocarina of Time in Kokiri Forest with the 144 Hz pacing set, which
  is chosen for this display by default: 24 real and 91.5 shown frames a second against 20 and
  50.5 with the 60 Hz set, in one session. The numbers and their caveats are in the
  [v20.3.0 CHANGELOG entry](../CHANGELOG.md) and next to the set in `src/core/game_presets.cpp`.
- **Not measured:** any `tools/bench` row on a verified scene, so no setting is chosen for this
  GPU alone. A preset for it needs a paired run on a verified scene first
  ([AGENTS.md](../AGENTS.md), rule 1).

### AMD Radeon

- Untested here; the AMD path is [next on the roadmap](../ROADMAP.md#x7-amd-radeon-igpu-path).
- AMD drivers handle some things Vulkan leaves undefined differently from NVIDIA's, and upstream
  keeps a list of the differences and the open AMD reports: [docs/nx2/amd.md](nx2/amd.md). Read it
  before filing an AMD bug.
- The switches in the play config were tuned on Intel. `EDEN_NX2_POWER_POLICY=igpu` engages on any
  integrated GPU, AMD included, but that is untested. Measure with and without each switch and send
  both rows.

## The device profile in the log

When a game boots, the log gets one `Render.Vulkan` Info line that says what the machine is, which
device class it falls in and which GPU family its GPU is in. It looks like this (the values are an
example):

```text
Device profile: class=Integrated gpu_family=IntelArrowLakeH label="Intel Arc 140T, integrated, 32 GB, hybrid 6P+8E+2LPE, NPU" gpu_vendor=Intel gpu_model="Intel(R) Arc(TM) 140T GPU (16GB)" gpu_type=integrated gpu_driver=101.9175 gpu_local_mib=16384 gpu_uma=yes cpu_logical=16 cpu_physical=16 cpu_hybrid=6P+8E+2LPE ram_mib=32768 ram_source=installed power=ac display_hz=60.00 npu=yes
```

| Field | Meaning |
|---|---|
| `class` | The device class, from the table below |
| `gpu_family` | The GPU's model family, from the second table below, or `None` |
| `label` | The same machine in a few words: GPU, its kind (and memory when discrete), RAM, cores, `on battery`, `NPU` |
| `gpu_vendor`, `gpu_model` | The GPU's vendor (by PCI vendor ID) and the name its driver reports |
| `gpu_type` | `integrated`, `discrete`, `virtual`, `cpu` (a software renderer) or `unknown` |
| `gpu_driver` | The driver version, as on the `Driver:` line |
| `gpu_local_mib` | The largest device-local memory heap the driver lists, in MiB. On an integrated GPU this is shared RAM |
| `gpu_uma` | `yes` when the GPU's memory is the machine's RAM: an integrated GPU, a software renderer, or a driver that lists only device-local heaps |
| `cpu_logical`, `cpu_physical` | Logical processors and cores (0: unknown) |
| `cpu_hybrid` | Performance, efficiency and low-power efficiency cores as the OS ranks them (Windows: the cores' efficiency class; Linux: the `cpu_atom` list or `cpu_capacity`), or `no` |
| `ram_mib`, `ram_source` | RAM in MiB. `installed` is the amount fitted (Windows, macOS); `os` is the memory the OS manages, less what firmware and the GPU reserve |
| `power` | `ac`, `battery` or `unknown`. Only Windows says |
| `display_hz` | The refresh rate of the display the window is on, or `unknown`. Only Windows says |
| `npu` | `yes` when Windows lists a device under **Neural processors** in Device Manager, `no` when it lists none, `unknown` elsewhere. An NPU without its driver is not listed |

| Class | When |
|---|---|
| `IntegratedLowPower` | An integrated GPU with less than 10 GiB of RAM, or on battery |
| `Integrated` | Any other integrated GPU |
| `DiscreteHigh` | A discrete GPU with 8 GB or more of its own memory. Drivers keep some back, so 7.5 GiB and up counts |
| `DiscreteEntry` | A discrete GPU with less |
| `Unknown` | A virtual GPU, a software renderer, or a GPU whose kind or memory the driver does not say |

The class is broad: an Arc 140T and the small Panther Lake GPU are both `Integrated`. The GPU
family is the model the PCI IDs Vulkan reports prove (vendor and device ID, and the name where one
ID covers several parts). It needs an integrated GPU on the vendor's own driver (Intel's or AMD's
Windows driver, or Mesa's ANV or RADV); anything else is `None`, and so is any GPU not listed here.

| GPU family | PCI IDs |
|---|---|
| `IntelArcMeteorLake` | Intel 7D55: Meteor Lake "Intel Arc Graphics", 128 or 112 EUs |
| `IntelArrowLakeH` | Intel 7D51: Arrow Lake-H "Intel Graphics", 128 or 112 EUs, whatever name the driver gives it (the size of an Arc 140T or 130T; Intel does not call 7D51 Arc). 7DD1 only when the name says 140T or 130T |
| `IntelArcLunarLake` | Intel 64A0: Lunar Lake "Intel Arc Graphics", 64 or 56 EUs (the size of an Arc 140V or 130V). 64B0 only when the name says 140V or 130V |
| `IntelGraphicsPantherLake` | Intel B090, B0A0: Panther Lake "Intel Graphics", 4, 3 or 2 Xe3 cores |
| `IntelArcB370` | Intel B081, B083, B085, B087: Panther Lake Arc B370 and Arc Pro B370, 11 or 10 Xe3 cores |
| `IntelArcB390` | Intel B080, B082, B084, B086: Panther Lake Arc B390 and Arc Pro B390, 12 Xe3 cores |
| `AmdRadeon780M` | AMD 15BF, 1900 when the name says 780M (15BF is also the 760M and 740M, 1900 also the 760M) |
| `AmdRadeon880M`, `AmdRadeon890M` | AMD 150E when the name says 880M or 890M |
| `AmdRadeon8060S` | AMD 1586 when the name says 8060S (the same ID is also the 8050S and 8040S) |
| `AmdCustomGpu0405` | AMD 163F, "AMD Custom GPU 0405" (the Steam Deck LCD's) |
| `AmdCustomGpu0932` | AMD 1435, "AMD Custom GPU 0932" (the Steam Deck OLED's) |

The IDs come from the Linux kernel's `include/drm/intel/pciids.h`, Intel's lists of the GPUs its
i915 and Xe drivers support, and libdrm's `amdgpu.ids`. A GPU of yours in no family can be added
with its source.

The class and the family change nothing yet. A preset can match either one, but a default for a
class or a family comes only with a paired run on a verified scene of that class or family
([AGENTS.md](../AGENTS.md), rule 1). The line holds no paths or names from your machine, so you
can paste it into a report as it is.

## The driver verdict in the log

Right after the `Device profile:` line the engine says what it knows about the graphics driver's version (`Common::AssessDriver` in `src/common/device_profile.cpp`):

```text
Driver version: at or above 32.0.101.9175
The NVIDIA driver 572.16.0.0 is older than 580.119.02: dynamic vertex input is turned off on older ones, so more pipeline variants are built. Updating the graphics driver is worth it.
```

The second form is logged for an old driver. Where the renderer works around the old driver (the first two rows) it is a warning and the window shows it once per run of the program; the Intel row is a log line only, because its version is the one measured on, not a known defect. A driver gets a verdict only where the code or a measurement names a version:

| Driver | Version to have | What an older one costs |
|---|---|---|
| NVIDIA's own driver | 580.119.02 | Dynamic vertex input is turned off, so more pipeline variants are built |
| Mesa RADV | 26.2.5 | Native mesh shaders take a slower workaround |
| Intel's Windows driver, on the Core Ultra and Arc B-series integrated families (every Intel `gpu_family`) | 32.0.101.9175 | It is the driver the Intel numbers were measured on; older ones are not tested |

Every other driver has no line: no finding is not a verdict. Older Intel GPUs have their own driver branch with lower numbers and are left out on purpose. A new row needs the same evidence as these: a version the code tests for, or one a measurement was taken on.

## The resolution line in the log

After the device profile, the log gets one `Core` Info line that starts `Resolution:`. It says what
the game's settings preset, its add-ons and its pacing switches were resolved to on this machine,
what was decided for each given what you had set (`set`, `already set`, `kept`, `over shipped`),
and how many of the values rest on a measured row of [switches.md](switches.md). The same data,
with each entry's reason and evidence, is the debug server's `status.resolution`
([nx2/debug-mcp.md](nx2/debug-mcp.md)). The line also records the output size against the GPU's
size: the pixels of the display the window is on, the GPU's compute units or SMs where the driver
reports them (AMD and NVIDIA; Intel's Windows driver does not, so it says `not reported by the
driver`), and the output pixels per core. On the reference laptop (2880x1800 on 4 Xe3 cores) 72
shown frames measured better than 144 on every metric; nothing decides by these numbers yet, they
are logged so that reports from other machines carry them. A default that follows them needs a
paired run on a verified scene first ([AGENTS.md](../AGENTS.md), rule 1).

## Send a device report

[Open a device report](https://github.com/karimalsalah/guarana/issues/new/choose), or attach bench rows
to a pull request as described in [BENCHMARKING.md](BENCHMARKING.md). A good report has:

```text
Device profile:    (the "Device profile:" line from the log)
Device:            (model, for example "MSI Claw 8 AI+")
CPU / GPU:         (for example "Core Ultra 9 285H / Arc 140T")
GPU driver:        (exact version)
RAM:               (total, and how much the GPU reserves if your firmware shows it)
Power:             (AC or battery; power mode or TDP)
Display:           (resolution and refresh rate; handheld screen or external)
Guarana build:    (first line of the log, or the release name)
Game and scene:    (title, and where in the game)
Switches:          (contents of your nx2_switches.txt)
Frame generation:  (off, or type, multiplier and target)
Result:            (game fps and how you measured it; stutters, crashes, graphics errors)
```

Leave out keys, firmware, game files and anything personal. If you attach a log, read it first: it
contains paths from your machine.
