//! `warm_shaders`: runs `<current>\guarana-cli.exe --warm-pipelines <title id> -c <qt-config.ini>`
//! (`eden-cli.exe` in builds from before the rename; `docs/warm-up.md`) hidden at below-normal priority and turns its stdout into
//! `warm://progress` events and a `WarmResult`.
//!
//! guarana-cli stops politely only on Ctrl+C, which a launcher reading its stdout through a pipe
//! cannot send; a hard kill leaves the cache's "ended while building" guard, so that title's next
//! run (or boot) builds nothing once. So the launcher never kills a warm-up of the game it is
//! about to start; it only stops one of another title, and the next warm-up of that title, which
//! then exits 3, is run again once.

use std::collections::HashMap;
use std::io::{BufRead, BufReader};
use std::os::windows::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::sync::{Arc, Mutex, OnceLock};
use std::time::{Duration, Instant, SystemTime};

use serde::Serialize;
use tauri::{AppHandle, Emitter, Manager};

use crate::state::{self, AppState};
use crate::util::{self, CmdResult, Context};
use crate::{notify, paths, system, taskbar};

pub const EVENT: &str = "warm://progress";
const CREATE_NO_WINDOW: u32 = 0x0800_0000;
const BELOW_NORMAL_PRIORITY_CLASS: u32 = 0x0000_4000;
/// The shader cache file guarana-cli builds from (`VideoCommon::ShaderCacheV2::FileName`).
const CACHE_FILE: &str = "lichii-cache.v2.bin";
const TIME_LIMIT: Duration = Duration::from_secs(2 * 60 * 60);

/// Payload of `warm://progress`.
#[derive(Debug, Clone, Serialize)]
pub struct WarmProgress {
    pub title_id: String,
    /// `ready`, `boot-builds`, `precache`, `retry` or `done`.
    pub stage: String,
    pub done: u64,
    pub total: u64,
    pub pass: u32,
    pub percent: u32,
    pub seconds: f64,
    /// True for the automatic run after a GPU driver change.
    pub auto: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub message: Option<String>,
}

#[derive(Debug, Clone, Serialize)]
pub struct WarmResult {
    pub title_id: String,
    /// `done`, `incomplete`, `stopped`, `nothing built`, `nothing to do` or `failed`.
    pub status: String,
    pub seconds: f64,
    /// Cached graphics pipelines for this device, already built for this driver, left.
    pub built: u64,
    pub already: u64,
    pub left: u64,
    pub exit_code: Option<i32>,
    pub message: Option<String>,
}

impl WarmResult {
    fn simple(title_id: &str, status: &str, message: &str) -> Self {
        Self {
            title_id: title_id.to_string(),
            status: status.to_string(),
            seconds: 0.0,
            built: 0,
            already: 0,
            left: 0,
            exit_code: None,
            message: Some(message.to_string()),
        }
    }
}

pub struct RunningWarm {
    pub title_id: String,
    pub auto: bool,
    pub percent: Arc<AtomicU32>,
    pub stop: Arc<AtomicBool>,
}

#[derive(Default)]
pub struct WarmState {
    pub running: Option<RunningWarm>,
    /// The automatic run was stopped for a game; it resumes when the game exits.
    pub auto_interrupted: bool,
    /// The user stopped the automatic run: it is not resumed.
    pub user_stopped: bool,
}

/// Clears `running` however the run ends.
struct RunningGuard<'a>(&'a Mutex<WarmState>);

impl Drop for RunningGuard<'_> {
    fn drop(&mut self) {
        if let Ok(mut w) = self.0.lock() {
            w.running = None;
        }
    }
}

/// `warm: pass 1 boot-builds 5% (70/1346) 2.6 s` -> (pass, stage, percent, done, total, seconds).
fn parse_progress(line: &str) -> Option<(u32, String, u32, u64, u64, f64)> {
    let rest = line.strip_prefix("warm: pass ")?;
    let mut it = rest.split_whitespace();
    let pass = it.next()?.parse().ok()?;
    let stage = it.next()?.to_string();
    let percent = it.next()?.strip_suffix('%')?.parse().ok()?;
    let counts = it.next()?.strip_prefix('(')?.strip_suffix(')')?;
    let (done, total) = counts.split_once('/')?;
    let seconds = it.next()?.parse().ok()?;
    Some((pass, stage, percent, done.parse().ok()?, total.parse().ok()?, seconds))
}

