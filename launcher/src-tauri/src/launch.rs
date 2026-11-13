//! `launch_game` and `prefetch_caches`.
//!
//! The emulator boots a game with `guarana.exe -f -g <path>` (`lichii.exe` in builds from before the rename): `-g/--game` is the file and
//! `-f/--fullscreen` starts it fullscreen (`src/core/launch_params.cpp`, read by
//! `MainWindow::MainWindow` in `src/lichii/main_window.cpp`). Without a path it opens its own
//! window. Builds that understand it also get `--launcher-session` first: the emulator then shows
//! only the fullscreen loading screen and game, never its desktop window, and exits when the game
//! stops (0 after a stop, 1 when the game did not start) instead of showing its game list.

use std::collections::BTreeMap;
use std::ffi::OsString;
use std::fs;
use std::io::Read;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::Instant;

use tauri::{AppHandle, Emitter, Manager};
use windows::Win32::System::Threading::{
    GetCurrentThread, SetThreadPriority, THREAD_MODE_BACKGROUND_BEGIN, THREAD_MODE_BACKGROUND_END,
};

use crate::state::{self, AppState};
use crate::types::{GameExited, LaunchResult, PrefetchResult};
use crate::util::{self, CmdResult, Context};
use crate::{autopilot, boot, install, modes, paths, profiles, system, warm};

pub const EXITED_EVENT: &str = "game://exited";

/// The first emulator release that understands `--launcher-session`. The flag was added when the
/// repository's `VERSION` read 20.2.0, the newest release then, so it ships in the next one; this
/// assumes that is 20.3.0 or later. Raise it if the next release is numbered otherwise.
const LAUNCHER_SESSION_SINCE: &str = "20.3.0";

/// Whether the emulator version `current.json` names understands `--launcher-session`. Older
/// builds, which the launcher can roll back to, get `-f -g` only. A nightly, a pre-release or any
/// label that is not a release version counts as older: its label does not say what it was built
/// from.
fn understands_launcher_session(version: &str) -> bool {
    !install::is_newer(LAUNCHER_SESSION_SINCE, version)
}

