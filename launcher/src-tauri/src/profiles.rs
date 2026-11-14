//! Profiles: sets of toggles, the recommendation from `detect_system`, and `apply_profile`,
//! which writes them to the emulator's own files:
//!
//! - `%APPDATA%\guarana\nx2_switches.txt`: one block between `# >>> Guarana launcher` and
//!   `# <<< Guarana launcher`. Lines outside it are the user's and are never touched. The block
//!   goes at the end of the file, so its values win over earlier lines (the emulator reads the
//!   file top to bottom, later lines overriding earlier ones; `src/lichii/nx2_switches.h`).
//! - `%APPDATA%\guarana\config\qt-config.ini`, `[Renderer]`: only the keys below, each with its
//!   `\default` flag, which the emulator needs to read a value at all
//!   (`Config::ReadSettingGeneric`, `src/frontend_common/config.cpp`).
//!
//! Both files are backed up to `guarana-backups\` beside them before they change.

use std::collections::BTreeMap;
use std::fs;

use crate::ini::IniDoc;
use crate::types::{AppliedProfile, Gpu, Profile, SystemInfo, Toggle, ToggleKind, ToggleValue, Vendor, Form};
use crate::{paths, util};

pub const BLOCK_BEGIN: &str = "# >>> Guarana launcher";
pub const BLOCK_END: &str = "# <<< Guarana launcher";

const EXTRAPOLATE: &str = "EDEN_NX2_EXTRAPOLATE";

/// Whether any GPU in the machine is an Intel one the launcher calls integrated (by PCI id,
/// `system::is_integrated`). On such a GPU the emulator gives Ocarina of Time a locked pacing set
/// when the primary display runs at 115-125 or 140-148 Hz (`ChoosePacingSet`,
/// `src/core/game_presets.cpp`). Any GPU, not only the launcher's primary one: the launcher picks
/// the first discrete adapter as primary, while the emulator may render on the integrated one.
fn has_intel_igpu(gpus: &[Gpu]) -> bool {
    gpus.iter().any(|g| g.vendor == Vendor::Intel && g.is_integrated)
}

/// The `EDEN_NX2_EXTRAPOLATE` value the launcher's block carries; None leaves the line out.
/// `predict` is the `ocarina_extrapolation` toggle; the switch does something only in Ocarina of
/// Time (its motion vectors are the only source the presenter predicts from).
///
/// Off writes 0 (the shipped `nx2_switches.txt` sets 1). On writes 1, except with an Intel
/// integrated GPU in the machine: there the line is left out at every refresh rate, because the
/// player's own file wins over the emulator's defaults, and a 1 there would turn off the
/// interpolation of the emulator's locked 120 and 144 Hz sets for Ocarina of Time (issue #119).
/// Left out, nothing is lost where no locked set applies: the emulator's 60 Hz Intel set names
/// `EDEN_NX2_EXTRAPOLATE=1`, and so does the shipped file. Not reading the refresh rate keeps the
/// block right when the display changes rate after it was written.
/// The frame generation toggle does not enter into it: the emulator's Ocarina of Time preset turns
/// frame generation on whatever the launcher's default says, and chooses the set either way.
fn extrapolate_switch(predict: bool, gpus: &[Gpu]) -> Option<&'static str> {
    if !predict {
        Some("0")
    } else if has_intel_igpu(gpus) {
        None
    } else {
        Some("1")
    }
}

/// Appended to the `ocarina_extrapolation` help on a machine with an Intel integrated GPU.
const INTEL_IGPU_HELP: &str = " With an Intel integrated GPU the launcher leaves EDEN_NX2_EXTRAPOLATE to the emulator when this is on: Ocarina of Time still predicts at most refresh rates, but at 115-125 or 140-148 Hz the emulator's 120 or 144 Hz set shows a frame for every refresh, the in-between ones interpolated, and a 1 in your nx2_switches.txt would turn that off. Older emulator builds predict there too (the shipped nx2_switches.txt sets 1).";

const RENDERER: &str = "Renderer";

/// Value choices of the `resolution` toggle and their `ResolutionSetup` indices
/// (`src/common/settings_enums.h`: Res1_4X=0, Res1_2X=1, Res3_4X=2, Res1X=3, Res5_4X=4,
/// Res3_2X=5, Res2X=6).
pub const RESOLUTIONS: [(&str, u32); 6] = [("0.5x", 1), ("0.75x", 2), ("1x", 3), ("1.25x", 4), ("1.5x", 5), ("2x", 6)];
const FPS_CAPS: [&str; 5] = ["off", "30", "40", "45", "60"];
const VRAM_MODES: [&str; 2] = ["conservative", "aggressive"];

/// Defaults per profile.
struct Defaults {
    frame_generation: bool,
    ocarina_extrapolation: bool,
    gm_routing: bool,
    igpu_power_policy: bool,
    async_shaders: bool,
    vram_mode: &'static str,
    resolution: &'static str,
    fps_cap: &'static str,
    fluid_presenter: bool,
    /// Proven on Intel Arc iGPUs only.
    fluid_experimental: bool,
    hdr_auto: bool,
}

struct ProfileDef {
    id: &'static str,
    name: &'static str,
    description: &'static str,
    defaults: Defaults,
}

