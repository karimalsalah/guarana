//! The Game Booster: three opt-in levels of Windows tuning, applied while a game launched from
//! Guarana runs and taken back when the game exits, when the launcher starts after a crash (from
//! the journal), or from "Undo all". Off by default.
//!
//! | Level | Needs admin | What it does (each level adds to the one before) |
//! |---|---|---|
//! | 1 | no | Guarana at Above Normal priority; power throttling (EcoQoS) off for it; the
//! |   |    | high-performance GPU asked for `guarana.exe` (`HKCU\...\DirectX\UserGpuPreferences`) |
//! | 2 | one prompt | the active power plan switched to Ultimate Performance (if present) or High
//! |   |            | performance for the session, then put back |
//! | 3 | one prompt | as Level 2, but Guarana at High priority, and SysMain and Windows Search paused
//! |   |            | while you play and restarted after |
//!
//! Only two things need administrator rights: the power-plan switch and pausing services. They are
//! done by a small elevated helper (`--booster-helper`, started with ShellExecuteEx "runas"), which
//! applies them, waits for the emulator to exit, and restores them, so the whole session costs one
//! UAC prompt. Everything else (priority, EcoQoS, the GPU preference) is set by the launcher itself
//! without elevation. Security settings are never touched: no Defender, firewall, VBS/HVCI,
//! SmartScreen, UAC, Windows Update or BitLocker, and nothing under HKLM except what `powercfg`
//! itself writes.
//!
//! The pure parts here (the plan for each level, the GPU-preference merge and the journal format)
//! are unit-tested; the Windows calls live in `sys` and the elevated steps in `run_helper`.

use std::path::Path;

use serde::{Deserialize, Serialize};
use tauri::{AppHandle, Emitter, Manager};

use crate::paths;
use crate::state::AppState;
use crate::util::{self, CmdResult};

pub const EVENT: &str = "booster://changed";
/// First argument of the elevated helper process (the launcher started again with "runas").
pub const HELPER_FLAG: &str = "--booster-helper";
pub const MAX_LEVEL: u8 = 3;

/// `HKCU` key and value token the GPU preference lives in.
const GPU_PREF_KEY: &str = r"Software\Microsoft\DirectX\UserGpuPreferences";
const GPU_PREF_TOKEN: &str = "GpuPreference";
/// 2 = "High performance" GPU for this program (Windows graphics preference).
const GPU_PREF_HIGH: &str = "2";

/// The two high-performance power schemes, best first. GUIDs are the Windows built-ins.
const ULTIMATE_PERF_GUID: &str = "e9a42b02-d5df-448d-aa00-03f14749eb61";
const HIGH_PERF_GUID: &str = "8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c";

/// Services paused for the session at Level 3 (only if running), then restarted. Never disabled,
/// never reconfigured. Their start type is left alone, so a reboot brings them back on its own.
const SERVICES: [(&str, &str); 2] = [("SysMain", "SysMain"), ("WSearch", "Windows Search")];

// ---- The plan for each level (pure) ----------------------------------------------------------------

/// What the machine looks like before boosting: what the plan can actually do here.
#[derive(Debug, Clone, Default)]
pub struct Facts {
    /// The high-performance scheme to switch to, if one is installed.
    pub scheme: SchemeTarget,
    /// That scheme is already the active one, so there is nothing to switch.
    pub scheme_active: bool,
    /// SysMain and WSearch, and whether each is running now.
    pub services: Vec<ServiceFact>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum SchemeTarget {
    #[default]
    None,
    High,
    Ultimate,
}

impl SchemeTarget {
    fn label(self) -> Option<&'static str> {
        match self {
            SchemeTarget::None => None,
            SchemeTarget::High => Some("High performance"),
            SchemeTarget::Ultimate => Some("Ultimate Performance"),
        }
    }

    fn guid(self) -> Option<&'static str> {
        match self {
            SchemeTarget::None => None,
            SchemeTarget::High => Some(HIGH_PERF_GUID),
            SchemeTarget::Ultimate => Some(ULTIMATE_PERF_GUID),
        }
    }
}

#[derive(Debug, Clone)]
pub struct ServiceFact {
    pub name: String,
    pub display: String,
    pub running: bool,
}

/// One level as the UI shows it: its title, one line, whether it asks for admin, and the exact
/// list of changes it makes on this PC. The UI renders the picker and the "what it changes" list
/// from this, so the two can never drift from what the backend does.
#[derive(Debug, Clone, Serialize)]
pub struct BoosterLevelInfo {
    pub level: u8,
    pub title: String,
    pub summary: String,
    pub elevated: bool,
    pub changes: Vec<String>,
}

#[derive(Debug, Clone, Serialize)]
pub struct BoosterStatus {
    /// The chosen level (0 = Off), saved and used at the next launch.
    pub level: u8,
    /// A boosted session is running now.
    pub active: bool,
    pub levels: Vec<BoosterLevelInfo>,
    pub note: Option<String>,
}

fn priority_name(level: u8) -> &'static str {
    if level >= 3 {
        "High"
    } else {
        "Above Normal"
    }
}

/// Whether a power-plan switch would actually happen for `level` here (a plan is installed, and it
/// is not already the active one).
fn switches_scheme(level: u8, facts: &Facts) -> bool {
    level >= 2 && facts.scheme != SchemeTarget::None && !facts.scheme_active
}

/// Whether `level` has any step that needs administrator rights on this machine: a power-plan switch
/// that will really happen, or a Level 3 service that is actually running. With nothing to do, the
/// launcher does not prompt at all.
pub fn needs_elevation(level: u8, facts: &Facts) -> bool {
    switches_scheme(level, facts) || (level >= 3 && facts.services.iter().any(|s| s.running))
}

/// The changes `level` makes, in order, as plain sentences for the UI and the journal description.
fn changes_for(level: u8, facts: &Facts) -> Vec<String> {
    let mut out = Vec::new();
    if level == 0 {
        return out;
    }
    out.push(format!("Run Guarana at {} priority", priority_name(level)));
    out.push("Turn off power throttling (EcoQoS) for Guarana".to_string());
    out.push("Ask Windows for the high-performance GPU for guarana.exe".to_string());
    if level >= 2 {
        match (facts.scheme.label(), facts.scheme_active) {
            (Some(name), false) => out.push(format!("Switch the power plan to {name} for this session, then put it back")),
            (Some(name), true) => out.push(format!("Use the {name} power plan (it is already the active one)")),
            (None, _) => out.push("Switch to a high-performance power plan (none is installed on this PC, so this is skipped)".to_string()),
        }
    }
    if level >= 3 {
        let running: Vec<&str> = facts.services.iter().filter(|s| s.running).map(|s| s.display.as_str()).collect();
        if running.is_empty() {
            out.push("Pause SysMain and Windows Search while you play (neither is running now)".to_string());
        } else {
            out.push(format!("Pause {} while you play, and restart {} after", running.join(" and "), if running.len() == 1 { "it" } else { "them" }));
        }
    }
    out
}