/// The arguments that boot `game` in the emulator `version`.
fn game_args(version: &str, game: &Path) -> Vec<OsString> {
    let mut args = Vec::new();
    if understands_launcher_session(version) {
        args.push(OsString::from("--launcher-session"));
    }
    args.push(OsString::from("-f"));
    args.push(OsString::from("-g"));
    args.push(game.as_os_str().to_owned());
    args
}

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
        cmd.args(game_args(&current.version, gp));
    }
    // A launcher session writes its boot to stdout until the game is on screen (`boot.rs`).
    let follow_boot = game_path.is_some() && understands_launcher_session(&current.version);
    if follow_boot {
        cmd.stdout(Stdio::piped());
    }

    // Game Booster (Off by default). The GPU preference must be set before the emulator starts, so
    // it and the journal go down now; priority and throttling come after the process exists, and the
    // one elevated step after the game-watch thread does. It never fails the launch.
    let booster_level = crate::booster::level_of(&persisted);
    let booster_prep = crate::booster::apply_before_spawn(&exe, booster_level);

    let mut child = match cmd.spawn() {
        Ok(child) => child,
        Err(e) => {
            if booster_prep.is_some() {
                crate::booster::restore_after_session(app);
            }
            return Err(format!("Could not start Guarana: {e}"));
        }
    };
    let pid = child.id();
    // The emulator's first window can appear seconds from now, after the player has used the
    // launcher; let it take the foreground then so it does not open behind the launcher.
    unsafe {
        let _ = windows::Win32::UI::WindowsAndMessaging::AllowSetForegroundWindow(pid);
    }
    log::info!(
        "Started Guarana {} (pid {pid}){}",
        current.version,
        game_path.as_ref().map(|p| format!(" with {}", p.display())).unwrap_or_default()
    );
    if booster_prep.is_some() {
        use std::os::windows::io::AsRawHandle;
        crate::booster::apply_after_spawn(app, child.as_raw_handle(), pid, booster_level);
    }

    let app2 = app.clone();
    let started = Instant::now();
    let session_tid = tid.as_ref().map(|t| t.to_ascii_uppercase());
    let had_game = game_path.is_some();
    // The launch screen holds until the game is on screen: `game://boot` from the session output
    // and from the foreground watcher; both stop once the game is shown or the process is gone.
    let boot_done = Arc::new(AtomicBool::new(false));
    let exit_done = boot_done.clone();
    let boot_out = if had_game { child.stdout.take() } else { None };
    std::thread::Builder::new()
        .name("game-watch".into())
        .spawn(move || {
            let code = child.wait().ok().and_then(|s| s.code()).unwrap_or(-1);
            let seconds = started.elapsed().as_secs_f64();
            exit_done.store(true, Ordering::Relaxed);
            // Taken before the event, while the emulator's window may still be the last one active.
            let foreground = boot::foreground_pid();
            log::info!("Guarana (pid {pid}) exited with {code} after {seconds:.0} s");
            let _ = app2.emit(EXITED_EVENT, GameExited { code, seconds });
            if had_game {
                // Back to the launcher at once; to the front only if the player was not in another app.
                if let Some(window) = app2.get_webview_window("main") {
                    let _ = window.unminimize();
                    let _ = window.show();
                    if !matches!(foreground, Some(p) if p != pid && p != std::process::id()) {
                        let _ = window.set_focus();
                    }
                }
                let info = autopilot::SessionInfo { title_id: session_tid, exit_code: code, seconds, launch_on_battery };
                crate::share::after_session(&app2, &info);
                autopilot::after_session(&app2, info);
            }
            // Take the booster's changes back now the game has exited.
            if booster_level >= 1 {
                crate::booster::restore_after_session(&app2);
            }
            warm::resume_if_interrupted(&app2);
        })
        .ctx("Could not watch the game")?;
    // After the game-watch thread exists, so `boot_done` is always set when the process ends.
    if had_game {
        if let Some(out) = boot_out {
            boot::follow(app, out, boot_done.clone());
        }
        boot::watch_foreground(app, pid, boot_done);
    }

    // The game-watch thread now exists, so start the booster's elevated helper (one UAC prompt) on
    // its own thread: the prompt never holds up the launch or competes with the game before it is up.
    if let Some(prep) = booster_prep {
        crate::booster::launch_helper_async(app, pid, booster_level, prep);
    }
    Ok(LaunchResult { pid, follows_boot: follow_boot })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn strings(args: Vec<OsString>) -> Vec<String> {
        args.into_iter().map(|a| a.to_string_lossy().into_owned()).collect()
    }

    #[test]
    fn launcher_session_only_for_builds_that_know_it() {
        assert!(understands_launcher_session("20.3.0"));
        assert!(understands_launcher_session("v20.3.0"));
        assert!(understands_launcher_session("20.10.1"));
        assert!(understands_launcher_session("21.0.0"));
        assert!(!understands_launcher_session("20.2.0"), "the newest release before the flag");
        assert!(!understands_launcher_session("20.0.2"));
        assert!(!understands_launcher_session("20.3.0-rc1"), "a pre-release counts as older");
        assert!(!understands_launcher_session("nightly-20261113-1a2b3c4"));
        assert!(!understands_launcher_session("bundled"));
    }

    #[test]
    fn game_arguments_by_version() {
        let game = Path::new(r"D:\Games\Zelda [0400486024DEC000].nsp");
        assert_eq!(strings(game_args("20.2.0", game)), ["-f", "-g", r"D:\Games\Zelda [0400486024DEC000].nsp"]);
        assert_eq!(
            strings(game_args("20.3.0", game)),
            ["--launcher-session", "-f", "-g", r"D:\Games\Zelda [0400486024DEC000].nsp"]
        );
    }
}