const PROFILES: [ProfileDef; 5] = [
    ProfileDef {
        id: "intel-arc-igpu",
        name: "Intel Arc integrated",
        description: "Arc 130T/140T/140V, Meteor Lake and Panther Lake Arc graphics, and Arrow Lake-H Intel Graphics (by PCI id): asynchronous shaders, aggressive VRAM use, frame generation off (Ocarina of Time's own preset handles it).",
        defaults: Defaults {
            frame_generation: false,
            ocarina_extrapolation: true,
            gm_routing: false,
            igpu_power_policy: false,
            async_shaders: true,
            vram_mode: "aggressive",
            resolution: "1x",
            fps_cap: "off",
            fluid_presenter: true,
            fluid_experimental: false,
            hdr_auto: true,
        },
    },
    ProfileDef {
        id: "amd-igpu-handheld",
        name: "AMD Radeon integrated",
        description: "Radeon 780M, 880M and 890M, Ryzen Z1 and Z2 handhelds: asynchronous shaders, conservative VRAM use.",
        defaults: Defaults {
            frame_generation: false,
            ocarina_extrapolation: true,
            gm_routing: false,
            igpu_power_policy: false,
            async_shaders: true,
            vram_mode: "conservative",
            resolution: "1x",
            fps_cap: "off",
            fluid_presenter: true,
            fluid_experimental: true,
            hdr_auto: true,
        },
    },
    ProfileDef {
        id: "low-power-handheld",
        name: "Low power (on battery)",
        description: "Any integrated GPU on battery: a 30 fps cap and 0.75x resolution for steadier frames and longer play.",
        defaults: Defaults {
            frame_generation: false,
            ocarina_extrapolation: true,
            gm_routing: false,
            igpu_power_policy: false,
            async_shaders: true,
            vram_mode: "conservative",
            resolution: "0.75x",
            fps_cap: "30",
            fluid_presenter: false,
            fluid_experimental: true,
            hdr_auto: true,
        },
    },
    ProfileDef {
        id: "desktop-dgpu",
        name: "Dedicated GPU",
        description: "A desktop or laptop graphics card with its own memory: asynchronous shaders, aggressive VRAM use.",
        defaults: Defaults {
            frame_generation: false,
            ocarina_extrapolation: true,
            gm_routing: false,
            igpu_power_policy: false,
            async_shaders: true,
            vram_mode: "aggressive",
            resolution: "1x",
            fps_cap: "off",
            fluid_presenter: false,
            fluid_experimental: true,
            hdr_auto: true,
        },
    },
    ProfileDef {
        id: "safe-defaults",
        name: "Safe defaults",
        description: "The emulator's own defaults and no experiments: the profile to compare against when something looks wrong.",
        defaults: Defaults {
            frame_generation: false,
            ocarina_extrapolation: true,
            gm_routing: false,
            igpu_power_policy: false,
            async_shaders: false,
            vram_mode: "conservative",
            resolution: "1x",
            fps_cap: "off",
            fluid_presenter: false,
            fluid_experimental: true,
            hdr_auto: false,
        },
    },
];

fn choices(list: &[&str]) -> Option<Vec<String>> {
    Some(list.iter().map(|s| s.to_string()).collect())
}

fn toggles(d: &Defaults) -> Vec<Toggle> {
    let b = |id: &str, label: &str, help: &str, value: bool, experimental: bool| Toggle {
        id: id.into(),
        label: label.into(),
        help: help.into(),
        value: ToggleValue::Bool(value),
        default: ToggleValue::Bool(value),
        kind: ToggleKind::Bool,
        choices: None,
        experimental: experimental.then_some(true),
    };
    let c = |id: &str, label: &str, help: &str, value: &str, list: &[&str]| Toggle {
        id: id.into(),
        label: label.into(),
        help: help.into(),
        value: ToggleValue::Str(value.into()),
        default: ToggleValue::Str(value.into()),
        kind: ToggleKind::Choice,
        choices: choices(list),
        experimental: None,
    };
    let resolutions: Vec<&str> = RESOLUTIONS.iter().map(|(n, _)| *n).collect();
    vec![
        b(
            "frame_generation",
            "Frame generation",
            "Shows generated frames between the game's own: 3x up to 60 fps. Costs GPU time, so it suits games that already run near 20 fps or more.",
            d.frame_generation,
            false,
        ),
        b(
            "ocarina_extrapolation",
            "Ocarina of Time: predict frames forward",
            "With frame generation on, Ocarina of Time's generated frames are predicted from the game's motion vectors instead of interpolated, adding no latency (EDEN_NX2_EXTRAPOLATE).",
            d.ocarina_extrapolation,
            false,
        ),
        b(
            "gm_routing",
            "Global-memory routing in every game",
            "Cheaper global memory access in Switch 2 shaders (EDEN_NX2_GM=11). Ocarina of Time uses it already; this turns it on for every game.",
            d.gm_routing,
            false,
        ),
        b(
            "igpu_power_policy",
            "Give the power budget to the GPU",
            "While a game runs, the emulator turns processor boost off in the active power plan so an integrated GPU gets more of the shared power, and restores the plan at exit (EDEN_NX2_POWER_POLICY=igpu).",
            d.igpu_power_policy,
            true,
        ),
        b(
            "fluid_presenter",
            "Even frame pacing",
            "Shows frames on a steady vblank clock (16.7 ms apart at 60 Hz) instead of as soon as each is ready. Measured on an Arc iGPU: the fastest 5% of frame times went from about 11 ms to 16.4 ms, so motion looks even (EDEN_NX2_FLUID=1).",
            d.fluid_presenter,
            d.fluid_experimental,
        ),
        b(
            "hdr_auto",
            "HDR when Windows HDR is on",
            "Uses the game's native HDR when Windows HDR is on (Win+Alt+B). Does nothing otherwise.",
            d.hdr_auto,
            false,
        ),
        b(
            "async_shaders",
            "Asynchronous shaders",
            "Builds shaders in the background instead of pausing the game. Less stutter on first sight of new effects, which can be missing for a moment.",
            d.async_shaders,
            false,
        ),
        c(
            "vram_mode",
            "Video memory use",
            "Aggressive keeps more textures and buffers in GPU memory: faster when there is room, more page-file use when there is not.",
            d.vram_mode,
            &VRAM_MODES,
        ),
        c(
            "resolution",
            "Resolution",
            "Render resolution relative to the game's own. Below 1x saves GPU time on small screens.",
            d.resolution,
            &resolutions,
        ),
        c(
            "fps_cap",
            "Frame rate cap",
            "Caps how often the game presents a frame. A cap the GPU can always hold gives steadier frame times and saves power.",
            d.fps_cap,
            &FPS_CAPS,
        ),
    ]
}

