//! The launcher's own state: `launcher-state.json` (installed versions, profile, game folders,
//! game list cache) and `current.json` (the version `launch_game` starts), both in
//! `%LOCALAPPDATA%\Guarana`.

use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::time::Instant;

use serde::{Deserialize, Serialize};

use crate::types::{Controller, Game, InstalledVersion, LauncherState, SystemInfo, ToggleValue};
use crate::{paths, util};

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct Persisted {
    #[serde(default)]
    pub installed: Vec<InstalledVersion>,
    #[serde(default)]
    pub profile_id: Option<String>,
    #[serde(default)]
    pub overrides: BTreeMap<String, ToggleValue>,
    #[serde(default)]
    pub game_folders: Vec<String>,
    #[serde(default)]
    pub games: Vec<Game>,
    /// `vendor:device:driver` of the primary GPU when every cached title was last warmed up.
    #[serde(default)]
    pub last_warm_driver: Option<String>,
    /// `auto`, `smooth`, `quality`, `battery`, or `custom` after a manual Apply in Advanced.
    /// Unset means `auto`.
    #[serde(default)]
    pub mode: Option<String>,
    #[serde(default)]
    pub autopilot: crate::autopilot::AutopilotState,
    /// "Share my numbers": the session Home offers to share (until Share or Not now).
    #[serde(default)]
    pub share_offer: Option<crate::share::ShareOffer>,
    /// True after "Don't ask again" / the setting turned off.
    #[serde(default)]
    pub share_prompt_off: bool,
    /// `mtime:len` of the last log session looked at, so one session is offered once.
    #[serde(default)]
    pub share_session_key: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct CurrentFile {
    pub version: String,
    pub channel: String,
    pub path: String,
    pub updated_at: String,
}

/// Shared by every command. The std mutex serializes read-modify-write of the state file; the
/// tokio mutexes keep installs (and the first-run bundled install) one at a time.
pub struct AppState {
    pub file_lock: Mutex<()>,
    pub install_lock: tokio::sync::Mutex<()>,
    pub bundled_checked: tokio::sync::Mutex<bool>,
    pub controllers: Arc<Mutex<Vec<Controller>>>,
    pub system_cache: Mutex<Option<(Instant, SystemInfo)>>,
    pub warm: Mutex<crate::warm::WarmState>,
}

impl AppState {
    pub fn new() -> Self {
        Self {
            file_lock: Mutex::new(()),
            install_lock: tokio::sync::Mutex::new(()),
            bundled_checked: tokio::sync::Mutex::new(false),
            controllers: Arc::new(Mutex::new(Vec::new())),
            system_cache: Mutex::new(None),
            warm: Mutex::new(crate::warm::WarmState::default()),
        }
    }

    /// `detect_system`, reused for 30 s (the doctor and the profiles ask for it together).
    pub fn system(&self) -> SystemInfo {
        if let Ok(cache) = self.system_cache.lock() {
            if let Some((at, info)) = cache.as_ref() {
                if at.elapsed().as_secs() < 30 {
                    return info.clone();
                }
            }
        }
        let info = crate::system::detect();
        if let Ok(mut cache) = self.system_cache.lock() {
            *cache = Some((Instant::now(), info.clone()));
        }
        info
    }

    pub fn load(&self) -> Persisted {
        let _guard = self.file_lock.lock().unwrap_or_else(|e| e.into_inner());
        load_unlocked()
    }

    /// Loads, applies `f`, saves. Returns what `f` returned.
    pub fn update<T>(&self, f: impl FnOnce(&mut Persisted) -> T) -> util::CmdResult<T> {
        let _guard = self.file_lock.lock().unwrap_or_else(|e| e.into_inner());
        let mut state = load_unlocked();
        let out = f(&mut state);
        save_unlocked(&state)?;
        Ok(out)
    }
}

fn load_unlocked() -> Persisted {
    let mut state: Persisted = fs::read(paths::state_file())
        .ok()
        .and_then(|bytes| serde_json::from_slice(&bytes).ok())
        .unwrap_or_default();
    reconcile_installed(&mut state);
    state
}

fn save_unlocked(state: &Persisted) -> util::CmdResult<()> {
    let bytes = serde_json::to_vec_pretty(state).map_err(|e| e.to_string())?;
    util::write_atomic(&paths::state_file(), &bytes)
        .map_err(|e| format!("Could not save the launcher state: {e}"))
}

/// The folder holding `guarana.exe` (`lichii.exe` in builds from before the rename) in a version
/// folder: the folder itself or its only subfolder.
pub fn emulator_dir_in(version_dir: &Path) -> Option<PathBuf> {
    if paths::emulator_exe_in(version_dir).is_some() {
        return Some(version_dir.to_path_buf());
    }
    let subdirs: Vec<PathBuf> = fs::read_dir(version_dir)
        .ok()?
        .flatten()
        .filter(|e| e.file_type().map(|t| t.is_dir()).unwrap_or(false))
        .map(|e| e.path())
        .collect();
    match subdirs.as_slice() {
        [only] if paths::emulator_exe_in(only).is_some() => Some(only.clone()),
        _ => None,
    }
}

/// Drops entries whose folder is gone and adopts version folders the state does not list (a
/// deleted or older state file), so `installed` always matches `versions\`.
fn reconcile_installed(state: &mut Persisted) {
    state.installed.retain(|v| paths::emulator_exe_in(Path::new(&v.path)).is_some());
    let Ok(entries) = fs::read_dir(paths::versions_dir()) else {
        return;
    };
    for entry in entries.flatten() {
        let name = entry.file_name().to_string_lossy().into_owned();
        if name.starts_with('.') || !entry.file_type().map(|t| t.is_dir()).unwrap_or(false) {
            continue;
        }
        if state.installed.iter().any(|v| v.version == name) {
            continue;
        }
        let Some(dir) = emulator_dir_in(&entry.path()) else {
            continue;
        };
        let installed_at = entry
            .metadata()
            .and_then(|m| m.modified())
            .map(util::rfc3339)
            .unwrap_or_else(|_| util::now_rfc3339());
        let channel = if name.starts_with("nightly") { "nightly" } else { "stable" };
        state.installed.push(InstalledVersion {
            version: name,
            channel: channel.to_string(),
            path: dir.to_string_lossy().into_owned(),
            installed_at,
        });
    }
    state.installed.sort_by(|a, b| b.installed_at.cmp(&a.installed_at));
}

pub fn read_current() -> Option<CurrentFile> {
    let current: CurrentFile = serde_json::from_slice(&fs::read(paths::current_file()).ok()?).ok()?;
    paths::emulator_exe_in(Path::new(&current.path)).is_some().then_some(current)
}

pub fn write_current(v: &InstalledVersion) -> util::CmdResult<()> {
    let current = CurrentFile {
        version: v.version.clone(),
        channel: v.channel.clone(),
        path: v.path.clone(),
        updated_at: util::now_rfc3339(),
    };
    let bytes = serde_json::to_vec_pretty(&current).map_err(|e| e.to_string())?;
    util::write_atomic(&paths::current_file(), &bytes)
        .map_err(|e| format!("Could not save current.json: {e}"))
}

/// The current version, falling back to the newest installed one when `current.json` is missing
/// or points at a folder that is gone.
pub fn current_version(state: &Persisted) -> Option<InstalledVersion> {
    if let Some(current) = read_current() {
        if let Some(v) = state.installed.iter().find(|v| v.version == current.version) {
            return Some(v.clone());
        }
        return Some(InstalledVersion {
            version: current.version,
            channel: current.channel,
            path: current.path,
            installed_at: current.updated_at,
        });
    }
    state.installed.first().cloned()
}

pub fn keys_present() -> bool {
    paths::keys_dir().join("prod.keys").is_file()
}

pub fn firmware_present() -> bool {
    fs::read_dir(paths::firmware_registered_dir())
        .map(|entries| {
            entries.flatten().any(|e| e.file_name().to_string_lossy().to_ascii_lowercase().ends_with(".nca"))
        })
        .unwrap_or(false)
}

pub fn snapshot(state: &Persisted) -> LauncherState {
    let current = current_version(state);
    LauncherState {
        install_dir: paths::install_dir().to_string_lossy().into_owned(),
        user_data_dir: paths::user_data_dir().to_string_lossy().into_owned(),
        installed: state.installed.clone(),
        current: current.as_ref().map(|v| v.version.clone()),
        profile_id: state.profile_id.clone(),
        first_run: current.is_none() || state.profile_id.is_none(),
        keys_present: keys_present(),
        firmware_present: firmware_present(),
        game_folders: state.game_folders.clone(),
    }
}
