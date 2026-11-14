// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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
    /// The GPU's compute units or SMs as the driver reports them (VK_AMD_shader_core_properties,
    /// VK_NV_shader_sm_builtins); 0 where it does not, as Intel's Windows driver does not. A
    /// resolver input only: nothing is decided by it yet.
    u32 cores{};
    /// What `cores` counts ("compute units", "SMs"); empty when `cores` is 0.
    std::string_view cores_unit{};
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

/// What measured a resolved value: `where` names the docs/switches.md row or section that holds
/// the numbers, or says that nothing was measured and why.
struct Evidence {
    bool measured{};
    std::string_view where;
};

/// A switchable setting by its label, and the value as BasicSetting::ToString gives it. The reason
/// and the evidence are the resolver's (Resolve); left out, the preset's reason and "not measured".
struct SettingValue {
    std::string_view label;
    std::string_view value;
    std::string_view reason{};
    Evidence evidence{};
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
    /// The pixels of its current mode (the output size); 0 when unknown. A resolver input only.
    u32 width_px{};
    u32 height_px{};
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

// The resolver: one pure function from what the machine is, the title and what the user chose, to
// the settings, add-ons and switches the title gets, each with its value, its reason and its
// evidence. Apply and ApplySwitchDefaults take their decisions from it and say what they did; one
// log line and the debug server's `status.resolution` show it. It changes no default: the values it
// resolves are the ones the tables above hold, and the output size and the GPU's core count are
// inputs it records and does not yet decide by.

/// One per-game setting as the settings hold it, as the resolver is told.
struct SettingState {
    bool known{};    ///< False: there is no per-game setting by that label.
    bool per_game{}; ///< The title's own configuration sets it.
    std::string global_value;
    std::string default_value;
    std::string value; ///< What the title runs with now (BasicSetting::ToString).
};

/// What the user chose, asked as the resolver needs it; a function left empty answers "nothing
/// chosen" (an unknown setting, no add-on disabled, no switch set).
struct UserChoices {
    bool presets_enabled{true}; ///< Off with the auto_presets setting or EDEN_NX2_NO_PRESETS=1.
    std::function<SettingState(std::string_view label)> setting;
    std::function<bool(u64 title_id, std::string_view addon)> addon_disabled;
    /// A switch's value in the process environment; nullopt when unset.
    std::function<std::optional<std::string>(std::string_view name)> environment;
    /// The value the shipped nx2_switches.txt alone gave a switch (Common::SwitchOrigin).
    std::function<std::optional<std::string>(std::string_view name)> shipped_only;
    u32 primary_refresh_hz{}; ///< The primary display's rate in Hz; 0 when unknown.
};

struct ResolveInput {
    u64 title_id{};
    GpuInfo gpu;
    DisplayInfo display;
    UserChoices user;
    bool want_settings{true}; ///< Resolve the settings and add-ons.
    bool want_switches{true}; ///< Resolve the switches.
};

struct SettingEntry {
    std::string_view label;
    std::string_view value;
    Decision decision{Decision::Apply};
    bool available{true}; ///< False: no per-game setting by that label (decision is not used).
    std::string current;  ///< The user's value where decision is UserPerGame or UserGlobal.
    std::string_view reason;
    Evidence evidence;
};

struct AddonEntry {
    std::string_view name;
    bool disabled{}; ///< The user disabled it in Add-Ons; that is kept.
    std::string_view reason;
    Evidence evidence;
};

struct SwitchEntry {
    std::string_view name;
    std::string_view value;
    SwitchDecision decision{SwitchDecision::Set};
    std::string player_value;           ///< The value kept where decision is KeepPlayers.
    std::optional<std::string> shipped; ///< The value the shipped nx2_switches.txt alone gave it.
    std::string_view reason;
    Evidence evidence;
};

/// What the resolver was given, as it logs it. The output size against the GPU's size is recorded
/// because on the reference laptop (2880x1800 on 4 Xe3 cores) 72 shown frames measured better than
/// 144 on every metric; nothing decides by it yet.
struct ResolverInputs {
    GameSettings::GPUVendor vendor{GameSettings::GPUVendor::Unknown};
    bool integrated{};
    bool software_tensor_math{};
    Common::DeviceClass device_class{Common::DeviceClass::Unknown};
    Common::GpuFamily gpu_family{Common::GpuFamily::None};
    u32 refresh_hz{};    ///< The whole-Hz rate the pacing set was chosen at (ChooseRefreshHz).
    std::string display; ///< DescribeDisplay's text.
    u32 output_width{};  ///< Pixels of the display the window is on; 0 when unknown.
    u32 output_height{};
    u32 gpu_cores{}; ///< As GpuInfo::cores; 0 when the driver does not report it.
    std::string_view gpu_cores_unit;
    u64 pixels_per_core{}; ///< PixelsPerGpuCore; 0 when either is unknown.
};

struct Resolution {
    u64 title_id{};
    bool presets_enabled{true};
    std::string_view preset_reason; ///< The settings preset found; empty without one.
    std::vector<SettingEntry> settings;
    std::vector<AddonEntry> addons;
    PacingSet pacing{PacingSet::None};
    std::string pacing_why; ///< DescribePacingChoice's text.
    std::vector<SwitchEntry> switches;
    ResolverInputs inputs;
};

/// The names the log line and the debug server use. Pure.
[[nodiscard]] std::string_view VendorName(GameSettings::GPUVendor vendor);
[[nodiscard]] std::string_view DecisionName(Decision decision);
[[nodiscard]] std::string_view SwitchDecisionName(SwitchDecision decision);
[[nodiscard]] std::string_view PacingSetName(PacingSet set);

/// Output pixels per GPU core (or compute unit or SM): width * height / cores; 0 when the size or
/// the core count is unknown. Pure.
[[nodiscard]] u64 PixelsPerGpuCore(u32 width_px, u32 height_px, u32 cores);

/// Resolves the title's settings, add-ons and switches on this machine for what the user chose.
/// Pure, as far as the functions in `input.user` are. The values are the preset's and the pacing
/// set's (Find, ChoosePacingSet, PacingSwitches); `decision` says what to do with each given the
/// user's choices (Decide, DecideSwitch); `reason` and `evidence` say why and what measured it.
[[nodiscard]] Resolution Resolve(const ResolveInput& input);

/// The resolution in one line for the log: the inputs, then the settings, add-ons and switches with
/// what was decided for each and how many are measured. Pure.
[[nodiscard]] std::string DescribeResolution(const Resolution& resolution);

/// The resolution the process made at boot: the switches and the inputs from ApplySwitchDefaults,
/// the settings and add-ons from Apply; nullopt before either ran. For the debug server.
[[nodiscard]] std::optional<Resolution> LastResolution();

/// Whether the title's first boot on this GPU gets the line ReportFirstBoot logs: Ocarina of
/// Time on an Intel or AMD integrated GPU.
[[nodiscard]] bool ReportsFirstBoot(u64 title_id, const GpuInfo& gpu);

/// On the title's first boot on such a GPU, logs one Info line with the GPU and what the presets
/// did (`summary`, from Apply). The titles reported are listed in the config folder.
void ReportFirstBoot(u64 title_id, const GpuInfo& gpu, std::string_view gpu_name,
                     std::string_view summary);

} // namespace Core::GamePresets
