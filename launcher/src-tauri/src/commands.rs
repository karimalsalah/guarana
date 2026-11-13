//! The commands of `launcher/CONTRACT.md`. Every one is async so nothing runs on the UI thread;
//! file and registry work goes to the blocking pool. Arguments are snake_case on the JS side
//! (`invoke("apply_profile", { profile_id, overrides })`), as the contract writes them.

use std::collections::BTreeMap;
use std::path::PathBuf;
use std::time::Duration;

use tauri::{AppHandle, Emitter, Manager};

use crate::state::{self, AppState};
use crate::types::{
    AppliedProfile, Check, Controller, Found, Game, ImportResult, InstallResult, LaunchResult, LauncherState,
    PrefetchResult, Profile, Progress, ReportResult, SystemInfo, ToggleValue, UpdateInfo,
};
use crate::util::CmdResult;
use crate::warm::WarmResult;
use crate::{autopilot, community, credits, doctor, install, keys, launch, locate, modes, profiles, report, util, warm};

const LOCATE_EVENT: &str = "locate://progress";

async fn blocking<T, F>(app: &AppHandle, f: F) -> CmdResult<T>
where
    T: Send + 'static,
    F: FnOnce(&AppHandle) -> T + Send + 'static,
{
    let app = app.clone();
    tauri::async_runtime::spawn_blocking(move || f(&app)).await.map_err(|e| e.to_string())
}

#[tauri::command(rename_all = "snake_case")]
pub async fn detect_system(app: AppHandle) -> CmdResult<SystemInfo> {
    blocking(&app, |app| app.state::<AppState>().system()).await
}

#[tauri::command(rename_all = "snake_case")]
pub async fn list_profiles(app: AppHandle) -> CmdResult<Vec<Profile>> {
    blocking(&app, |app| {
        let st = app.state::<AppState>();
        let sys = st.system();
        let persisted = st.load();
        profiles::list(&sys, persisted.profile_id.as_deref(), &persisted.overrides)
    })
    .await
}

#[tauri::command(rename_all = "snake_case")]
pub async fn get_state(app: AppHandle) -> CmdResult<LauncherState> {
    install::ensure_bundled(&app).await;
    blocking(&app, |app| state::snapshot(&app.state::<AppState>().load())).await
}

#[tauri::command(rename_all = "snake_case")]
pub async fn check_updates(app: AppHandle, channel: Option<String>) -> CmdResult<UpdateInfo> {
    install::check_updates(&app, channel.as_deref()).await
}

#[tauri::command(rename_all = "snake_case")]
pub async fn install_version(app: AppHandle, version: String, channel: String) -> CmdResult<InstallResult> {
    install::install_version(&app, &version, &channel).await
}

#[tauri::command(rename_all = "snake_case")]
pub async fn set_current_version(app: AppHandle, version: String) -> CmdResult<LauncherState> {
    blocking(&app, move |app| install::set_current(app, &version)).await?
}

#[tauri::command(rename_all = "snake_case")]
pub async fn locate_files(app: AppHandle, scope: String, roots: Option<Vec<String>>) -> CmdResult<Found> {
    let roots = roots.unwrap_or_default();
    let deep = match scope.as_str() {
        "quick" => false,
        "deep" => true,
        other => return Err(format!("Unknown scope {other}: use quick or deep")),
    };
    // No folders given: a deep search covers every fixed drive.
    let roots = if deep && roots.is_empty() { locate::fixed_drives() } else { roots };
    blocking(&app, move |app| {
        let (list, budget) = if deep {
            (locate::deep_roots(&roots), Duration::from_secs(180))
        } else {
            let path = app.path();
            let known: Vec<(PathBuf, &str)> = [
                (path.desktop_dir().ok(), "Desktop"),
                (path.document_dir().ok(), "Documents"),
                (path.download_dir().ok(), "Downloads"),
            ]
            .into_iter()
            .filter_map(|(p, hint)| p.filter(|p| p.is_dir()).map(|p| (p, hint)))
            .collect();
            let mut list = locate::quick_roots(&known);
            // Folders the UI passes along with a quick search get the same shallow look.
            for mut r in locate::deep_roots(&roots) {
                r.max_depth = 3;
                list.push(r);
            }
            (list, Duration::from_secs(20))
        };
        let emit = |p: Progress| {
            let _ = app.emit(LOCATE_EVENT, p);
        };
        let found = locate::locate(&list, budget, &emit);
        log::info!(
            "Located {} key file(s), {} firmware folder(s), {} game(s) in {} folders ({:.1} s)",
            found.keys.len(),
            found.firmware.len(),
            found.games.len(),
            found.scanned_dirs,
            found.seconds
        );
        found
    })
    .await
}