/// One line describing what the level does here, and — exactly — what its Windows prompt is for,
/// built from the same facts as the plan so the two cannot disagree.
fn summary_for(level: u8, facts: &Facts) -> String {
    if level == 0 {
        return "No changes. Windows runs games as usual.".to_string();
    }
    if level == 1 {
        return "No admin needed: priority, power throttling and the GPU choice for Guarana.".to_string();
    }
    // Level 2 or 3: the parts that would need the one Windows prompt, in plain words.
    let mut prompt: Vec<String> = Vec::new();
    if switches_scheme(level, facts) {
        if let Some(name) = facts.scheme.label() {
            prompt.push(format!("switch the power plan to {name}"));
        }
    }
    if level >= 3 {
        let running: Vec<&str> = facts.services.iter().filter(|s| s.running).map(|s| s.display.as_str()).collect();
        if !running.is_empty() {
            prompt.push(format!("pause {}", running.join(" and ")));
        }
    }
    let base = if level >= 3 {
        "High priority, power throttling off and the high-performance GPU for Guarana"
    } else {
        "Above Normal priority, power throttling off and the high-performance GPU for Guarana"
    };
    if prompt.is_empty() {
        let why = if facts.scheme == SchemeTarget::None {
            "; no high-performance power plan is installed to switch to"
        } else if facts.scheme_active {
            "; the high-performance power plan is already active"
        } else {
            ""
        };
        format!("No Windows prompt here: {base}{why}.")
    } else {
        format!("{base}, and one Windows permission prompt to {}.", prompt.join(", and "))
    }
}

fn title_for(level: u8) -> &'static str {
    match level {
        0 => "Off",
        1 => "Level 1",
        2 => "Level 2",
        _ => "Level 3",
    }
}

pub fn level_info(level: u8, facts: &Facts) -> BoosterLevelInfo {
    BoosterLevelInfo {
        level,
        title: title_for(level).to_string(),
        summary: summary_for(level, facts),
        elevated: needs_elevation(level, facts),
        changes: changes_for(level, facts),
    }
}

/// All four levels (Off..3) for the picker.
pub fn all_levels(facts: &Facts) -> Vec<BoosterLevelInfo> {
    (0..=MAX_LEVEL).map(|l| level_info(l, facts)).collect()
}

// ---- The GPU preference token (pure) ---------------------------------------------------------------

/// Splits a `UserGpuPreferences` value like `GpuPreference=2;SomethingElse=1;` into its tokens,
/// keeping their order and dropping empty ones.
fn parse_tokens(value: &str) -> Vec<(String, String)> {
    value
        .split(';')
        .filter(|s| !s.trim().is_empty())
        .map(|pair| match pair.split_once('=') {
            Some((k, v)) => (k.trim().to_string(), v.trim().to_string()),
            None => (pair.trim().to_string(), String::new()),
        })
        .collect()
}

fn serialize_tokens(tokens: &[(String, String)]) -> String {
    tokens.iter().map(|(k, v)| format!("{k}={v};")).collect()
}

fn token_of<'a>(tokens: &'a [(String, String)], key: &str) -> Option<&'a str> {
    tokens.iter().find(|(k, _)| k.eq_ignore_ascii_case(key)).map(|(_, v)| v.as_str())
}

/// The value to write for the GPU preference, and the previous `GpuPreference` token (so it can be
/// put back): other tokens, such as Auto HDR, are kept as they are.
pub struct GpuPrefApply {
    pub previous: Option<String>,
    pub wrote: String,
}

pub fn gpu_pref_apply(current: Option<&str>) -> GpuPrefApply {
    let mut tokens = parse_tokens(current.unwrap_or_default());
    let previous = token_of(&tokens, GPU_PREF_TOKEN).map(str::to_string);
    match tokens.iter_mut().find(|(k, _)| k.eq_ignore_ascii_case(GPU_PREF_TOKEN)) {
        Some(entry) => entry.1 = GPU_PREF_HIGH.to_string(),
        None => tokens.push((GPU_PREF_TOKEN.to_string(), GPU_PREF_HIGH.to_string())),
    }
    GpuPrefApply { previous, wrote: serialize_tokens(&tokens) }
}

/// What to do with the GPU-preference value on undo.
#[derive(Debug, PartialEq, Eq)]
pub enum GpuPrefRestore {
    /// Write this value back.
    Write(String),
    /// Nothing of ours is left in it (the whole value): delete the value.
    Delete,
    /// The user changed it since, or it is not ours any more: leave it.
    Leave,
}

/// Restores the GPU preference only when it still holds the `2` we wrote, so a change the user made
/// in the meantime is never clobbered. `previous` is the token value before we set it (None when it
/// was absent).
pub fn gpu_pref_restore(current: Option<&str>, previous: Option<&str>) -> GpuPrefRestore {
    let mut tokens = parse_tokens(current.unwrap_or_default());
    if token_of(&tokens, GPU_PREF_TOKEN) != Some(GPU_PREF_HIGH) {
        return GpuPrefRestore::Leave;
    }
    match previous {
        Some(prev) => {
            if let Some(entry) = tokens.iter_mut().find(|(k, _)| k.eq_ignore_ascii_case(GPU_PREF_TOKEN)) {
                entry.1 = prev.to_string();
            }
            GpuPrefRestore::Write(serialize_tokens(&tokens))
        }
        None => {
            tokens.retain(|(k, _)| !k.eq_ignore_ascii_case(GPU_PREF_TOKEN));
            if tokens.is_empty() {
                GpuPrefRestore::Delete
            } else {
                GpuPrefRestore::Write(serialize_tokens(&tokens))
            }
        }
    }
}

// ---- The journal (pure) ----------------------------------------------------------------------------

const JOURNAL_VERSION: u32 = 1;

/// One change, with what is needed to take it back. Priority and EcoQoS belong to the emulator
/// process and vanish when it exits, so they carry nothing to restore; they are kept for the record.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum JournalEntry {
    Priority { class: String },
    EcoQos,
    GpuPreference { exe: String, previous: Option<String>, wrote: String },
    PowerScheme { previous: String, target: String },
    Service { name: String, was_running: bool },
}

/// The booster journal: the session that owns it and the changes it made. Written atomically before
/// any change, and the single source of truth for undo and crash recovery.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Journal {
    pub version: u32,
    pub active: bool,
    pub level: u8,
    /// The launcher process that wrote it, and its creation time (FILETIME as u64), for the record.
    pub owner_pid: u32,
    pub owner_started: u64,
    /// The emulator the session boosted, and its creation time, filled in once it has spawned. Undo
    /// and crash recovery use this to tell a live boosted game from a stale journal with a reused pid.
    pub emulator_pid: u32,
    pub emulator_started: u64,
    pub entries: Vec<JournalEntry>,
}

impl Default for Journal {
    fn default() -> Self {
        Self {
            version: JOURNAL_VERSION,
            active: false,
            level: 0,
            owner_pid: 0,
            owner_started: 0,
            emulator_pid: 0,
            emulator_started: 0,
            entries: Vec::new(),
        }
    }
}

impl Journal {
    fn read() -> Option<Journal> {
        let bytes = std::fs::read(paths::booster_journal()).ok()?;
        let journal: Journal = serde_json::from_slice(&bytes).ok()?;
        (journal.version == JOURNAL_VERSION).then_some(journal)
    }

