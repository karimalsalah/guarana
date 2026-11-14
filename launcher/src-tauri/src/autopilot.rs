//! Autopilot: after each game session it reads the emulator's log and makes at most one small,
//! reversible change for the next session, within the current mode's bounds.
//!
//! What it may change: the resolution (one step at a time, never outside the mode's bounds), the
//! even-frame-pacing switch (a trial that is kept only when the log's `Shown frames` lines are
//! better with it, else undone), and it may start a shader warm-up for a title that keeps
//! stuttering on shader builds. Every change is kept in a history with the reason, shows on Home
//! and can be undone; an undone setting is left alone until the mode changes. It never writes
//! anything except Guarana's own settings (through `modes::apply_current`) and `launcher-state.json`.
//!
//! Log lines it reads (the log is the emulator's `eden_log.txt`, the session is everything from the
//! last `Booting game:` line):
//! - `Shown frames: F per second, interval p5 .. p50 X p95 Y ... N% within 2 ms of the median`
//!   (needs `EDEN_NX2_SHOWN_STATS=1`, which the launcher sets for the games it starts)
//! - `Fluid presenter ...` (active or not, matched loosely: the wording is the emulator's)
//! - `Nx2Stutter frame=.. cause=..`, `Nx2StutterSummary window_s=.. frames=..`, `Nx2Boot event=.. mono_s=..`
//!   (only with `EDEN_NX2_STUTTER_TRACE=1`), `Graphics pipeline built on the GPU thread in X ms`,
//!   `Shader precache: finished, N built, .. S s`, `Frame generation on:`.

use std::collections::BTreeMap;
use std::fs;
use std::io::{Read, Seek, SeekFrom};
use std::path::Path;
use std::time::{SystemTime, UNIX_EPOCH};

use serde::{Deserialize, Serialize};
use tauri::{AppHandle, Emitter, Manager};

use crate::modes;
use crate::state::AppState;
use crate::types::ToggleValue;
use crate::{paths, profiles, util, warm};

pub const EVENT: &str = "autopilot://changed";
pub const MIN_SESSION_SECS: f64 = 120.0;
const HISTORY_MAX: usize = 50;
const SAMPLES_MAX: usize = 24;
const LOG_TAIL: u64 = 8 << 20;
const DAY: i64 = 86_400;