#[tauri::command(rename_all = "snake_case")]
pub async fn import_keys(app: AppHandle, path: String) -> CmdResult<ImportResult> {
    blocking(&app, move |_| keys::import(&path)).await
}

fn rescan(app: &AppHandle) -> CmdResult<Vec<Game>> {
    let st = app.state::<AppState>();
    let persisted = st.load();
    let games = locate::scan_game_folders(&persisted.game_folders, &persisted.games);
    let out = games.clone();
    st.update(move |s| s.games = games)?;
    Ok(out)
}

#[tauri::command(rename_all = "snake_case")]
pub async fn add_game_folder(app: AppHandle, path: String) -> CmdResult<Vec<Game>> {
    blocking(&app, move |app| {
        let dir = PathBuf::from(path.trim());
        if !dir.is_absolute() || !dir.is_dir() {
            return Err(format!("{} is not a folder", dir.display()));
        }
        let text = dir.to_string_lossy().into_owned();
        app.state::<AppState>().update(|s| {
            if !s.game_folders.iter().any(|f| f.eq_ignore_ascii_case(&text)) {
                s.game_folders.push(text.clone());
            }
        })?;
        rescan(app)
    })
    .await?
}

#[tauri::command(rename_all = "snake_case")]
pub async fn list_games(app: AppHandle) -> CmdResult<Vec<Game>> {
    blocking(&app, rescan).await?
}

#[tauri::command(rename_all = "snake_case")]
pub async fn apply_profile(
    app: AppHandle,
    profile_id: String,
    overrides: Option<BTreeMap<String, ToggleValue>>,
) -> CmdResult<AppliedProfile> {
    blocking(&app, move |app| {
        let overrides = overrides.unwrap_or_default();
        let applied = profiles::apply(&profile_id, &overrides)?;
        // A hand-made profile: the modes and the autopilot leave it alone until a mode is picked.
        app.state::<AppState>().update(|s| {
            s.profile_id = Some(profile_id.clone());
            s.overrides = overrides;
            s.mode = Some(modes::CUSTOM.to_string());
        })?;
        Ok(applied)
    })
    .await?
}

#[tauri::command(rename_all = "snake_case")]
pub async fn run_doctor(app: AppHandle) -> CmdResult<Vec<Check>> {
    doctor::run(&app).await
}

#[tauri::command(rename_all = "snake_case")]
pub async fn apply_fix(app: AppHandle, fix_id: String) -> CmdResult<Check> {
    doctor::apply_fix(&app, &fix_id).await
}

#[tauri::command(rename_all = "snake_case")]
pub async fn prefetch_caches(title_id: String) -> CmdResult<PrefetchResult> {
    launch::prefetch(&title_id).await
}

#[tauri::command(rename_all = "snake_case")]
pub async fn launch_game(
    app: AppHandle,
    title_id: Option<String>,
    path: Option<String>,
    profile_id: Option<String>,
) -> CmdResult<LaunchResult> {
    install::ensure_bundled(&app).await;
    blocking(&app, move |app| launch::launch(app, title_id, path, profile_id)).await?
}

#[tauri::command(rename_all = "snake_case")]
pub async fn collect_report(app: AppHandle, kind: String, note: Option<String>) -> CmdResult<ReportResult> {
    blocking(&app, move |app| report::collect(app, &kind, note)).await?
}

#[tauri::command(rename_all = "snake_case")]
pub async fn open_path(path: String) -> CmdResult<()> {
    util::open_in_explorer(&PathBuf::from(path))
}

