# Guarana documentation

Every document under `docs/`, one line each. New here? Start with the [project README](../README.md),
then the [FAQ](FAQ.md). Many pages are inherited from upstream (Lichii, Eglantine, Eden) and still use
those names; where a page and the project README disagree, the README and [AGENTS.md](../AGENTS.md) win.

## Players and testers

| Doc | What it is |
|---|---|
| [FAQ.md](FAQ.md) | Short answers: legality, frame generation, hitches, settings, updates |
| [DEVICES.md](DEVICES.md) | Which GPUs and handhelds are tested, requirements, and how to send a device report |
| [TROUBLESHOOTING.md](TROUBLESHOOTING.md) | Start-up errors, hitches, pacing problems and crashes |
| [TESTING.md](TESTING.md) | The 10-minute test protocol: install, Hyrule Field, what to send, no debugging |
| [BENCHMARKING.md](BENCHMARKING.md) | How to measure your device so your numbers can be compared |
| [switches.md](switches.md) | The registry of every `EDEN_NX2_*` switch Guarana adds or relies on |
| [SIGNING.md](SIGNING.md) | How release files are signed and attested, how to check a download, and how to turn signing on |
| [frame-generation.md](frame-generation.md) | How frame generation predicts frames and the FLUID presenter paces them: the pipeline, known limits, how to test |
| [warm-up.md](warm-up.md) | Headless shader warm-up: `guarana-cli --warm-pipelines` builds a game's cached pipelines ahead of time |
| [user/ui.md](user/ui.md) | The window, play modes (Smooth, Quality, Battery), the performance overlay and shortcuts |
| [Options.md](Options.md) | CMake build options |
| [compatibility-report.md](compatibility-report.md) | Opt-in desktop compatibility reports |
| [overwolf-vulkan.md](overwolf-vulkan.md) | Overwolf Vulkan start-up protection |

## Contributors

| Doc | What it is |
|---|---|
| [../CONTRIBUTING.md](../CONTRIBUTING.md) | How to contribute, in order |
| [../AGENTS.md](../AGENTS.md) | Rules, repo map, build and verification for AI agents and humans |
| [AI_POLICY.md](AI_POLICY.md) | What AI assistance is welcome and what you must disclose |
| [ARCHITECTURE.md](ARCHITECTURE.md) | Map of the code, including frame generation and pacing |
| [LANES.md](LANES.md) | Long-lived branches, what lands where, and release steps |
| [FEEDBACK.md](FEEDBACK.md) | How a player's report becomes a measured fix in a release |
| [Build.md](Build.md) | General build instructions |
| [Deps.md](Deps.md) | Dependencies |
| [CrossCompile.md](CrossCompile.md) | Cross compiling |
| [build/Android.md](build/Android.md) | Android build notes |
| [build/OlderCommits.md](build/OlderCommits.md) | Building older commits |
| [Caveats.md](Caveats.md) | Platform-specific caveats (Arch, Gentoo) |
| [CI.md](CI.md) | Upstream Eglantine's CI and release guide; PGO retraining steps still apply |
| [CPMUtil.md](CPMUtil.md) | CPM, the CMake package manager wrapper |
| [Coding.md](Coding.md) | Coding guidelines |
| [Development.md](Development.md) | Development guidelines |
| [Debug.md](Debug.md) | Debugging guidelines, including RenderDoc |
| [Fuzzing.md](Fuzzing.md) | Fuzzing the game-file parsers |
| [../.github/CODEOWNERS](../.github/CODEOWNERS) | Review ownership |
| [../ROADMAP.md](../ROADMAP.md) | What is being worked on, with a definition of done for each item |
| [../CHANGELOG.md](../CHANGELOG.md) | What shipped in each version |

## Debug server and AI agents

| Doc | What it is |
|---|---|
| [AI-QUICKSTART.md](AI-QUICKSTART.md) | One command per AI tool, from zero to a pull request |
| [AGENTS-INVITE.md](AGENTS-INVITE.md) | The invitation and the contract for autonomous agents: picking, claiming and proving a task |
| [nx2/debug-mcp.md](nx2/debug-mcp.md) | The debug server and its MCP adapter: screenshots, frame times, logs, input |
| [nx2/amd.md](nx2/amd.md) | AMD GPUs and Switch 2 titles: differences found and how they are handled |
| [nx2/async-native-pipelines.md](nx2/async-native-pipelines.md) | Native graphics pipeline worker emission |
| [nx2/microphone.md](nx2/microphone.md) | Microphone input |
| [nx2/ocarina-dynamic-fps.md](nx2/ocarina-dynamic-fps.md) | The Ocarina of Time Dynamic FPS add-on |
| [nx2/oot-network-bypass.md](nx2/oot-network-bypass.md) | Ocarina of Time: skipping the game's anti-aliasing network |
| [nx2/rename.md](nx2/rename.md) | Renaming inherited yuzu names |
| [performance/MKW-INTEGRATION.md](performance/MKW-INTEGRATION.md) | Mario Kart World integration test |