// ---- persisted state ----------------------------------------------------------------------------

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Change {
    pub id: u64,
    pub at: String,
    pub mode: String,
    /// `change` (undoable), `action` (a shader warm-up) or `note` (something kept).
    pub kind: String,
    pub setting: String,
    pub from: ToggleValue,
    pub to: ToggleValue,
    pub because: String,
    /// "Autopilot changed X because Y."
    pub text: String,
    #[serde(default)]
    pub title_id: Option<String>,
    #[serde(default)]
    pub undone: bool,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Sample {
    pub at: i64,
    pub mode: String,
    pub fluid: bool,
    pub within2: f64,
    pub p50: f64,
    pub p95: f64,
    pub fps: Option<f64>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct FluidTrial {
    pub base_within2: f64,
    pub base_spread: f64,
    pub base_fps: Option<f64>,
    pub waited: u32,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct AutopilotState {
    #[serde(default)]
    pub disabled: bool,
    #[serde(default)]
    pub overlay_mode: String,
    #[serde(default)]
    pub overlay: BTreeMap<String, ToggleValue>,
    #[serde(default)]
    pub history: Vec<Change>,
    #[serde(default)]
    pub samples: Vec<Sample>,
    #[serde(default)]
    pub frozen: Vec<String>,
    #[serde(default)]
    pub strikes: BTreeMap<String, u32>,
    #[serde(default)]
    pub last_warm: BTreeMap<String, i64>,
    #[serde(default)]
    pub fluid_trial: Option<FluidTrial>,
    #[serde(default)]
    pub fluid_rejected_at: Option<i64>,
    #[serde(default)]
    pub headroom_streak: u32,
    /// `mtime:len` of the last log session read, so one session is never counted twice.
    #[serde(default)]
    pub last_session_key: Option<String>,
}

impl AutopilotState {
    /// A new mode starts from its own defaults. The history stays.
    pub fn reset_for(&mut self, mode: &str) {
        self.overlay_mode = mode.to_string();
        self.overlay.clear();
        self.frozen.clear();
        self.fluid_trial = None;
        self.headroom_streak = 0;
        self.strikes.clear();
    }

    fn next_id(&self) -> u64 {
        self.history.iter().map(|c| c.id).max().unwrap_or(0) + 1
    }
}

#[derive(Debug, Clone, Serialize)]
pub struct AutopilotView {
    pub enabled: bool,
    pub history: Vec<Change>,
    pub can_undo: bool,
    /// The newest entry that has not been undone, for the line on Home.
    pub latest: Option<Change>,
}

pub fn view(s: &AutopilotState) -> AutopilotView {
    let history: Vec<Change> = s.history.iter().rev().take(10).cloned().collect();
    AutopilotView {
        enabled: !s.disabled,
        can_undo: s.history.iter().any(|c| c.kind == "change" && !c.undone),
        latest: s.history.iter().rev().find(|c| !c.undone).cloned(),
        history,
    }
}

fn now_secs() -> i64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs() as i64).unwrap_or(0)
}

// ---- reading the log ------------------------------------------------------------------------------

#[derive(Debug, Clone, Default, PartialEq)]
pub struct Shown {
    pub fps: f64,
    pub p50: f64,
    pub p95: f64,
    pub p99: f64,
    pub within2: f64,
    pub windows: u32,
}

#[derive(Debug, Clone, Default, PartialEq)]
pub struct SessionMetrics {
    pub title_id: Option<String>,
    pub shown: Option<Shown>,
    pub fluid_active: Option<bool>,
    pub game_fps: Option<f64>,
    /// Frames over 33 ms caused by shader work, plus pipeline builds over 33 ms on the GPU thread.
    pub hitches: u32,
    pub stutter_events: u32,
    pub boot_seconds: Option<f64>,
    pub precache_built: Option<u64>,
    pub precache_seconds: Option<f64>,
    pub frame_gen: bool,
    /// Seconds of gameplay the numbers cover: the fluid presenter's windows with real frames,
    /// else the stutter summaries' windows, else 10 s per `Shown frames:` window used.
    pub gameplay_secs: f64,
}

impl SessionMetrics {
    pub fn slow_boot(&self) -> bool {
        self.boot_seconds.map(|s| s >= 120.0).unwrap_or(false)
            || (self.precache_built.unwrap_or(0) >= 100 && self.precache_seconds.unwrap_or(0.0) >= 20.0)
    }
}

fn num_after(line: &str, key: &str) -> Option<f64> {
    let i = line.find(key)? + key.len();
    let rest = &line[i..];
    let end = rest.find(|c: char| !(c.is_ascii_digit() || c == '.' || c == '-')).unwrap_or(rest.len());
    rest[..end].parse().ok()
}

fn num_before(line: &str, marker: &str) -> Option<f64> {
    let head = &line[..line.find(marker)?];
    let start = head.rfind(|c: char| !(c.is_ascii_digit() || c == '.')).map(|i| i + 1).unwrap_or(0);
    head[start..].parse().ok()
}

const SHADER_CAUSES: [&str; 4] = ["gfx_miss", "compute_miss", "pipeline_wait", "shader_capture"];

/// Metrics of the last session in a log: everything from the last `Booting game:` line.
pub fn parse_log(text: &str) -> SessionMetrics {
    let start = text.rfind("Booting game:").map(|i| text[..i].rfind('\n').map(|n| n + 1).unwrap_or(0)).unwrap_or(0);
    let mut m = SessionMetrics::default();
    let mut shown: Vec<(f64, f64, f64, f64, f64)> = Vec::new();
    let (mut sum_frames, mut sum_window) = (0.0, 0.0);
    let (mut real_frames, mut real_secs) = (0.0, 0.0);
    let (mut boot_start, mut first_frame) = (None, None);
    for line in text[start..].lines() {
        if let Some(i) = line.find("Booting game:") {
            let id: String = line[i + 13..].trim_start().trim_start_matches("0x").chars().take_while(|c| c.is_ascii_hexdigit()).collect();
            if id.len() == 16 {
                m.title_id = Some(id.to_ascii_uppercase());
            }
        } else if line.contains("Shown frames:") {
            if let (Some(fps), Some(p50), Some(p95), Some(w)) =
                (num_after(line, "Shown frames: "), num_after(line, " p50 "), num_after(line, " p95 "), num_before(line, "% within"))
            {
                if fps > 0.0 {
                    shown.push((fps, p50, p95, w, num_after(line, " p99 ").unwrap_or(p95)));
                }
            }
        } else if line.contains("Nx2StutterSummary ") {
            if let (Some(w), Some(f)) = (num_after(line, "window_s="), num_after(line, "frames=")) {
                sum_window += w;
                sum_frames += f;
            }
        } else if line.contains("Nx2Stutter frame=") {
            m.stutter_events += 1;
            if let Some(i) = line.find("cause=") {
                let cause: String = line[i + 6..].chars().take_while(|c| !c.is_whitespace()).collect();
                if SHADER_CAUSES.contains(&cause.as_str()) {
                    m.hitches += 1;
                }
            }
        } else if line.contains("Nx2Boot event=") {
            let mono = num_after(line, "mono_s=");
            if line.contains("event=boot_start") {
                boot_start = boot_start.or(mono);
            } else if line.contains("event=first_frame") {
                first_frame = first_frame.or(mono);
            }
        } else if line.contains("pipeline built on the GPU thread in ") {
            if num_after(line, "thread in ").map(|ms| ms >= 33.0).unwrap_or(false) {
                m.hitches += 1;
            }
        } else if let Some(i) = line.find("Shader precache:") {
            let rest = &line[i..];
            if let Some(b) = rest.find(" built") {
                m.precache_built = num_before(&rest[..b + 6], " built").map(|n| n as u64);
            }
            m.precache_seconds = rest.trim_end().strip_suffix(" s").and_then(|r| {
                let start = r.rfind(|c: char| !(c.is_ascii_digit() || c == '.')).map(|i| i + 1).unwrap_or(0);
                r[start..].parse().ok()
            });
        } else if line.to_ascii_lowercase().contains("fluid presenter") {
            let l = line.to_ascii_lowercase();
            // The emulator's own stats lines (FluidLogStats, every 10 s) mean it is running:
            //   "Fluid presenter (10 s, DXGI WaitForVBlank at 120.00 Hz, ...): 601 slots, 331 shown on their slot, ..."
            //   "Fluid presenter (10 s): 91% of shown frames held one slot, ...; ... (1405 real frames); ..."
            let stats = l.contains(" slots,") || l.contains("of shown frames held") || l.contains("real frames)");
            if stats {
                m.fluid_active = Some(true);
                // A window with no real frames (boot, menus, loading) says nothing about the game.
                if let Some(n) = num_before(line, " real frames)") {
                    if n > 0.0 {
                        real_frames += n;
                        real_secs += num_after(line, "Fluid presenter (").filter(|w| *w > 0.0).unwrap_or(10.0);
                    }
                }
            } else {
                let off = ["disabled", " off", "unsupported", "not available", "fell back", "fallback", "inactive", "failed"];
                m.fluid_active = Some(!off.iter().any(|w| l.contains(w)));
            }
        } else if line.contains("Frame generation on:") {
            m.frame_gen = true;
        }
    }
    if let (Some(a), Some(b)) = (boot_start, first_frame) {
        if b >= a {
            m.boot_seconds = Some(b - a);
        }
    }
    if !shown.is_empty() {
        // The first window still holds the boot; leave it out once there are enough.
        let used = if shown.len() >= 3 { &shown[1..] } else { &shown[..] };
        let n = used.len() as f64;
        let avg = |f: fn(&(f64, f64, f64, f64, f64)) -> f64| used.iter().map(f).sum::<f64>() / n;
        m.shown = Some(Shown { fps: avg(|s| s.0), p50: avg(|s| s.1), p95: avg(|s| s.2), p99: avg(|s| s.4), within2: avg(|s| s.3), windows: used.len() as u32 });
    }
    // With even frame pacing on, its "(N real frames)" per window is the game's own frame count and
    // needs no stutter trace; else the stutter summary; else the shown rate when nothing is generated.
    m.gameplay_secs = if real_secs > 0.0 {
        real_secs
    } else if sum_window > 0.0 {
        sum_window
    } else {
        m.shown.as_ref().map(|s| s.windows as f64 * 10.0).unwrap_or(0.0)
    };
    m.game_fps = if real_secs > 0.0 {
        Some(real_frames / real_secs)
    } else if sum_window > 0.0 {
        Some(sum_frames / sum_window)
    } else if !m.frame_gen {
        m.shown.as_ref().map(|s| s.fps)
    } else {
        None
    };
    m
}

fn read_tail(path: &Path) -> Option<String> {
    let mut f = fs::File::open(path).ok()?;
    let len = f.metadata().ok()?.len();
    if len > LOG_TAIL {
        f.seek(SeekFrom::Start(len - LOG_TAIL)).ok()?;
    }
    let mut bytes = Vec::new();
    f.read_to_end(&mut bytes).ok()?;
    Some(String::from_utf8_lossy(&bytes).into_owned())
}

/// The newest log that holds a game session and was written within `window_secs`:
/// (text, session key).
pub(crate) fn last_session_log(window_secs: u64) -> Option<(String, String)> {
    let dir = paths::emulator_log_dir();
    let mut candidates = vec![
        dir.join("eden_log.txt"),
        dir.join("eden_log.txt.old.txt"),
        dir.join("eden_log.old.txt"),
        dir.join("eden_log.txt.old"),
    ];
    candidates.push(paths::kept_game_log());
    for path in candidates {
        let Ok(meta) = fs::metadata(&path) else { continue };
        let Ok(modified) = meta.modified() else { continue };
        let age = SystemTime::now().duration_since(modified).map(|d| d.as_secs()).unwrap_or(0);
        if age > window_secs {
            continue;
        }
        let Some(text) = read_tail(&path) else { continue };
        if text.contains("Booting game:") {
            let key = format!("{}:{}", modified.duration_since(UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0), meta.len());
            return Some((text, key));
        }
    }
    None
}

// ---- deciding -------------------------------------------------------------------------------------

pub struct Ctx {
    pub mode: String,
    pub res_min: usize,
    pub res_max: usize,
    /// The values the session ran with.
    pub values: BTreeMap<String, ToggleValue>,
    pub on_battery: bool,
    pub exit_code: i32,
    pub seconds: f64,
    pub now: i64,
}

#[derive(Debug, Clone, PartialEq)]
pub enum Outcome {
    Set { setting: String, to: ToggleValue, because: String },
    Note { setting: String, because: String },
    Warm { title_id: String, because: String },
}

fn label(setting: &str) -> &'static str {
    match setting {
        "resolution" => "the resolution",
        "fluid_presenter" => "even frame pacing",
        "warm_shaders" => "shader preparation",
        _ => "a setting",
    }
}