/// `warm: done after 142.3 s: 6006 cached graphics pipelines, 0 already built for this driver, 0 left`
/// -> (status, seconds, built, already, left).
fn parse_final(line: &str) -> Option<(String, f64, u64, u64, u64)> {
    let rest = line.strip_prefix("warm: ")?;
    let (status, rest) = rest.split_once(" after ")?;
    if !matches!(status, "done" | "incomplete" | "stopped" | "nothing built") {
        return None;
    }
    let (secs, rest) = rest.split_once(" s:")?;
    let numbers: Vec<u64> = rest
        .split(|c: char| !c.is_ascii_digit())
        .filter(|s| !s.is_empty())
        .filter_map(|s| s.parse().ok())
        .collect();
    if numbers.len() < 3 {
        return None;
    }
    Some((status.to_string(), secs.trim().parse().ok()?, numbers[0], numbers[1], numbers[2]))
}

/// Title ids (lowercase) with a shader cache to build from, most recently used first.
pub fn titles_with_cache() -> Vec<String> {
    let Ok(entries) = std::fs::read_dir(paths::shader_cache_dir()) else {
        return Vec::new();
    };
    let mut titles: Vec<(std::time::SystemTime, String)> = entries
        .flatten()
        .filter_map(|e| {
            let name = e.file_name().to_string_lossy().to_ascii_lowercase();
            let tid = util::normalize_title_id(&name)?;
            let modified = std::fs::metadata(e.path().join(CACHE_FILE)).ok()?.modified().ok()?;
            Some((modified, tid))
        })
        .collect();
    titles.sort_by(|a, b| b.0.cmp(&a.0));
    titles.into_iter().map(|(_, t)| t).collect()
}

const NO_WARM_UP: &str =
    "This Guarana build cannot prepare shaders ahead (its guarana-cli has no --warm-pipelines); a newer build can";

/// Whether `exe` knows `--warm-pipelines`: the option's name is in its option table. An older
/// guarana-cli would take the title id for a game path, and still rewrite the configuration and
/// rotate the log, so it is never started for this. Cached per file size and time.
fn supports_warm_up(exe: &Path) -> bool {
    static CACHE: OnceLock<Mutex<HashMap<PathBuf, (u64, Option<SystemTime>, bool)>>> = OnceLock::new();
    let Ok(meta) = std::fs::metadata(exe) else { return false };
    let stamp = (meta.len(), meta.modified().ok());
    let cache = CACHE.get_or_init(|| Mutex::new(HashMap::new()));
    if let Some((len, modified, found)) = cache.lock().ok().and_then(|c| c.get(exe).cloned()) {
        if (len, modified) == stamp {
            return found;
        }
    }
    let found = file_contains(exe, b"warm-pipelines");
    if let Ok(mut c) = cache.lock() {
        c.insert(exe.to_path_buf(), (stamp.0, stamp.1, found));
    }
    found
}

fn file_contains(path: &Path, needle: &[u8]) -> bool {
    let Ok(mut f) = std::fs::File::open(path) else { return false };
    let mut buf = vec![0u8; (1 << 20) + needle.len()];
    let mut carry = 0;
    loop {
        let n = match std::io::Read::read(&mut f, &mut buf[carry..]) {
            Ok(0) | Err(_) => return false,
            Ok(n) => n,
        };
        let filled = carry + n;
        if buf[..filled].windows(needle.len()).any(|w| w == needle) {
            return true;
        }
        // Keep the tail so a match across two reads is found.
        carry = (needle.len() - 1).min(filled);
        buf.copy_within(filled - carry..filled, 0);
    }
}

fn cli_exe(app: &AppHandle) -> CmdResult<PathBuf> {
    let current = state::current_version(&app.state::<AppState>().load()).ok_or("Guarana is not installed yet")?;
    let exe = paths::cli_exe_in(Path::new(&current.path))
        .ok_or_else(|| format!("This Guarana build has no {}", paths::CLI_EXE))?;
    if !supports_warm_up(&exe) {
        return Err(NO_WARM_UP.into());
    }
    Ok(exe)
}