## Emulator internals

| Doc | What it is |
|---|---|
| [DesignOverview.md](DesignOverview.md) | High-level design of the emulator |
| [HosKernel.md](HosKernel.md) | The HOS kernel |
| [Settings.md](Settings.md) | How settings are defined and stored |
| [NvidiaGpu.md](NvidiaGpu.md) | The NVIDIA SM86 GPU instruction set that Switch 2 shaders target |
| [DriverBugs.md](DriverBugs.md) | Known GPU driver bugs and workarounds |
| [autopilot.md](autopilot.md) | The experimental closed-loop autopilot (`EDEN_NX2_AUTOPILOT`) and its measurement plan |
| [native-dlss-replacement.md](native-dlss-replacement.md) | Replacing the game's DLSS networks |
| [dynarmic/README.md](dynarmic/README.md) | Dynarmic, the ARM JIT |
| [dynarmic/Design.md](dynarmic/Design.md) | Dynarmic design documentation |

## Investigations and reports

Working notes from specific games and sweeps. Useful as history and as templates for new reports.

| Doc | What it is |
|---|---|
| [bananza-atomic-fix.md](bananza-atomic-fix.md) | Donkey Kong Bananza global atomic-add fix |
| [bananza-global-atomics.md](bananza-global-atomics.md) | Bananza global atomics: unmapped append counters |
| [bananza-compute-followup.md](bananza-compute-followup.md) | Bananza compute compilation follow-up |
| [bananza-diagnostics.md](bananza-diagnostics.md) | Bananza diagnosis build |
| [bananza-live-trace-followup.md](bananza-live-trace-followup.md) | Bananza live trace follow-up |
| [bananza-pacing-followup.md](bananza-pacing-followup.md) | Bananza pacing follow-up |
| [bananza-performance-candidates.md](bananza-performance-candidates.md) | Bananza performance candidates |
| [bananza-profile-fixes.md](bananza-profile-fixes.md) | Bananza fixes after the lava-cave profile |
| [bananza-query-trace.md](bananza-query-trace.md) | Bananza query diagnostics |
| [bananza-timing-trace.md](bananza-timing-trace.md) | Bananza timing trace reference |
| [bananza-upstream-comparison.md](bananza-upstream-comparison.md) | Bananza shader comparison with upstream |
| [duskbloods-bindless-surfaces.md](duskbloods-bindless-surfaces.md) | Duskbloods register-handle image instructions |
| [duskbloods-uniform-immediate-shift.md](duskbloods-uniform-immediate-shift.md) | Duskbloods uniform shift with an immediate high word |
| [hitches.md](hitches.md) | Root-cause notes on the remaining Hyrule Field hitches on the Arc 140T |
| [kirby-white-world.md](kirby-white-world.md) | Kirby Air Riders white-world investigation |
| [FE_UPSCALE_STANDIN.md](FE_UPSCALE_STANDIN.md) | Fire Emblem: Fortune's Weave spatial upscale candidate |
| [STARFOX_UPSCALE_STANDIN.md](STARFOX_UPSCALE_STANDIN.md) | Star Fox spatial upscale candidate |
| [STARFOX_AFTERMATH_DIAGNOSTIC.md](STARFOX_AFTERMATH_DIAGNOSTIC.md) | Packaging an opt-in Star Fox Aftermath diagnostic |
| [STARFOX_COMMUNITY_CRASHES.md](STARFOX_COMMUNITY_CRASHES.md) | Star Fox community crash investigation |
| [VideoPlaybackDebug.md](VideoPlaybackDebug.md) | Debugging video playback |
| [VideoPlaybackUpstreamAudit.md](VideoPlaybackUpstreamAudit.md) | Upstream video playback audit |
| [VideoPlaybackUpstreamInventory.md](VideoPlaybackUpstreamInventory.md) | Upstream video search inventory |
| [shader-sweep-20261104.md](shader-sweep-20261104.md) | Offline shader sweep, 2026-11-04 |
| [shader-sweep-before-20261104.md](shader-sweep-before-20261104.md) | Before: shader sweep failures |
| [shader-sweep-after-20261104.md](shader-sweep-after-20261104.md) | After: shader sweep failures |
| [shader-sweep-all-games-20261104.md](shader-sweep-all-games-20261104.md) | Static library shader sweep, 2026-11-04 |
| [shader-sweep-all-games-before-20261104.md](shader-sweep-all-games-before-20261104.md) | Before: static library shader diagnostics |
| [shader-sweep-all-games-after-20261104.md](shader-sweep-all-games-after-20261104.md) | After: static library shader diagnostics |

