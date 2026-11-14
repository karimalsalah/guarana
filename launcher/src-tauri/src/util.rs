//! Small helpers: time stamps, atomic writes, backups, process checks, opening pages.

use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

/// `Result<T, String>`: what every command returns; Tauri hands the string to the UI.
pub type CmdResult<T> = Result<T, String>;

pub trait Context<T> {
    fn ctx(self, what: &str) -> CmdResult<T>;
}

impl<T, E: std::fmt::Display> Context<T> for Result<T, E> {
    fn ctx(self, what: &str) -> CmdResult<T> {
        self.map_err(|e| format!("{what}: {e}"))
    }
}

/// Days since 1970-01-01 to (year, month, day), proleptic Gregorian (H. Hinnant's algorithm).
fn civil_from_days(days: i64) -> (i64, u32, u32) {
    let z = days + 719_468;
    let era = if z >= 0 { z } else { z - 146_096 } / 146_097;
    let doe = z - era * 146_097;
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let y = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = (doy - (153 * mp + 2) / 5 + 1) as u32;
    let m = if mp < 10 { mp + 3 } else { mp - 9 } as u32;
    (if m <= 2 { y + 1 } else { y }, m, d)
}

fn utc_parts(t: SystemTime) -> (i64, u32, u32, u32, u32, u32) {
    let secs = t.duration_since(UNIX_EPOCH).map(|d| d.as_secs() as i64).unwrap_or(0);
    let (y, mo, d) = civil_from_days(secs.div_euclid(86_400));
    let rem = secs.rem_euclid(86_400);
    (y, mo, d, (rem / 3600) as u32, ((rem % 3600) / 60) as u32, (rem % 60) as u32)
}

/// RFC 3339 in UTC, e.g. `2026-11-13T09:41:07Z`.
pub fn rfc3339(t: SystemTime) -> String {
    let (y, mo, d, h, mi, s) = utc_parts(t);
    format!("{y:04}-{mo:02}-{d:02}T{h:02}:{mi:02}:{s:02}Z")
}

pub fn now_rfc3339() -> String {
    rfc3339(SystemTime::now())
}

/// `20261113-094107`, for file names.
pub fn stamp() -> String {
    let (y, mo, d, h, mi, s) = utc_parts(SystemTime::now());
    format!("{y:04}{mo:02}{d:02}-{h:02}{mi:02}{s:02}")
}

/// Writes through a temporary file in the same folder and renames it over `path`, so a crash
/// never leaves a half-written file.
pub fn write_atomic(path: &Path, bytes: &[u8]) -> std::io::Result<()> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)?;
    }
    let mut tmp = path.as_os_str().to_owned();
    tmp.push(format!(".guarana-{}.tmp", std::process::id()));
    let tmp = PathBuf::from(tmp);
    {
        let mut f = fs::File::create(&tmp)?;
        f.write_all(bytes)?;
        f.sync_all()?;
    }
    fs::rename(&tmp, path).inspect_err(|_| {
        let _ = fs::remove_file(&tmp);
    })
}

/// Copies `path` to `<dir>\guarana-backups\<name>.<stamp>.bak` and keeps the newest ten copies
/// of that file. Returns the backup's path; `None` when there was nothing to back up.
pub fn backup_file(path: &Path) -> std::io::Result<Option<PathBuf>> {
    if !path.is_file() {
        return Ok(None);
    }
    let dir = path.parent().unwrap_or(Path::new(".")).join("guarana-backups");
    fs::create_dir_all(&dir)?;
    let name = path.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
    let dest = dir.join(format!("{name}.{}.bak", stamp()));
    fs::copy(path, &dest)?;
    let prefix = format!("{name}.");
    let mut copies: Vec<PathBuf> = fs::read_dir(&dir)?
        .flatten()
        .map(|e| e.path())
        .filter(|p| {
            p.file_name()
                .map(|n| {
                    let n = n.to_string_lossy();
                    n.starts_with(&prefix) && n.ends_with(".bak")
                })
                .unwrap_or(false)
        })
        .collect();
    copies.sort();
    while copies.len() > 10 {
        let _ = fs::remove_file(copies.remove(0));
    }
    Ok(Some(dest))
}

/// Keeps `[A-Za-z0-9._+-]` so a version label is always one safe folder name.
pub fn sanitize_component(s: &str) -> String {
    let out: String = s
        .chars()
        .map(|c| if c.is_ascii_alphanumeric() || matches!(c, '.' | '_' | '-' | '+') { c } else { '_' })
        .collect();
    let out = out.trim_matches('.').to_string();
    if out.is_empty() {
        "unknown".to_string()
    } else {
        out
    }
}

