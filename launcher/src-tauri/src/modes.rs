//! The one switch on Home: Auto, Smooth, Quality, Battery. Each mode is a profile plus a few
//! bounded changes to its toggles; Auto adds what the community data says for this GPU class.
//! `plan` is pure (system info, community data, autopilot overlay in; settings out), `apply_current`
//! writes the result through `profiles::apply`, which only touches Guarana's own files.
//!
//! A manual `apply_profile` (Advanced) switches the mode to `custom`: nothing here or in the
//! autopilot then changes the user's settings until they pick a mode again.

use std::collections::BTreeMap;

use serde::Serialize;
use tauri::{AppHandle, Manager};

use crate::autopilot::{self, AutopilotView};
use crate::community::{self, BestSettings};
use crate::state::{AppState, Persisted};
use crate::types::{SystemInfo, ToggleValue};
use crate::util::{self, CmdResult};
use crate::{profiles};

pub const MODES: [&str; 4] = ["auto", "smooth", "quality", "battery"];
pub const CUSTOM: &str = "custom";

pub fn current(p: &Persisted) -> String {
    match (&p.mode, &p.profile_id) {
        (Some(m), _) => m.clone(),
        // A profile applied by hand before modes existed is the user's choice: left as it is.
        (None, Some(_)) => CUSTOM.to_string(),
        (None, None) => "auto".to_string(),
    }
}

/// Resolution indices (into `profiles::RESOLUTIONS`) a mode may use, autopilot included.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
pub struct Bounds {
    pub res_min: usize,
    pub res_max: usize,
}

fn idx(name: &str) -> usize {
    profiles::resolution_index(name).unwrap_or(2)
}

pub fn bounds(mode: &str, integrated: bool) -> Bounds {
    let (lo, hi) = match (mode, integrated) {
        ("battery", _) => ("0.5x", "0.75x"),
        ("smooth", true) => ("0.5x", "1x"),
        ("smooth", false) => ("0.75x", "1.5x"),
        ("quality", true) => ("1x", "1.25x"),
        ("quality", false) => ("1x", "2x"),
        (_, true) => ("0.5x", "1.25x"),
        (_, false) => ("0.75x", "2x"),
    };
    Bounds { res_min: idx(lo), res_max: idx(hi) }
}

pub fn res_name(i: usize) -> &'static str {
    profiles::RESOLUTIONS[i.min(profiles::RESOLUTIONS.len() - 1)].0
}

#[derive(Debug, Clone)]
pub struct Plan {
    pub mode: String,
    pub profile_id: String,
    /// Every toggle's value, defaults included.
    pub values: BTreeMap<String, ToggleValue>,
    /// Only the values that differ from the profile's own (what `apply_profile` stores).
    pub overrides: BTreeMap<String, ToggleValue>,
    pub reason: String,
    pub bounds: Bounds,
    pub community_class: &'static str,
    pub community_applied: Vec<String>,
}

fn text(v: &BTreeMap<String, ToggleValue>, id: &str) -> String {
    v.get(id).map(|x| x.as_text()).unwrap_or_default()
}

fn plugged_view(sys: &SystemInfo) -> SystemInfo {
    let mut s = sys.clone();
    if s.power.on_ac == Some(false) {
        s.power.on_ac = Some(true);
    }
    s
}

