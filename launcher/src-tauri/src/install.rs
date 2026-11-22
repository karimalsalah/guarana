//! Releases, updates and installs. Versions live side by side in
//! `%LOCALAPPDATA%\Guarana\versions\<version>\`; `current.json` names the one that runs, and it
//! runs from `%LOCALAPPDATA%\Guarana\app\`, the same path for every version (`appdir.rs`).
//!
//! A release asset is `guarana-<tag without v | nightly>-windows-x86_64-pgo.zip` (the plain
//! `-windows-x86_64.zip` when there is no PGO build), checked against the release's
//! `SHA256SUMS.txt` before anything is unpacked (`.github/workflows/release-binaries.yml`).

use std::collections::HashMap;
use std::fs;
use std::io::{BufReader, BufWriter, Read, Write};
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

use serde::de::DeserializeOwned;
use serde::Deserialize;
use sha2::{Digest, Sha256};
use tauri::{AppHandle, Emitter, Manager};

use crate::state::{self, AppState};
use crate::types::{InstallResult, InstalledVersion, LauncherState, Progress, UpdateInfo};
use crate::util::{self, CmdResult, Context};
use crate::{appdir, notify, paths, taskbar};

const API: &str = "https://api.github.com/repos/karimalsalah/guarana";
const DOWNLOAD_PREFIX: &str = "https://github.com/karimalsalah/guarana/releases/download/";
const SUMS_FILE: &str = "SHA256SUMS.txt";
/// Refuse archives that unpack to more than this (a damaged or hostile zip).
const MAX_UNPACKED: u64 = 8 << 30;
pub const EVENT: &str = "install://progress";

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Channel {
    Stable,
    Nightly,
}

impl Channel {
    pub fn parse(s: &str) -> CmdResult<Self> {
        match s.trim().to_ascii_lowercase().as_str() {
            "" | "stable" => Ok(Channel::Stable),
            "nightly" => Ok(Channel::Nightly),
            other => Err(format!("Unknown channel {other}: use stable or nightly")),
        }
    }

    pub fn as_str(self) -> &'static str {
        match self {
            Channel::Stable => "stable",
            Channel::Nightly => "nightly",
        }
    }
}

#[derive(Debug, Clone, Deserialize)]
pub struct GhRelease {
    pub tag_name: String,
    #[serde(default)]
    pub draft: bool,
    #[serde(default)]
    pub prerelease: bool,
    #[serde(default)]
    pub published_at: Option<String>,
    #[serde(default)]
    pub body: Option<String>,
    #[serde(default)]
    pub target_commitish: Option<String>,
    #[serde(default)]
    pub assets: Vec<GhAsset>,
}

#[derive(Debug, Clone, Deserialize)]
pub struct GhAsset {
    pub name: String,
    #[serde(default)]
    pub size: u64,
    pub browser_download_url: String,
    /// `sha256:<hex>`, which GitHub computes for assets uploaded since mid-2025.
    #[serde(default)]
    pub digest: Option<String>,
}

fn emit(app: &AppHandle, stage: &str, done: u64, total: u64, message: Option<String>) {
    match stage {
        "done" => taskbar::clear(app, taskbar::Job::Install),
        "download" | "extract" if total > 0 => {
            taskbar::set(app, taskbar::Job::Install, Some(done.saturating_mul(100) / total))
        }
        _ => taskbar::set(app, taskbar::Job::Install, None),
    }
    let _ = app.emit(EVENT, Progress::new(stage, done, total, message));
}

pub fn client() -> CmdResult<reqwest::Client> {
    reqwest::Client::builder()
        .user_agent(concat!("Guarana-Launcher/", env!("CARGO_PKG_VERSION")))
        .connect_timeout(Duration::from_secs(15))
        .read_timeout(Duration::from_secs(60))
        .build()
        .ctx("Could not start the HTTP client")
}

async fn get_bytes(client: &reqwest::Client, url: &str, limit: usize) -> CmdResult<Vec<u8>> {
    let resp = client
        .get(url)
        .header("Accept", "application/vnd.github+json")
        .header("X-GitHub-Api-Version", "2022-11-28")
        .send()
        .await
        .ctx("GitHub is not reachable")?;
    let status = resp.status();
    if status.as_u16() == 404 {
        return Err("Not found on GitHub".into());
    }
    if status.as_u16() == 403 || status.as_u16() == 429 {
        return Err("GitHub's rate limit for this network is used up; try again in an hour".into());
    }
    let mut resp = resp.error_for_status().ctx("GitHub refused the request")?;
    let mut out = Vec::new();
    while let Some(chunk) = resp.chunk().await.ctx("GitHub connection dropped")? {
        out.extend_from_slice(&chunk);
        if out.len() > limit {
            return Err("GitHub sent more than expected".into());
        }
    }
    Ok(out)
}