/// Runs the warm-up for one title (twice when the first run says "run it again"). Blocking.
pub fn run(app: &AppHandle, title_id: &str, auto: bool) -> CmdResult<WarmResult> {
    let tid = util::normalize_title_id(title_id).ok_or("Not a title id (16 hex digits)")?;
    let exe = match cli_exe(app) {
        Ok(exe) => exe,
        Err(e) if e == NO_WARM_UP => return Ok(WarmResult::simple(&tid.to_ascii_uppercase(), "nothing to do", NO_WARM_UP)),
        Err(e) => return Err(e),
    };
    if util::emulator_running() {
        return Err("Close Guarana first: preparing shaders needs the GPU to itself".into());
    }
    let config = paths::qt_config();
    if !config.is_file() || !paths::shader_cache_dir().join(&tid).join(CACHE_FILE).is_file() {
        return Ok(WarmResult::simple(&tid, "nothing to do", "No shader cache yet: start the game once first"));
    }
    let st = app.state::<AppState>();
    let percent = Arc::new(AtomicU32::new(0));
    let stop = Arc::new(AtomicBool::new(false));
    {
        let mut w = st.warm.lock().unwrap_or_else(|e| e.into_inner());
        if let Some(r) = &w.running {
            return Err(format!("Already preparing shaders for {}", r.title_id.to_ascii_uppercase()));
        }
        w.running = Some(RunningWarm { title_id: tid.clone(), auto, percent: percent.clone(), stop: stop.clone() });
    }
    let _guard = RunningGuard(&st.warm);
    let _taskbar = taskbar::Guard::new(app, taskbar::Job::Warm);
    // guarana-cli rewrites the configuration it is given (it saves what it read).
    let _ = util::backup_file(&config);
    keep_game_log();

    let mut result = run_once(app, &exe, &tid, &config, auto, &percent, &stop)?;
    if result.exit_code == Some(3) && !stop.load(Ordering::SeqCst) {
        emit(app, &tid, "retry", 0, 0, 0, 0, 0.0, auto, Some("The last run ended while building; running again once".into()));
        result = run_once(app, &exe, &tid, &config, auto, &percent, &stop)?;
    }
    log::info!("Shader warm-up {}: {} after {:.1} s (exit {:?})", tid, result.status, result.seconds, result.exit_code);
    Ok(result)
}

#[allow(clippy::too_many_arguments)]
fn emit(app: &AppHandle, tid: &str, stage: &str, pass: u32, percent: u32, done: u64, total: u64, seconds: f64, auto: bool, message: Option<String>) {
    match stage {
        "done" => taskbar::clear(app, taskbar::Job::Warm),
        "ready" | "retry" => taskbar::set(app, taskbar::Job::Warm, None),
        _ => taskbar::set(app, taskbar::Job::Warm, Some(u64::from(percent))),
    }
    let _ = app.emit(
        EVENT,
        WarmProgress {
            title_id: tid.to_ascii_uppercase(),
            stage: stage.to_string(),
            done,
            total,
            pass,
            percent,
            seconds,
            auto,
            message,
        },
    );
}

