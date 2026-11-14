// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "common/common_types.h"
#include "common/device_profile.h"
#include "core/game_settings.h"

// Settings a game is known to run better with on a kind of GPU, for the ones the user left alone.
// A preset value is a per-game value of the running session only: no config file takes it
// (Settings::HoldPresetValue). Off with the auto_presets setting or EDEN_NX2_NO_PRESETS=1.
namespace Core::GamePresets {

struct GpuInfo {
    GameSettings::GPUVendor vendor{GameSettings::GPUVendor::Unknown};
    bool integrated{};
    /// The guest's tensor operations (HMMA) have no cooperative matrix shape to run on, so they
    /// are emulated with scalar shader math.
    bool software_tensor_math{};
    /// The machine's device class (Common::Classify). No preset or switch default uses it yet.
    Common::DeviceClass device_class{Common::DeviceClass::Unknown};
    /// The GPU's model family (Common::ClassifyGpuFamily). No preset or switch default uses it
    /// yet.
    Common::GpuFamily gpu_family{Common::GpuFamily::None};
};

/// The vendor a PCI vendor ID (VkPhysicalDeviceProperties::vendorID) names: Intel 0x8086, AMD
/// 0x1002, NVIDIA 0x10DE, else Unknown. Pure. The renderer reads the vendor once, from the device
/// profile's PCI vendor ID, and gives it to the settings presets and the switch defaults alike.
[[nodiscard]] GameSettings::GPUVendor VendorOfPciId(u32 vendor_id);

/// What a preset asks of the GPU. Unknown takes any vendor or device class, None any GPU family,
/// a false flag takes either state.
struct GpuMatch {
    GameSettings::GPUVendor vendor{GameSettings::GPUVendor::Unknown};
    bool integrated{};
    bool software_tensor_math{};
    Common::DeviceClass device_class{Common::DeviceClass::Unknown};
    Common::GpuFamily gpu_family{Common::GpuFamily::None};
};

/// A switchable setting by its label, and the value as BasicSetting::ToString gives it.
struct SettingValue {
    std::string_view label;
    std::string_view value;
};

struct Preset {
    u64 title_id{};
    GpuMatch gpu{};
    std::span<const SettingValue> settings{};
    /// Built-in add-ons the preset counts on. They are on unless the user disabled them in
    /// Add-Ons, which is kept: this only reports it.
    std::span<const std::string_view> addons{};
    std::string_view reason{};
};

enum class Decision {
    Apply,
    AlreadySet,  ///< The global value is the preset's.
    UserPerGame, ///< The title's config sets it.
    UserGlobal,  ///< The global value is not the default.
};

[[nodiscard]] std::span<const Preset> Presets();

[[nodiscard]] bool Matches(const GpuMatch& match, const GpuInfo& gpu);

/// The first preset of the title this GPU matches, or nullptr. The list's order decides, not how
/// narrow a match is: a preset for a GPU family goes before a broader one of the same title.
[[nodiscard]] const Preset* Find(std::span<const Preset> presets, u64 title_id,
                                 const GpuInfo& gpu);

/// What to do with one preset value, from the setting's state as its strings.
[[nodiscard]] Decision Decide(std::string_view preset_value, bool per_game,
                              std::string_view global_value, std::string_view default_value);

/// Applies the title's preset for this GPU to Settings::values. Call it once the GPU is known,
/// before the settings are read. Returns what it did in one line, empty without a preset.
std::string Apply(u64 title_id, const GpuInfo& gpu);

/// An EDEN_NX2_* switch a title runs better with on a kind of GPU.
struct SwitchValue {
    std::string_view name;
    std::string_view value;
};

/// The pacing switch set a title gets, by what the machine is.
enum class PacingSet {
    None,  ///< No switch defaults.
    Hz60,  ///< The 60 Hz set: real frames shown and predicted toward 60.
    Hz120, ///< Locked to a 120 Hz display, in-between frames interpolated, 20 real frames.
    Hz144, ///< Paced to a 144 Hz display, in-between frames interpolated, real frames held to 24
           ///< a second (CADENCE_DIVISOR=6).
};

/// The display the game's window is on, as the host reports it (Common::VblankClock).
struct DisplayInfo {
    double refresh_hz{}; ///< Its refresh rate in Hz; 0 when unknown: the primary display's is used.
    std::string name;    ///< The host's name for it, such as \\.\DISPLAY2; empty when unknown.
    bool primary{};      ///< It is the primary display. Only meaningful with a name.
};

/// The whole-Hz rate ChoosePacingSet is given: the window's display's (`window_hz`, rounded to
/// the nearest Hz) where the host reported one, else `primary_hz`, the primary display's (0 when
/// unknown). On a machine with one display the two are the same display, so the choice is the one
/// the primary display's rate gave. Pure.
[[nodiscard]] u32 ChooseRefreshHz(double window_hz, u32 primary_hz);

/// Names the display and its rate for the log: `display \\.\DISPLAY2 (primary) at 144 Hz`, or
/// `(not the primary display)`, or `the window's display at 144 Hz` when the host gave no name;
/// where the window's display's rate was not reported, `primary display at 144 Hz` or `primary
/// display rate unknown`. `refresh_hz` is what ChooseRefreshHz gave. Pure.
[[nodiscard]] std::string DescribeDisplay(const DisplayInfo& display, u32 refresh_hz);

/// Which pacing set the title gets on this GPU with the display at `refresh_hz` (0 when
/// unknown). Pure. Ocarina of Time on an AMD integrated GPU: the 60 Hz set; on an Intel one the
/// 120 Hz set at 115 to 125 Hz, the 144 Hz set at 140 to 148 Hz, else the 60 Hz set. None for
/// other titles and discrete GPUs. The sets exist on Windows only (PacingSwitches).
[[nodiscard]] PacingSet ChoosePacingSet(u64 title_id, const GpuInfo& gpu, u32 refresh_hz);

/// The display rate and the set chosen because of it, in plain words for the log. `display` is
/// DescribeDisplay's text; empty, it is the primary display at `refresh_hz`.
[[nodiscard]] std::string DescribePacingChoice(const GpuInfo& gpu, u32 refresh_hz, PacingSet set,
                                               std::string_view display = {});

/// The switches of a pacing set on this GPU (the 60 Hz set adds GPL on Intel). Empty for None,
/// and off Windows, where the presenter they pace is not measured.
[[nodiscard]] std::span<const SwitchValue> PacingSwitches(PacingSet set, const GpuInfo& gpu);

/// The switches the title gets by default on this GPU: Ocarina of Time on a Windows integrated
/// GPU gets the pacing measured best (docs/switches.md), by ChoosePacingSet with the refresh rate
/// of the display the window is on (`display`; left out or without a rate, the primary display's).
/// Empty for everything else.
[[nodiscard]] std::span<const SwitchValue> SwitchDefaults(u64 title_id, const GpuInfo& gpu,
                                                          const DisplayInfo& display = {});

/// What a game's switch default does with the switch's value.
enum class SwitchDecision {
    Set,            ///< Nothing set the switch: the default is set.
    ReplaceShipped, ///< Only the shipped nx2_switches.txt set it: the default replaces that value.
    KeepPlayers,    ///< The player set it (the real environment, their own nx2_switches.txt).
};

/// What to do with one switch default, from the switch's value in the environment (`current`,
/// nullopt when unset) and the value the shipped nx2_switches.txt alone gave it (`shipped_only`,
/// Common::SwitchOrigin::ShippedOnlyValue; nullopt when it did not). A value that is no longer
/// the shipped file's is the player's. Pure.
[[nodiscard]] SwitchDecision DecideSwitch(std::optional<std::string_view> current,
                                          std::optional<std::string_view> shipped_only);

/// Sets the title's SwitchDefaults as environment variables of this process, by DecideSwitch:
/// where nothing set the switch, and over a value only the shipped nx2_switches.txt set; a switch
/// the player set to anything (0 included), in the environment or in their own nx2_switches.txt,
/// stays as set. Call it before the device and the renderer are built: they read most switches
/// once. The renderer calls it through VideoCore::ApplyGameSwitchDefaults, which then reads again
/// the switches VideoCore::CreateGPU read earlier (the read-back's EARLY_*). Off with the presets
/// (auto_presets, EDEN_NX2_NO_PRESETS=1). Logs and returns what it did in one line that names the
/// display rate found, the set chosen because of it, the values set, the ones set over the shipped
/// file (with its value) and the ones the player set. A later call in the same process (another
/// game start) first puts back what the earlier one set, and its line says that switches read once
/// keep the first start's values. `display` is the display the window is on: its rate decides the
/// set and the line names it; without a rate, the primary display's is used.
std::string ApplySwitchDefaults(u64 title_id, const GpuInfo& gpu, const DisplayInfo& display = {});

/// Whether the title's first boot on this GPU gets the line ReportFirstBoot logs: Ocarina of
/// Time on an Intel or AMD integrated GPU.
[[nodiscard]] bool ReportsFirstBoot(u64 title_id, const GpuInfo& gpu);

/// On the title's first boot on such a GPU, logs one Info line with the GPU and what the presets
/// did (`summary`, from Apply). The titles reported are listed in the config folder.
void ReportFirstBoot(u64 title_id, const GpuInfo& gpu, std::string_view gpu_name,
                     std::string_view summary);

} // namespace Core::GamePresets