fn find(id: &str) -> Option<&'static ProfileDef> {
    PROFILES.iter().find(|p| p.id == id)
}

/// The profile `detect_system` points at.
pub fn recommend(sys: &SystemInfo) -> &'static str {
    let gpu = sys.primary();
    let integrated = gpu.map(|g| g.is_integrated).unwrap_or(false);
    let vendor = gpu.map(|g| g.vendor).unwrap_or(Vendor::Other);
    let name = gpu.map(|g| g.name.to_ascii_lowercase()).unwrap_or_default();
    // Arc-class iGPUs named only "Intel(R) Graphics" are known by their PCI ids.
    let arc_class = gpu.is_some_and(|g| crate::system::intel_igpu_arc_class(g.vendor_id, g.device_id, &g.name));
    if integrated && sys.power.on_ac == Some(false) {
        return "low-power-handheld";
    }
    if let Some(hint) = sys.device.profile_hint.as_deref() {
        if hint.starts_with("steam-deck") {
            return "low-power-handheld";
        }
    }
    match (vendor, integrated) {
        (Vendor::Intel, true) if name.contains("arc") || arc_class => "intel-arc-igpu",
        (Vendor::Intel, true) => "low-power-handheld",
        (Vendor::Amd, true) => "amd-igpu-handheld",
        (_, false) if gpu.is_some() => "desktop-dgpu",
        _ if sys.device.form == Form::Handheld => "low-power-handheld",
        _ => "safe-defaults",
    }
}

/// Every profile with `recommended` set; the applied profile shows the user's saved overrides.
pub fn list(sys: &SystemInfo, applied: Option<&str>, overrides: &BTreeMap<String, ToggleValue>) -> Vec<Profile> {
    let recommended = recommend(sys);
    let intel_igpu = has_intel_igpu(&sys.gpus);
    PROFILES
        .iter()
        .map(|p| {
            let mut ts = toggles(&p.defaults);
            if intel_igpu {
                if let Some(t) = ts.iter_mut().find(|t| t.id == "ocarina_extrapolation") {
                    t.help.push_str(INTEL_IGPU_HELP);
                }
            }
            if applied == Some(p.id) {
                for t in &mut ts {
                    if let Some(v) = overrides.get(&t.id) {
                        if let Ok(v) = validate(t, v) {
                            t.value = v;
                        }
                    }
                }
            }
            Profile {
                id: p.id.into(),
                name: p.name.into(),
                description: p.description.into(),
                recommended: p.id == recommended,
                toggles: ts,
            }
        })
        .collect()
}

/// Position of a resolution choice in `RESOLUTIONS` (0.5x is 0).
pub fn resolution_index(name: &str) -> Option<usize> {
    RESOLUTIONS.iter().position(|(n, _)| n.eq_ignore_ascii_case(name))
}

/// A profile's own value for every toggle.
pub fn defaults_of(profile_id: &str) -> Option<BTreeMap<String, ToggleValue>> {
    find(profile_id).map(|p| toggles(&p.defaults).into_iter().map(|t| (t.id, t.value)).collect())
}

/// Profile id of the hardware-neutral toggle set, used to check a setting by id.
pub fn validate_setting(id: &str, v: &ToggleValue) -> Result<ToggleValue, String> {
    let ts = toggles(&PROFILES[PROFILES.len() - 1].defaults);
    let t = ts.iter().find(|t| t.id == id).ok_or_else(|| format!("Unknown setting {id}"))?;
    validate(t, v)
}

pub fn profile_name(id: &str) -> String {
    find(id).map(|p| p.name.to_string()).unwrap_or_else(|| id.to_string())
}

/// Checks an override against its toggle and returns it in the toggle's own type.
fn validate(t: &Toggle, v: &ToggleValue) -> Result<ToggleValue, String> {
    match t.kind {
        ToggleKind::Bool => v
            .as_bool()
            .map(ToggleValue::Bool)
            .ok_or_else(|| format!("{} takes true or false", t.id)),
        ToggleKind::Choice => {
            let text = v.as_text();
            let list = t.choices.as_deref().unwrap_or_default();
            list.iter()
                .find(|c| c.eq_ignore_ascii_case(&text))
                .map(|c| ToggleValue::Str(c.clone()))
                .ok_or_else(|| format!("{} takes one of {}", t.id, list.join(", ")))
        }
        ToggleKind::Number => match v {
            ToggleValue::Int(_) | ToggleValue::Float(_) => Ok(v.clone()),
            other => other
                .as_text()
                .parse::<i64>()
                .map(ToggleValue::Int)
                .map_err(|_| format!("{} takes a number", t.id)),
        },
    }
}