async fn get_json<T: DeserializeOwned>(client: &reqwest::Client, url: &str) -> CmdResult<T> {
    let bytes = tokio::time::timeout(Duration::from_secs(45), get_bytes(client, url, 16 << 20))
        .await
        .map_err(|_| "GitHub did not answer in time".to_string())??;
    serde_json::from_slice(&bytes).ctx("Unexpected answer from GitHub")
}

/// The version in asset names: the tag without its `v` (`nightly` for the nightly).
fn file_version(tag: &str) -> &str {
    tag.strip_prefix('v').unwrap_or(tag)
}

/// The version label the launcher shows and names folders after: `20.0.2`, or for the rolling
/// nightly `nightly-<YYYYMMDD>-<commit>` (the tag never changes, so it can't name a folder).
pub fn label(release: &GhRelease, channel: Channel) -> String {
    match channel {
        Channel::Stable => util::sanitize_component(file_version(&release.tag_name)),
        Channel::Nightly => {
            let date: String = release
                .published_at
                .as_deref()
                .map(|d| d.chars().take(10).filter(|c| c.is_ascii_digit()).collect())
                .unwrap_or_default();
            let sha: Option<String> = release
                .target_commitish
                .as_deref()
                .filter(|s| s.len() >= 7 && s.chars().all(|c| c.is_ascii_hexdigit()))
                .map(|s| s[..7].to_ascii_lowercase());
            let mut out = String::from("nightly");
            if date.len() == 8 {
                out.push('-');
                out.push_str(&date);
            }
            if let Some(sha) = sha {
                out.push('-');
                out.push_str(&sha);
            }
            out
        }
    }
}

/// The host's Windows architecture, not this process's: an x64 launcher running under emulation
/// on an ARM64 machine still wants the native ARM64 build. `PROCESSOR_ARCHITEW6432` names the
/// real architecture when a process runs emulated; `PROCESSOR_ARCHITECTURE` names it otherwise.
pub fn host_windows_arch() -> &'static str {
    arch_from_env(
        std::env::var("PROCESSOR_ARCHITECTURE").ok().as_deref(),
        std::env::var("PROCESSOR_ARCHITEW6432").ok().as_deref(),
    )
}

pub fn arch_from_env(architecture: Option<&str>, archite_w6432: Option<&str>) -> &'static str {
    for v in [archite_w6432, architecture] {
        if v.map(|s| s.eq_ignore_ascii_case("ARM64")).unwrap_or(false) {
            return "arm64";
        }
    }
    "x86_64"
}

pub fn pick_asset(release: &GhRelease) -> Option<&GhAsset> {
    pick_asset_for(release, host_windows_arch())
}

/// The native build for `arch` first (PGO over plain). An ARM64 host falls back to the x86_64
/// build, which Windows runs emulated, when the release has no ARM64 asset; an x86_64 host never
/// takes an ARM64 one.
pub fn pick_asset_for<'a>(release: &'a GhRelease, arch: &str) -> Option<&'a GhAsset> {
    let v = file_version(&release.tag_name);
    let exact = |name: String| release.assets.iter().find(move |a| a.name.eq_ignore_ascii_case(&name));
    let suffix = |s: String| {
        release.assets.iter().find(move |a| {
            let n = a.name.to_ascii_lowercase();
            n.starts_with("guarana-") && n.ends_with(&s)
        })
    };
    let mut arches = vec![arch];
    if arch == "arm64" {
        arches.push("x86_64");
    }
    for a in arches {
        let found = exact(format!("guarana-{v}-windows-{a}-pgo.zip"))
            .or_else(|| exact(format!("guarana-{v}-windows-{a}.zip")))
            .or_else(|| suffix(format!("-windows-{a}-pgo.zip")))
            .or_else(|| suffix(format!("-windows-{a}.zip")));
        if found.is_some() {
            return found;
        }
    }
    None
}

/// (major, minor, patch, is_final, pre-release text) of `20.1.0` / `20.1.0-rc1`.
fn version_key(v: &str) -> Option<(u64, u64, u64, bool, String)> {
    let v = v.trim().trim_start_matches('v');
    let (core, pre) = match v.split_once('-') {
        Some((c, p)) => (c, p.to_string()),
        None => (v, String::new()),
    };
    let mut it = core.split('.');
    let major = it.next()?.parse().ok()?;
    let minor = it.next().unwrap_or("0").parse().ok()?;
    let patch = it.next().unwrap_or("0").parse().ok()?;
    Some((major, minor, patch, pre.is_empty(), pre))
}

/// Whether `a` is a newer version than `b`. A label that is not a version counts as older.
pub fn is_newer(a: &str, b: &str) -> bool {
    match (version_key(a), version_key(b)) {
        (Some(x), Some(y)) => x > y,
        (Some(_), None) => true,
        _ => false,
    }
}