fn run_once(
    app: &AppHandle,
    exe: &Path,
    tid: &str,
    config: &Path,
    auto: bool,
    percent: &Arc<AtomicU32>,
    stop: &Arc<AtomicBool>,
) -> CmdResult<WarmResult> {
    let started = Instant::now();
    let mut child = Command::new(exe)
        .arg("--warm-pipelines")
        .arg(tid.to_ascii_uppercase())
        .arg("-c")
        .arg(config)
        .current_dir(exe.parent().unwrap_or(Path::new(".")))
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .creation_flags(CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS)
        .spawn()
        .ctx("Could not start guarana-cli")?;
    let final_line: Arc<Mutex<Option<String>>> = Arc::new(Mutex::new(None));
    let last_lines: Arc<Mutex<Vec<String>>> = Arc::new(Mutex::new(Vec::new()));

    let out_thread = child.stdout.take().map(|stdout| {
        let (app, tid, final_line, last_lines, percent) =
            (app.clone(), tid.to_string(), final_line.clone(), last_lines.clone(), percent.clone());
        std::thread::spawn(move || {
            for line in BufReader::new(stdout).lines().map_while(Result::ok) {
                let line = line.trim().to_string();
                if let Some((pass, stage, pct, done, total, secs)) = parse_progress(&line) {
                    percent.store(pct, Ordering::Relaxed);
                    emit(&app, &tid, &stage, pass, pct, done, total, secs, auto, None);
                } else if parse_final(&line).is_some() {
                    *final_line.lock().unwrap_or_else(|e| e.into_inner()) = Some(line.clone());
                } else if line.starts_with("warm: ") {
                    if line.contains("renderer ready") {
                        emit(&app, &tid, "ready", 0, 0, 0, 0, 0.0, auto, Some(line.clone()));
                    }
                    log::info!("guarana-cli: {line}");
                    push_last(&last_lines, line);
                }
            }
        })
    });
    let err_thread = child.stderr.take().map(|stderr| {
        let last_lines = last_lines.clone();
        std::thread::spawn(move || {
            for line in BufReader::new(stderr).lines().map_while(Result::ok) {
                let line = line.trim().to_string();
                if line.starts_with("warm: ") {
                    log::warn!("guarana-cli: {line}");
                    push_last(&last_lines, line);
                }
            }
        })
    });

    let mut killed = false;
    let status = loop {
        if !killed && (stop.load(Ordering::SeqCst) || started.elapsed() > TIME_LIMIT) {
            let _ = child.kill();
            killed = true;
        }
        match child.try_wait() {
            Ok(Some(status)) => break Some(status),
            Ok(None) => std::thread::sleep(Duration::from_millis(200)),
            Err(_) => break None,
        }
    };
    if let Some(t) = out_thread {
        let _ = t.join();
    }
    if let Some(t) = err_thread {
        let _ = t.join();
    }
    let exit_code = status.and_then(|s| s.code());
    let seconds = started.elapsed().as_secs_f64();
    let last = last_lines.lock().unwrap_or_else(|e| e.into_inner()).last().cloned();
    let final_line = final_line.lock().unwrap_or_else(|e| e.into_inner()).clone();
    let mut result = match final_line.as_deref().and_then(parse_final) {
        Some((status, secs, built, already, left)) => WarmResult {
            title_id: tid.to_ascii_uppercase(),
            status,
            seconds: secs,
            built,
            already,
            left,
            exit_code,
            message: last,
        },
        None => WarmResult {
            title_id: tid.to_ascii_uppercase(),
            status: if killed { "stopped" } else { "failed" }.to_string(),
            seconds,
            built: 0,
            already: 0,
            left: 0,
            exit_code,
            // No "warm:" line at all: a guarana-cli (eden-cli) from before --warm-pipelines.
            message: last.or_else(|| {
                Some(if killed {
                    "Stopped".to_string()
                } else {
                    "This Guarana build cannot prepare shaders yet (no guarana-cli --warm-pipelines); update Guarana".to_string()
                })
            }),
        },
    };
    if killed {
        result.status = "stopped".into();
    }
    emit(app, tid, "done", 0, 100, result.built, result.built, result.seconds, auto, Some(result.status.clone()));
    Ok(result)
}

/// guarana-cli logs to the emulator's `eden_log.txt` and rotates it, so two warm-ups would push the
/// last game's log out. A log that booted a game is copied aside first.
fn keep_game_log() {
    let log = paths::emulator_log_dir().join("eden_log.txt");
    let Ok(mut f) = std::fs::File::open(&log) else { return };
    let mut head = vec![0u8; 4 << 20];
    let mut filled = 0;
    while filled < head.len() {
        match std::io::Read::read(&mut f, &mut head[filled..]) {
            Ok(0) | Err(_) => break,
            Ok(n) => filled += n,
        }
    }
    let needle = b"Booting game:";
    if !head[..filled].windows(needle.len()).any(|w| w == needle) {
        return;
    }
    let dest = paths::kept_game_log();
    if let Some(dir) = dest.parent() {
        let _ = std::fs::create_dir_all(dir);
    }
    if let Err(e) = std::fs::copy(&log, &dest) {
        log::warn!("Could not keep the last game log: {e}");
    }
}