fn shown_text(v: &ToggleValue) -> String {
    match v {
        ToggleValue::Bool(true) => "on".into(),
        ToggleValue::Bool(false) => "off".into(),
        other => other.as_text(),
    }
}

fn res_idx(values: &BTreeMap<String, ToggleValue>) -> usize {
    values.get("resolution").and_then(|v| profiles::resolution_index(&v.as_text())).unwrap_or(2)
}

fn spread(p50: f64, p95: f64) -> f64 {
    if p50 > 0.0 {
        p95 / p50
    } else {
        1.0
    }
}

/// Looks at one finished session and returns at most one outcome. Updates the counters and the
/// samples in `st`; the caller records the outcome.
pub fn decide(st: &mut AutopilotState, m: &SessionMetrics, c: &Ctx) -> Option<Outcome> {
    if c.seconds < MIN_SESSION_SECS {
        return None;
    }
    // Exit code 1 with no frame shown: a launcher session whose game did not start (a load error
    // left open for a while). Nothing ran, so nothing is judged, the trial included.
    if c.exit_code == 1 && m.shown.is_none() && m.game_fps.is_none() {
        return None;
    }
    let crashed = c.exit_code != 0;
    let fluid_setting = c.values.get("fluid_presenter").and_then(|v| v.as_bool()).unwrap_or(false);
    let fluid_session = m.fluid_active.unwrap_or(fluid_setting);
    let fps_limit = if c.on_battery { 20.0 } else { 15.0 };

    // 1. A trial that ended badly goes back at once.
    if crashed && st.fluid_trial.is_some() && fluid_setting && !st.frozen.iter().any(|f| f == "fluid_presenter") {
        st.fluid_trial = None;
        st.fluid_rejected_at = Some(c.now);
        st.frozen.push("fluid_presenter".into());
        return Some(Outcome::Set {
            setting: "fluid_presenter".into(),
            to: false.into(),
            because: "the session with it on ended with an error".into(),
        });
    }
    if crashed {
        return None;
    }

    // Record what this session showed.
    if let Some(sh) = &m.shown {
        st.samples.push(Sample { at: c.now, mode: c.mode.clone(), fluid: fluid_session, within2: sh.within2, p50: sh.p50, p95: sh.p95, fps: m.game_fps });
        if st.samples.len() > SAMPLES_MAX {
            st.samples.remove(0);
        }
    }
    // Shader-stutter strikes per title.
    if let Some(t) = &m.title_id {
        if m.hitches >= 3 || m.slow_boot() {
            *st.strikes.entry(t.clone()).or_insert(0) += 1;
        } else {
            st.strikes.remove(t);
        }
    }

    // 2. Judge a running even-frame-pacing trial.
    if let (Some(trial), Some(sh)) = (st.fluid_trial.clone(), m.shown.as_ref()) {
        if fluid_session {
            let now_spread = spread(sh.p50, sh.p95);
            let better = sh.within2 >= trial.base_within2 + 5.0 || (now_spread <= trial.base_spread * 0.9 && sh.within2 >= trial.base_within2);
            let fps_ok = match (m.game_fps, trial.base_fps) {
                (Some(now), Some(base)) => now >= base * 0.95,
                _ => true,
            };
            st.fluid_trial = None;
            let facts = format!(
                "{:.0}% of shown frames landed within 2 ms of the median with it, against {:.0}% without",
                sh.within2, trial.base_within2
            );
            return Some(if better && fps_ok {
                Outcome::Note { setting: "fluid_presenter".into(), because: facts }
            } else {
                st.fluid_rejected_at = Some(c.now);
                st.frozen.push("fluid_presenter".into());
                Outcome::Set {
                    setting: "fluid_presenter".into(),
                    to: false.into(),
                    because: format!("it did not make the shown frames better ({facts})"),
                }
            });
        } else if let Some(t) = st.fluid_trial.as_mut() {
            t.waited += 1;
            if t.waited > 3 {
                st.fluid_trial = None;
            }
        }
    }

    // 3. Too slow: one resolution step down, inside the mode's bounds.
    if let Some(fps) = m.game_fps {
        let r = res_idx(&c.values);
        if fps < fps_limit && r > c.res_min && !st.frozen.iter().any(|f| f == "resolution") {
            st.headroom_streak = 0;
            return Some(Outcome::Set {
                setting: "resolution".into(),
                to: modes::res_name(r - 1).into(),
                because: format!("the game ran at {fps:.0} fps{}, under {fps_limit:.0}", if c.on_battery { " on battery" } else { "" }),
            });
        }
    }

    // 4. Repeated shader-build stutter: prepare the title's shaders ahead.
    if let Some(t) = &m.title_id {
        let strikes = st.strikes.get(t).copied().unwrap_or(0);
        let last = st.last_warm.get(t).copied().unwrap_or(0);
        if strikes >= 2 && c.now - last >= DAY {
            st.last_warm.insert(t.clone(), c.now);
            st.strikes.remove(t);
            return Some(Outcome::Warm {
                title_id: t.clone(),
                because: format!("the last {strikes} sessions stuttered while shaders were built{}", if m.slow_boot() { " and the start was slow" } else { "" }),
            });
        }
    }

    // 5. Try even frame pacing where it is off and the frames were uneven.
    let rejected_recently = st.fluid_rejected_at.map(|t| c.now - t < 30 * DAY).unwrap_or(false);
    if !fluid_setting && !c.on_battery && c.mode != "battery" && st.fluid_trial.is_none() && !rejected_recently && !st.frozen.iter().any(|f| f == "fluid_presenter") {
        let base: Vec<&Sample> = st.samples.iter().rev().filter(|s| !s.fluid && s.mode == c.mode).take(4).collect();
        if base.len() >= 2 {
            let n = base.len() as f64;
            let within = base.iter().map(|s| s.within2).sum::<f64>() / n;
            if within < 92.0 {
                let fps_vals: Vec<f64> = base.iter().filter_map(|s| s.fps).collect();
                st.fluid_trial = Some(FluidTrial {
                    base_within2: within,
                    base_spread: base.iter().map(|s| spread(s.p50, s.p95)).sum::<f64>() / n,
                    base_fps: (!fps_vals.is_empty()).then(|| fps_vals.iter().sum::<f64>() / fps_vals.len() as f64),
                    waited: 0,
                });
                return Some(Outcome::Set {
                    setting: "fluid_presenter".into(),
                    to: true.into(),
                    because: format!("only {within:.0}% of shown frames landed within 2 ms of the median; it is kept only if the next session is better"),
                });
            }
        }
    }

    // 6. Plenty of headroom twice in a row: one step up, inside the bounds.
    if let (Some(fps), Some(sh)) = (m.game_fps, &m.shown) {
        let r = res_idx(&c.values);
        if !c.on_battery && fps >= 58.0 && sh.within2 >= 85.0 && r < c.res_max && !st.frozen.iter().any(|f| f == "resolution") {
            st.headroom_streak += 1;
            if st.headroom_streak >= 2 {
                st.headroom_streak = 0;
                return Some(Outcome::Set {
                    setting: "resolution".into(),
                    to: modes::res_name(r + 1).into(),
                    because: format!("the game held {fps:.0} fps with even frames in two sessions in a row"),
                });
            }
        } else {
            st.headroom_streak = 0;
        }
    }
    None
}