async fn latest_release(client: &reqwest::Client, channel: Channel) -> CmdResult<GhRelease> {
    match channel {
        Channel::Nightly => get_json(client, &format!("{API}/releases/tags/nightly"))
            .await
            .map_err(|e| if e == "Not found on GitHub" { "There is no nightly build yet".to_string() } else { e }),
        Channel::Stable => {
            let list: Vec<GhRelease> = get_json(client, &format!("{API}/releases?per_page=30")).await?;
            let mut candidates: Vec<GhRelease> = list
                .into_iter()
                .filter(|r| !r.draft && r.tag_name != "nightly" && pick_asset(r).is_some())
                .collect();
            candidates.sort_by(|a, b| {
                version_key(&b.tag_name).cmp(&version_key(&a.tag_name))
            });
            let final_release = candidates.iter().position(|r| !r.prerelease);
            match final_release {
                Some(i) => Ok(candidates.swap_remove(i)),
                None => candidates.into_iter().next().ok_or_else(|| "No release with a Windows build yet".to_string()),
            }
        }
    }
}

async fn release_for(client: &reqwest::Client, channel: Channel, version: &str) -> CmdResult<GhRelease> {
    let v = version.trim();
    if channel == Channel::Nightly || v.is_empty() || v.eq_ignore_ascii_case("latest") {
        return latest_release(client, channel).await;
    }
    let tag = if v.starts_with('v') { v.to_string() } else { format!("v{v}") };
    match get_json(client, &format!("{API}/releases/tags/{tag}")).await {
        Ok(r) => Ok(r),
        Err(e) if e == "Not found on GitHub" => Err(format!("There is no release {v}")),
        Err(e) => Err(e),
    }
}

fn parse_sums(text: &str) -> HashMap<String, String> {
    text.lines()
        .filter_map(|line| {
            let mut parts = line.split_whitespace();
            let hash = parts.next()?;
            let name = parts.next()?.trim_start_matches('*');
            (hash.len() == 64 && hash.chars().all(|c| c.is_ascii_hexdigit()))
                .then(|| (name.to_string(), hash.to_ascii_lowercase()))
        })
        .collect()
}

async fn fetch_sums(client: &reqwest::Client, release: &GhRelease) -> Option<HashMap<String, String>> {
    let asset = release.assets.iter().find(|a| a.name == SUMS_FILE)?;
    if !asset.browser_download_url.starts_with(DOWNLOAD_PREFIX) {
        return None;
    }
    let bytes = tokio::time::timeout(Duration::from_secs(45), get_bytes(client, &asset.browser_download_url, 1 << 20))
        .await
        .ok()?
        .ok()?;
    Some(parse_sums(&String::from_utf8_lossy(&bytes)))
}

fn digest_of(asset: &GhAsset) -> Option<String> {
    asset
        .digest
        .as_deref()
        .and_then(|d| d.strip_prefix("sha256:"))
        .map(|d| d.to_ascii_lowercase())
        .filter(|d| d.len() == 64)
}

/// The latest release on `channel`, or on the installed version's channel when `None`.
pub async fn check_updates(app: &AppHandle, channel: Option<&str>) -> CmdResult<UpdateInfo> {
    let persisted = app.state::<AppState>().load();
    let current = state::current_version(&persisted);
    let channel = match channel.map(str::trim).filter(|c| !c.is_empty()) {
        Some(c) => Channel::parse(c)?,
        None => current
            .as_ref()
            .and_then(|c| Channel::parse(&c.channel).ok())
            .unwrap_or(Channel::Stable),
    };
    let client = client()?;
    let release = latest_release(&client, channel).await?;
    let asset = pick_asset(&release).ok_or("The latest release has no Windows build")?;
    let sums = fetch_sums(&client, &release).await;
    let sha256 = sums
        .as_ref()
        .and_then(|s| s.get(&asset.name).cloned())
        .or_else(|| digest_of(asset))
        .unwrap_or_default();
    let latest = label(&release, channel);
    let update_available = match &current {
        None => true,
        // Moving to the other channel is always an install.
        Some(c) if c.channel != channel.as_str() => latest != c.version,
        Some(c) => match channel {
            Channel::Stable => is_newer(&latest, &c.version),
            Channel::Nightly => latest != c.version,
        },
    };
    Ok(UpdateInfo {
        current: current.map(|c| c.version),
        latest,
        channel: channel.as_str().to_string(),
        published_at: release.published_at.clone().unwrap_or_default(),
        notes_md: release.body.clone().unwrap_or_default(),
        size_bytes: asset.size,
        sha256,
        update_available,
    })
}

/// Download to `part`, resuming a previous partial download when the server honors ranges, and
/// retrying a broken transfer a few times before giving up. Returns the SHA-256 of the complete
/// file, hashed by reading it back so a resumed download is verified exactly like a fresh one.
async fn download(app: &AppHandle, client: &reqwest::Client, asset: &GhAsset, part: &Path) -> CmdResult<String> {
    const ATTEMPTS: u32 = 3;
    let mut last_err = String::new();
    for attempt in 1..=ATTEMPTS {
        match download_attempt(app, client, asset, part).await {
            Ok(()) => return sha256_file(part).ctx("Could not read back the download"),
            Err(e) => {
                last_err = e;
                if attempt < ATTEMPTS {
                    emit(app, "download", 0, 0, Some(format!("Download interrupted, retrying ({attempt}/{ATTEMPTS})")));
                    tokio::time::sleep(Duration::from_secs(2)).await;
                }
            }
        }
    }
    Err(format!("Download failed after {ATTEMPTS} attempts: {last_err}"))
}