/// One `qt-config.ini` key: value and the emulator's default for it (as the emulator writes it).
struct IniWrite {
    key: &'static str,
    value: String,
    default: &'static str,
}

struct Outputs {
    switches: Vec<(String, String)>,
    ini: Vec<IniWrite>,
}

/// `gpus`: every GPU in the machine, for `extrapolate_switch`.
fn outputs(values: &BTreeMap<String, ToggleValue>, gpus: &[Gpu]) -> Outputs {
    let flag = |id: &str| values.get(id).and_then(|v| v.as_bool()).unwrap_or(false);
    let text = |id: &str| values.get(id).map(|v| v.as_text()).unwrap_or_default();
    let mut switches = Vec::new();
    let mut ini = Vec::new();

    if let Some(v) = extrapolate_switch(flag("ocarina_extrapolation"), gpus) {
        switches.push((EXTRAPOLATE.to_string(), v.to_string()));
    }
    // Unset keeps the per-title default (Ocarina of Time on); 11 is every game.
    if flag("gm_routing") {
        switches.push(("EDEN_NX2_GM".to_string(), "11".to_string()));
    }
    if flag("igpu_power_policy") {
        switches.push(("EDEN_NX2_POWER_POLICY".to_string(), "igpu".to_string()));
    }
    // Intel iGPU: Ocarina's measured set is FLUID=2. A 1 in the player's file wins over it
    // and turns that set off, the same way EXTRAPOLATE=1 does (issue #119). Leave the line out.
    if flag("fluid_presenter") && !has_intel_igpu(gpus) {
        switches.push(("EDEN_NX2_FLUID".to_string(), "1".to_string()));
    }

    // HdrMode: Off=0, On=1, Auto=2 (HDR only while Windows runs the display in HDR);
    // HdrSwapchain: Hdr10=0 (the default), Scrgb=1.
    if flag("hdr_auto") {
        ini.push(IniWrite { key: "hdr_mode", value: "2".into(), default: "0" });
        ini.push(IniWrite { key: "hdr_swapchain", value: "0".into(), default: "0" });
    } else {
        ini.push(IniWrite { key: "hdr_mode", value: "0".into(), default: "0" });
    }

    let frame_gen = flag("frame_generation");
    ini.push(IniWrite { key: "frame_gen", value: frame_gen.to_string(), default: "false" });
    if frame_gen {
        ini.push(IniWrite { key: "frame_gen_multiplier", value: "3".into(), default: "2" });
        ini.push(IniWrite { key: "frame_gen_target_rate", value: "60".into(), default: "0" });
    }
    ini.push(IniWrite {
        key: "use_asynchronous_shaders",
        value: flag("async_shaders").to_string(),
        default: "false",
    });
    ini.push(IniWrite {
        key: "vram_usage_mode",
        value: if text("vram_mode") == "aggressive" { "1" } else { "0" }.into(),
        default: "0",
    });
    let res = RESOLUTIONS.iter().find(|(n, _)| *n == text("resolution")).map(|(_, i)| *i).unwrap_or(3);
    ini.push(IniWrite { key: "resolution_setup", value: res.to_string(), default: "3" });
    let fps = text("fps_cap").parse::<u32>().unwrap_or(0);
    ini.push(IniWrite { key: "fps_limit", value: fps.to_string(), default: "0" });

    Outputs { switches, ini }
}

/// What the emulator writes into a new `qt-config.ini` on its first start
/// (`Config::SetUpIni`, `new_global_config`): it skips that once the file exists, so a file the
/// launcher creates first carries the same values. ScalingFilter::Fsr3 = 15,
/// GpuAccuracy::Low = 0, NvdecEmulation::Gpu = 2.
pub(crate) const FIRST_RUN_SEEDS: [(&str, &str); 5] = [
    ("scaling_filter", "15"),
    ("gpu_accuracy", "0"),
    ("nvdec_emulation", "2"),
    ("async_presentation", "true"),
    ("use_asynchronous_shaders", "true"),
];

/// Every `[Renderer]` key `outputs` can write. The doctor's `user_folder` row counts them, like the
/// first-run seeds, as not chosen by the player (`hygiene::chosen_settings`).
pub(crate) const LAUNCHER_INI_KEYS: [&str; 9] = [
    "hdr_mode",
    "hdr_swapchain",
    "frame_gen",
    "frame_gen_multiplier",
    "frame_gen_target_rate",
    "use_asynchronous_shaders",
    "vram_usage_mode",
    "resolution_setup",
    "fps_limit",
];

/// Replaces (or appends) the managed block. Never removes a line outside it.
pub fn replace_block(original: &str, block: &[String]) -> String {
    let crlf = original.contains("\r\n") || !original.contains('\n');
    let mut lines: Vec<String> = original.split('\n').map(|l| l.trim_end_matches('\r').to_string()).collect();
    if lines.last().map(|l| l.is_empty()).unwrap_or(false) {
        lines.pop();
    }
    let begin = lines.iter().position(|l| l.trim_start().starts_with(BLOCK_BEGIN));
    let end = begin.and_then(|b| {
        lines[b + 1..].iter().position(|l| l.trim_start().starts_with(BLOCK_END)).map(|i| b + 1 + i)
    });
    match (begin, end) {
        (Some(b), Some(e)) => {
            lines.splice(b..=e, block.iter().cloned());
        }
        (Some(b), None) => {
            // The end marker was deleted: drop only the begin line and append a whole block.
            lines.remove(b);
            if lines.last().map(|l| !l.trim().is_empty()).unwrap_or(false) {
                lines.push(String::new());
            }
            lines.extend(block.iter().cloned());
        }
        _ => {
            if lines.last().map(|l| !l.trim().is_empty()).unwrap_or(false) {
                lines.push(String::new());
            }
            lines.extend(block.iter().cloned());
        }
    }
    let nl = if crlf { "\r\n" } else { "\n" };
    let mut out = lines.join(nl);
    out.push_str(nl);
    out
}