fn push_last(lines: &Arc<Mutex<Vec<String>>>, line: String) {
    let mut l = lines.lock().unwrap_or_else(|e| e.into_inner());
    l.push(line);
    if l.len() > 20 {
        l.remove(0);
    }
}

/// The GPU driver the warm-ups were last built for: vendor, device and driver version.
fn driver_key() -> Option<String> {
    let sys = system::detect();
    let gpu = sys.primary()?;
    Some(format!("{:04x}:{:04x}:{}", gpu.vendor_id, gpu.device_id, gpu.driver_version))
}

/// After a GPU driver change (or the first start with caches on disk), warms every title that
/// has a shader cache, newest first. The driver is recorded only once all of them ran, so a run
/// cut short by a game resumes when the game exits or at the next start.
pub async fn auto_after_driver_change(app: AppHandle) {
    let app2 = app.clone();
    let _ = tauri::async_runtime::spawn_blocking(move || auto_blocking(&app2)).await;
}

fn auto_blocking(app: &AppHandle) {
    let st = app.state::<AppState>();
    let Some(key) = driver_key() else { return };
    if st.load().last_warm_driver.as_deref() == Some(key.as_str()) {
        return;
    }
    if cli_exe(app).is_err() {
        return;
    }
    let titles = titles_with_cache();
    log::info!("GPU driver {key}: preparing shaders for {} title(s)", titles.len());
    if let Ok(mut w) = st.warm.lock() {
        w.auto_interrupted = false;
    }
    let mut ready = 0;
    for tid in titles {
        if util::emulator_running() {
            mark_interrupted(app);
            return;
        }
        match run(app, &tid, true) {
            Ok(r) if r.status == "done" => ready += 1,
            Ok(r) if r.status == "stopped" => {
                let by_user = st.warm.lock().map(|mut w| std::mem::take(&mut w.user_stopped)).unwrap_or(false);
                if by_user {
                    // The user's call: this driver counts as done, so it is not offered again.
                    let _ = st.update(|s| s.last_warm_driver = Some(key));
                } else {
                    mark_interrupted(app);
                }
                return;
            }
            Ok(_) => {}
            Err(e) => {
                log::warn!("Shader warm-up of {tid} not run: {e}");
                if util::emulator_running() || e.starts_with("Already") {
                    mark_interrupted(app);
                    return;
                }
            }
        }
    }
    let _ = st.update(|s| s.last_warm_driver = Some(key));
    notify::driver_warm_finished(app, ready);
}

fn mark_interrupted(app: &AppHandle) {
    if let Ok(mut w) = app.state::<AppState>().warm.lock() {
        w.auto_interrupted = true;
    }
}

/// Before a game starts: a warm-up of that title must finish first (killing it would cost the
/// boot its cache); one of another title is stopped. Blocking.
pub fn make_way_for(app: &AppHandle, title_id: Option<&str>) -> CmdResult<()> {
    let st = app.state::<AppState>();
    let auto = {
        let w = st.warm.lock().unwrap_or_else(|e| e.into_inner());
        let Some(r) = &w.running else { return Ok(()) };
        if title_id.map(|t| t.eq_ignore_ascii_case(&r.title_id)).unwrap_or(false) {
            return Err(format!(
                "Shaders for this game are being prepared ({}%). It can start as soon as that finishes.",
                r.percent.load(Ordering::Relaxed)
            ));
        }
        r.stop.store(true, Ordering::SeqCst);
        r.auto
    };
    if auto {
        // The automatic run goes on when the game exits.
        mark_interrupted(app);
    }
    let deadline = Instant::now() + Duration::from_secs(15);
    while Instant::now() < deadline {
        if st.warm.lock().map(|w| w.running.is_none()).unwrap_or(true) {
            return Ok(());
        }
        std::thread::sleep(Duration::from_millis(100));
    }
    Err("The shader preparation did not stop in time; try again in a moment".into())
}