async fn download_attempt(app: &AppHandle, client: &reqwest::Client, asset: &GhAsset, part: &Path) -> CmdResult<()> {
    let existing = fs::metadata(part).map(|m| m.len()).unwrap_or(0);
    let mut req = client.get(&asset.browser_download_url);
    if existing > 0 {
        req = req.header(reqwest::header::RANGE, format!("bytes={existing}-"));
    }
    let mut resp = req
        .send()
        .await
        .ctx("Download failed")?
        .error_for_status()
        .ctx("Download failed")?;
    let (start, total, append) = resume_plan(existing, resp.status(), resp.content_length(), asset.size);
    let file = fs::OpenOptions::new()
        .create(true)
        .write(true)
        .append(append)
        .truncate(!append)
        .open(part)
        .ctx("Could not write the download")?;
    let mut out = BufWriter::with_capacity(1 << 20, file);
    let mut done = start;
    let mut last = Instant::now();
    emit(app, "download", done, total, Some(asset.name.clone()));
    while let Some(chunk) = resp.chunk().await.ctx("Download interrupted")? {
        out.write_all(&chunk).ctx("Could not write the download")?;
        done += chunk.len() as u64;
        if last.elapsed() >= Duration::from_millis(200) {
            emit(app, "download", done, total, Some(asset.name.clone()));
            last = Instant::now();
        }
    }
    out.flush().ctx("Could not write the download")?;
    drop(out);
    emit(app, "download", done, total, Some(asset.name.clone()));
    if total > 0 && done != total {
        return Err(format!("Download incomplete: {done} of {total} bytes"));
    }
    Ok(())
}

/// `(bytes to keep, expected total, append)`. A 206 answers the range request, so the part file
/// is kept and appended to; any other status (a server that ignores Range) starts over.
fn resume_plan(existing: u64, status: reqwest::StatusCode, content_length: Option<u64>, asset_size: u64) -> (u64, u64, bool) {
    if existing > 0 && status == reqwest::StatusCode::PARTIAL_CONTENT {
        let total = content_length.map(|n| existing + n).unwrap_or(asset_size);
        (existing, total, true)
    } else {
        (0, content_length.unwrap_or(asset_size), false)
    }
}

pub async fn install_version(app: &AppHandle, version: &str, channel: &str) -> CmdResult<InstallResult> {
    let channel = Channel::parse(channel)?;
    let started = Instant::now();
    let st = app.state::<AppState>();
    let _guard = st.install_lock.try_lock().map_err(|_| "Another install is already running".to_string())?;
    let _taskbar = taskbar::Guard::new(app, taskbar::Job::Install);
    let result = install_locked(app, version, channel, started).await;
    if let Ok(r) = &result {
        prune_locked(app, KEEP_VERSIONS);
        notify::update_installed(app, &r.version);
    }
    result
}

async fn install_locked(app: &AppHandle, version: &str, channel: Channel, started: Instant) -> CmdResult<InstallResult> {
    // The build the installer carries needs no download.
    if let Some(b) = bundled(app) {
        if b.channel == channel && !version.trim().is_empty() && util::sanitize_component(version.trim()) == b.version {
            let app2 = app.clone();
            let result = tauri::async_runtime::spawn_blocking(move || install_bundled(&app2, &b))
                .await
                .map_err(|e| e.to_string())?;
            if result.is_ok() {
                prune_locked(app, KEEP_VERSIONS);
            }
            return result;
        }
    }

    emit(app, "resolve", 0, 1, Some("Looking up the release on GitHub".into()));
    let client = client()?;
    let release = release_for(&client, channel, version).await?;
    let asset = pick_asset(&release).cloned().ok_or("This release has no Windows build")?;
    if !asset.browser_download_url.starts_with(DOWNLOAD_PREFIX) {
        return Err("The release points somewhere other than this project's GitHub releases; not downloading".into());
    }
    let label = label(&release, channel);
    let listed = fetch_sums(&client, &release).await.and_then(|s| s.get(&asset.name).cloned());
    let github = digest_of(&asset);
    let expected = match (listed, github) {
        (Some(a), Some(b)) if a != b => {
            return Err("SHA256SUMS.txt and GitHub's own checksum disagree about this file; not installing".into())
        }
        (Some(a), _) => a,
        (None, Some(b)) => b,
        (None, None) => return Err("The release lists no SHA-256 for this file; not installing an unverified build".into()),
    };
    emit(app, "resolve", 1, 1, Some(format!("Guarana {label}")));

    let downloads = paths::downloads_dir();
    fs::create_dir_all(&downloads).ctx("Could not create the downloads folder")?;
    let part = downloads.join(format!("{}.part", util::sanitize_component(&asset.name)));
    let result = async {
        let actual = download(app, &client, &asset, &part).await?;
        emit(app, "verify", 1, 1, Some(format!("SHA-256 {actual}")));
        if actual != expected {
            return Err(format!(
                "Checksum mismatch: the release lists {expected}, the download is {actual}. Nothing was installed."
            ));
        }
        let app2 = app.clone();
        let part2 = part.clone();
        let label2 = label.clone();
        tauri::async_runtime::spawn_blocking(move || install_zip(&app2, &part2, &label2, channel, true))
            .await
            .map_err(|e| e.to_string())?
    }
    .await;
    let _ = fs::remove_file(&part);
    result.map(|mut r| {
        r.seconds = started.elapsed().as_secs_f64();
        r
    })
}