/// Writes an outcome into the history and the overlay. Returns the new entry.
pub fn record(st: &mut AutopilotState, c: &Ctx, title_id: Option<&str>, outcome: &Outcome, at: &str) -> Change {
    let id = st.next_id();
    let base = Change {
        id,
        at: at.to_string(),
        mode: c.mode.clone(),
        kind: "change".into(),
        setting: String::new(),
        from: false.into(),
        to: false.into(),
        because: String::new(),
        text: String::new(),
        title_id: title_id.map(|t| t.to_string()),
        undone: false,
    };
    let change = match outcome {
        Outcome::Set { setting, to, because } => {
            let from = c.values.get(setting).cloned().unwrap_or_else(|| false.into());
            st.overlay_mode = c.mode.clone();
            st.overlay.insert(setting.clone(), to.clone());
            Change {
                setting: setting.clone(),
                from: from.clone(),
                to: to.clone(),
                because: because.clone(),
                text: format!("Autopilot changed {} from {} to {} because {}.", label(setting), shown_text(&from), shown_text(to), because),
                ..base
            }
        }
        Outcome::Note { setting, because } => Change {
            kind: "note".into(),
            setting: setting.clone(),
            from: true.into(),
            to: true.into(),
            because: because.clone(),
            text: format!("Autopilot kept {} on because {}.", label(setting), because),
            ..base
        },
        Outcome::Warm { title_id: t, because } => Change {
            kind: "action".into(),
            setting: "warm_shaders".into(),
            because: because.clone(),
            to: true.into(),
            text: format!("Autopilot prepared shaders for {} because {}.", t, because),
            title_id: Some(t.clone()),
            ..base
        },
    };
    st.history.push(change.clone());
    if st.history.len() > HISTORY_MAX {
        st.history.remove(0);
    }
    change
}

