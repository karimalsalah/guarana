//! The launch timeline in `launcher.log`: one line for each step between Play and the game on
//! screen, each with the milliseconds since Play was pressed (the moment `launch_game` reached
//! the backend), so a slow start can be placed: before the process (settings, the Game Booster),
//! in the emulator's boot phases, or after the game was shown.
//!
//! ```text
//! 2026-11-14T09:41:07.120Z INFO  guarana_launcher::timeline: Launch timeline: Play pressed at +0.0 ms
//! 2026-11-14T09:41:07.188Z INFO  guarana_launcher::timeline: Launch timeline: command built at +68.4 ms
//! ```

use std::time::{Duration, Instant};

use crate::types::BootProgress;

/// When Play was pressed. Copied into the threads that log later steps.
#[derive(Debug, Clone, Copy)]
pub struct Timeline {
    start: Instant,
}

impl Timeline {
    pub fn start() -> Self {
        Self { start: Instant::now() }
    }

    /// Logs `what` with the time since Play.
    pub fn mark(&self, what: &str) {
        log::info!("{}", line(what, self.start.elapsed()));
    }
}

/// The text of one timeline line.
pub fn line(what: &str, since_start: Duration) -> String {
    format!("Launch timeline: {what} at +{:.1} ms", since_start.as_secs_f64() * 1000.0)
}

/// A `game://boot` event as words: the phase, with its counts where it has them (`shaders 120/1346`).
pub fn phase_text(progress: &BootProgress) -> String {
    if progress.total > 0 {
        format!("{} {}/{}", progress.phase, progress.done, progress.total)
    } else {
        progress.phase.clone()
    }
}

/// Tells when a boot phase is a new one: the shader counts move many times inside one phase, and
/// only the phase's first event belongs in the timeline.
#[derive(Debug, Default)]
pub struct PhaseChanges {
    last: Option<String>,
}

impl PhaseChanges {
    pub fn is_new(&mut self, phase: &str) -> bool {
        if self.last.as_deref() == Some(phase) {
            return false;
        }
        self.last = Some(phase.to_string());
        true
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn progress(phase: &str, done: u64, total: u64) -> BootProgress {
        BootProgress { phase: phase.to_string(), done, total }
    }

    #[test]
    fn lines_carry_the_time_since_play_in_milliseconds() {
        assert_eq!(line("Play pressed", Duration::ZERO), "Launch timeline: Play pressed at +0.0 ms");
        assert_eq!(
            line("command built", Duration::from_micros(68_400)),
            "Launch timeline: command built at +68.4 ms"
        );
        assert_eq!(
            line("boot phase shown", Duration::from_micros(1_234_500)),
            "Launch timeline: boot phase shown at +1234.5 ms"
        );
    }

    #[test]
    fn phases_read_with_their_counts() {
        assert_eq!(phase_text(&progress("loading", 0, 0)), "loading");
        assert_eq!(phase_text(&progress("shaders", 120, 1346)), "shaders 120/1346");
        assert_eq!(phase_text(&progress("precache", 0, 400)), "precache 0/400");
    }

    #[test]
    fn only_a_new_phase_is_news() {
        let mut p = PhaseChanges::default();
        assert!(p.is_new("starting"));
        assert!(!p.is_new("starting"));
        assert!(p.is_new("shaders"));
        assert!(!p.is_new("shaders"));
        assert!(!p.is_new("shaders"));
        assert!(p.is_new("precache"));
        assert!(p.is_new("shaders"), "the build can follow the precache");
    }
}
