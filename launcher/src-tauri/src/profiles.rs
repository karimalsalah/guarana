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
use crate::types::{AppliedProfile, Profile, SystemInfo, Toggle, ToggleKind, ToggleValue, Vendor, Form};
use crate::{paths, util};

pub const BLOCK_BEGIN: &str = "# >>> Guarana launcher";
pub const BLOCK_END: &str = "# <<< Guarana launcher";

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
        description: "Arc 130T/140T/140V and Meteor Lake or Panther Lake Arc graphics: asynchronous shaders, aggressive VRAM use, frame generation off (Ocarina of Time's own preset handles it).",
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
    if integrated && sys.power.on_ac == Some(false) {
        return "low-power-handheld";
    }
    if let Some(hint) = sys.device.profile_hint.as_deref() {
        if hint.starts_with("steam-deck") {
            return "low-power-handheld";
        }
    }
    match (vendor, integrated) {
        (Vendor::Intel, true) if name.contains("arc") => "intel-arc-igpu",
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
    PROFILES
        .iter()
        .map(|p| {
            let mut ts = toggles(&p.defaults);
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

fn outputs(values: &BTreeMap<String, ToggleValue>) -> Outputs {
    let flag = |id: &str| values.get(id).and_then(|v| v.as_bool()).unwrap_or(false);
    let text = |id: &str| values.get(id).map(|v| v.as_text()).unwrap_or_default();
    let mut switches = Vec::new();
    let mut ini = Vec::new();

    // Shipped nx2_switches.txt sets EXTRAPOLATE=1, so "off" has to say 0.
    switches.push((
        "EDEN_NX2_EXTRAPOLATE".to_string(),
        if flag("ocarina_extrapolation") { "1" } else { "0" }.to_string(),
    ));
    // Unset keeps the per-title default (Ocarina of Time on); 11 is every game.
    if flag("gm_routing") {
        switches.push(("EDEN_NX2_GM".to_string(), "11".to_string()));
    }
    if flag("igpu_power_policy") {
        switches.push(("EDEN_NX2_POWER_POLICY".to_string(), "igpu".to_string()));
    }
    if flag("fluid_presenter") {
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
const FIRST_RUN_SEEDS: [(&str, &str); 5] = [
    ("scaling_filter", "15"),
    ("gpu_accuracy", "0"),
    ("nvdec_emulation", "2"),
    ("async_presentation", "true"),
    ("use_asynchronous_shaders", "true"),
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

pub fn apply(profile_id: &str, overrides: &BTreeMap<String, ToggleValue>) -> util::CmdResult<AppliedProfile> {
    let def = find(profile_id).ok_or_else(|| format!("Unknown profile {profile_id}"))?;
    if util::emulator_running() {
        return Err("Close Guarana first: it saves its own settings when it exits, which would undo these.".into());
    }
    if util::warm_up_running() {
        return Err("Shaders are being prepared, and that saves the settings when it ends: apply the profile once it finishes.".into());
    }
    let mut values: BTreeMap<String, ToggleValue> = BTreeMap::new();
    let ts = toggles(&def.defaults);
    for t in &ts {
        values.insert(t.id.clone(), t.value.clone());
    }
    for (id, v) in overrides {
        let t = ts.iter().find(|t| &t.id == id).ok_or_else(|| format!("Unknown setting {id}"))?;
        values.insert(id.clone(), validate(t, v)?);
    }
    let out = outputs(&values);
    let mut changed = Vec::new();

    // nx2_switches.txt
    let switches_path = paths::user_switches();
    let old_text = fs::read(&switches_path).map(|b| String::from_utf8_lossy(&b).into_owned()).unwrap_or_default();
    let mut block = vec![format!("{BLOCK_BEGIN} (profile {profile_id}; this block is rewritten by the launcher, edit lines outside it)")];
    block.extend(out.switches.iter().map(|(k, v)| format!("{k}={v}")));
    block.push(BLOCK_END.to_string());
    let base = if old_text.is_empty() {
        "# Your own switches for Guarana (NAME=VALUE, one per line). Lines outside the launcher's\r\n# block are yours: the launcher never changes them.\r\n".to_string()
    } else {
        old_text.clone()
    };
    let new_text = replace_block(&base, &block);
    if new_text != old_text {
        let before = block_values(&old_text);
        for (k, v) in &out.switches {
            if before.get(k) != Some(v) {
                changed.push(format!("nx2_switches.txt: {k}={v}"));
            }
        }
        for k in before.keys() {
            if !out.switches.iter().any(|(name, _)| name == k) {
                changed.push(format!("nx2_switches.txt: {k} removed"));
            }
        }
        util::backup_file(&switches_path).map_err(|e| format!("Could not back up nx2_switches.txt: {e}"))?;
        util::write_atomic(&switches_path, new_text.as_bytes())
            .map_err(|e| format!("Could not write nx2_switches.txt: {e}"))?;
    }

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
        let out = outputs(&values);
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
    fn overrides_are_type_checked() {
        let ts = toggles(&PROFILES[0].defaults);
        let fg = ts.iter().find(|t| t.id == "frame_generation").unwrap();
        assert_eq!(validate(fg, &ToggleValue::Str("true".into())), Ok(ToggleValue::Bool(true)));
        assert!(validate(fg, &ToggleValue::Str("maybe".into())).is_err());
        let res = ts.iter().find(|t| t.id == "resolution").unwrap();
        assert_eq!(validate(res, &ToggleValue::Str("0.75X".into())), Ok(ToggleValue::Str("0.75x".into())));
        assert!(validate(res, &ToggleValue::Str("3x".into())).is_err());
    }
}
