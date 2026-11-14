//! Follows a game's boot for the launch screen (`game://boot`).
//!
//! In a launcher session (`--launcher-session`, see `launch.rs`) the emulator writes its boot marks
//! to the standard output the launcher gives it, one line each and flushed at once, until the game
//! is on screen (`Common::BootTiming::SetSessionOutput` in `src/common/boot_timing.cpp`): the same
//! `Boot timing: <phase> at <ms> ms ...` text its log has, plus `Boot progress: <stage> <done>
//! <total>` for the shader counts its loading screen shows (`Common::BootTiming::SessionProgress`).
//! The log file itself is no use while the game boots: it is written in 4 KiB blocks unless the
//! player turned on per-line flushing, and the log filter can leave the marks out.
//!
//! A second watcher reports when a window of the emulator's process has the foreground, which
//! works with every emulator version, including those that write nothing.

use std::io::{BufRead, BufReader, Read};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

use tauri::{AppHandle, Emitter};

use crate::timeline::{phase_text, PhaseChanges, Timeline};
use crate::types::BootProgress;

pub const BOOT_EVENT: &str = "game://boot";

/// How long the foreground watcher looks for the emulator's window before it gives up.
const FOREGROUND_WATCH: Duration = Duration::from_secs(15 * 60);
const FOREGROUND_POLL: Duration = Duration::from_millis(200);
/// The longest line read at once; the emulator's lines are well under 200 bytes.
const MAX_LINE: u64 = 4096;

/// Where the boot is, in the order it gets there.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Phase {
    /// The emulator's own start (its window, logging, the Vulkan instance).
    Starting,
    /// The game file, the kernel, the program and its RomFS.
    Loading,
    /// The shader cache load and the boot pipeline builds (`build` counts).
    Shaders,
    /// The shader precache before the game starts (`warm` counts; the loading screen can skip it).
    Precache,
    /// Shaders done, the guest cores released: the game runs towards its first frame.
    Launching,
    /// The first guest frame was presented, behind the loading screen that now fades out.
    FirstFrame,
    /// The loading screen is gone and the game is on screen. The emulator writes nothing after it.
    Shown,
}

impl Phase {
    pub fn name(self) -> &'static str {
        match self {
            Phase::Starting => "starting",
            Phase::Loading => "loading",
            Phase::Shaders => "shaders",
            Phase::Precache => "precache",
            Phase::Launching => "launching",
            Phase::FirstFrame => "first-frame",
            Phase::Shown => "shown",
        }
    }

    /// Phases only move forward; the shader build and the precache share a rank, as either can
    /// follow the other.
    fn rank(self) -> u8 {
        match self {
            Phase::Starting => 0,
            Phase::Loading => 1,
            Phase::Shaders | Phase::Precache => 2,
            Phase::Launching => 3,
            Phase::FirstFrame => 4,
            Phase::Shown => 5,
        }
    }
}

/// One line the emulator wrote, read.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Signal {
    /// A `Boot timing:` mark: the phase text before ` at <ms> ms`.
    Mark(String),
    /// `Boot progress: <stage> <done> <total>`; stage is prepare, build, warm or complete.
    Progress { stage: String, done: u64, total: u64 },
}

/// Reads a line of the session output, or the same line from the log (with its log prefix).
pub fn parse_line(line: &str) -> Option<Signal> {
    const MARK: &str = "Boot timing: ";
    const PROGRESS: &str = "Boot progress: ";
    if let Some(i) = line.find(MARK) {
        let rest = &line[i + MARK.len()..];
        let phase = rest[..rest.rfind(" at ")?].trim();
        return (!phase.is_empty()).then(|| Signal::Mark(phase.to_string()));
    }
    let i = line.find(PROGRESS)?;
    let mut it = line[i + PROGRESS.len()..].split_whitespace();
    let stage = it.next()?.to_string();
    let done = it.next()?.parse().ok()?;
    let total = it.next()?.parse().ok()?;
    Some(Signal::Progress { stage, done, total })
}