/// The settings for `mode` on this machine. `overlay` is the autopilot's adjustments for this mode.
pub fn plan(mode: &str, sys: &SystemInfo, best: &BestSettings, overlay: &BTreeMap<String, ToggleValue>) -> Plan {
    let gpu = sys.primary();
    let integrated = gpu.map(|g| g.is_integrated).unwrap_or(false);
    let on_battery = sys.power.on_ac == Some(false);
    let class = community::gpu_class(sys);
    let profile_id = match mode {
        "battery" => "low-power-handheld",
        "smooth" | "quality" => profiles::recommend(&plugged_view(sys)),
        _ => profiles::recommend(sys),
    };
    let defaults = profiles::defaults_of(profile_id).unwrap_or_default();
    let mut values = defaults.clone();
    let b = bounds(mode, integrated);
    let mut applied: Vec<String> = Vec::new();
    let mut community_note = String::new();

    match mode {
        "smooth" => {
            if integrated || defaults.get("fluid_presenter") == Some(&ToggleValue::Bool(true)) {
                values.insert("fluid_presenter".into(), true.into());
            }
            values.insert("async_shaders".into(), true.into());
            values.insert("frame_generation".into(), false.into());
            if integrated {
                let r = idx(&text(&defaults, "resolution")).saturating_sub(1);
                values.insert("resolution".into(), res_name(r).into());
            }
        }
        "quality" => {
            let r = idx(&text(&defaults, "resolution")) + 1;
            values.insert("resolution".into(), res_name(r).into());
            if sys.ram_gb >= 15.0 {
                values.insert("vram_mode".into(), "aggressive".into());
            }
            values.insert("hdr_auto".into(), true.into());
        }
        "battery" => {}
        _ => {
            if !on_battery {
                if let Some(c) = community::usable(best, class) {
                    for key in community::APPLIED {
                        if let Some(v) = c.settings.get(key) {
                            if values.get(key) != Some(v) {
                                values.insert(key.to_string(), v.clone());
                                applied.push(key.to_string());
                            }
                        }
                    }
                    community_note = if applied.is_empty() {
                        format!(", which already matches the {} validated reports for {}", c.validated, c.label)
                    } else {
                        format!(", and set {} like the {} validated reports for {}", applied.iter().map(|k| friendly(k)).collect::<Vec<_>>().join(" and "), c.validated, c.label)
                    };
                } else {
                    community_note = match best.classes.get(class) {
                        Some(c) if c.samples > 0 => format!(
                            ", community data for {} is still too thin ({} of {} validated reports needed)",
                            c.label,
                            c.validated,
                            community::MIN_VALIDATED
                        ),
                        _ => ", with no community data for this GPU yet".to_string(),
                    };
                }
            }
        }
    }

    for (k, v) in overlay {
        if profiles::validate_setting(k, v).is_ok() {
            values.insert(k.clone(), v.clone());
        }
    }
    let r = idx(&text(&values, "resolution")).clamp(b.res_min, b.res_max);
    values.insert("resolution".into(), res_name(r).into());

    let overrides: BTreeMap<String, ToggleValue> =
        values.iter().filter(|(k, v)| defaults.get(*k) != Some(*v)).map(|(k, v)| (k.clone(), v.clone())).collect();

    let gpu_name = gpu.map(|g| crate_gpu_name(&g.name)).unwrap_or_else(|| "graphics".into());
    let power = match sys.power.on_ac {
        Some(false) => "on battery",
        Some(true) => "plugged in",
        None => "",
    };
    let name = profiles::profile_name(profile_id);
    let reason = match mode {
        "battery" => format!("Battery mode runs {name}: 0.75x resolution and a 30 fps cap to save power."),
        "smooth" => format!("Smooth mode builds on {name} for your {gpu_name} with even frame pacing on and no frame generation, trading a little resolution for steadier frames."),
        "quality" => format!("Quality mode builds on {name} for your {gpu_name} with one step more resolution{}.", if sys.ram_gb >= 15.0 { " and more video memory" } else { "" }),
        _ => format!(
            "Auto chose {name} for your {gpu_name}{}{community_note}.",
            if power.is_empty() { String::new() } else { format!(" ({power})") }
        ),
    };
    Plan {
        mode: mode.to_string(),
        profile_id: profile_id.to_string(),
        values,
        overrides,
        reason,
        bounds: b,
        community_class: class,
        community_applied: applied,
    }
}

fn friendly(setting: &str) -> &str {
    match setting {
        "fluid_presenter" => "even frame pacing",
        "ocarina_extrapolation" => "Ocarina frame prediction",
        "gm_routing" => "global-memory routing",
        other => other,
    }
}