#[tauri::command(rename_all = "snake_case")]
pub async fn open_url(url: String) -> CmdResult<()> {
    util::open_https(&url)
}

#[tauri::command(rename_all = "snake_case")]
pub async fn list_controllers(app: AppHandle) -> CmdResult<Vec<Controller>> {
    Ok(app.state::<AppState>().controllers.lock().map(|c| c.clone()).unwrap_or_default())
}

#[tauri::command(rename_all = "snake_case")]
pub async fn warm_shaders(app: AppHandle, title_id: String) -> CmdResult<WarmResult> {
    blocking(&app, move |app| warm::run(app, &title_id, false)).await?
}

#[tauri::command(rename_all = "snake_case")]
pub async fn stop_warm_shaders(app: AppHandle) -> CmdResult<warm::StopResult> {
    blocking(&app, warm::stop).await?
}

/// The folder picker for "Add folder". `None` when the user cancels.
#[tauri::command(rename_all = "snake_case")]
pub async fn pick_folder(title: Option<String>) -> CmdResult<Option<String>> {
    tauri::async_runtime::spawn_blocking(move || {
        rfd::FileDialog::new()
            .set_title(title.as_deref().unwrap_or("Choose a folder"))
            .pick_folder()
            .map(|p| p.to_string_lossy().into_owned())
    })
    .await
    .map_err(|e| e.to_string())
}

/// The mode, what it chose and why, the community data behind it and the autopilot's history.
/// Refreshes the community data first when it is older than 24 hours (6 s at most).
#[tauri::command(rename_all = "snake_case")]
pub async fn get_mode(app: AppHandle) -> CmdResult<modes::ModeStatus> {
    let best = tokio::time::timeout(Duration::from_secs(6), community::load(false))
        .await
        .unwrap_or_else(|_| community::cached_or_builtin());
    blocking(&app, move |app| {
        let st = app.state::<AppState>();
        modes::status_with(&st.system(), &best, &st.load(), None)
    })
    .await
}

#[tauri::command(rename_all = "snake_case")]
pub async fn set_mode(app: AppHandle, mode: String) -> CmdResult<modes::ModeStatus> {
    blocking(&app, move |app| modes::set_mode(app, &mode)).await?
}

#[tauri::command(rename_all = "snake_case")]
pub async fn autopilot_undo(app: AppHandle) -> CmdResult<modes::ModeStatus> {
    blocking(&app, |app| {
        let st = app.state::<AppState>();
        let undone = st.update(|s| autopilot::undo(&mut s.autopilot))?;
        let Some(c) = undone else { return Err("Nothing to undo".to_string()) };
        let note = match modes::apply_current(app) {
            Ok(_) => None,
            Err(_) => Some("Undone. It is written when Guarana closes.".to_string()),
        };
        log::info!("Autopilot undone: {}", c.text);
        Ok(modes::status(app, note))
    })
    .await?
}

#[tauri::command(rename_all = "snake_case")]
pub async fn autopilot_enable(app: AppHandle, enabled: bool) -> CmdResult<modes::ModeStatus> {
    blocking(&app, move |app| {
        app.state::<AppState>().update(|s| s.autopilot.disabled = !enabled)?;
        let _ = modes::apply_current(app);
        Ok(modes::status(app, None))
    })
    .await?
}

/// A scrubbed diagnostic summary and a prefilled chat link (under 2000 characters). `issues` are
/// the Doctor's non-green lines ("title: detail") the UI already holds.
#[tauri::command(rename_all = "snake_case")]
pub async fn ask_ai(app: AppHandle, service: String, issues: Option<Vec<String>>, question: Option<String>) -> CmdResult<report::AskAi> {
    blocking(&app, move |app| report::ask_ai(app, &service, &issues.unwrap_or_default(), question.as_deref())).await?
}

/// The hall of fame for the About screen: people first, tools apart. Refreshed at most once a day
/// (6 s at most, then the cache or the copy embedded at build time).
#[tauri::command(rename_all = "snake_case")]
pub async fn get_credits() -> CmdResult<credits::Credits> {
    Ok(tokio::time::timeout(Duration::from_secs(6), credits::load(false)).await.unwrap_or_else(|_| credits::builtin()))
}