/// `NAME=VALUE` pairs currently inside the managed block.
fn block_values(text: &str) -> BTreeMap<String, String> {
    let mut inside = false;
    let mut out = BTreeMap::new();
    for line in text.lines() {
        let t = line.trim();
        if t.starts_with(BLOCK_BEGIN) {
            inside = true;
        } else if t.starts_with(BLOCK_END) {
            inside = false;
        } else if inside && !t.starts_with('#') {
            if let Some((k, v)) = t.split_once('=') {
                out.insert(k.trim().to_string(), v.trim().to_string());
            }
        }
    }
    out
}

/// The profile's toggle values with these overrides, each override checked.
fn profile_values(def: &ProfileDef, overrides: &BTreeMap<String, ToggleValue>) -> util::CmdResult<BTreeMap<String, ToggleValue>> {
    let mut values: BTreeMap<String, ToggleValue> = BTreeMap::new();
    let ts = toggles(&def.defaults);
    for t in &ts {
        values.insert(t.id.clone(), t.value.clone());
    }
    for (id, v) in overrides {
        let t = ts.iter().find(|t| &t.id == id).ok_or_else(|| format!("Unknown setting {id}"))?;
        values.insert(id.clone(), validate(t, v)?);
    }
    Ok(values)
}

/// `old_text` (the player's `nx2_switches.txt`, empty when there is none) with the launcher's block
/// replaced by these switches, and what changed in the block. Equal text means nothing to write.
fn rewrite_block(old_text: &str, profile_id: &str, switches: &[(String, String)]) -> (String, Vec<String>) {
    let mut block = vec![format!("{BLOCK_BEGIN} (profile {profile_id}; this block is rewritten by the launcher, edit lines outside it)")];
    block.extend(switches.iter().map(|(k, v)| format!("{k}={v}")));
    block.push(BLOCK_END.to_string());
    let base = if old_text.is_empty() {
        "# Your own switches for Guarana (NAME=VALUE, one per line). Lines outside the launcher's\r\n# block are yours: the launcher never changes them.\r\n"
    } else {
        old_text
    };
    let new_text = replace_block(base, &block);
    let mut changed = Vec::new();
    if new_text != old_text {
        let before = block_values(old_text);
        for (k, v) in switches {
            if before.get(k) != Some(v) {
                changed.push(format!("nx2_switches.txt: {k}={v}"));
            }
        }
        for k in before.keys() {
            if !switches.iter().any(|(name, _)| name == k) {
                changed.push(if k == EXTRAPOLATE {
                    format!("nx2_switches.txt: {k} removed, left to the emulator on an Intel integrated GPU")
                } else {
                    format!("nx2_switches.txt: {k} removed")
                });
            }
        }
    }
    (new_text, changed)
}

/// Writes the launcher's block into the player's `nx2_switches.txt` when it differs (backing the
/// file up first). Returns the file's path and what changed.
fn write_block(profile_id: &str, switches: &[(String, String)]) -> util::CmdResult<(std::path::PathBuf, Vec<String>)> {
    let switches_path = paths::user_switches();
    let old_text = fs::read(&switches_path).map(|b| String::from_utf8_lossy(&b).into_owned()).unwrap_or_default();
    let (new_text, changed) = rewrite_block(&old_text, profile_id, switches);
    if new_text != old_text {
        util::backup_file(&switches_path).map_err(|e| format!("Could not back up nx2_switches.txt: {e}"))?;
        util::write_atomic(&switches_path, new_text.as_bytes())
            .map_err(|e| format!("Could not write nx2_switches.txt: {e}"))?;
    }
    if switches.iter().all(|(k, _)| k != EXTRAPOLATE) {
        log::info!("{EXTRAPOLATE} left to the emulator in nx2_switches.txt (Intel integrated GPU, issue #119)");
    }
    Ok((switches_path, changed))
}

/// Rewrites only the launcher's block in `nx2_switches.txt` from this profile and these overrides,
/// leaving `qt-config.ini` alone. Custom mode calls it at every start (`launch.rs`), as the other
/// modes re-apply theirs, so the block follows this launcher's rules even when it was written by an
/// older one (an `EDEN_NX2_EXTRAPOLATE=1` that would defeat the emulator's locked sets, issue #119)
/// or on another machine; a hand-edited `qt-config.ini` stays as it is. The emulator does not write
/// this file, so unlike `apply` this does not wait for it to close.
pub fn write_switches(profile_id: &str, overrides: &BTreeMap<String, ToggleValue>, sys: &SystemInfo) -> util::CmdResult<Vec<String>> {
    let def = find(profile_id).ok_or_else(|| format!("Unknown profile {profile_id}"))?;
    let out = outputs(&profile_values(def, overrides)?, &sys.gpus);
    write_block(profile_id, &out.switches).map(|(_, changed)| changed)
}