    fn write(&self) -> CmdResult<()> {
        let bytes = serde_json::to_vec_pretty(self).map_err(|e| e.to_string())?;
        util::write_atomic(&paths::booster_journal(), &bytes).map_err(|e| format!("Could not write the booster journal: {e}"))
    }

    fn clear() {
        let _ = std::fs::remove_file(paths::booster_journal());
    }
}

// ---- Status and settings ---------------------------------------------------------------------------

/// The saved level, clamped to a level that exists.
pub fn level_of(persisted: &crate::state::Persisted) -> u8 {
    persisted.booster_level.min(MAX_LEVEL)
}

pub fn status(app: &AppHandle, note: Option<String>) -> BoosterStatus {
    let st = app.state::<AppState>();
    let level = level_of(&st.load());
    let facts = gather_facts();
    BoosterStatus { level, active: Journal::read().map(|j| j.active).unwrap_or(false), levels: all_levels(&facts), note }
}

fn emit(app: &AppHandle, note: Option<String>) {
    let s = status(app, note);
    let _ = app.emit(EVENT, s);
}

pub fn set_level(app: &AppHandle, level: u8) -> CmdResult<BoosterStatus> {
    if level > MAX_LEVEL {
        return Err(format!("Unknown booster level {level}: use 0 (Off) to {MAX_LEVEL}"));
    }
    let st = app.state::<AppState>();
    st.update(|s| s.booster_level = level)?;
    log::info!("Game Booster set to level {level}");
    Ok(status(app, None))
}

// ---- Applying a session (launcher side, no elevation) ---------------------------------------------

/// Reads what the plan can do here: which high-performance scheme exists, whether it is already
/// active, and which services run.
pub fn gather_facts() -> Facts {
    let scheme = sys::best_scheme_target();
    let active = sys::active_scheme_guid();
    let scheme_active = scheme.guid().map(|g| active.as_deref() == Some(g)).unwrap_or(false);
    let services = SERVICES
        .iter()
        .map(|(name, display)| ServiceFact { name: name.to_string(), display: display.to_string(), running: sys::service_running(name).unwrap_or(false) })
        .collect();
    Facts { scheme, scheme_active, services }
}

/// What `apply_before_spawn` decided, carried through to the elevated helper.
pub struct Prep {
    /// The power-plan guid to switch to, or `"none"`.
    scheme_arg: String,
    /// The session needs the elevated helper (power plan and/or services).
    elevated: bool,
}

/// Step one, before the emulator is spawned: journal every change, then set the GPU preference. The
/// GPU preference must be written before the emulator creates its graphics device, or Windows will
/// not read it for this run. Returns `None` (do not boost) for level 0 or if the journal cannot be
/// written; on a failed spawn the caller calls `restore_after_session` to take the GPU change back.
pub fn apply_before_spawn(exe: &Path, level: u8) -> Option<Prep> {
    if level == 0 {
        return None;
    }
    let facts = gather_facts();
    let exe_name = exe.to_string_lossy().into_owned();
    let apply = sys::read_gpu_pref(&exe_name).map(|cur| gpu_pref_apply(Some(&cur))).unwrap_or_else(|| gpu_pref_apply(None));

    let mut entries = vec![
        JournalEntry::Priority { class: priority_name(level).to_string() },
        JournalEntry::EcoQos,
        JournalEntry::GpuPreference { exe: exe_name.clone(), previous: apply.previous.clone(), wrote: apply.wrote.clone() },
    ];
    let want_scheme = switches_scheme(level, &facts);
    let scheme_guid = facts.scheme.guid().map(str::to_string);
    if want_scheme {
        if let Some(prev) = sys::active_scheme_guid() {
            entries.push(JournalEntry::PowerScheme { previous: prev, target: scheme_guid.clone().unwrap() });
        }
    }
    if level >= 3 {
        for s in &facts.services {
            entries.push(JournalEntry::Service { name: s.name.clone(), was_running: s.running });
        }
    }

    let journal = Journal {
        version: JOURNAL_VERSION,
        active: true,
        level,
        owner_pid: std::process::id(),
        owner_started: sys::process_started(std::process::id()).unwrap_or(0),
        emulator_pid: 0,
        emulator_started: 0,
        entries,
    };
    if let Err(e) = journal.write() {
        log::warn!("Game Booster: {e}; not boosting this session");
        return None;
    }
    if let Err(e) = sys::write_gpu_pref(&exe_name, &apply.wrote) {
        log::warn!("Game Booster: could not set the GPU preference: {e}");
    }
    Some(Prep {
        scheme_arg: if want_scheme { scheme_guid.unwrap() } else { "none".to_string() },
        elevated: needs_elevation(level, &facts),
    })
}

/// Step two, right after the emulator spawned (before the game-watch thread): set its priority and
/// turn its power throttling off, and record which process the session boosted.
pub fn apply_after_spawn(app: &AppHandle, handle: std::os::windows::io::RawHandle, pid: u32, level: u8) {
    let priority = if level >= 3 { sys::Priority::High } else { sys::Priority::AboveNormal };
    sys::set_priority(handle, priority);
    sys::disable_eco_qos(handle);
    let started = sys::process_started(pid).unwrap_or(0);
    if let Some(mut journal) = Journal::read() {
        journal.emulator_pid = pid;
        journal.emulator_started = started;
        let _ = journal.write();
    }
    log::info!("Game Booster: level {level} applied for pid {pid}");
    emit(app, None);
}

/// Step three, after the game-watch thread exists: start the elevated helper on its own thread, so
/// the UAC prompt never holds the launch up. The helper switches the power plan and pauses the
/// services, waits for the emulator to exit, and restores both.
pub fn launch_helper_async(app: &AppHandle, pid: u32, level: u8, prep: Prep) {
    if !prep.elevated {
        emit(app, None);
        return;
    }
    let started = Journal::read().map(|j| j.emulator_started).unwrap_or(0);
    let app = app.clone();
    let _ = std::thread::Builder::new().name("booster-helper".into()).spawn(move || match sys::launch_helper(level, &prep.scheme_arg, pid, started) {
        Ok(()) => {
            log::info!("Game Booster: elevated helper started for level {level}");
            emit(&app, None);
        }
        Err(sys::HelperError::Declined) => {
            log::info!("Game Booster: UAC declined; running the no-admin parts only");
            emit(&app, Some("Boost is on, but you declined the Windows prompt, so the power plan and services were left as they are.".to_string()));
        }
        Err(sys::HelperError::Failed(e)) => {
            log::warn!("Game Booster: could not start the elevated helper: {e}");
            emit(&app, Some("Boost is on, but the elevated step could not start, so the power plan and services were left as they are.".to_string()));
        }
    });
}

/// After the game exits (or from "Undo all"): takes back the no-admin changes from the journal and
/// marks it inactive. The elevated helper puts the power plan and services back on its own when the
/// emulator exits; the scheme is restored here too (no admin needed) as a backstop.
pub fn restore_after_session(app: &AppHandle) {
    let Some(journal) = Journal::read() else { return };
    if !journal.active {
        return;
    }
    undo_unelevated(&journal);
    let mut cleared = journal.clone();
    cleared.active = false;
    let _ = cleared.write();
    Journal::clear();
    log::info!("Game Booster: session restored");
    emit(app, None);
}