/// Undoes the newest undoable change: the old value comes back and the setting is left alone.
pub fn undo(st: &mut AutopilotState) -> Option<Change> {
    let pos = st.history.iter().rposition(|c| c.kind == "change" && !c.undone)?;
    st.history[pos].undone = true;
    let c = st.history[pos].clone();
    st.overlay.insert(c.setting.clone(), c.from.clone());
    if !st.frozen.contains(&c.setting) {
        st.frozen.push(c.setting.clone());
    }
    if c.setting == "fluid_presenter" {
        st.fluid_trial = None;
    }
    Some(c)
}

// ---- after a session ------------------------------------------------------------------------------

pub struct SessionInfo {
    pub title_id: Option<String>,
    pub exit_code: i32,
    pub seconds: f64,
    pub launch_on_battery: bool,
}

/// Called when a game started by the launcher exits. Blocking; runs on the watcher thread.
pub fn after_session(app: &AppHandle, info: SessionInfo) {
    let st = app.state::<AppState>();
    let persisted = st.load();
    let mode = modes::current(&persisted);
    if mode == modes::CUSTOM || persisted.autopilot.disabled || !modes::MODES.contains(&mode.as_str()) {
        return;
    }
    let window = info.seconds as u64 + 600;
    let Some((text, key)) = last_session_log(window) else {
        log::info!("Autopilot: no session log to read");
        return;
    };
    if persisted.autopilot.last_session_key.as_deref() == Some(key.as_str()) {
        return;
    }
    let mut metrics = parse_log(&text);
    if metrics.title_id.is_none() {
        metrics.title_id = info.title_id.clone();
    }
    let plan = modes::compute(app, &persisted);
    let ctx = Ctx {
        mode: mode.clone(),
        res_min: plan.bounds.res_min,
        res_max: plan.bounds.res_max,
        values: plan.values.clone(),
        on_battery: info.launch_on_battery,
        exit_code: info.exit_code,
        seconds: info.seconds,
        now: now_secs(),
    };
    log::info!(
        "Autopilot: {mode}, {:.0} s, shown {:?}, game fps {:?}, hitches {}",
        info.seconds,
        metrics.shown.as_ref().map(|s| (s.fps, s.within2)),
        metrics.game_fps,
        metrics.hitches
    );
    let title = metrics.title_id.clone();
    let at = util::now_rfc3339();
    let mut recorded: Option<(Outcome, Change)> = None;
    let _ = st.update(|s| {
        s.autopilot.last_session_key = Some(key.clone());
        if s.autopilot.overlay_mode != mode {
            s.autopilot.reset_for(&mode);
        }
        if let Some(o) = decide(&mut s.autopilot, &metrics, &ctx) {
            let ch = record(&mut s.autopilot, &ctx, title.as_deref(), &o, &at);
            recorded = Some((o, ch));
        }
    });
    if let Some((outcome, change)) = recorded {
        log::info!("{}", change.text);
        if let Outcome::Warm { title_id, .. } = outcome {
            let app2 = app.clone();
            let _ = std::thread::Builder::new().name("autopilot-warm".into()).spawn(move || {
                match warm::run(&app2, &title_id, true) {
                    Ok(r) => {
                        log::info!("Autopilot shader warm-up of {title_id}: {}", r.status);
                        crate::notify::warm_finished(&app2, &r);
                    }
                    Err(e) => log::warn!("Autopilot shader warm-up of {title_id} not run: {e}"),
                }
            });
        }
        if let Err(e) = modes::apply_current(app) {
            log::info!("Autopilot change written at the next start: {e}");
        }
    }
    let _ = app.emit(EVENT, modes::status(app, None));
}

#[cfg(test)]
mod tests {
    use super::*;