/// `sys` gives the machine's GPUs (`extrapolate_switch`).
pub fn apply(profile_id: &str, overrides: &BTreeMap<String, ToggleValue>, sys: &SystemInfo) -> util::CmdResult<AppliedProfile> {
    let def = find(profile_id).ok_or_else(|| format!("Unknown profile {profile_id}"))?;
    if util::emulator_running() {
        return Err("Close Guarana first: it saves its own settings when it exits, which would undo these.".into());
    }
    if util::warm_up_running() {
        return Err("Shaders are being prepared, and that saves the settings when it ends: apply the profile once it finishes.".into());
    }
    let out = outputs(&profile_values(def, overrides)?, &sys.gpus);
    let (switches_path, mut changed) = write_block(profile_id, &out.switches)?;

    // qt-config.ini
    let config_path = paths::qt_config();
    let existed = config_path.is_file();
    let old_ini = fs::read(&config_path).map(|b| String::from_utf8_lossy(&b).into_owned()).unwrap_or_default();
    let mut doc = IniDoc::parse(&old_ini);
    if !existed {
        for (key, value) in FIRST_RUN_SEEDS {
            doc.set(RENDERER, &format!("{key}\\default"), "false");
            doc.set(RENDERER, key, value);
        }
        changed.push("qt-config.ini: created with the emulator's own first-start values".to_string());
    }
    for w in &out.ini {
        let before = doc.get(RENDERER, w.key);
        let before_default = doc.get(RENDERER, &format!("{}\\default", w.key));
        let is_default = w.value == w.default;
        let a = doc.set(RENDERER, &format!("{}\\default", w.key), if is_default { "true" } else { "false" });
        let b = doc.set(RENDERER, w.key, &w.value);
        if a || b {
            let was = match (before_default.as_deref(), before) {
                (Some("false"), Some(v)) => v,
                (_, _) => format!("{} (default)", w.default),
            };
            changed.push(format!("qt-config.ini [Renderer] {}: {} -> {}", w.key, was, w.value));
        }
    }
    let new_ini = doc.render();
    if new_ini != old_ini {
        util::backup_file(&config_path).map_err(|e| format!("Could not back up qt-config.ini: {e}"))?;
        util::write_atomic(&config_path, new_ini.as_bytes())
            .map_err(|e| format!("Could not write qt-config.ini: {e}"))?;
    }

    log::info!("Applied profile {profile_id}: {} change(s)", changed.len());
    Ok(AppliedProfile {
        profile_id: profile_id.to_string(),
        switches_path: switches_path.to_string_lossy().into_owned(),
        config_path: config_path.to_string_lossy().into_owned(),
        changed,
    })
}

pub fn is_known(id: &str) -> bool {
    find(id).is_some()
}