fn crate_gpu_name(name: &str) -> String {
    crate_trim(name)
}

fn crate_trim(name: &str) -> String {
    name.replace("(R)", "").replace("(TM)", "").split_whitespace().collect::<Vec<_>>().join(" ")
}

/// The plan for the saved mode, from cached community data (no network).
pub fn compute(app: &AppHandle, persisted: &Persisted) -> Plan {
    let sys = app.state::<AppState>().system();
    let best = community::cached_or_builtin();
    compute_with(&sys, &best, persisted)
}

pub fn compute_with(sys: &SystemInfo, best: &BestSettings, persisted: &Persisted) -> Plan {
    let mode = current(persisted);
    if mode == CUSTOM {
        let id = persisted.profile_id.clone().unwrap_or_else(|| profiles::recommend(sys).to_string());
        let defaults = profiles::defaults_of(&id).unwrap_or_default();
        let mut values = defaults.clone();
        for (k, v) in &persisted.overrides {
            values.insert(k.clone(), v.clone());
        }
        let integrated = sys.primary().map(|g| g.is_integrated).unwrap_or(false);
        return Plan {
            mode,
            profile_id: id,
            values,
            overrides: persisted.overrides.clone(),
            reason: "Custom settings from Advanced. Pick a mode to hand control back.".into(),
            bounds: bounds("auto", integrated),
            community_class: community::gpu_class(sys),
            community_applied: Vec::new(),
        };
    }
    let ap = &persisted.autopilot;
    let overlay = if !ap.disabled && ap.overlay_mode == mode { ap.overlay.clone() } else { BTreeMap::new() };
    plan(&mode, sys, best, &overlay)
}

/// Writes the saved mode's settings. Fails (with a sentence) while the emulator or a warm-up runs.
pub fn apply_current(app: &AppHandle) -> CmdResult<Plan> {
    let st = app.state::<AppState>();
    let persisted = st.load();
    let plan = compute(app, &persisted);
    if plan.mode == CUSTOM {
        return Ok(plan);
    }
    profiles::apply(&plan.profile_id, &plan.overrides)?;
    let (id, ov) = (plan.profile_id.clone(), plan.overrides.clone());
    st.update(move |s| {
        s.profile_id = Some(id);
        s.overrides = ov;
    })?;
    Ok(plan)
}

#[derive(Debug, Clone, Serialize)]
pub struct CommunityView {
    pub class: String,
    pub label: String,
    pub samples: u32,
    pub validated: u32,
    pub min_validated: u32,
    pub source: community::Source,
    pub used: bool,
    pub applied: Vec<String>,
}

#[derive(Debug, Clone, Serialize)]
pub struct ModeStatus {
    pub mode: String,
    pub profile_id: String,
    pub profile_name: String,
    /// One sentence: what the mode chose and why.
    pub reason: String,
    pub overrides: BTreeMap<String, ToggleValue>,
    pub community: CommunityView,
    pub autopilot: AutopilotView,
    /// Set when the change is saved but could not be written yet (the game is running).
    pub note: Option<String>,
}

pub fn status_with(sys: &SystemInfo, best: &BestSettings, persisted: &Persisted, note: Option<String>) -> ModeStatus {
    let plan = compute_with(sys, best, persisted);
    let class = plan.community_class;
    let entry = best.classes.get(class);
    ModeStatus {
        mode: plan.mode.clone(),
        profile_name: profiles::profile_name(&plan.profile_id),
        profile_id: plan.profile_id.clone(),
        reason: plan.reason.clone(),
        overrides: plan.overrides.clone(),
        community: CommunityView {
            class: class.to_string(),
            label: entry.map(|c| c.label.clone()).unwrap_or_default(),
            samples: entry.map(|c| c.samples).unwrap_or(0),
            validated: entry.map(|c| c.validated).unwrap_or(0),
            min_validated: community::MIN_VALIDATED,
            source: best.source,
            used: !plan.community_applied.is_empty(),
            applied: plan.community_applied.clone(),
        },
        autopilot: autopilot::view(&persisted.autopilot),
        note,
    }
}