/// Skips a top-level `user` folder: beside `guarana.exe` it would switch the emulator to portable
/// mode and away from `%APPDATA%\guarana` (`src/common/fs/path_util.cpp`).
fn is_portable_user_dir(rel: &Path) -> bool {
    let comps: Vec<String> = rel.components().map(|c| c.as_os_str().to_string_lossy().to_ascii_lowercase()).collect();
    comps.first().map(|c| c == "user").unwrap_or(false) || (comps.len() >= 2 && comps[1] == "user")
}

fn extract(app: &AppHandle, zip_path: &Path, dest: &Path) -> CmdResult<()> {
    let file = fs::File::open(zip_path).ctx("Could not open the archive")?;
    let mut archive = zip::ZipArchive::new(BufReader::new(file)).ctx("Not a valid zip archive")?;
    let count = archive.len();
    let mut unpacked = 0u64;
    fs::create_dir_all(dest).ctx("Could not create the version folder")?;
    for i in 0..count {
        let mut entry = archive.by_index(i).ctx("The archive is damaged")?;
        let Some(rel) = entry.enclosed_name() else {
            // zip 9 decodes the name fallibly; fall back to the raw bytes so the warning still names it.
            let name = entry.name().unwrap_or_else(|_| String::from_utf8_lossy(entry.name_raw()));
            log::warn!("Skipped archive entry with an unsafe path: {name}");
            continue;
        };
        if is_portable_user_dir(&rel) {
            continue;
        }
        let out = dest.join(&rel);
        if entry.is_dir() {
            fs::create_dir_all(&out).ctx("Could not unpack")?;
            continue;
        }
        unpacked += entry.size();
        if unpacked > MAX_UNPACKED {
            return Err("The archive unpacks to more than 8 GB; not installing".into());
        }
        if let Some(parent) = out.parent() {
            fs::create_dir_all(parent).ctx("Could not unpack")?;
        }
        let mut f = BufWriter::with_capacity(1 << 20, fs::File::create(&out).ctx("Could not unpack")?);
        std::io::copy(&mut entry, &mut f).ctx("Could not unpack")?;
        f.flush().ctx("Could not unpack")?;
        if i % 32 == 0 || i + 1 == count {
            emit(app, "extract", (i + 1) as u64, count as u64, None);
        }
    }
    Ok(())
}

/// Unpacks `zip` into `versions\<label>\` (replacing an older copy of the same version), records
/// it and makes it current. Blocking.
fn install_zip(app: &AppHandle, zip: &Path, label: &str, channel: Channel, verified: bool) -> CmdResult<InstallResult> {
    let started = Instant::now();
    let folder = util::sanitize_component(label);
    let versions = paths::versions_dir();
    fs::create_dir_all(&versions).ctx("Could not create the versions folder")?;
    let tmp = versions.join(format!(".tmp-{folder}-{}", std::process::id()));
    if tmp.exists() {
        let _ = fs::remove_dir_all(&tmp);
    }
    if let Err(e) = extract(app, zip, &tmp) {
        let _ = fs::remove_dir_all(&tmp);
        return Err(e);
    }
    let Some(exe_dir) = state::emulator_dir_in(&tmp) else {
        let _ = fs::remove_dir_all(&tmp);
        return Err(format!("The archive has no {}", paths::EMULATOR_EXE));
    };
    let rel = exe_dir.strip_prefix(&tmp).map(Path::to_path_buf).unwrap_or_default();
    let final_dir = versions.join(&folder);
    if final_dir.exists() {
        let old = versions.join(format!(".old-{folder}-{}", util::stamp()));
        if fs::rename(&final_dir, &old).is_err() {
            let _ = fs::remove_dir_all(&tmp);
            return Err(format!("Guarana {folder} is in use: close it and install again"));
        }
        let _ = fs::remove_dir_all(&old);
    }
    fs::rename(&tmp, &final_dir).ctx("Could not move the new version into place")?;
    let exe_dir = final_dir.join(rel);
    let entry = InstalledVersion {
        version: folder.clone(),
        channel: channel.as_str().to_string(),
        path: exe_dir.to_string_lossy().into_owned(),
        installed_at: util::now_rfc3339(),
    };
    app.state::<AppState>().update(|s| {
        s.installed.retain(|v| v.version != entry.version);
        s.installed.insert(0, entry.clone());
    })?;
    state::write_current(&entry)?;
    log::info!("Installed Guarana {folder} ({}) to {}", channel.as_str(), exe_dir.display());
    appdir::switch_to(&entry);
    emit(app, "done", 1, 1, Some(format!("Guarana {folder} installed")));
    Ok(InstallResult {
        version: folder,
        path: entry.path,
        verified_sha256: verified,
        seconds: started.elapsed().as_secs_f64(),
    })
}

