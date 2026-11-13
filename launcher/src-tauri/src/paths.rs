//! Every folder the launcher reads or writes. All per-user, nothing needs admin rights.
//!
//! - `%LOCALAPPDATA%\Guarana`: the launcher's own folder (`versions\<version>\`, `current.json`,
//!   `launcher-state.json`, `launcher.log`, `downloads\`, `reports\`).
//! - `%APPDATA%\lichii`: the emulator's user folder (keys, config, caches, logs). The launcher
//!   never moves it; it writes there only on Import and Apply profile, and deletes only stale
//!   shader-cache temp files when the user asks the doctor to.

use std::path::{Path, PathBuf};

fn env_dir(name: &str) -> Option<PathBuf> {
    std::env::var_os(name).map(PathBuf::from).filter(|p| p.is_absolute())
}

pub fn user_profile() -> Option<PathBuf> {
    env_dir("USERPROFILE")
}

pub fn local_appdata() -> PathBuf {
    env_dir("LOCALAPPDATA")
        .or_else(|| user_profile().map(|p| p.join("AppData").join("Local")))
        .unwrap_or_else(|| PathBuf::from(r"C:\Guarana-data"))
}

pub fn roaming_appdata() -> PathBuf {
    env_dir("APPDATA")
        .or_else(|| user_profile().map(|p| p.join("AppData").join("Roaming")))
        .unwrap_or_else(|| PathBuf::from(r"C:\Guarana-data"))
}

pub fn install_dir() -> PathBuf {
    local_appdata().join("Guarana")
}

pub fn versions_dir() -> PathBuf {
    install_dir().join("versions")
}

pub fn downloads_dir() -> PathBuf {
    install_dir().join("downloads")
}

pub fn reports_dir() -> PathBuf {
    install_dir().join("reports")
}

pub fn current_file() -> PathBuf {
    install_dir().join("current.json")
}

pub fn state_file() -> PathBuf {
    install_dir().join("launcher-state.json")
}

pub fn launcher_log() -> PathBuf {
    install_dir().join("launcher.log")
}

/// The last game session's emulator log, kept before a shader warm-up (eden-cli writes and
/// rotates the same `eden_log.txt`), so a later report still has it.
pub fn kept_game_log() -> PathBuf {
    install_dir().join("logs").join("eden_log.last-game.txt")
}

/// The emulator's user folder (`EDEN_DIR` in `src/common/fs/fs_paths.h`).
pub fn user_data_dir() -> PathBuf {
    roaming_appdata().join("lichii")
}

pub fn keys_dir() -> PathBuf {
    user_data_dir().join("keys")
}

pub fn config_dir() -> PathBuf {
    user_data_dir().join("config")
}

pub fn qt_config() -> PathBuf {
    config_dir().join("qt-config.ini")
}

pub fn user_switches() -> PathBuf {
    user_data_dir().join("nx2_switches.txt")
}

/// `<user>\cache\shader`; each game has a `{:016x}` (lowercase) folder in it.
pub fn shader_cache_dir() -> PathBuf {
    user_data_dir().join("cache").join("shader")
}

pub fn emulator_log_dir() -> PathBuf {
    user_data_dir().join("log")
}

/// Where installed firmware lives (`nand\system\Contents\registered\*.nca`).
pub fn firmware_registered_dir() -> PathBuf {
    user_data_dir().join("nand").join("system").join("Contents").join("registered")
}

/// Name of the emulator's executable inside a version folder.
pub const EMULATOR_EXE: &str = "lichii.exe";

/// Case-insensitive "is `path` inside `base`" on normalized components.
pub fn is_within(path: &Path, base: &Path) -> bool {
    let norm = |p: &Path| -> Vec<String> {
        p.components()
            .map(|c| c.as_os_str().to_string_lossy().to_lowercase())
            .filter(|c| c != "\\" && c != "/")
            .collect()
    };
    let p = norm(path);
    let b = norm(base);
    !b.is_empty() && p.len() >= b.len() && p[..b.len()] == b[..]
}

/// The launcher's small caches (the community best-settings file and its ETag).
pub fn cache_dir() -> PathBuf {
    install_dir().join("cache")
}