pub fn status(app: &AppHandle, note: Option<String>) -> ModeStatus {
    let st = app.state::<AppState>();
    status_with(&st.system(), &community::cached_or_builtin(), &st.load(), note)
}

pub fn set_mode(app: &AppHandle, mode: &str) -> CmdResult<ModeStatus> {
    let mode = mode.to_ascii_lowercase();
    if !MODES.contains(&mode.as_str()) {
        return Err(format!("Unknown mode {mode}: use auto, smooth, quality or battery"));
    }
    let st = app.state::<AppState>();
    let m = mode.clone();
    st.update(move |s| {
        s.mode = Some(m.clone());
        s.autopilot.reset_for(&m);
    })?;
    let note = match apply_current(app) {
        Ok(_) => None,
        Err(e) => {
            log::info!("Mode {mode} saved, not written yet: {e}");
            Some(format!("Saved. It is written when {} ends.", if util::emulator_running() { "Guarana closes" } else { "the shader preparation" }))
        }
    };
    log::info!("Mode set to {mode}");
    Ok(status(app, note))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::community::{ClassEntry, Source};
    use crate::types::*;

    fn sys(vendor: Vendor, name: &str, integrated: bool, on_ac: Option<bool>) -> SystemInfo {
        SystemInfo {
            os: OsInfo { name: "Windows 11".into(), build: 26100, display_version: "24H2".into() },
            cpu: CpuInfo { name: "cpu".into(), cores: 8, threads: 16, avx2: true, avx512: false, hybrid: false },
            ram_gb: 32.0,
            pagefile_gb: 8.0,
            gpus: vec![Gpu {
                name: name.into(),
                vendor,
                driver_version: "1".into(),
                is_integrated: integrated,
                dedicated_mb: 0,
                shared_mb: 0,
                vulkan: true,
                arch_hint: None,
                vendor_id: 0,
                device_id: 0,
                driver_date: None,
            }],
            primary_gpu: 0,
            display: DisplayInfo { width: 1920, height: 1080, refresh_hz: 60, vrr: None, hdr: None },
            power: PowerInfo { on_ac, battery_percent: None, plan: None },
            hags: None,
            device: DeviceInfo { manufacturer: "x".into(), model: "y".into(), form: Form::Laptop, profile_hint: None },
            vc_redist: true,
            webview2: None,
            free_disk_gb: 100.0,
        }
    }

    fn best(validated: u32, fluid: bool) -> BestSettings {
        let mut b = community::builtin();
        let c = b.classes.get_mut("intel-arc-igpu").unwrap();
        *c = ClassEntry {
            label: "Intel Arc iGPU".into(),
            samples: validated + 2,
            validated,
            settings: BTreeMap::from([("fluid_presenter".to_string(), ToggleValue::Bool(fluid)), ("gm_routing".to_string(), ToggleValue::Bool(true))]),
        };
        b.source = Source::Network;
        b
    }

    #[test]
    fn auto_follows_detection_and_power() {
        let arc = sys(Vendor::Intel, "Intel(R) Arc(TM) 140T GPU", true, Some(true));
        let p = plan("auto", &arc, &community::builtin(), &BTreeMap::new());
        assert_eq!(p.profile_id, "intel-arc-igpu");
        assert!(p.reason.starts_with("Auto chose Intel Arc integrated for your Intel Arc 140T GPU (plugged in)"), "{}", p.reason);
        let batt = sys(Vendor::Intel, "Intel(R) Arc(TM) 140T GPU", true, Some(false));
        assert_eq!(plan("auto", &batt, &community::builtin(), &BTreeMap::new()).profile_id, "low-power-handheld");
        let nv = sys(Vendor::Nvidia, "NVIDIA GeForce RTX 4070", false, Some(true));
        assert_eq!(plan("auto", &nv, &community::builtin(), &BTreeMap::new()).profile_id, "desktop-dgpu");
    }

    #[test]
    fn community_data_applies_only_with_enough_validated_reports() {
        let arc = sys(Vendor::Intel, "Intel Arc 140T", true, Some(true));
        let few = plan("auto", &arc, &best(2, false), &BTreeMap::new());
        assert!(few.community_applied.is_empty());
        assert!(few.reason.contains("too thin"), "{}", few.reason);
        // Arc's profile already has fluid on and gm off: the data turns gm routing on.
        let many = plan("auto", &arc, &best(5, true), &BTreeMap::new());
        assert_eq!(many.community_applied, vec!["gm_routing".to_string()]);
        assert_eq!(many.overrides.get("gm_routing"), Some(&ToggleValue::Bool(true)));
        assert!(many.reason.contains("5 validated reports"), "{}", many.reason);
        // Not on battery.
        let batt = sys(Vendor::Intel, "Intel Arc 140T", true, Some(false));
        assert!(plan("auto", &batt, &best(5, true), &BTreeMap::new()).community_applied.is_empty());
        // Other modes ignore community data.
        assert!(plan("smooth", &arc, &best(5, true), &BTreeMap::new()).community_applied.is_empty());
    }

    #[test]
    fn modes_stay_inside_their_bounds() {
        let arc = sys(Vendor::Intel, "Intel Arc 140T", true, Some(true));
        let b = community::builtin();
        let smooth = plan("smooth", &arc, &b, &BTreeMap::new());
        assert_eq!(smooth.values["resolution"], ToggleValue::Str("0.75x".into()));
        assert_eq!(smooth.values["frame_generation"], ToggleValue::Bool(false));
        let quality = plan("quality", &arc, &b, &BTreeMap::new());
        assert_eq!(quality.values["resolution"], ToggleValue::Str("1.25x".into()));
        let battery = plan("battery", &arc, &b, &BTreeMap::new());
        assert_eq!(battery.profile_id, "low-power-handheld");
        assert_eq!(battery.values["fps_cap"], ToggleValue::Str("30".into()));
        // An overlay beyond the bounds is clamped.
        let over = BTreeMap::from([("resolution".to_string(), ToggleValue::Str("2x".into()))]);
        assert_eq!(plan("quality", &arc, &b, &over).values["resolution"], ToggleValue::Str("1.25x".into()));
        let low = BTreeMap::from([("resolution".to_string(), ToggleValue::Str("0.5x".into()))]);
        assert_eq!(plan("quality", &arc, &b, &low).values["resolution"], ToggleValue::Str("1x".into()));
    }

    #[test]
    fn overrides_hold_only_real_changes() {
        let nv = sys(Vendor::Nvidia, "NVIDIA GeForce RTX 4070", false, Some(true));
        let p = plan("auto", &nv, &community::builtin(), &BTreeMap::new());
        assert!(p.overrides.is_empty(), "{:?}", p.overrides);
    }

    #[test]
    fn an_earlier_hand_made_profile_is_not_taken_over() {
        let mut p = Persisted::default();
        assert_eq!(current(&p), "auto");
        p.profile_id = Some("desktop-dgpu".into());
        assert_eq!(current(&p), CUSTOM);
        p.mode = Some("smooth".into());
        assert_eq!(current(&p), "smooth");
    }

    #[test]
    fn custom_mode_is_left_alone() {
        let arc = sys(Vendor::Intel, "Intel Arc 140T", true, Some(true));
        let mut persisted = Persisted::default();
        persisted.mode = Some(CUSTOM.into());
        persisted.profile_id = Some("safe-defaults".into());
        persisted.overrides.insert("fps_cap".into(), ToggleValue::Str("45".into()));
        let p = compute_with(&arc, &community::builtin(), &persisted);
        assert_eq!(p.profile_id, "safe-defaults");
        assert_eq!(p.values["fps_cap"], ToggleValue::Str("45".into()));
    }
}