/// The phase a mark belongs to, by the area its text starts with (the `Mark` and `Span` calls in
/// `src/`: frontend, startup, vulkan, boot, core, nup, romfs, shaders, guest).
fn mark_phase(mark: &str) -> Phase {
    if mark.starts_with("frontend: loading screen hidden") {
        Phase::Shown
    } else if mark.starts_with("frontend: first guest frame presented") {
        Phase::FirstFrame
    } else if mark.starts_with("guest: ") {
        Phase::Launching
    } else if mark.starts_with("shaders: ") {
        Phase::Shaders
    } else if ["boot: ", "core: ", "nup: ", "romfs: "].iter().any(|p| mark.starts_with(p)) {
        Phase::Loading
    } else {
        // frontend, startup and vulkan marks; the Vulkan instance can also be created ahead of
        // the game, and a later mark never moves the phase back.
        Phase::Starting
    }
}

/// Turns the lines into the events the launch screen needs: one when the phase moves forward or
/// the shader counts change, none for the many marks in between.
#[derive(Debug, Default)]
pub struct Tracker {
    last: Option<(Phase, u64, u64)>,
}

impl Tracker {
    pub fn feed(&mut self, line: &str) -> Option<BootProgress> {
        let signal = parse_line(line)?;
        let is_mark = matches!(signal, Signal::Mark(_));
        let (phase, done, total) = match signal {
            Signal::Mark(mark) => (mark_phase(&mark), 0, 0),
            Signal::Progress { stage, done, total } => match stage.as_str() {
                "prepare" => (Phase::Shaders, 0, 0),
                "build" => (Phase::Shaders, done, total),
                "warm" => (Phase::Precache, done, total),
                "complete" => (Phase::Launching, 0, 0),
                _ => return None,
            },
        };
        let done = done.min(total);
        if let Some((last, last_done, last_total)) = self.last {
            // Only progress lines switch between `shaders` and `precache`: a `shaders:` mark
            // after the precache (and the marks reach the pipe before the queued progress lines).
            if phase.rank() < last.rank() || (is_mark && phase.rank() == last.rank()) {
                return None;
            }
            // The same phase again: only new counts are news (a mark inside the shader stage
            // keeps the counts the progress lines gave).
            if phase == last && (total == 0 || (done, total) == (last_done, last_total)) {
                return None;
            }
        }
        self.last = Some((phase, done, total));
        Some(BootProgress { phase: phase.name().to_string(), done, total })
    }
}

/// Reads the session output on its own thread until the emulator exits (it stops writing once the
/// game is shown, but keeps the pipe open), and emits `game://boot`. It reads to the end even after
/// the last event: the emulator writes with blocking calls, so a full pipe would hold it up.
pub fn follow(app: &AppHandle, out: impl Read + Send + 'static, done: Arc<AtomicBool>, timeline: Timeline) {
    let app = app.clone();
    let spawned = std::thread::Builder::new().name("game-boot".into()).spawn(move || {
        let mut reader = BufReader::new(out);
        let mut tracker = Tracker::default();
        let mut phases = PhaseChanges::default();
        let mut buf = Vec::new();
        loop {
            buf.clear();
            // At most MAX_LINE bytes at a time: a longer line is read in pieces, never in one buffer.
            match reader.by_ref().take(MAX_LINE).read_until(b'\n', &mut buf) {
                Ok(0) | Err(_) => break,
                Ok(_) => {}
            }
            let line = String::from_utf8_lossy(&buf);
            if let Some(progress) = tracker.feed(line.trim_end()) {
                if phases.is_new(&progress.phase) {
                    timeline.mark(&format!("boot phase {}", phase_text(&progress)));
                }
                if progress.phase == Phase::Shown.name() {
                    log::info!("The game is on screen");
                    done.store(true, Ordering::Relaxed);
                }
                let _ = app.emit(BOOT_EVENT, progress);
            }
        }
    });
    // Without the thread the pipe closes, and the emulator stops writing on its first failure.
    if let Err(e) = spawned {
        log::warn!("Could not follow the boot: {e}");
    }
}