/// `resources\emulator.json`, written by CI next to the bundled `emulator.zip`.
#[derive(Debug, Default, Deserialize)]
struct BundledMeta {
    #[serde(default)]
    version: Option<String>,
    #[serde(default)]
    channel: Option<String>,
    #[serde(default)]
    sha256: Option<String>,
}

pub struct Bundled {
    zip: PathBuf,
    pub version: String,
    channel: Channel,
    sha256: Option<String>,
}

pub fn bundled(app: &AppHandle) -> Option<Bundled> {
    let dir = app.path().resource_dir().ok()?.join("resources");
    let zip = dir.join("emulator.zip");
    if !zip.is_file() {
        return None;
    }
    let meta: BundledMeta = fs::read(dir.join("emulator.json"))
        .ok()
        .and_then(|b| {
            let body = b.strip_prefix(b"\xEF\xBB\xBF").unwrap_or(&b);
            serde_json::from_slice(body).ok()
        })
        .unwrap_or_default();
    Some(Bundled {
        zip,
        version: meta.version.as_deref().map(util::sanitize_component).unwrap_or_else(|| "bundled".into()),
        channel: meta.channel.as_deref().and_then(|c| Channel::parse(c).ok()).unwrap_or(Channel::Stable),
        sha256: meta.sha256.map(|s| s.to_ascii_lowercase()).filter(|s| s.len() == 64),
    })
}

fn sha256_file(path: &Path) -> std::io::Result<String> {
    let mut f = BufReader::with_capacity(1 << 20, fs::File::open(path)?);
    let mut hasher = Sha256::new();
    let mut buf = vec![0u8; 1 << 20];
    loop {
        let n = f.read(&mut buf)?;
        if n == 0 {
            break;
        }
        hasher.update(&buf[..n]);
    }
    Ok(hex::encode(hasher.finalize()))
}

fn install_bundled(app: &AppHandle, b: &Bundled) -> CmdResult<InstallResult> {
    emit(app, "verify", 0, 1, Some("Checking the bundled build".into()));
    let verified = match &b.sha256 {
        Some(expected) => {
            let actual = sha256_file(&b.zip).ctx("Could not read the bundled build")?;
            if &actual != expected {
                return Err("The bundled build does not match its checksum; reinstall Guarana".into());
            }
            true
        }
        None => false,
    };
    emit(app, "verify", 1, 1, None);
    install_zip(app, &b.zip, &b.version, b.channel, verified)
}

/// First run: when nothing is installed and the installer carries a build, install it. Runs at
/// start-up; `get_state` waits for it, so the first state the UI sees already has the build.
pub async fn ensure_bundled(app: &AppHandle) {
    let st = app.state::<AppState>();
    let mut checked = st.bundled_checked.lock().await;
    if *checked {
        return;
    }
    *checked = true;
    cleanup_leftovers();
    // Before anything can start from the program folder: finish or undo a switch a crash cut
    // short, then move an install from before the folder existed into it.
    let persisted = st.load();
    let current = state::current_version(&persisted);
    let _ = tauri::async_runtime::spawn_blocking(move || {
        appdir::recover();
        if let Some(v) = current {
            appdir::switch_to(&v);
        }
    })
    .await;
    if !persisted.installed.is_empty() {
        return;
    }
    let Some(b) = bundled(app) else {
        return;
    };
    let Ok(_install) = st.install_lock.try_lock() else {
        return;
    };
    let _taskbar = taskbar::Guard::new(app, taskbar::Job::Install);
    let app2 = app.clone();
    match tauri::async_runtime::spawn_blocking(move || install_bundled(&app2, &b)).await {
        Ok(Ok(r)) => log::info!("First run: installed the bundled build {}", r.version),
        Ok(Err(e)) => log::warn!("First run: bundled build not installed: {e}"),
        Err(e) => log::warn!("First run: bundled install task failed: {e}"),
    }
}

/// Removes `.tmp-*` and `.old-*` folders an interrupted install left in `versions\`.
fn cleanup_leftovers() {
    let Ok(entries) = fs::read_dir(paths::versions_dir()) else {
        return;
    };
    for e in entries.flatten() {
        let name = e.file_name().to_string_lossy().into_owned();
        if (name.starts_with(".tmp-") || name.starts_with(".old-")) && e.path().is_dir() {
            let _ = fs::remove_dir_all(e.path());
        }
    }
}

/// How many installed versions auto-prune keeps after an install (the current one counts).
/// Every version is a full copy of the program, so an unbounded versions folder costs gigabytes.
pub const KEEP_VERSIONS: usize = 3;