/// Result of `stop_warm_shaders`.
#[derive(Debug, Clone, Serialize)]
pub struct StopResult {
    pub was_running: bool,
    pub title_id: Option<String>,
    pub stopped: bool,
    pub message: String,
}

/// Ends the running warm-up. guarana-cli stops politely only on Ctrl+C, which a launcher reading its
/// output through a pipe cannot send, so the process is ended; the title's next warm-up then
/// repairs the cache guard with one extra run (see the module comment). Blocking, 15 s at most.
pub fn stop(app: &AppHandle) -> CmdResult<StopResult> {
    let st = app.state::<AppState>();
    let (tid, auto) = {
        let w = st.warm.lock().unwrap_or_else(|e| e.into_inner());
        match &w.running {
            None => {
                return Ok(StopResult {
                    was_running: false,
                    title_id: None,
                    stopped: false,
                    message: "No shader preparation is running.".into(),
                })
            }
            Some(r) => {
                r.stop.store(true, Ordering::SeqCst);
                (r.title_id.to_ascii_uppercase(), r.auto)
            }
        }
    };
    if auto {
        if let Ok(mut w) = st.warm.lock() {
            w.user_stopped = true;
        }
    }
    let deadline = Instant::now() + Duration::from_secs(15);
    while Instant::now() < deadline {
        if st.warm.lock().map(|w| w.running.is_none()).unwrap_or(true) {
            return Ok(StopResult {
                was_running: true,
                title_id: Some(tid),
                stopped: true,
                message: "Stopped. Preparing this game again later finishes the job; it may start with a short repair pass.".into(),
            });
        }
        std::thread::sleep(Duration::from_millis(100));
    }
    Ok(StopResult {
        was_running: true,
        title_id: Some(tid),
        stopped: false,
        message: "It did not stop within 15 seconds; try again in a moment.".into(),
    })
}

/// After a game exits: picks the automatic run up again if a game interrupted it.
pub fn resume_if_interrupted(app: &AppHandle) {
    let resume = app
        .state::<AppState>()
        .warm
        .lock()
        .map(|mut w| std::mem::take(&mut w.auto_interrupted))
        .unwrap_or(false);
    if resume {
        let app = app.clone();
        tauri::async_runtime::spawn(auto_after_driver_change(app));
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn progress_lines() {
        assert_eq!(
            parse_progress("warm: pass 1 boot-builds 5% (70/1346) 2.6 s"),
            Some((1, "boot-builds".to_string(), 5, 70, 1346, 2.6))
        );
        assert_eq!(
            parse_progress("warm: pass 2 precache 100% (4660/4660) 141.0 s"),
            Some((2, "precache".to_string(), 100, 4660, 4660, 141.0))
        );
        assert_eq!(parse_progress("warm: title 0400486024dec000, renderer ready after 1.4 s"), None);
    }

    #[test]
    fn probe_finds_the_option_across_reads() {
        let dir = std::env::temp_dir().join(format!("guarana-warm-test-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("guarana-cli.exe");
        let needle = b"warm-pipelines";
        // The first read takes 1 MiB + the needle's length; start the needle 5 bytes before that.
        let mut data = vec![b'x'; (1 << 20) + needle.len() - 5];
        data.extend_from_slice(needle);
        data.extend_from_slice(&[b'y'; 100]);
        std::fs::write(&path, &data).unwrap();
        assert!(file_contains(&path, needle));
        std::fs::write(&path, vec![b'x'; 3 << 20]).unwrap();
        assert!(!file_contains(&path, needle));
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn final_lines() {
        assert_eq!(
            parse_final("warm: done after 142.3 s: 6006 cached graphics pipelines, 0 already built for this driver, 0 left"),
            Some(("done".to_string(), 142.3, 6006, 0, 0))
        );
        assert_eq!(
            parse_final("warm: nothing built after 3.0 s: 6006 cached graphics pipelines, 12 already built for this driver, 5994 left"),
            Some(("nothing built".to_string(), 3.0, 6006, 12, 5994))
        );
        assert_eq!(parse_final("warm: the last run ended while building the cache; run it again"), None);
    }
}