    const LOG: &str = "\
[  0.1] Lichii Version: 20.0.2\n\
[  1.0] Loader <Info> Booting game: 0400C3F00006E000\n\
[  2.0] old session line Shown frames: 99.0 per second, interval p5 1.0 p50 1.0 p95 1.0 p99 1.0 max 1.0 ms, 99% within 2 ms of the median\n\
[ 10.0] Loader <Info> Booting game: 0400486024DEC000 | something\n\
[ 11.0] HW_GPU <Info> Nx2Boot event=boot_start mono_s=1.000 rss_mib=10.0\n\
[ 20.0] HW_GPU <Info> Nx2Boot event=first_frame mono_s=31.500 rss_mib=10.0\n\
[ 21.0] Shader precache: finished, 250 built, 30 skipped, 42.5 s\n\
[ 30.0] Render_Vulkan <Info> Shown frames: 10.0 per second, interval p5 30.0 p50 90.0 p95 200.0 p99 300.0 max 400.0 ms, 10% within 2 ms of the median\n\
[ 40.0] Render_Vulkan <Info> Shown frames: 20.0 per second, interval p5 30.0 p50 50.0 p95 60.0 p99 70.0 max 80.0 ms, 60% within 2 ms of the median\n\
[ 50.0] Render_Vulkan <Info> Shown frames: 24.0 per second, interval p5 30.0 p50 40.0 p95 50.0 p99 70.0 max 80.0 ms, 80% within 2 ms of the median\n\
[ 51.0] Render_Vulkan <Info> Fluid presenter: locked to vblank, 60 Hz\n\
[ 52.0] HW_GPU <Info> Nx2Stutter frame=5 ms=80.0 cause=gfx_miss gfx_miss=60.0 other=1.0 busy=front:80 builds=gfx:1,compute:0\n\
[ 53.0] HW_GPU <Info> Nx2Stutter frame=9 ms=50.0 cause=texture texture=40.0 builds=gfx:0,compute:0\n\
[ 54.0] HW_GPU <Info> Nx2StutterSummary window_s=10.00 frames=200 over33=2 over100=0 hist=none\n\
[ 55.0] Render_Vulkan <Info> Graphics pipeline built on the GPU thread in 45.2 ms\n";

    #[test]
    fn reads_the_last_session_only() {
        let m = parse_log(LOG);
        assert_eq!(m.title_id.as_deref(), Some("0400486024DEC000"));
        let s = m.shown.clone().unwrap();
        // First of three windows is left out: the mean of the last two.
        assert_eq!(s.windows, 2);
        assert!((s.fps - 22.0).abs() < 1e-9);
        assert!((s.p50 - 45.0).abs() < 1e-9);
        assert!((s.p95 - 55.0).abs() < 1e-9);
        assert!((s.within2 - 70.0).abs() < 1e-9);
        assert_eq!(m.fluid_active, Some(true));
        assert_eq!(m.stutter_events, 2);
        assert_eq!(m.hitches, 2); // one shader-cause frame, one slow pipeline build
        assert!((m.boot_seconds.unwrap() - 30.5).abs() < 1e-9);
        assert_eq!(m.precache_built, Some(250));
        assert!((m.precache_seconds.unwrap() - 42.5).abs() < 1e-9);
        assert!(m.slow_boot());
        assert!((m.game_fps.unwrap() - 20.0).abs() < 1e-9);
    }

    #[test]
    fn real_session_lines_give_game_fps_and_fluid_state() {
        let log = "Loader <Info> Booting game: 0400486024DEC000
Render.Vulkan <Info> video_core/renderer_vulkan/vk_present_manager.cpp:1082:FluidLogStats: Fluid presenter (10 s, DXGI WaitForVBlank at 120.00 Hz, a slot every 2 refreshes): 601 slots, 331 shown on their slot, 270 missed (265 with no frame ready, 0 with the last copy still queued, 5 slept through)
Render.Vulkan <Info> video_core/renderer_vulkan/vk_present_manager.cpp:1082:FluidLogStats: Fluid presenter (10 s): 91% of shown frames held one slot, held 1/2/3/4+ refreshes 0/300/0/30; real frame age at display 12.4 ms; GPU work done 5.0 ms after recording (0 real frames); 5 frames received, 0 dropped unshown
Render.Vulkan <Info> video_core/renderer_vulkan/vk_present_manager.cpp:1082:FluidLogStats: Fluid presenter (10 s): 91% of shown frames held one slot, held 1/2/3/4+ refreshes 0/300/0/30; real frame age at display 12.4 ms; GPU work done 5.0 ms after recording (200 real frames); 1406 frames received, 1070 dropped unshown
Render.Vulkan <Info> video_core/renderer_vulkan/vk_present_manager.cpp:1082:FluidLogStats: Fluid presenter (10 s): 91% of shown frames held one slot, held 1/2/3/4+ refreshes 0/300/0/30; real frame age at display 12.4 ms; GPU work done 5.0 ms after recording (250 real frames); 1406 frames received, 1070 dropped unshown
Render.Vulkan <Info> video_core/renderer_vulkan/vk_present_manager.cpp:1130:NoteShownFrame: Shown frames: 49.6 per second, interval p5 16.3 p50 16.7 p95 33.4 p99 100.1 max 249.0 ms, 93% within 2 ms of the median
";
        let m = parse_log(log);
        assert_eq!(m.fluid_active, Some(true));
        // The 0-frame window is ignored: (200 + 250) / 20 s.
        assert!((m.game_fps.unwrap() - 22.5).abs() < 1e-9, "{:?}", m.game_fps);
        let s = m.shown.unwrap();
        assert!((s.fps - 49.6).abs() < 1e-9 && (s.within2 - 93.0).abs() < 1e-9 && (s.p95 - 33.4).abs() < 1e-9);
        // Only empty windows: no game fps from them.
        let idle = "Booting game: 0400486024DEC000
FluidLogStats: Fluid presenter (10 s): 91% of shown frames held one slot (0 real frames); 5 frames received
";
        assert_eq!(parse_log(idle).game_fps, None);
    }