/// Restores the parts the launcher can do without admin: the GPU preference and the active power
/// scheme (both are per-user). Services and the scheme are also handled by the elevated helper.
fn undo_unelevated(journal: &Journal) {
    for entry in &journal.entries {
        match entry {
            JournalEntry::GpuPreference { exe, previous, .. } => {
                let current = sys::read_gpu_pref(exe);
                match gpu_pref_restore(current.as_deref(), previous.as_deref()) {
                    GpuPrefRestore::Write(value) => {
                        if let Err(e) = sys::write_gpu_pref(exe, &value) {
                            log::warn!("Game Booster: could not restore the GPU preference: {e}");
                        }
                    }
                    GpuPrefRestore::Delete => sys::delete_gpu_pref(exe),
                    GpuPrefRestore::Leave => {}
                }
            }
            JournalEntry::PowerScheme { previous, target } => {
                // Only when our scheme is still the active one, like the emulator's own policy does.
                if sys::active_scheme_guid().as_deref() == Some(target.as_str()) {
                    if let Err(e) = sys::set_active_scheme(previous) {
                        log::warn!("Game Booster: could not restore the power plan: {e}");
                    }
                }
            }
            JournalEntry::Priority { .. } | JournalEntry::EcoQos | JournalEntry::Service { .. } => {}
        }
    }
}

/// Services the journal says the booster paused (they were running when it started).
fn paused_services(journal: &Journal) -> Vec<&str> {
    journal
        .entries
        .iter()
        .filter_map(|e| match e {
            JournalEntry::Service { name, was_running: true } => Some(name.as_str()),
            _ => None,
        })
        .collect()
}

/// "Undo all": restore now, whether or not a game is running. The no-admin parts (GPU preference,
/// the power plan, and the emulator's priority while it still runs) are put back here. Paused
/// services that are still stopped are restarted by a short elevated helper (one prompt), since the
/// session helper otherwise only restores them when the game exits.
pub fn undo_all(app: &AppHandle) -> CmdResult<BoosterStatus> {
    let Some(journal) = Journal::read().filter(|j| j.active) else {
        return Ok(status(app, Some("Nothing to undo.".to_string())));
    };
    if journal.emulator_pid != 0 && sys::process_matches(journal.emulator_pid, journal.emulator_started) {
        sys::reset_priority(journal.emulator_pid);
    }
    undo_unelevated(&journal);
    let mut note = "Undone. Priority, the GPU choice and the power plan are back to normal.".to_string();
    let still_stopped: Vec<&str> = paused_services(&journal).into_iter().filter(|n| sys::service_running(n) == Some(false)).collect();
    if !still_stopped.is_empty() {
        match sys::launch_restore_helper(&still_stopped) {
            Ok(()) => note = "Undone. Priority, the GPU choice, the power plan and the paused services are back to normal.".to_string(),
            Err(sys::HelperError::Declined) => note = "Undone, except the paused services: you declined the Windows prompt. They restart on reboot.".to_string(),
            Err(_) => note = "Undone. SysMain and Windows Search restart on their own; a reboot is enough.".to_string(),
        }
    }
    let mut cleared = journal;
    cleared.active = false;
    let _ = cleared.write();
    Journal::clear();
    log::info!("Game Booster: Undo all");
    Ok(status(app, Some(note)))
}

/// On launcher startup, after a crash left a journal: if the boosted game is still running, wait for
/// it and then take the no-admin changes back (the elevated helper, if it survived, restores the
/// power plan and services itself). If the game is already gone, take them back now. Paused services
/// are restarted by the session helper or on reboot; the launcher never prompts for admin at startup,
/// so a service still stopped here is left for the user's next "Undo all" or a reboot.
pub fn recover_after_crash(app: &AppHandle) {
    let Some(journal) = Journal::read() else { return };
    if !journal.active {
        Journal::clear();
        return;
    }
    if journal.emulator_pid != 0 && sys::process_matches(journal.emulator_pid, journal.emulator_started) {
        log::info!("Game Booster: a boosted game is still running; will restore when it exits");
        let app = app.clone();
        let pid = journal.emulator_pid;
        let _ = std::thread::Builder::new().name("booster-recover".into()).spawn(move || {
            sys::wait_for_exit(pid);
            restore_after_session(&app);
        });
        return;
    }
    log::info!("Game Booster: recovering a boosted session left by a crash");
    undo_unelevated(&journal);
    Journal::clear();
    emit(app, None);
}

// ---- The elevated helper ---------------------------------------------------------------------------

/// The elevated helper, run by the launcher (`--booster-helper ...`). Two forms:
/// `<level> <scheme-guid|none> <emulator-pid> <emulator-created>` confirms the emulator, switches
/// the power plan, pauses the services, waits for that process to exit, then restores both;
/// `restore <service>...` just restarts the named services (for "Undo all"). It reads nothing
/// user-writable: the service list is fixed here, the scheme guid is validated against the installed
/// schemes, and it never touches the journal. Returns a process exit code.
pub fn run_helper(args: &[String]) -> i32 {
    // Restore-only mode (from "Undo all"): restart the named services, then exit.
    if args.first().map(String::as_str) == Some("restore") {
        return sys::restore_services(&args[1..]);
    }
    let level: u8 = args.first().and_then(|s| s.parse().ok()).unwrap_or(0);
    let scheme_arg = args.get(1).map(String::as_str).unwrap_or("none");
    let pid: u32 = args.get(2).and_then(|s| s.parse().ok()).unwrap_or(0);
    let started: u64 = args.get(3).and_then(|s| s.parse().ok()).unwrap_or(0);
    if level < 2 || pid == 0 {
        return 2;
    }
    sys::helper_main(level, scheme_arg, pid, started)
}

// ---- Windows calls ---------------------------------------------------------------------------------

mod sys {
    use std::ffi::c_void;
    use std::os::windows::ffi::OsStrExt;
    use std::os::windows::io::RawHandle;

    use windows::core::{HRESULT, GUID, PCWSTR};
    use windows::Win32::Foundation::{CloseHandle, ERROR_CANCELLED, FILETIME, HANDLE, HLOCAL};
    use windows::Win32::System::Power::{PowerEnumerate, PowerGetActiveScheme, PowerSetActiveScheme, ACCESS_SCHEME};
    use windows::Win32::System::Services::{
        CloseServiceHandle, ControlService, OpenSCManagerW, OpenServiceW, QueryServiceStatus, StartServiceW, SC_HANDLE,
        SC_MANAGER_CONNECT, SERVICE_CONTROL_STOP, SERVICE_QUERY_STATUS, SERVICE_RUNNING, SERVICE_START, SERVICE_STATUS,
        SERVICE_STOP, SERVICE_STOPPED, SERVICE_STOP_PENDING,
    };
    use windows::Win32::System::Threading::{
        GetProcessTimes, OpenProcess, SetPriorityClass, SetProcessInformation, WaitForSingleObject, ABOVE_NORMAL_PRIORITY_CLASS,
        HIGH_PRIORITY_CLASS, NORMAL_PRIORITY_CLASS, PROCESS_ACCESS_RIGHTS, PROCESS_CREATION_FLAGS, PROCESS_POWER_THROTTLING_CURRENT_VERSION,
        PROCESS_POWER_THROTTLING_EXECUTION_SPEED, PROCESS_POWER_THROTTLING_STATE, PROCESS_QUERY_LIMITED_INFORMATION,
        PROCESS_SET_INFORMATION, PROCESS_SYNCHRONIZE, ProcessPowerThrottling,
    };
    use windows::Win32::System::Com::{CoInitializeEx, CoUninitialize, COINIT_APARTMENTTHREADED, COINIT_DISABLE_OLE1DDE};
    use windows::Win32::UI::Shell::{ShellExecuteExW, SEE_MASK_FLAG_NO_UI, SEE_MASK_NOASYNC, SEE_MASK_NOCLOSEPROCESS, SHELLEXECUTEINFOW};

