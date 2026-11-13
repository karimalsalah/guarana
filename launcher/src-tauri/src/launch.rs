//! `launch_game` and `prefetch_caches`.
//!
//! The emulator boots a game with `guarana.exe -f -g <path>` (`lichii.exe` in builds from before the rename): `-g/--game` is the file and
//! `-f/--fullscreen` starts it fullscreen (`src/core/launch_params.cpp`, read by
//! `MainWindow::MainWindow` in `src/lichii/main_window.cpp`). Without a path it opens its own
//! window.

use std::collections::BTreeMap;
use std::fs;
use std::io::Read;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::Instant;

use tauri::{AppHandle, Emitter, Manager};
use windows::Win32::System::Threading::{
    GetCurrentThread, SetThreadPriority, THREAD_MODE_BACKGROUND_BEGIN, THREAD_MODE_BACKGROUND_END,
};

use crate::state::{self, AppState};
use crate::types::{GameExited, LaunchResult, PrefetchResult};
use crate::util::{self, CmdResult, Context};
use crate::{autopilot, modes, paths, profiles, system, warm};

pub const EXITED_EVENT: &str = "game://exited";

/// Reads the title's shader and pipeline caches (`*.bin`) once, front to back, so the OS file
/// cache holds them when the emulator boots. Background I/O priority; at most half the free RAM
/// and never more than 4 GiB.
fn warm_files(dir: &Path) -> u64 {
    let mut files: Vec<(PathBuf, u64)> = fs::read_dir(dir)
        .map(|entries| {
            entries
                .flatten()
                .filter(|e| e.file_type().map(|t| t.is_file()).unwrap_or(false))
                .filter(|e| e.file_name().to_string_lossy().to_ascii_lowercase().ends_with(".bin"))
                .filter_map(|e| Some((e.path(), e.metadata().ok()?.len())))
                .collect()
        })
        .unwrap_or_default();
    files.sort();
    let budget = (system::available_ram_bytes() / 2).min(4 << 30);
    // SAFETY: changes only this thread's scheduling; the thread is ours and ends after this.
    let background = unsafe { SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN) }.is_ok();
    let mut read_total = 0u64;
    let mut buf = vec![0u8; 1 << 20];
    'files: for (path, _) in files {
        let Ok(mut f) = fs::File::open(&path) else { continue };
        loop {
            match f.read(&mut buf) {
                Ok(0) | Err(_) => break,
                Ok(n) => {
                    read_total += n as u64;
                    if read_total >= budget {
                        break 'files;
                    }
                }
            }
        }
    }
    if background {
        // SAFETY: as above.
        let _ = unsafe { SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END) };
    }
    read_total
}

pub async fn prefetch(title_id: &str) -> CmdResult<PrefetchResult> {
    let tid = util::normalize_title_id(title_id).ok_or("Not a title id (16 hex digits)")?;
    let dir = paths::shader_cache_dir().join(&tid);
    if !dir.is_dir() {
        return Ok(PrefetchResult { bytes_warmed: 0 });
    }
    let (tx, rx) = tokio::sync::oneshot::channel();
    std::thread::Builder::new()
        .name("cache-prefetch".into())
        .spawn(move || {
            let started = Instant::now();
            let bytes = warm_files(&dir);
            log::info!("Prefetched {} of {tid} caches in {:.1} s", util::human_bytes(bytes), started.elapsed().as_secs_f64());
            let _ = tx.send(bytes);
        })
        .ctx("Could not start the prefetch")?;
    let bytes = rx.await.map_err(|_| "The prefetch stopped".to_string())?;
    Ok(PrefetchResult { bytes_warmed: bytes })
}