    #[test]
    fn a_single_window_is_used_and_fluid_off_is_read() {
        let log = "Booting game: 0400486024DEC000\nShown frames: 30.0 per second, interval p5 1.0 p50 33.0 p95 40.0 p99 1.0 max 1.0 ms, 90% within 2 ms of the median\nFluid presenter disabled: no vblank clock\n";
        let m = parse_log(log);
        assert_eq!(m.shown.as_ref().unwrap().windows, 1);
        assert_eq!(m.fluid_active, Some(false));
        assert_eq!(m.game_fps, Some(30.0)); // no frame generation line: shown rate is the game's
        let fg = format!("{log}Frame generation on: 3x, presenting 60\n");
        assert_eq!(parse_log(&fg).game_fps, None);
        assert_eq!(parse_log("nothing here").shown, None);
    }

    fn ctx(mode: &str, res: &str, battery: bool) -> Ctx {
        let b = modes::bounds(mode, true);
        Ctx {
            mode: mode.into(),
            res_min: b.res_min,
            res_max: b.res_max,
            values: BTreeMap::from([("resolution".to_string(), res.into()), ("fluid_presenter".to_string(), false.into())]),
            on_battery: battery,
            exit_code: 0,
            seconds: 600.0,
            now: 10_000_000,
        }
    }

    fn metrics(fps: f64, within: f64) -> SessionMetrics {
        SessionMetrics {
            title_id: Some("0400486024DEC000".into()),
            shown: Some(Shown { fps, p50: 40.0, p95: 60.0, p99: 80.0, within2: within, windows: 3 }),
            game_fps: Some(fps),
            ..Default::default()
        }
    }

    #[test]
    fn slow_on_battery_lowers_resolution_one_step_and_stops_at_the_bound() {
        let mut st = AutopilotState::default();
        let o = decide(&mut st, &metrics(14.0, 95.0), &ctx("battery", "0.75x", true)).unwrap();
        match &o {
            Outcome::Set { setting, to, .. } => {
                assert_eq!(setting, "resolution");
                assert_eq!(to, &ToggleValue::Str("0.5x".into()));
            }
            other => panic!("{other:?}"),
        }
        // At the bound (0.5x in battery mode) nothing more happens.
        let mut st = AutopilotState::default();
        assert_eq!(decide(&mut st, &metrics(14.0, 95.0), &ctx("battery", "0.5x", true)), None);
        // Quality never goes under 1x.
        let mut st = AutopilotState::default();
        assert_eq!(decide(&mut st, &metrics(10.0, 95.0), &ctx("quality", "1x", false)), None);
        // 18 fps is fine on mains.
        let mut st = AutopilotState::default();
        assert_eq!(decide(&mut st, &metrics(18.0, 95.0), &ctx("auto", "1x", false)), None);
    }

    #[test]
    fn short_and_crashed_sessions_change_nothing() {
        let mut st = AutopilotState::default();
        let mut c = ctx("auto", "1x", false);
        c.seconds = 30.0;
        assert_eq!(decide(&mut st, &metrics(5.0, 50.0), &c), None);
        let mut c = ctx("auto", "1x", false);
        c.exit_code = -1073741819;
        assert_eq!(decide(&mut st, &metrics(5.0, 50.0), &c), None);
        assert!(st.samples.is_empty());
    }

    #[test]
    fn repeated_shader_stutter_schedules_a_warm_up_once_a_day() {
        let mut st = AutopilotState::default();
        let mut m = metrics(30.0, 95.0);
        m.hitches = 5;
        assert_eq!(decide(&mut st, &m, &ctx("auto", "1x", false)), None);
        let o = decide(&mut st, &m, &ctx("auto", "1x", false));
        assert!(matches!(o, Some(Outcome::Warm { .. })), "{o:?}");
        // Strikes again, but a warm-up ran within a day.
        assert_eq!(decide(&mut st, &m, &ctx("auto", "1x", false)), None);
        assert_eq!(decide(&mut st, &m, &ctx("auto", "1x", false)), None);
        // A clean session resets the count.
        let mut st = AutopilotState::default();
        decide(&mut st, &m, &ctx("auto", "1x", false));
        decide(&mut st, &metrics(30.0, 95.0), &ctx("auto", "1x", false));
        assert!(st.strikes.is_empty());
    }