    use super::{HIGH_PERF_GUID, SchemeTarget, GPU_PREF_KEY, SERVICES, ULTIMATE_PERF_GUID};
    use crate::util::CmdResult;

    pub enum Priority {
        AboveNormal,
        High,
    }

    pub enum HelperError {
        Declined,
        Failed(String),
    }

    fn wide(s: &str) -> Vec<u16> {
        std::ffi::OsStr::new(s).encode_wide().chain(std::iter::once(0)).collect()
    }

    fn to_handle(raw: RawHandle) -> HANDLE {
        HANDLE(raw as *mut c_void)
    }

    // ---- GUIDs -------------------------------------------------------------------------------------

    /// Parses `xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx` into a GUID, or None.
    fn parse_guid(text: &str) -> Option<GUID> {
        let hex: String = text.chars().filter(|c| *c != '-').collect();
        if hex.len() != 32 || !hex.chars().all(|c| c.is_ascii_hexdigit()) {
            return None;
        }
        Some(GUID::from_u128(u128::from_str_radix(&hex, 16).ok()?))
    }

    fn guid_text(guid: &GUID) -> String {
        let d4 = guid.data4;
        format!(
            "{:08x}-{:04x}-{:04x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
            guid.data1, guid.data2, guid.data3, d4[0], d4[1], d4[2], d4[3], d4[4], d4[5], d4[6], d4[7]
        )
    }

    // ---- Priority and EcoQoS (on the emulator the launcher spawned) --------------------------------

    pub fn set_priority(handle: RawHandle, priority: Priority) {
        let class: PROCESS_CREATION_FLAGS = match priority {
            Priority::AboveNormal => ABOVE_NORMAL_PRIORITY_CLASS,
            Priority::High => HIGH_PRIORITY_CLASS,
        };
        // SAFETY: a process handle the launcher owns; the call only changes its priority class.
        if let Err(e) = unsafe { SetPriorityClass(to_handle(handle), class) } {
            log::warn!("Game Booster: could not set priority: {e}");
        }
    }

    /// Puts the emulator back to Normal priority while it still runs (for "Undo all"). No elevation:
    /// it is the user's own process.
    pub fn reset_priority(pid: u32) {
        // SAFETY: opens the process for a priority change and closes the handle.
        unsafe {
            let Ok(handle) = OpenProcess(PROCESS_SET_INFORMATION, false, pid) else {
                return;
            };
            let _ = SetPriorityClass(handle, NORMAL_PRIORITY_CLASS);
            let _ = CloseHandle(handle);
        }
    }

    /// Turns power throttling off for the emulator (EcoQoS off): ControlMask names the execution
    /// speed, StateMask 0 means do not throttle. The process exits with the game, so nothing is
    /// restored.
    pub fn disable_eco_qos(handle: RawHandle) {
        let state = PROCESS_POWER_THROTTLING_STATE {
            Version: PROCESS_POWER_THROTTLING_CURRENT_VERSION,
            ControlMask: PROCESS_POWER_THROTTLING_EXECUTION_SPEED,
            StateMask: 0,
        };
        // SAFETY: `state` outlives the call; the handle is the emulator process the launcher owns.
        let r = unsafe {
            SetProcessInformation(
                to_handle(handle),
                ProcessPowerThrottling,
                &state as *const _ as *const c_void,
                std::mem::size_of::<PROCESS_POWER_THROTTLING_STATE>() as u32,
            )
        };
        if let Err(e) = r {
            log::warn!("Game Booster: could not turn off power throttling: {e}");
        }
    }

    // ---- The GPU preference (HKCU, per-user, no admin) ---------------------------------------------

    fn gpu_pref_key(write: bool) -> Option<winreg::RegKey> {
        let hkcu = winreg::RegKey::predef(winreg::enums::HKEY_CURRENT_USER);
        if write {
            hkcu.create_subkey(GPU_PREF_KEY).ok().map(|(k, _)| k)
        } else {
            hkcu.open_subkey(GPU_PREF_KEY).ok()
        }
    }

    pub fn read_gpu_pref(exe: &str) -> Option<String> {
        gpu_pref_key(false)?.get_value::<String, _>(exe).ok()
    }

    pub fn write_gpu_pref(exe: &str, value: &str) -> CmdResult<()> {
        gpu_pref_key(true)
            .ok_or_else(|| "no UserGpuPreferences key".to_string())?
            .set_value(exe, &value.to_string())
            .map_err(|e| e.to_string())
    }

    pub fn delete_gpu_pref(exe: &str) {
        if let Some(key) = gpu_pref_key(true) {
            let _ = key.delete_value(exe);
        }
    }

    // ---- Power schemes (per-user, no admin to switch) ----------------------------------------------

    /// The best high-performance scheme installed: Ultimate Performance, else High performance, else
    /// none.
    pub fn best_scheme_target() -> SchemeTarget {
        let schemes = installed_schemes();
        let has = |guid: &str| parse_guid(guid).map(|g| schemes.iter().any(|s| *s == g)).unwrap_or(false);
        if has(ULTIMATE_PERF_GUID) {
            SchemeTarget::Ultimate
        } else if has(HIGH_PERF_GUID) {
            SchemeTarget::High
        } else {
            SchemeTarget::None
        }
    }

    fn installed_schemes() -> Vec<GUID> {
        let mut out = Vec::new();
        let mut index = 0u32;
        loop {
            let mut buf = [0u8; std::mem::size_of::<GUID>()];
            let mut size = buf.len() as u32;
            // SAFETY: ACCESS_SCHEME writes one scheme GUID into `buf`; `size` holds its length. The root
            // key is reserved and must be null, which windows 0.62 spells None.
            let r = unsafe { PowerEnumerate(None, None, None, ACCESS_SCHEME, index, Some(buf.as_mut_ptr()), &mut size) };
            if r.0 != 0 {
                break; // ERROR_NO_MORE_ITEMS past the last scheme
            }
            // SAFETY: `buf` is exactly a GUID written by PowerEnumerate.
            out.push(unsafe { (buf.as_ptr() as *const GUID).read_unaligned() });
            index += 1;
            if index > 64 {
                break;
            }
        }
        out
    }