/// The versions a prune removes: everything but the current one and the `keep` newest entries
/// (newest by install time; the current one counts toward `keep`). Pure, so it is testable.
pub fn prune_selection(installed: &[InstalledVersion], current: Option<&str>, keep: usize) -> Vec<String> {
    let mut sorted: Vec<&InstalledVersion> = installed.iter().collect();
    sorted.sort_by(|a, b| b.installed_at.cmp(&a.installed_at).then(a.version.cmp(&b.version)));
    let keep = keep.max(1);
    let mut kept = 0usize;
    let mut remove = Vec::new();
    for v in sorted {
        if current == Some(v.version.as_str()) || kept < keep {
            kept += 1;
            continue;
        }
        remove.push(v.version.clone());
    }
    remove
}

/// Remove old installed versions, keeping the current one and the newest `keep`. The running
/// `app` folder is never touched: it is a copy, and the versions folder is only the store.
/// Returns the versions removed.
pub async fn prune_versions(app: &AppHandle, keep: usize) -> CmdResult<Vec<String>> {
    let st = app.state::<AppState>();
    let _guard = st.install_lock.try_lock().map_err(|_| "Another install is already running".to_string())?;
    Ok(prune_locked(app, keep))
}

fn prune_locked(app: &AppHandle, keep: usize) -> Vec<String> {
    let st = app.state::<AppState>();
    let persisted = st.load();
    let current = state::current_version(&persisted).map(|v| v.version.clone());
    let candidates = prune_selection(&persisted.installed, current.as_deref(), keep);
    let versions = paths::versions_dir();
    let mut removed = Vec::new();
    for version in candidates {
        let Some(v) = persisted.installed.iter().find(|v| v.version == version) else { continue };
        let dir = PathBuf::from(&v.path);
        // Only ever delete inside versions/: a hand-edited state file must not aim this elsewhere.
        if !dir.starts_with(&versions) {
            removed.push(version);
            continue;
        }
        match fs::remove_dir_all(&dir) {
            Ok(()) => removed.push(version),
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => removed.push(version),
            Err(e) => log::warn!("Could not remove old version {}: {e}", dir.display()),
        }
    }
    if removed.is_empty() {
        return removed;
    }
    if let Err(e) = st.update(|s| s.installed.retain(|v| !removed.contains(&v.version))) {
        log::warn!("Could not save the pruned version list: {e}");
    }
    log::info!("Pruned {} old version(s), keeping {keep}: {}", removed.len(), removed.join(", "));
    removed
}