    #[test]
    fn fluid_trial_is_kept_only_when_shown_frames_improve() {
        let run = |after_within: f64, after_fps: f64| {
            let mut st = AutopilotState::default();
            // Two baseline sessions, fluid off, 60% within 2 ms.
            assert_eq!(decide(&mut st, &metrics(30.0, 60.0), &ctx("auto", "1x", false)), None);
            let o = decide(&mut st, &metrics(30.0, 60.0), &ctx("auto", "1x", false)).unwrap();
            assert!(matches!(&o, Outcome::Set { setting, to, .. } if setting == "fluid_presenter" && *to == ToggleValue::Bool(true)));
            assert!(st.fluid_trial.is_some());
            // The next session ran with it on.
            let mut c = ctx("auto", "1x", false);
            c.values.insert("fluid_presenter".into(), true.into());
            let mut m = metrics(after_fps, after_within);
            m.fluid_active = Some(true);
            (decide(&mut st, &m, &c), st)
        };
        let (kept, st) = run(80.0, 30.0);
        assert!(matches!(kept, Some(Outcome::Note { .. })), "{kept:?}");
        assert!(st.fluid_trial.is_none() && st.frozen.is_empty());
        let (rejected, st) = run(61.0, 30.0);
        assert!(matches!(&rejected, Some(Outcome::Set { to, .. }) if *to == ToggleValue::Bool(false)), "{rejected:?}");
        assert!(st.frozen.contains(&"fluid_presenter".to_string()) && st.fluid_rejected_at.is_some());
        // Better pacing but a slower game is rejected too.
        let (slow, _) = run(90.0, 20.0);
        assert!(matches!(&slow, Some(Outcome::Set { to, .. }) if *to == ToggleValue::Bool(false)), "{slow:?}");
    }

    #[test]
    fn no_trial_without_baselines_or_on_battery_or_when_frames_are_even() {
        let mut st = AutopilotState::default();
        assert_eq!(decide(&mut st, &metrics(30.0, 60.0), &ctx("auto", "1x", false)), None);
        let mut st = AutopilotState::default();
        decide(&mut st, &metrics(30.0, 60.0), &ctx("auto", "1x", true));
        assert_eq!(decide(&mut st, &metrics(30.0, 60.0), &ctx("auto", "1x", true)), None);
        let mut st = AutopilotState::default();
        decide(&mut st, &metrics(30.0, 97.0), &ctx("auto", "1x", false));
        assert_eq!(decide(&mut st, &metrics(30.0, 97.0), &ctx("auto", "1x", false)), None);
    }

    #[test]
    fn a_crash_during_the_trial_reverts_it() {
        let mut st = AutopilotState::default();
        st.fluid_trial = Some(FluidTrial { base_within2: 60.0, base_spread: 1.5, base_fps: None, waited: 0 });
        let mut c = ctx("auto", "1x", false);
        c.values.insert("fluid_presenter".into(), true.into());
        c.exit_code = 3;
        let o = decide(&mut st, &metrics(30.0, 80.0), &c);
        assert!(matches!(&o, Some(Outcome::Set { setting, to, .. }) if setting == "fluid_presenter" && *to == ToggleValue::Bool(false)));
    }

    #[test]
    fn a_game_that_did_not_start_keeps_the_trial() {
        let mut st = AutopilotState::default();
        st.fluid_trial = Some(FluidTrial { base_within2: 60.0, base_spread: 1.5, base_fps: None, waited: 0 });
        let mut c = ctx("auto", "1x", false);
        c.values.insert("fluid_presenter".into(), true.into());
        c.exit_code = 1;
        assert_eq!(decide(&mut st, &SessionMetrics::default(), &c), None);
        assert!(st.fluid_trial.is_some() && st.frozen.is_empty() && st.samples.is_empty());
    }

    #[test]
    fn headroom_raises_resolution_after_two_good_sessions_within_bounds() {
        let mut st = AutopilotState::default();
        assert_eq!(decide(&mut st, &metrics(60.0, 95.0), &ctx("auto", "1x", false)), None);
        let o = decide(&mut st, &metrics(60.0, 95.0), &ctx("auto", "1x", false)).unwrap();
        assert!(matches!(&o, Outcome::Set { to, .. } if *to == ToggleValue::Str("1.25x".into())));
        // Auto on an iGPU stops at 1.25x.
        let mut st = AutopilotState::default();
        decide(&mut st, &metrics(60.0, 95.0), &ctx("auto", "1.25x", false));
        assert_eq!(decide(&mut st, &metrics(60.0, 95.0), &ctx("auto", "1.25x", false)), None);
    }

    #[test]
    fn history_records_and_undo_restores_and_freezes() {
        let mut st = AutopilotState::default();
        let c = ctx("auto", "1x", false);
        let o = Outcome::Set { setting: "resolution".into(), to: "0.75x".into(), because: "the game ran at 12 fps, under 15".into() };
        let ch = record(&mut st, &c, None, &o, "2026-11-13T10:00:00Z");
        assert_eq!(ch.text, "Autopilot changed the resolution from 1x to 0.75x because the game ran at 12 fps, under 15.");
        assert_eq!(st.overlay.get("resolution"), Some(&ToggleValue::Str("0.75x".into())));
        assert!(view(&st).can_undo);
        let undone = undo(&mut st).unwrap();
        assert_eq!(undone.id, ch.id);
        assert_eq!(st.overlay.get("resolution"), Some(&ToggleValue::Str("1x".into())));
        assert!(st.frozen.contains(&"resolution".to_string()));
        assert!(!view(&st).can_undo);
        assert!(undo(&mut st).is_none());
        // Frozen: slow sessions no longer lower it.
        assert_eq!(decide(&mut st, &metrics(5.0, 95.0), &c), None);
        // A mode change clears the freeze and the overlay, keeps the history.
        st.reset_for("smooth");
        assert!(st.frozen.is_empty() && st.overlay.is_empty() && st.history.len() == 1);
    }

    #[test]
    fn history_is_capped() {
        let mut st = AutopilotState::default();
        let c = ctx("auto", "1x", false);
        let o = Outcome::Set { setting: "resolution".into(), to: "0.75x".into(), because: "x".into() };
        for _ in 0..(HISTORY_MAX + 5) {
            record(&mut st, &c, None, &o, "t");
        }
        assert_eq!(st.history.len(), HISTORY_MAX);
        assert_eq!(st.history.last().unwrap().id as usize, HISTORY_MAX + 5);
    }
}