pub fn launch(
    app: &AppHandle,
    title_id: Option<String>,
    path: Option<String>,
    profile_id: Option<String>,
) -> CmdResult<LaunchResult> {
    let st = app.state::<AppState>();
    let persisted = st.load();
    let current = state::current_version(&persisted).ok_or("Guarana is not installed yet")?;
    let exe = paths::emulator_exe_in(Path::new(&current.path)).ok_or_else(|| {
        format!(
            "{} is missing: reinstall this version",
            Path::new(&current.path).join(paths::EMULATOR_EXE).display()
        )
    })?;
    if util::emulator_running() {
        return Err("Guarana is already running: close it first".into());
    }

    let path = path.filter(|p| !p.trim().is_empty());
    let wanted_tid = title_id.as_deref().and_then(util::normalize_title_id);
    let (game_path, tid): (Option<PathBuf>, Option<String>) = match (path, wanted_tid) {
        (Some(p), tid) => {
            let p = PathBuf::from(p);
            if !p.is_file() {
                return Err(format!("{} is not there any more", p.display()));
            }
            let known = persisted
                .games
                .iter()
                .find(|g| Path::new(&g.path) == p)
                .and_then(|g| util::normalize_title_id(&g.title_id));
            (Some(p), tid.or(known))
        }
        (None, Some(tid)) => {
            let game = persisted
                .games
                .iter()
                .find(|g| g.title_id.eq_ignore_ascii_case(&tid) && Path::new(&g.path).is_file())
                .ok_or_else(|| format!("No game with title id {} in the game list", tid.to_ascii_uppercase()))?;
            (Some(PathBuf::from(&game.path)), Some(tid))
        }
        (None, None) => (None, None),
    };

    // A warm-up of this title must finish; one of another title is stopped.
    warm::make_way_for(app, tid.as_deref())?;

    // A mode (Auto by default) decides the settings at every start: power source, community data
    // and the autopilot's last adjustments are read now. A hand-made profile (custom) is left as is.
    let mode = modes::current(&persisted);
    if mode != modes::CUSTOM {
        if let Err(e) = modes::apply_current(app) {
            log::warn!("Settings for mode {mode} not written before the start: {e}");
        }
    } else if let Some(pid) = profile_id.filter(|p| !p.is_empty()) {
        if persisted.profile_id.as_deref() != Some(pid.as_str()) {
            if !profiles::is_known(&pid) {
                return Err(format!("Unknown profile {pid}"));
            }
            profiles::apply(&pid, &BTreeMap::new())?;
            st.update(|s| {
                s.profile_id = Some(pid.clone());
                s.overrides.clear();
            })?;
        }
    }

    let launch_on_battery = st.system().power.on_ac == Some(false);
    let mut cmd = Command::new(&exe);
    cmd.current_dir(exe.parent().unwrap_or(Path::new(".")));
    // Every 10 s the emulator logs how evenly frames were shown (a few lines per session); the
    // autopilot reads them after the game.
    cmd.env("EDEN_NX2_SHOWN_STATS", "1");
    if let Some(gp) = &game_path {
        cmd.arg("-f").arg("-g").arg(gp);
    }
    let mut child = cmd.spawn().ctx("Could not start Guarana")?;
    let pid = child.id();
    log::info!(
        "Started Guarana {} (pid {pid}){}",
        current.version,
        game_path.as_ref().map(|p| format!(" with {}", p.display())).unwrap_or_default()
    );
    let app2 = app.clone();
    let started = Instant::now();
    let session_tid = tid.as_ref().map(|t| t.to_ascii_uppercase());
    let had_game = game_path.is_some();
    std::thread::Builder::new()
        .name("game-watch".into())
        .spawn(move || {
            let code = child.wait().ok().and_then(|s| s.code()).unwrap_or(-1);
            let seconds = started.elapsed().as_secs_f64();
            log::info!("Guarana (pid {pid}) exited with {code} after {seconds:.0} s");
            let _ = app2.emit(EXITED_EVENT, GameExited { code, seconds });
            if had_game {
                let info = autopilot::SessionInfo { title_id: session_tid, exit_code: code, seconds, launch_on_battery };
                crate::share::after_session(&app2, &info);
                autopilot::after_session(&app2, info);
            }
            warm::resume_if_interrupted(&app2);
        })
        .ctx("Could not watch the game")?;
    Ok(LaunchResult { pid })
}
