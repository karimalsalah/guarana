//! Every folder the launcher reads or writes. All per-user, nothing needs admin rights.
//!
//! - `%LOCALAPPDATA%\Guarana`: the launcher's own folder (`versions\<version>\`, `current.json`,
//!   `launcher-state.json`, `launcher.log`, `downloads\`, `reports\`).
//! - `%APPDATA%\guarana`: the emulator's user folder (keys, config, caches, logs). The launcher
//!   never moves it; it writes there only on Import and Apply profile, and deletes only stale
//!   shader-cache temp files when the user asks the doctor to. The emulator copies `%APPDATA%\lichii`
//!   there on its first start; until then (and for an emulator from before the rename) the launcher
//!   keeps using `%APPDATA%\lichii`, see `user_data_dir`.

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

/// The last game session's emulator log, kept before a shader warm-up (guarana-cli writes and
/// rotates the same `eden_log.txt`), so a later report still has it.
pub fn kept_game_log() -> PathBuf {
    install_dir().join("logs").join("eden_log.last-game.txt")
}

/// The emulator's user folder (`EDEN_DIR` in `src/common/fs/fs_paths.h`): `%APPDATA%\guarana`.
/// An emulator from before the rename reads only `%APPDATA%\lichii`, so this is `lichii` while the
/// current version has `lichii.exe` and no `guarana.exe`. Otherwise it is `guarana`, except while
/// only `lichii` exists: the emulator copies it to `guarana` on its first start (and leaves it in
/// place), but only while `guarana` does not exist yet, so the launcher never creates `guarana`
/// ahead of it.
pub fn user_data_dir() -> PathBuf {
    pick_user_dir(&roaming_appdata(), current_engine_is_legacy)
}

const USER_DIR: &str = "guarana";
const USER_DIR_LEGACY: &str = "lichii";

fn pick_user_dir(roaming: &Path, legacy_engine: impl FnOnce() -> bool) -> PathBuf {
    let new = roaming.join(USER_DIR);
    let old = roaming.join(USER_DIR_LEGACY);
    if legacy_engine() {
        old
    } else if new.is_dir() || !old.is_dir() {
        new
    } else {
        old
    }
}

/// Whether the current version is an emulator from before the rename (`lichii.exe` and no
/// `guarana.exe`), read from `current.json`.
fn current_engine_is_legacy() -> bool {
    let Ok(bytes) = std::fs::read(current_file()) else { return false };
    let Ok(value) = serde_json::from_slice::<serde_json::Value>(&bytes) else { return false };
    let Some(dir) = value.get("path").and_then(|p| p.as_str()) else { return false };
    let dir = Path::new(dir);
    dir.join(EMULATOR_EXE_LEGACY).is_file() && !dir.join(EMULATOR_EXE).is_file()
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
pub const EMULATOR_EXE: &str = "guarana.exe";
/// Its name in builds from before the rename.
pub const EMULATOR_EXE_LEGACY: &str = "lichii.exe";
/// The command-line frontend (shader warm-up) and its name before the rename.
pub const CLI_EXE: &str = "guarana-cli.exe";
pub const CLI_EXE_LEGACY: &str = "eden-cli.exe";

fn exe_in(dir: &Path, names: [&str; 2]) -> Option<PathBuf> {
    names.iter().map(|n| dir.join(n)).find(|p| p.is_file())
}

/// The emulator in `dir`: `guarana.exe`, or `lichii.exe` when only that exists.
pub fn emulator_exe_in(dir: &Path) -> Option<PathBuf> {
    exe_in(dir, [EMULATOR_EXE, EMULATOR_EXE_LEGACY])
}

/// The command-line frontend in `dir`: `guarana-cli.exe`, or `eden-cli.exe` when only that exists.
pub fn cli_exe_in(dir: &Path) -> Option<PathBuf> {
    exe_in(dir, [CLI_EXE, CLI_EXE_LEGACY])
}

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

#[cfg(test)]
mod tests {
    use super::*;

    fn scratch(tag: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("guarana-paths-test-{tag}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    #[test]
    fn user_dir_is_guarana_unless_only_the_old_folder_exists() {
        let roaming = scratch("user-dir");
        assert_eq!(pick_user_dir(&roaming, || false), roaming.join("guarana"));
        std::fs::create_dir_all(roaming.join("lichii")).unwrap();
        assert_eq!(pick_user_dir(&roaming, || false), roaming.join("lichii"));
        std::fs::create_dir_all(roaming.join("guarana")).unwrap();
        assert_eq!(pick_user_dir(&roaming, || false), roaming.join("guarana"));
        let _ = std::fs::remove_dir_all(&roaming);
    }

    #[test]
    fn user_dir_follows_an_emulator_from_before_the_rename() {
        let roaming = scratch("legacy-engine");
        assert_eq!(pick_user_dir(&roaming, || true), roaming.join("lichii"));
        // A renamed build was run once and left guarana behind; the old engine still reads lichii.
        std::fs::create_dir_all(roaming.join("guarana")).unwrap();
        assert_eq!(pick_user_dir(&roaming, || true), roaming.join("lichii"));
        let _ = std::fs::remove_dir_all(&roaming);
    }

    #[test]
    fn exe_lookup_prefers_the_new_names_and_falls_back() {
        let dir = scratch("exe");
        assert!(emulator_exe_in(&dir).is_none() && cli_exe_in(&dir).is_none());
        std::fs::write(dir.join("lichii.exe"), b"x").unwrap();
        std::fs::write(dir.join("eden-cli.exe"), b"x").unwrap();
        assert_eq!(emulator_exe_in(&dir), Some(dir.join("lichii.exe")));
        assert_eq!(cli_exe_in(&dir), Some(dir.join("eden-cli.exe")));
        std::fs::write(dir.join("guarana.exe"), b"x").unwrap();
        std::fs::write(dir.join("guarana-cli.exe"), b"x").unwrap();
        assert_eq!(emulator_exe_in(&dir), Some(dir.join("guarana.exe")));
        assert_eq!(cli_exe_in(&dir), Some(dir.join("guarana-cli.exe")));
        let _ = std::fs::remove_dir_all(&dir);
    }
}