    pub fn active_scheme_guid() -> Option<String> {
        // SAFETY: PowerGetActiveScheme allocates a GUID we free with LocalFree.
        unsafe {
            let mut ptr: *mut GUID = std::ptr::null_mut();
            if PowerGetActiveScheme(None, &mut ptr).0 != 0 || ptr.is_null() {
                return None;
            }
            let guid = *ptr;
            let _ = windows::Win32::Foundation::LocalFree(Some(HLOCAL(ptr as *mut c_void)));
            Some(guid_text(&guid))
        }
    }

    pub fn set_active_scheme(guid: &str) -> CmdResult<()> {
        let parsed = parse_guid(guid).ok_or_else(|| format!("not a scheme guid: {guid}"))?;
        // Only switch to a scheme that is actually installed.
        if !installed_schemes().iter().any(|s| *s == parsed) {
            return Err(format!("power plan {guid} is not installed"));
        }
        // SAFETY: `parsed` outlives the call; switching the active scheme is a per-user operation.
        let r = unsafe { PowerSetActiveScheme(None, Some(&parsed as *const GUID)) };
        if r.0 == 0 {
            Ok(())
        } else {
            Err(format!("PowerSetActiveScheme failed with {}", r.0))
        }
    }

    // ---- Services (query needs no admin; stop/start do) --------------------------------------------

    fn open_scm(access: u32) -> Option<SC_HANDLE> {
        // SAFETY: opens the service control manager; the handle is closed by the caller.
        unsafe { OpenSCManagerW(PCWSTR::null(), PCWSTR::null(), access).ok() }
    }

    fn open_service(scm: SC_HANDLE, name: &str, access: u32) -> Option<SC_HANDLE> {
        let wname = wide(name);
        // SAFETY: `wname` is a valid null-terminated string for the length of the call.
        unsafe { OpenServiceW(scm, PCWSTR(wname.as_ptr()), access).ok() }
    }

    fn query_state(service: SC_HANDLE) -> Option<u32> {
        let mut status = SERVICE_STATUS::default();
        // SAFETY: `status` is a valid out-parameter.
        unsafe { QueryServiceStatus(service, &mut status).ok()? };
        Some(status.dwCurrentState.0)
    }

    pub fn service_running(name: &str) -> Option<bool> {
        let scm = open_scm(SC_MANAGER_CONNECT)?;
        let result = (|| {
            let service = open_service(scm, name, SERVICE_QUERY_STATUS)?;
            let state = query_state(service);
            // SAFETY: both handles are valid and closed exactly once.
            unsafe { let _ = CloseServiceHandle(service); };
            state.map(|s| s == SERVICE_RUNNING.0)
        })();
        unsafe { let _ = CloseServiceHandle(scm); };
        result
    }

    /// Stops `name` if it is running. Returns whether it was running (so it can be restarted).
    fn stop_service(scm: SC_HANDLE, name: &str) -> bool {
        let Some(service) = open_service(scm, name, SERVICE_QUERY_STATUS | SERVICE_STOP) else {
            return false;
        };
        let was_running = query_state(service) == Some(SERVICE_RUNNING.0);
        if was_running {
            let mut status = SERVICE_STATUS::default();
            // SAFETY: valid service handle and out-parameter.
            let _ = unsafe { ControlService(service, SERVICE_CONTROL_STOP, &mut status) };
        }
        // SAFETY: the handle is valid and closed once.
        unsafe { let _ = CloseServiceHandle(service); };
        was_running
    }

    fn start_service(scm: SC_HANDLE, name: &str) {
        let Some(service) = open_service(scm, name, SERVICE_START | SERVICE_QUERY_STATUS) else {
            return;
        };
        // A stop started just before the game exited can still be pending; StartServiceW fails then,
        // so wait a few seconds for it to reach STOPPED before starting it.
        for _ in 0..25 {
            match query_state(service) {
                Some(s) if s == SERVICE_STOPPED.0 => {
                    // SAFETY: valid service handle; no start arguments.
                    let _ = unsafe { StartServiceW(service, None) };
                    break;
                }
                Some(s) if s == SERVICE_STOP_PENDING.0 => std::thread::sleep(std::time::Duration::from_millis(200)),
                _ => break, // already running, or not in a state we restart
            }
        }
        // SAFETY: the handle is valid and closed once.
        unsafe {
            let _ = CloseServiceHandle(service);
        }
    }

    // ---- Process timing and waiting ----------------------------------------------------------------