pub fn set_current(app: &AppHandle, version: &str) -> CmdResult<LauncherState> {
    let st = app.state::<AppState>();
    let persisted = st.load();
    let v = persisted
        .installed
        .iter()
        .find(|v| v.version == version)
        .ok_or_else(|| format!("Guarana {version} is not installed"))?;
    state::write_current(v)?;
    log::info!("Current version is now {version}");
    appdir::switch_to(v);
    Ok(state::snapshot(&persisted))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn release(tag: &str, assets: &[&str]) -> GhRelease {
        GhRelease {
            tag_name: tag.into(),
            draft: false,
            prerelease: false,
            published_at: Some("2026-11-13T03:40:00Z".into()),
            body: None,
            target_commitish: Some("1a2b3c4d5e6f7a8b9c0d1e2f3a4b5c6d7e8f9a0b".into()),
            assets: assets
                .iter()
                .map(|n| GhAsset {
                    name: n.to_string(),
                    size: 1,
                    browser_download_url: format!("{DOWNLOAD_PREFIX}{tag}/{n}"),
                    digest: None,
                })
                .collect(),
        }
    }

    #[test]
    fn picks_the_pgo_zip_first() {
        let r = release("v20.0.2", &["guarana-20.0.2-linux-x86_64.AppImage", "guarana-20.0.2-windows-x86_64.zip", "guarana-20.0.2-windows-x86_64-pgo.zip"]);
        assert_eq!(pick_asset_for(&r, "x86_64").unwrap().name, "guarana-20.0.2-windows-x86_64-pgo.zip");
        let r = release("v20.0.2", &["guarana-20.0.2-windows-x86_64.zip"]);
        assert_eq!(pick_asset_for(&r, "x86_64").unwrap().name, "guarana-20.0.2-windows-x86_64.zip");
        let n = release("nightly", &["guarana-nightly-windows-x86_64-pgo.zip"]);
        assert_eq!(pick_asset_for(&n, "x86_64").unwrap().name, "guarana-nightly-windows-x86_64-pgo.zip");
    }

    #[test]
    fn arm64_hosts_take_the_native_build_and_fallback() {
        let both = release("v20.0.2", &["guarana-20.0.2-windows-x86_64-pgo.zip", "guarana-20.0.2-windows-arm64.zip"]);
        assert_eq!(pick_asset_for(&both, "arm64").unwrap().name, "guarana-20.0.2-windows-arm64.zip");
        // No ARM64 asset: the x86_64 build runs emulated, better than nothing.
        let xonly = release("v20.0.2", &["guarana-20.0.2-windows-x86_64-pgo.zip"]);
        assert_eq!(pick_asset_for(&xonly, "arm64").unwrap().name, "guarana-20.0.2-windows-x86_64-pgo.zip");
        // An x86_64 host never takes an ARM64 asset.
        let aonly = release("v20.0.2", &["guarana-20.0.2-windows-arm64.zip"]);
        assert!(pick_asset_for(&aonly, "x86_64").is_none());
        assert_eq!(pick_asset_for(&aonly, "arm64").unwrap().name, "guarana-20.0.2-windows-arm64.zip");
    }

    #[test]
    fn host_arch_reads_the_emulation_variable_too() {
        assert_eq!(arch_from_env(Some("AMD64"), None), "x86_64");
        assert_eq!(arch_from_env(Some("ARM64"), None), "arm64");
        // An x64 process emulated on ARM64: ARCHITECTURE lies, ARCHITEW6432 does not.
        assert_eq!(arch_from_env(Some("AMD64"), Some("ARM64")), "arm64");
        assert_eq!(arch_from_env(None, None), "x86_64");
        assert_eq!(arch_from_env(Some("x86"), Some("AMD64")), "x86_64");
    }

    #[test]
    fn resume_plan_keeps_a_partial_only_on_206() {
        use reqwest::StatusCode;
        // Fresh download: nothing on disk.
        assert_eq!(resume_plan(0, StatusCode::OK, Some(100), 100), (0, 100, false));
        // Honored range: keep the 40 bytes, 60 remain.
        assert_eq!(resume_plan(40, StatusCode::PARTIAL_CONTENT, Some(60), 100), (40, 100, true));
        // Server ignored the range: start over, do not append to the old bytes.
        assert_eq!(resume_plan(40, StatusCode::OK, Some(100), 100), (0, 100, false));
        // No content-length on a 206: fall back to the advertised asset size.
        assert_eq!(resume_plan(40, StatusCode::PARTIAL_CONTENT, None, 100), (40, 100, true));
    }

    #[test]
    fn prune_keeps_current_and_the_newest() {
        let iv = |v: &str, at: &str| InstalledVersion {
            version: v.into(),
            channel: "stable".into(),
            path: format!("C:/Guarana/versions/{v}"),
            installed_at: at.into(),
        };
        let installed = vec![
            iv("20.0.1", "2026-11-01T00:00:00Z"),
            iv("20.0.4", "2026-11-04T00:00:00Z"),
            iv("20.0.2", "2026-11-02T00:00:00Z"),
            iv("20.0.3", "2026-11-03T00:00:00Z"),
        ];
        // Current counts toward keep: keep 3 = current + the 2 newest others.
        assert_eq!(prune_selection(&installed, Some("20.0.4"), 3), vec!["20.0.1".to_string()]);
        // An old current is never pruned, even when it is the oldest entry.
        assert_eq!(prune_selection(&installed, Some("20.0.1"), 2), vec!["20.0.2".to_string()]);
        // keep floors at 1; with nothing current set only the newest entry stays.
        assert_eq!(prune_selection(&installed, None, 0), vec!["20.0.3".to_string(), "20.0.2".to_string(), "20.0.1".to_string()]);
        assert!(prune_selection(&installed, Some("20.0.4"), 10).is_empty());
        assert!(prune_selection(&[], None, 3).is_empty());
    }

    #[test]
    fn labels() {
        assert_eq!(label(&release("v20.0.2", &[]), Channel::Stable), "20.0.2");
        assert_eq!(label(&release("nightly", &[]), Channel::Nightly), "nightly-20261113-1a2b3c4");
    }

    #[test]
    fn versions_compare() {
        assert!(is_newer("20.0.3", "20.0.2"));
        assert!(is_newer("20.1.0", "20.1.0-rc1"));
        assert!(!is_newer("20.0.2", "20.0.2"));
        assert!(is_newer("20.0.2", "bundled"));
        assert!(!is_newer("bundled", "20.0.2"));
    }

    #[test]
    fn sums_file() {
        let text = "aa".repeat(32) + "  guarana-20.0.2-windows-x86_64-pgo.zip\n" + &"BB".repeat(32) + " *SHA-other.zip\nnot a line\n";
        let sums = parse_sums(&text);
        assert_eq!(sums.get("guarana-20.0.2-windows-x86_64-pgo.zip").unwrap(), &"aa".repeat(32));
        assert_eq!(sums.get("SHA-other.zip").unwrap(), &"bb".repeat(32));
    }

    #[test]
    fn portable_user_dir_is_skipped() {
        assert!(is_portable_user_dir(Path::new("user/config/qt-config.ini")));
        assert!(is_portable_user_dir(Path::new("guarana/user/keys")));
        assert!(!is_portable_user_dir(Path::new("guarana.exe")));
        assert!(!is_portable_user_dir(Path::new("platforms/qwindows.dll")));
    }
}
