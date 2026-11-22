//! Progress on the launcher's taskbar button while an install or a shader warm-up runs (Tauri's
//! `set_progress_bar`, the taskbar's own progress on Windows). An install shows over a warm-up;
//! the bar goes when both have ended.

use std::sync::{Mutex, OnceLock};

use tauri::window::{ProgressBarState, ProgressBarStatus};
use tauri::{AppHandle, Manager};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Job {
    Install = 0,
    Warm = 1,
}

/// Per job: `None` when it is not running, `Some(None)` while it cannot tell how far it is.
type Shown = Option<Option<u64>>;

#[derive(Default)]
struct Bars {
    jobs: [Shown; 2],
    /// What the taskbar shows now; `None` before the first change.
    on_screen: Option<Shown>,
}

fn bars() -> &'static Mutex<Bars> {
    static BARS: OnceLock<Mutex<Bars>> = OnceLock::new();
    BARS.get_or_init(Mutex::default)
}

/// `percent` 0-100, or `None` for a pulsing bar.
pub fn set(app: &AppHandle, job: Job, percent: Option<u64>) {
    update(app, job, Some(percent.map(|p| p.min(100))));
}

pub fn clear(app: &AppHandle, job: Job) {
    update(app, job, None);
}

fn update(app: &AppHandle, job: Job, value: Shown) {
    let mut bars = bars().lock().unwrap_or_else(|e| e.into_inner());
    bars.jobs[job as usize] = value;
    let want = bars.jobs[Job::Install as usize].or(bars.jobs[Job::Warm as usize]);
    if bars.on_screen == Some(want) {
        return;
    }
    bars.on_screen = Some(want);
    let (status, progress) = match want {
        None => (ProgressBarStatus::None, None),
        Some(None) => (ProgressBarStatus::Indeterminate, None),
        Some(Some(p)) => (ProgressBarStatus::Normal, Some(p)),
    };
    if let Some(window) = app.get_webview_window("main") {
        let _ = window.set_progress_bar(ProgressBarState { status: Some(status), progress });
    }
}

/// Clears the job's progress however the job ends.
pub struct Guard {
    app: AppHandle,
    job: Job,
}

impl Guard {
    pub fn new(app: &AppHandle, job: Job) -> Self {
        Self { app: app.clone(), job }
    }
}

impl Drop for Guard {
    fn drop(&mut self) {
        clear(&self.app, self.job);
    }
}