## User handbook (inherited from Eden)

Player guides written for upstream Eden. They apply to Guarana where the program is called `guarana`
instead of `eden`; [Troubleshoot.md](user/Troubleshoot.md) and [Controllers.md](user/Controllers.md) are the most useful.

| Doc | What it is |
|---|---|
| [user/README.md](user/README.md) | Handbook index |
| [user/QuickStart.md](user/QuickStart.md) | Getting started |
| [user/Basics.md](user/Basics.md) | The basics |
| [user/Settings.md](user/Settings.md) | What each setting does |
| [user/Graphics.md](user/Graphics.md) | Graphics enhancements and visual quality |
| [user/DLSS.md](user/DLSS.md) | NVIDIA DLSS presets and quality |
| [user/Audio.md](user/Audio.md) | Audio notes |
| [user/Controllers.md](user/Controllers.md) | Controllers, including Steam Deck |
| [user/GyroControls.md](user/GyroControls.md) | Gyro and motion controls on Steam Deck |
| [user/Troubleshoot.md](user/Troubleshoot.md) | Common errors, such as Vulkan start-up and out-of-memory shaders |
| [user/HowToAccessLogs.md](user/HowToAccessLogs.md) | Finding logs to attach to a report |
| [user/Storage.md](user/Storage.md) | Data, save files and storage |
| [user/ImportingSaves.md](user/ImportingSaves.md) | Importing saves |
| [user/SyncthingGuide.md](user/SyncthingGuide.md) | Backing up and syncing saves |
| [user/Orphaned.md](user/Orphaned.md) | Recovering orphaned profiles |
| [user/InstallingUpdatesDLC.md](user/InstallingUpdatesDLC.md) | Installing updates and DLC |
| [user/Mods.md](user/Mods.md) | Installing mods |
| [user/InstallingAtmosphereMods.md](user/InstallingAtmosphereMods.md) | Installing Atmosphere-based mods |
| [user/UsingCheats.md](user/UsingCheats.md) | Using cheats |
| [user/UsingAmiibo.md](user/UsingAmiibo.md) | Using Amiibo |
| [user/AlterDateTime.md](user/AlterDateTime.md) | Setting a custom date and time |
| [user/CFW.md](user/CFW.md) | Why custom firmware is not supported |
| [user/CommandLine.md](user/CommandLine.md) | Command-line options for both frontends |
| [user/Architectures.md](user/Architectures.md) | Notes for different architectures and platforms |
| [user/Multiplayer.md](user/Multiplayer.md) | Multiplayer |
| [user/ServerHosting.md](user/ServerHosting.md) | Hosting a server or lobby |
| [user/SteamROM.md](user/SteamROM.md) | Adding the emulator to Steam with Steam ROM Manager |
| [user/ThirdParty.md](user/ThirdParty.md) | Third-party tools and extras |
| [user/Testing.md](user/Testing.md) | Testing a pull request and telling regressions apart |
| [user/Native.md](user/Native.md) | Native application development and debugging |

## Translations

| Doc | What it is |
|---|---|
| [i18n/README.md](i18n/README.md) | The translations, their status, and how to keep them in sync or add one |
| [i18n/README.zh-CN.md](i18n/README.zh-CN.md) | The project README in Simplified Chinese |
| [i18n/README.es.md](i18n/README.es.md) | The project README in Spanish |
| [i18n/README.pt-BR.md](i18n/README.pt-BR.md) | The project README in Brazilian Portuguese |
| [i18n/README.ja.md](i18n/README.ja.md) | The project README in Japanese |
| [i18n/README.ko.md](i18n/README.ko.md) | The project README in Korean |
| [i18n/README.fr.md](i18n/README.fr.md) | The project README in French |
| [i18n/README.de.md](i18n/README.de.md) | The project README in German |
| [i18n/README.ru.md](i18n/README.ru.md) | The project README in Russian |
| [i18n/README.ar.md](i18n/README.ar.md) | The project README in Arabic |

## Elsewhere in the repository

[LEGAL.md](../LEGAL.md) · [CREDITS.md](../CREDITS.md) · [SECURITY.md](../SECURITY.md) · [SUPPORT.md](../SUPPORT.md) · [CODE_OF_CONDUCT.md](../CODE_OF_CONDUCT.md) · [tools/README.md](../tools/README.md) · [tools/bench/README.md](../tools/bench/README.md)