pub fn gib(bytes: u64) -> f64 {
    (bytes as f64 / (1u64 << 30) as f64 * 10.0).round() / 10.0
}

pub fn human_bytes(bytes: u64) -> String {
    const UNITS: [&str; 5] = ["B", "KB", "MB", "GB", "TB"];
    let mut v = bytes as f64;
    let mut i = 0;
    while v >= 1024.0 && i < UNITS.len() - 1 {
        v /= 1024.0;
        i += 1;
    }
    if i == 0 {
        format!("{bytes} B")
    } else {
        format!("{v:.1} {}", UNITS[i])
    }
}

/// Whether a process with one of these executable names (lowercase, with `.exe`) runs.
fn process_running(names: &[&str]) -> bool {
    let mut sys = sysinfo::System::new();
    sys.refresh_processes();
    sys.processes().values().any(|p| {
        let n = p.name().to_ascii_lowercase();
        names.iter().any(|want| n == *want || Some(n.as_str()) == want.strip_suffix(".exe"))
    })
}

/// The emulator (`guarana.exe`, or `lichii.exe` from before the rename; from any folder).
pub fn emulator_running() -> bool {
    process_running(&[crate::paths::EMULATOR_EXE, crate::paths::EMULATOR_EXE_LEGACY])
}

/// A shader warm-up (`guarana-cli.exe`, or `eden-cli.exe` from before the rename): it saves the
/// configuration and caches when it ends.
pub fn warm_up_running() -> bool {
    process_running(&[crate::paths::CLI_EXE, crate::paths::CLI_EXE_LEGACY])
}

/// Opens an `https://` page in the default browser. Anything else is refused.
pub fn open_https(url: &str) -> CmdResult<()> {
    let parsed = url::Url::parse(url).ctx("Not a valid URL")?;
    if parsed.scheme() != "https" || parsed.host_str().is_none() {
        return Err("Only https:// pages can be opened".into());
    }
    tauri_plugin_opener::open_url(parsed.as_str(), None::<&str>).ctx("Could not open the browser")
}

/// Opens one of the fixed Windows pages (`ms-settings:`, `windowsdefender:`) the doctor links to.
pub fn open_windows_page(uri: &str) -> CmdResult<()> {
    let allowed = uri.starts_with("ms-settings:") || uri.starts_with("windowsdefender:");
    if !allowed {
        return Err("Not a Windows settings page".into());
    }
    tauri_plugin_opener::open_url(uri, None::<&str>).ctx("Could not open Windows Settings")
}

/// Folders open in Explorer; files are shown selected in their folder (never run).
pub fn open_in_explorer(path: &Path) -> CmdResult<()> {
    let meta = fs::metadata(path).ctx("Not found")?;
    if meta.is_dir() {
        tauri_plugin_opener::open_path(path, None::<&str>).ctx("Could not open Explorer")
    } else {
        tauri_plugin_opener::reveal_item_in_dir(path).ctx("Could not open Explorer")
    }
}

/// Lowercase 16-digit hex title id, or `None` when `s` is not one.
pub fn normalize_title_id(s: &str) -> Option<String> {
    let t = s.trim().trim_start_matches("0x").trim_start_matches("0X");
    (t.len() == 16 && t.chars().all(|c| c.is_ascii_hexdigit())).then(|| t.to_ascii_lowercase())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::Duration;

    #[test]
    fn rfc3339_known_dates() {
        assert_eq!(rfc3339(UNIX_EPOCH), "1970-01-01T00:00:00Z");
        // 2026-11-13T09:41:07Z
        assert_eq!(rfc3339(UNIX_EPOCH + Duration::from_secs(1_794_562_867)), "2026-11-13T09:41:07Z");
        // Leap day.
        assert_eq!(rfc3339(UNIX_EPOCH + Duration::from_secs(951_782_400)), "2000-02-29T00:00:00Z");
    }

    #[test]
    fn sanitize() {
        assert_eq!(sanitize_component("20.0.2"), "20.0.2");
        assert_eq!(sanitize_component("nightly-20261113-abc1234"), "nightly-20261113-abc1234");
        assert_eq!(sanitize_component("../evil"), "_evil");
        assert_eq!(sanitize_component("a b:c"), "a_b_c");
    }

    #[test]
    fn title_ids() {
        assert_eq!(normalize_title_id("0400486024DEC000").as_deref(), Some("0400486024dec000"));
        assert_eq!(normalize_title_id("0x0400486024DEC000").as_deref(), Some("0400486024dec000"));
        assert_eq!(normalize_title_id("0400486024DEC00"), None);
    }
}
