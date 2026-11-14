# Devices

Guarana is built for integrated GPUs: Intel Arc first, AMD Radeon handhelds next. One device has
been tested so far. Every other row below is waiting for a report, and a report from your device is
one of the most useful contributions you can make.

- **Tested:** the game ran and game fps was measured in a known scene, with the numbers published.
- **Untested:** nobody has sent a report yet. It may run well, badly or not at all.

## Requirements (v20)

| | Requirement | Tested with |
|---|---|---|
| Operating system | Windows x64 is the tested platform. Linux and macOS builds are on Releases; untested on these devices ([docs/Build.md](Build.md) to build from source) | Windows |
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
| AMD Radeon 780M | | ROG Ally, ROG Ally X, Legion Go | Untested: send a report |
| AMD Radeon 880M / 890M | | ROG Xbox Ally X, Legion Go 2, MSI Claw A8 | Untested: send a report |
| AMD Radeon 8060S | Strix Halo | | Untested: send a report |
| Steam Deck, Ryzen Z2 A | RDNA2 | | Untested; likely too weak |

Discrete GPUs are not a target of this fork and are not tested here; reports are still welcome.

## What has been measured

All Guarana numbers so far come from one laptop and one game: Core Ultra 9 285H, Arc 140T, Intel
driver 32.0.101.9175, 32 GB RAM, *The Legend of Zelda: Ocarina of Time*. The switch A/B was measured
at the Hyrule Field overlook. **Game fps** counts the frames the game renders; generated frames are
not included.

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
- Start with the play config from the [quick start](../README.md#quick-start): `EDEN_NX2_GM=11`,
  `EDEN_NX2_POWER_POLICY=igpu`, and for Ocarina of Time `EDEN_NX2_EXTRAPOLATE=1` with frame generation
  3x toward 60.
- Intel's driver takes 0.4-1 s to compile one pipeline, so first visits hitch. Saving the pipeline
  cache while pipelines are being built can crash the driver
  ([IGCIT #1614](https://github.com/IGCIT/Intel-GPU-Community-Issue-Tracker-IGCIT/issues/1614)); a
  fix is in progress.
- Results depend on the driver build: name yours in every report.

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