    pub fn process_started(pid: u32) -> Option<u64> {
        // SAFETY: opens the process for a timing query and closes the handle.
        unsafe {
            let handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, pid).ok()?;
            let started = creation_time(handle);
            let _ = CloseHandle(handle);
            started
        }
    }

    unsafe fn creation_time(handle: HANDLE) -> Option<u64> {
        let mut creation = FILETIME::default();
        let mut exit = FILETIME::default();
        let mut kernel = FILETIME::default();
        let mut user = FILETIME::default();
        GetProcessTimes(handle, &mut creation, &mut exit, &mut kernel, &mut user).ok()?;
        Some(((creation.dwHighDateTime as u64) << 32) | creation.dwLowDateTime as u64)
    }

    /// Whether the process with this pid is still the same one (same pid and creation time), so a
    /// stale journal with a reused pid is never mistaken for a live session.
    pub fn process_matches(pid: u32, started: u64) -> bool {
        if pid == 0 || started == 0 {
            return false;
        }
        // SAFETY: opens the process (if any) for a timing query and closes it.
        unsafe {
            let Ok(handle) = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, pid) else {
                return false;
            };
            let same = creation_time(handle) == Some(started);
            let _ = CloseHandle(handle);
            same
        }
    }

    // ---- The elevated helper -----------------------------------------------------------------------

    /// Starts the launcher again, elevated (`ShellExecuteEx` "runas"), with `helper_args` after the
    /// flag. Returns once the UAC prompt is answered: Ok if allowed, Declined if the user said no.
    fn run_elevated(helper_args: &str) -> Result<(), HelperError> {
        let exe = std::env::current_exe().map_err(|e| HelperError::Failed(e.to_string()))?;
        let params = format!("{} {}", super::HELPER_FLAG, helper_args);
        let file = wide(&exe.to_string_lossy());
        let verb = wide("runas");
        let args = wide(&params);
        // SAFETY: COM init for ShellExecuteEx; every pointer outlives the call.
        unsafe {
            let co = CoInitializeEx(None, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            let mut info = SHELLEXECUTEINFOW {
                cbSize: std::mem::size_of::<SHELLEXECUTEINFOW>() as u32,
                fMask: SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI,
                lpVerb: PCWSTR(verb.as_ptr()),
                lpFile: PCWSTR(file.as_ptr()),
                lpParameters: PCWSTR(args.as_ptr()),
                nShow: 0, // SW_HIDE: the helper has no window
                ..Default::default()
            };
            let result = ShellExecuteExW(&mut info);
            if co.is_ok() {
                CoUninitialize();
            }
            match result {
                Ok(()) => {
                    if !info.hProcess.is_invalid() {
                        let _ = CloseHandle(info.hProcess);
                    }
                    Ok(())
                }
                Err(e) if e.code() == HRESULT::from_win32(ERROR_CANCELLED.0) => Err(HelperError::Declined),
                Err(e) => Err(HelperError::Failed(e.to_string())),
            }
        }
    }

    /// Starts the session helper: it switches the power plan and pauses the services, then waits on
    /// the emulator (identified by pid and creation time) and restores them.
    pub fn launch_helper(level: u8, scheme_guid: &str, emulator_pid: u32, emulator_started: u64) -> Result<(), HelperError> {
        run_elevated(&format!("{level} {scheme_guid} {emulator_pid} {emulator_started}"))
    }

    /// Starts the restore-only helper for the given services (for "Undo all").
    pub fn launch_restore_helper(names: &[&str]) -> Result<(), HelperError> {
        run_elevated(&format!("restore {}", names.join(" ")))
    }

    /// Restore-only helper body: restarts the named services, but only those on the booster's fixed
    /// list (so the elevated process never acts on a name it was handed outside that list).
    pub fn restore_services(names: &[String]) -> i32 {
        if let Some(scm) = open_scm(SC_MANAGER_CONNECT) {
            for (name, _) in SERVICES {
                if names.iter().any(|n| n.eq_ignore_ascii_case(name)) {
                    start_service(scm, name);
                }
            }
            // SAFETY: valid handle closed once.
            unsafe {
                let _ = CloseServiceHandle(scm);
            }
        }
        0
    }

    /// The elevated helper body: confirm the emulator the launcher boosted is still the running
    /// process, then switch the power plan and pause the services, wait for that process to exit,
    /// and restore both. Reads nothing user-writable. Returns a process exit code.
    pub fn helper_main(level: u8, scheme_arg: &str, emulator_pid: u32, emulator_started: u64) -> i32 {
        // Open the emulator and check it is the one we were told about and still alive, before
        // changing anything: the prompt may have been answered after the game exited or the pid
        // reused. Then wait on this same handle, so there is no second pid lookup to race.
        let access = PROCESS_ACCESS_RIGHTS(PROCESS_SYNCHRONIZE.0 | PROCESS_QUERY_LIMITED_INFORMATION.0);
        // SAFETY: opens the emulator process; the handle is closed before returning.
        let handle = match unsafe { OpenProcess(access, false, emulator_pid) } {
            Ok(handle) => handle,
            Err(_) => return 1,
        };
        // SAFETY: `handle` is valid here.
        if emulator_started != 0 && unsafe { creation_time(handle) } != Some(emulator_started) {
            // SAFETY: closing the valid handle once.
            unsafe {
                let _ = CloseHandle(handle);
            }
            return 1;
        }

        let mut previous_scheme: Option<String> = None;
        let mut stopped: Vec<&'static str> = Vec::new();

        // Switch the power plan (Level 2+), only to one that is installed.
        if scheme_arg != "none" && parse_guid(scheme_arg).is_some() {
            if let Some(prev) = active_scheme_guid() {
                if prev != scheme_arg && set_active_scheme(scheme_arg).is_ok() {
                    previous_scheme = Some(prev);
                }
            }
        }

        // Pause the services (Level 3), only those running.
        if level >= 3 {
            if let Some(scm) = open_scm(SC_MANAGER_CONNECT) {
                for (name, _) in SERVICES {
                    if stop_service(scm, name) {
                        stopped.push(name);
                    }
                }
                // SAFETY: the SCM handle is valid and closed once.
                unsafe {
                    let _ = CloseServiceHandle(scm);
                }
            }
        }

        // SAFETY: wait on the verified handle until the emulator exits, then close it.
        unsafe {
            let _ = WaitForSingleObject(handle, u32::MAX);
            let _ = CloseHandle(handle);
        }

        // Restore the power plan if ours is still active, and restart the services we stopped.
        if let Some(prev) = &previous_scheme {
            if active_scheme_guid().as_deref() == Some(scheme_arg) {
                let _ = set_active_scheme(prev);
            }
        }
        if !stopped.is_empty() {
            if let Some(scm) = open_scm(SC_MANAGER_CONNECT) {
                for name in stopped {
                    start_service(scm, name);
                }
                // SAFETY: valid handle closed once.
                unsafe {
                    let _ = CloseServiceHandle(scm);
                }
            }
        }
        0
    }

    pub fn wait_for_exit(pid: u32) {
        // SAFETY: opens the emulator process for SYNCHRONIZE and waits; closes the handle after.
        unsafe {
            let Ok(handle) = OpenProcess(PROCESS_SYNCHRONIZE, false, pid) else {
                return; // already gone
            };
            let _ = WaitForSingleObject(handle, u32::MAX); // INFINITE
            let _ = CloseHandle(handle);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn facts(scheme: SchemeTarget, sysmain: bool, wsearch: bool) -> Facts {
        Facts {
            scheme,
            scheme_active: false,
            services: vec![
                ServiceFact { name: "SysMain".into(), display: "SysMain".into(), running: sysmain },
                ServiceFact { name: "WSearch".into(), display: "Windows Search".into(), running: wsearch },
            ],
        }
    }

    #[test]
    fn off_changes_nothing() {
        let info = level_info(0, &facts(SchemeTarget::High, true, true));
        assert!(info.changes.is_empty());
        assert!(!info.elevated);
    }

    #[test]
    fn level_1_is_three_no_admin_changes() {
        let info = level_info(1, &facts(SchemeTarget::Ultimate, true, true));
        assert!(!info.elevated);
        assert_eq!(info.changes.len(), 3);
        assert!(info.changes[0].contains("Above Normal"));
        assert!(info.changes[1].contains("EcoQoS"));
        assert!(info.changes[2].contains("guarana.exe"));
    }

    #[test]
    fn level_2_adds_the_scheme_and_prefers_ultimate() {
        let ultimate = level_info(2, &facts(SchemeTarget::Ultimate, false, false));
        assert!(ultimate.elevated);
        assert_eq!(ultimate.changes.len(), 4);
        assert!(ultimate.changes[3].contains("Ultimate Performance"), "{}", ultimate.changes[3]);
        assert!(ultimate.changes[0].contains("Above Normal"));

        let high = level_info(2, &facts(SchemeTarget::High, false, false));
        assert!(high.changes[3].contains("High performance"), "{}", high.changes[3]);
    }

    #[test]
    fn level_2_without_a_scheme_needs_no_admin_and_says_so() {
        let info = level_info(2, &facts(SchemeTarget::None, false, false));
        assert!(!info.elevated);
        assert!(info.changes[3].contains("none is installed"), "{}", info.changes[3]);
        assert!(info.summary.contains("no high-performance power plan"));
    }

    #[test]
    fn level_3_upgrades_priority_and_lists_running_services() {
        let info = level_info(3, &facts(SchemeTarget::High, true, false));
        assert!(info.elevated);
        assert!(info.changes[0].contains("High priority"), "{}", info.changes[0]);
        let services = info.changes.last().unwrap();
        assert!(services.contains("SysMain") && services.contains("restart it"), "{services}");
        assert!(!services.contains("Windows Search"));
    }

    #[test]
    fn level_3_with_both_services_running() {
        let info = level_info(3, &facts(SchemeTarget::Ultimate, true, true));
        let services = info.changes.last().unwrap();
        assert!(services.contains("SysMain and Windows Search") && services.contains("restart them"), "{services}");
    }

    #[test]
    fn level_3_with_nothing_to_do_needs_no_admin() {
        // No high-performance plan and neither service running: nothing to elevate, so no prompt.
        let info = level_info(3, &facts(SchemeTarget::None, false, false));
        assert!(!info.elevated);
        assert!(info.changes.last().unwrap().contains("neither is running"), "{:?}", info.changes);
        // One running service brings the prompt back.
        assert!(level_info(3, &facts(SchemeTarget::None, true, false)).elevated);
    }

    #[test]
    fn a_scheme_already_active_needs_no_switch_and_no_admin() {
        let mut f = facts(SchemeTarget::Ultimate, false, false);
        f.scheme_active = true;
        let info = level_info(2, &f);
        assert!(!info.elevated);
        assert!(info.changes[3].contains("already the active one"), "{}", info.changes[3]);
        assert!(info.summary.contains("already active"), "{}", info.summary);
        // Level 3 with the plan already active still needs admin if a service is running.
        let mut f3 = facts(SchemeTarget::Ultimate, true, false);
        f3.scheme_active = true;
        assert!(needs_elevation(3, &f3));
    }

    #[test]
    fn summary_names_exactly_what_the_prompt_does() {
        // Level 2 with a plan to switch: the summary names the plan, as the prompt will.
        let s2 = level_info(2, &facts(SchemeTarget::High, false, false)).summary;
        assert!(s2.contains("one Windows permission prompt to switch the power plan to High performance"), "{s2}");
        // Level 3 with a plan and SysMain running: both named, Windows Search (not running) not.
        let s3 = level_info(3, &facts(SchemeTarget::High, true, false)).summary;
        assert!(s3.contains("switch the power plan to High performance") && s3.contains("pause SysMain"), "{s3}");
        assert!(!s3.contains("Windows Search"), "{s3}");
        // Level 3, no plan but both services running: only the services are named.
        let s3b = level_info(3, &facts(SchemeTarget::None, true, true)).summary;
        assert!(s3b.contains("pause SysMain and Windows Search") && !s3b.contains("power plan to"), "{s3b}");
        // Nothing needs admin: the summary says there is no prompt.
        let none = level_info(3, &facts(SchemeTarget::None, false, false)).summary;
        assert!(none.contains("No Windows prompt here"), "{none}");
    }

    #[test]
    fn all_levels_returns_off_through_three() {
        let levels = all_levels(&facts(SchemeTarget::High, true, true));
        assert_eq!(levels.len(), 4);
        assert_eq!(levels.iter().map(|l| l.level).collect::<Vec<_>>(), vec![0, 1, 2, 3]);
        assert_eq!(levels[0].title, "Off");
    }

    #[test]
    fn needs_elevation_matches_the_plan() {
        assert!(!needs_elevation(1, &facts(SchemeTarget::Ultimate, true, true)));
        assert!(needs_elevation(2, &facts(SchemeTarget::High, false, false)));
        assert!(!needs_elevation(2, &facts(SchemeTarget::None, false, false)));
        // Level 3 with nothing installed and nothing running needs no admin; a running service does.
        assert!(!needs_elevation(3, &facts(SchemeTarget::None, false, false)));
        assert!(needs_elevation(3, &facts(SchemeTarget::None, true, false)));
    }

    // ---- GPU preference merge ----------------------------------------------------------------------

    #[test]
    fn gpu_pref_applied_to_an_absent_value() {
        let a = gpu_pref_apply(None);
        assert_eq!(a.previous, None);
        assert_eq!(a.wrote, "GpuPreference=2;");
    }

    #[test]
    fn gpu_pref_keeps_other_tokens() {
        let a = gpu_pref_apply(Some("GpuPreference=0;AutoHDREnable=1;"));
        assert_eq!(a.previous.as_deref(), Some("0"));
        assert_eq!(a.wrote, "GpuPreference=2;AutoHDREnable=1;");
    }

    #[test]
    fn gpu_pref_restore_puts_the_previous_token_back() {
        // Applied over a value that had GpuPreference=0 and another token.
        let restore = gpu_pref_restore(Some("GpuPreference=2;AutoHDREnable=1;"), Some("0"));
        assert_eq!(restore, GpuPrefRestore::Write("GpuPreference=0;AutoHDREnable=1;".to_string()));
    }

    #[test]
    fn gpu_pref_restore_deletes_a_value_we_created() {
        let restore = gpu_pref_restore(Some("GpuPreference=2;"), None);
        assert_eq!(restore, GpuPrefRestore::Delete);
    }

    #[test]
    fn gpu_pref_restore_keeps_other_tokens_when_removing_ours() {
        let restore = gpu_pref_restore(Some("GpuPreference=2;AutoHDREnable=1;"), None);
        assert_eq!(restore, GpuPrefRestore::Write("AutoHDREnable=1;".to_string()));
    }

    #[test]
    fn gpu_pref_restore_leaves_a_value_the_user_changed() {
        // The user set it to 1 after we boosted: not ours any more, so leave it.
        assert_eq!(gpu_pref_restore(Some("GpuPreference=1;"), Some("0")), GpuPrefRestore::Leave);
        assert_eq!(gpu_pref_restore(None, Some("0")), GpuPrefRestore::Leave);
    }

    // ---- Journal format ----------------------------------------------------------------------------

    #[test]
    fn journal_round_trips() {
        let journal = Journal {
            version: JOURNAL_VERSION,
            active: true,
            level: 3,
            owner_pid: 4321,
            owner_started: 133_000_000_000_000_000,
            emulator_pid: 9876,
            emulator_started: 133_000_000_000_000_042,
            entries: vec![
                JournalEntry::Priority { class: "High".into() },
                JournalEntry::EcoQos,
                JournalEntry::GpuPreference { exe: r"C:\g\guarana.exe".into(), previous: Some("0".into()), wrote: "GpuPreference=2;".into() },
                JournalEntry::PowerScheme { previous: HIGH_PERF_GUID.into(), target: ULTIMATE_PERF_GUID.into() },
                JournalEntry::Service { name: "SysMain".into(), was_running: true },
            ],
        };
        let json = serde_json::to_string(&journal).unwrap();
        let back: Journal = serde_json::from_str(&json).unwrap();
        assert_eq!(back.version, JOURNAL_VERSION);
        assert!(back.active);
        assert_eq!(back.level, 3);
        assert_eq!(back.emulator_pid, 9876);
        assert_eq!(back.emulator_started, 133_000_000_000_000_042);
        assert_eq!(back.entries, journal.entries);
    }

    #[test]
    fn journal_entries_are_tagged_by_kind() {
        let json = serde_json::to_string(&JournalEntry::Service { name: "WSearch".into(), was_running: false }).unwrap();
        assert!(json.contains("\"kind\":\"service\""), "{json}");
        let gpu = serde_json::to_string(&JournalEntry::GpuPreference { exe: "x".into(), previous: None, wrote: "GpuPreference=2;".into() }).unwrap();
        assert!(gpu.contains("\"kind\":\"gpu_preference\""), "{gpu}");
    }

    #[test]
    fn default_journal_is_inactive() {
        let j = Journal::default();
        assert!(!j.active);
        assert_eq!(j.version, JOURNAL_VERSION);
        assert!(j.entries.is_empty());
    }
}