/// The switches this launcher puts in its block for this profile and these overrides on a machine
/// with these GPUs, without writing anything: the doctor compares them with the file
/// (`hygiene::stale_block`). Fails, as `apply` does, on an unknown profile or override.
pub fn block_switches(profile_id: &str, overrides: &BTreeMap<String, ToggleValue>, gpus: &[Gpu]) -> util::CmdResult<Vec<(String, String)>> {
    let def = find(profile_id).ok_or_else(|| format!("Unknown profile {profile_id}"))?;
    Ok(outputs(&profile_values(def, overrides)?, gpus).switches)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn block_is_appended_then_replaced_in_place() {
        let user = "# mine\r\nEDEN_NX2_GM=0\r\n";
        let block1 = vec![BLOCK_BEGIN.to_string(), "EDEN_NX2_EXTRAPOLATE=1".to_string(), BLOCK_END.to_string()];
        let once = replace_block(user, &block1);
        assert_eq!(once, "# mine\r\nEDEN_NX2_GM=0\r\n\r\n# >>> Guarana launcher\r\nEDEN_NX2_EXTRAPOLATE=1\r\n# <<< Guarana launcher\r\n");
        let with_more = format!("{once}EDEN_NX2_SPLIT=2\r\n");
        let block2 = vec![BLOCK_BEGIN.to_string(), "EDEN_NX2_EXTRAPOLATE=0".to_string(), BLOCK_END.to_string()];
        let twice = replace_block(&with_more, &block2);
        assert!(twice.starts_with("# mine\r\nEDEN_NX2_GM=0\r\n"));
        assert!(twice.contains("EDEN_NX2_EXTRAPOLATE=0\r\n# <<< Guarana launcher\r\nEDEN_NX2_SPLIT=2\r\n"));
        assert!(!twice.contains("EXTRAPOLATE=1"));
    }

    #[test]
    fn missing_end_marker_never_eats_user_lines() {
        let text = "# >>> Guarana launcher\nEDEN_NX2_GM=11\nMY_LINE=1\n";
        let block = vec![BLOCK_BEGIN.to_string(), "EDEN_NX2_EXTRAPOLATE=1".to_string(), BLOCK_END.to_string()];
        let out = replace_block(text, &block);
        assert!(out.contains("MY_LINE=1"));
        assert!(out.contains("EDEN_NX2_GM=11"));
        assert!(out.ends_with("EDEN_NX2_EXTRAPOLATE=1\n# <<< Guarana launcher\n"));
    }

    #[test]
    fn frame_generation_writes_the_contract_keys() {
        let mut values = BTreeMap::new();
        values.insert("frame_generation".to_string(), ToggleValue::Bool(true));
        values.insert("ocarina_extrapolation".to_string(), ToggleValue::Bool(true));
        values.insert("gm_routing".to_string(), ToggleValue::Bool(true));
        values.insert("igpu_power_policy".to_string(), ToggleValue::Bool(true));
        values.insert("fluid_presenter".to_string(), ToggleValue::Bool(true));
        values.insert("hdr_auto".to_string(), ToggleValue::Bool(true));
        let out = outputs(&values, &[]);
        let keys: Vec<(&str, &str)> = out.ini.iter().map(|w| (w.key, w.value.as_str())).collect();
        assert!(keys.contains(&("hdr_mode", "2")));
        assert!(keys.contains(&("hdr_swapchain", "0")));
        assert!(out.switches.contains(&("EDEN_NX2_FLUID".into(), "1".into())));
        assert!(keys.contains(&("frame_gen", "true")));
        assert!(keys.contains(&("frame_gen_multiplier", "3")));
        assert!(keys.contains(&("frame_gen_target_rate", "60")));
        assert!(out.switches.contains(&("EDEN_NX2_EXTRAPOLATE".into(), "1".into())));
        assert!(out.switches.contains(&("EDEN_NX2_GM".into(), "11".into())));
        assert!(out.switches.contains(&("EDEN_NX2_POWER_POLICY".into(), "igpu".into())));
    }

    #[test]
    fn launcher_ini_keys_list_every_key_outputs_writes() {
        for def in &PROFILES {
            let mut values = profile_values(def, &BTreeMap::new()).unwrap();
            values.insert("frame_generation".to_string(), ToggleValue::Bool(true));
            values.insert("hdr_auto".to_string(), ToggleValue::Bool(true));
            for w in outputs(&values, &[]).ini {
                assert!(LAUNCHER_INI_KEYS.contains(&w.key), "{} in {}", w.key, def.id);
            }
        }
    }

    #[test]
    fn block_switches_are_what_the_block_gets() {
        let intel = block_switches("intel-arc-igpu", &BTreeMap::new(), &[ptl()]).unwrap();
        assert!(intel.is_empty());
        let dgpu = block_switches("intel-arc-igpu", &BTreeMap::new(), &[rtx()]).unwrap();
        assert!(dgpu.contains(&(EXTRAPOLATE.to_string(), "1".to_string())));
        assert!(block_switches("no-such-profile", &BTreeMap::new(), &[]).is_err());
        let mut unknown = BTreeMap::new();
        unknown.insert("from_a_newer_launcher".to_string(), ToggleValue::Bool(true));
        assert!(block_switches("intel-arc-igpu", &unknown, &[]).is_err());
    }

    #[test]
    fn overrides_are_type_checked() {
        let ts = toggles(&PROFILES[0].defaults);
        let fg = ts.iter().find(|t| t.id == "frame_generation").unwrap();
        assert_eq!(validate(fg, &ToggleValue::Str("true".into())), Ok(ToggleValue::Bool(true)));
        assert!(validate(fg, &ToggleValue::Str("maybe".into())).is_err());
        let res = ts.iter().find(|t| t.id == "resolution").unwrap();
        assert_eq!(validate(res, &ToggleValue::Str("0.75X".into())), Ok(ToggleValue::Str("0.75x".into())));
        assert!(validate(res, &ToggleValue::Str("3x".into())).is_err());
    }

    fn gpu(vendor: Vendor, integrated: bool, name: &str, vendor_id: u32, device_id: u32) -> Gpu {
        Gpu {
            name: name.into(),
            vendor,
            driver_version: "1".into(),
            is_integrated: integrated,
            dedicated_mb: if integrated { 128 } else { 12000 },
            shared_mb: 16000,
            vulkan: true,
            arch_hint: None,
            family: None,
            vendor_id,
            device_id,
            driver_date: None,
        }
    }

    // Panther Lake's 4-core "Intel(R) Graphics", the laptop of issue #119.
    fn ptl() -> Gpu {
        gpu(Vendor::Intel, true, "Intel(R) Graphics", 0x8086, 0xb0a0)
    }

    fn rtx() -> Gpu {
        gpu(Vendor::Nvidia, false, "NVIDIA GeForce RTX 4070 Laptop GPU", 0x10de, 0x2820)
    }

    #[test]
    fn extrapolation_is_left_to_the_emulator_with_an_intel_igpu() {
        let arc = gpu(Vendor::Intel, true, "Intel(R) Arc(TM) 140T GPU (16GB)", 0x8086, 0x7d51);
        assert_eq!(extrapolate_switch(true, &[ptl()]), None);
        assert_eq!(extrapolate_switch(true, &[arc]), None);
        // A hybrid laptop: the launcher's primary GPU is the discrete one, but the emulator may
        // render on the integrated one and choose a locked set there.
        assert_eq!(extrapolate_switch(true, &[rtx(), ptl()]), None);
        assert_eq!(extrapolate_switch(true, &[ptl(), rtx()]), None);
    }

    #[test]
    fn extrapolation_stays_without_an_intel_igpu() {
        // No locked set anywhere: AMD gets the 60 Hz set, other vendors and discrete Intel none.
        let amd = gpu(Vendor::Amd, true, "AMD Radeon(TM) 780M", 0x1002, 0x15bf);
        let b580 = gpu(Vendor::Intel, false, "Intel(R) Arc(TM) B580 Graphics", 0x8086, 0xe20b);
        assert_eq!(extrapolate_switch(true, &[amd.clone()]), Some("1"));
        assert_eq!(extrapolate_switch(true, &[rtx()]), Some("1"));
        assert_eq!(extrapolate_switch(true, &[b580]), Some("1"));
        assert_eq!(extrapolate_switch(true, &[amd, rtx()]), Some("1"));
        // No GPU found.
        assert_eq!(extrapolate_switch(true, &[]), Some("1"));
    }

    #[test]
    fn prediction_off_still_writes_zero() {
        // The shipped nx2_switches.txt sets 1, so off has to say 0, with an Intel iGPU too (the
        // emulator's locked sets name 0 themselves, so it agrees with them).
        assert_eq!(extrapolate_switch(false, &[ptl()]), Some("0"));
        assert_eq!(extrapolate_switch(false, &[rtx(), ptl()]), Some("0"));
        assert_eq!(extrapolate_switch(false, &[rtx()]), Some("0"));
        assert_eq!(extrapolate_switch(false, &[]), Some("0"));
    }

    #[test]
    fn the_block_leaves_extrapolate_out_with_frame_generation_on_or_off() {
        for frame_gen in [true, false] {
            let mut values = defaults_of("low-power-handheld").unwrap();
            values.insert("ocarina_extrapolation".to_string(), ToggleValue::Bool(true));
            values.insert("frame_generation".to_string(), ToggleValue::Bool(frame_gen));
            let intel = outputs(&values, &[ptl()]);
            assert!(intel.switches.iter().all(|(k, _)| k != EXTRAPOLATE), "{:?}", intel.switches);
            let keys: Vec<(&str, &str)> = intel.ini.iter().map(|w| (w.key, w.value.as_str())).collect();
            assert!(keys.contains(&("frame_gen", if frame_gen { "true" } else { "false" })));
            // The same values without an Intel iGPU write 1, as before.
            let other = outputs(&values, &[rtx()]);
            assert!(other.switches.contains(&(EXTRAPOLATE.to_string(), "1".to_string())));
        }
    }

    #[test]
    fn an_old_block_with_extrapolate_is_rewritten_without_it() {
        // What an older launcher wrote (or this one at 60 Hz before the rule stopped reading the
        // rate): the line that defeats the emulator's 144 Hz set. Custom mode rewrites the block
        // at every start (`write_switches`), so on the 144 Hz Panther Lake laptop it goes.
        let old = "# mine\r\nEDEN_NX2_SPLIT=2\r\n\r\n# >>> Guarana launcher (profile low-power-handheld; this block is rewritten by the launcher, edit lines outside it)\r\nEDEN_NX2_EXTRAPOLATE=1\r\n# <<< Guarana launcher\r\nMY_LINE=1\r\n";
        let mut values = defaults_of("low-power-handheld").unwrap();
        values.insert("ocarina_extrapolation".to_string(), ToggleValue::Bool(true));
        let out = outputs(&values, &[ptl()]);
        let (new_text, changed) = rewrite_block(old, "low-power-handheld", &out.switches);
        assert!(!block_values(&new_text).contains_key(EXTRAPOLATE), "{new_text}");
        assert!(!new_text.contains("EDEN_NX2_EXTRAPOLATE"), "{new_text}");
        // The player's own lines stay where they were.
        assert!(new_text.starts_with("# mine\r\nEDEN_NX2_SPLIT=2\r\n"), "{new_text}");
        assert!(new_text.ends_with("# <<< Guarana launcher\r\nMY_LINE=1\r\n"), "{new_text}");
        assert!(
            changed.contains(&"nx2_switches.txt: EDEN_NX2_EXTRAPOLATE removed, left to the emulator on an Intel integrated GPU".to_string()),
            "{changed:?}"
        );
        // The next start finds nothing to write.
        let (again, changed_again) = rewrite_block(&new_text, "low-power-handheld", &out.switches);
        assert_eq!(again, new_text);
        assert!(changed_again.is_empty(), "{changed_again:?}");
        // Without an Intel iGPU the same block keeps its 1 and nothing changes.
        let other = outputs(&values, &[rtx()]);
        let (kept, kept_changed) = rewrite_block(old, "low-power-handheld", &other.switches);
        assert_eq!(block_values(&kept).get(EXTRAPOLATE).map(String::as_str), Some("1"));
        assert!(kept_changed.iter().all(|c| !c.contains("EXTRAPOLATE")), "{kept_changed:?}");
    }

    #[test]
    fn the_prediction_toggle_says_what_happens_with_an_intel_igpu() {
        use crate::types::{CpuInfo, DeviceInfo, DisplayInfo, OsInfo, PowerInfo};
        let help = |gpus: Vec<Gpu>| {
            let sys = SystemInfo {
                os: OsInfo { name: "Windows 11".into(), build: 26100, display_version: "24H2".into() },
                cpu: CpuInfo { name: "cpu".into(), cores: 8, threads: 16, avx2: true, avx512: false, hybrid: false },
                ram_gb: 32.0,
                pagefile_gb: 8.0,
                gpus,
                primary_gpu: 0,
                display: DisplayInfo { width: 2880, height: 1800, refresh_hz: 144, vrr: None, hdr: None },
                power: PowerInfo { on_ac: Some(true), battery_percent: None, plan: None },
                hags: None,
                device: DeviceInfo { manufacturer: "x".into(), model: "y".into(), form: Form::Laptop, profile_hint: None },
                vc_redist: true,
                webview2: None,
                free_disk_gb: 100.0,
            };
            list(&sys, None, &BTreeMap::new())[0]
                .toggles
                .iter()
                .find(|t| t.id == "ocarina_extrapolation")
                .unwrap()
                .help
                .clone()
        };
        let intel = help(vec![ptl()]);
        assert!(intel.starts_with("With frame generation on,"), "{intel}");
        assert!(intel.ends_with(INTEL_IGPU_HELP), "{intel}");
        assert!(help(vec![rtx(), ptl()]).ends_with(INTEL_IGPU_HELP));
        let amd = gpu(Vendor::Amd, true, "AMD Radeon(TM) 780M", 0x1002, 0x15bf);
        for gpus in [vec![rtx()], vec![amd], vec![]] {
            assert!(!help(gpus).contains("Intel integrated GPU"));
        }
    }
}