/// Emits `game://boot` with phase `window` once a window of process `pid` has the foreground,
/// polling until then, until `done` (the game was shown or has exited) or for 15 minutes.
pub fn watch_foreground(app: &AppHandle, pid: u32, done: Arc<AtomicBool>, timeline: Timeline) {
    let app = app.clone();
    let spawned = std::thread::Builder::new().name("game-window".into()).spawn(move || {
        let started = Instant::now();
        while !done.load(Ordering::Relaxed) && started.elapsed() < FOREGROUND_WATCH {
            if foreground_pid() == Some(pid) {
                timeline.mark("emulator window has the foreground");
                let _ = app.emit(BOOT_EVENT, BootProgress { phase: "window".into(), done: 0, total: 0 });
                return;
            }
            std::thread::sleep(FOREGROUND_POLL);
        }
    });
    if let Err(e) = spawned {
        log::warn!("Could not watch for the game window: {e}");
    }
}

/// The process that owns the foreground window, if there is one.
pub(crate) fn foreground_pid() -> Option<u32> {
    use windows::Win32::UI::WindowsAndMessaging::{GetForegroundWindow, GetWindowThreadProcessId};
    let mut pid = 0u32;
    // SAFETY: GetForegroundWindow takes nothing; GetWindowThreadProcessId writes one u32 through a
    // pointer to a local, and returns 0 without writing for a null or stale window.
    unsafe {
        let hwnd = GetForegroundWindow();
        let _ = GetWindowThreadProcessId(hwnd, Some(&mut pid as *mut u32));
    }
    (pid != 0).then_some(pid)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn feed_all(lines: &[&str]) -> Vec<(String, u64, u64)> {
        let mut t = Tracker::default();
        lines.iter().filter_map(|l| t.feed(l)).map(|p| (p.phase, p.done, p.total)).collect()
    }

    fn ev(phase: &str, done: u64, total: u64) -> (String, u64, u64) {
        (phase.to_string(), done, total)
    }

    #[test]
    fn reads_marks_from_the_session_output_and_the_log() {
        assert_eq!(
            parse_line("Boot timing: core: load started at 812.4 ms (+3.1 ms)"),
            Some(Signal::Mark("core: load started".into()))
        );
        assert_eq!(
            parse_line("[  1.2345] Common <Info> common\\boot_timing.cpp:Mark:59: Boot timing: frontend: first guest frame presented at 9001.0 ms (+12.0 ms)"),
            Some(Signal::Mark("frontend: first guest frame presented".into()))
        );
        assert_eq!(
            parse_line("Boot timing: romfs: game RomFS patch done at 2000.0 ms (+1.0 ms), took 300.5 ms"),
            Some(Signal::Mark("romfs: game RomFS patch done".into()))
        );
        // A Span, and a phase whose own text has " at " in it.
        assert_eq!(
            parse_line("Boot timing: startup: Vulkan check skipped at boot at 40.0 ms, took 2.0 ms"),
            Some(Signal::Mark("startup: Vulkan check skipped at boot".into()))
        );
        assert_eq!(
            parse_line("Boot progress: build 120 1346"),
            Some(Signal::Progress { stage: "build".into(), done: 120, total: 1346 })
        );
    }

    #[test]
    fn ignores_other_lines() {
        for line in [
            "",
            "Boot timing: ",
            "Boot timing: no time here",
            "Boot progress: build",
            "Boot progress: build x 10",
            "Booting game: 0400486024DEC000 | Zelda | 1.0.0",
            "warm: pass 1 boot-builds 5% (70/1346) 2.6 s",
        ] {
            assert_eq!(parse_line(line), None, "{line:?}");
        }
    }

    #[test]
    fn a_whole_boot_becomes_few_events() {
        let lines = [
            "Boot timing: frontend: main window shown at 300.0 ms (+10.0 ms)",
            "Boot timing: vulkan: instance creation started ahead of the renderer at 310.0 ms (+10.0 ms)",
            "Boot timing: frontend: content providers and keys ready, game list scan started at 400.0 ms (+90.0 ms)",
            "Boot timing: boot: game boot requested at 420.0 ms (+20.0 ms)",
            "Boot timing: core: load started at 500.0 ms (+80.0 ms)",
            "Boot timing: vulkan: instance taken from the early creation at 510.0 ms (+10.0 ms), took 1.0 ms",
            "Boot timing: core: load done at 1500.0 ms (+990.0 ms)",
            "Boot progress: prepare 0 0",
            "Boot timing: shaders: disk cache load started at 1600.0 ms (+100.0 ms)",
            "Boot progress: build 0 200",
            "Boot timing: shaders: disk cache read, 0 of 200 pipelines built so far at 1700.0 ms (+100.0 ms)",
            "Boot progress: build 100 200",
            "Boot progress: build 200 200",
            "Boot timing: shaders: boot builds done at 3000.0 ms (+1300.0 ms)",
            "Boot progress: complete 0 0",
            "Boot timing: guest: GPU started, guest cores released at 3100.0 ms (+100.0 ms)",
            "Boot timing: frontend: first guest frame presented at 4000.0 ms (+900.0 ms)",
            "Boot timing: frontend: loading screen hidden, game shown at 4500.0 ms (+500.0 ms)",
        ];
        assert_eq!(
            feed_all(&lines),
            [
                ev("starting", 0, 0),
                ev("loading", 0, 0),
                ev("shaders", 0, 0),
                ev("shaders", 0, 200),
                ev("shaders", 100, 200),
                ev("shaders", 200, 200),
                ev("launching", 0, 0),
                ev("first-frame", 0, 0),
                ev("shown", 0, 0),
            ]
        );
    }

    #[test]
    fn the_precache_and_the_build_can_follow_each_other() {
        let lines = [
            "Boot progress: warm 10 400",
            "Boot progress: warm 10 400",
            "Boot progress: warm 400 400",
            "Boot progress: build 5 20",
            "Boot timing: shaders: boot builds done at 3000.0 ms (+1300.0 ms)",
            "Boot progress: complete 0 0",
        ];
        assert_eq!(
            feed_all(&lines),
            [ev("precache", 10, 400), ev("precache", 400, 400), ev("shaders", 5, 20), ev("launching", 0, 0)]
        );
    }

    #[test]
    fn a_shaders_mark_after_the_precache_keeps_the_precache() {
        let lines = [
            "Boot progress: warm 10 400",
            "Boot progress: warm 400 400",
            "Boot timing: shaders: LoadPipelinesV2 done at 3000.0 ms (+1.0 ms)",
            "Boot progress: complete 0 0",
        ];
        assert_eq!(feed_all(&lines), [ev("precache", 10, 400), ev("precache", 400, 400), ev("launching", 0, 0)]);
    }

    #[test]
    fn phases_never_go_back_and_counts_stay_within_the_total() {
        let lines = [
            "Boot timing: core: load done at 1500.0 ms (+990.0 ms)",
            "Boot timing: vulkan: device created at 1600.0 ms (+100.0 ms), took 50.0 ms",
            "Boot progress: build 250 200",
            "Boot timing: boot: launch finished, the loading screen waits for the game at 1700.0 ms (+100.0 ms)",
            "Boot progress: unknown 1 2",
        ];
        assert_eq!(feed_all(&lines), [ev("loading", 0, 0), ev("shaders", 200, 200)]);
    }
}
