//! `list_controllers` and `controller://changed`: a thread owns the gilrs context (Windows.Gaming.Input),
//! keeps the connected pads in `AppState::controllers` and emits the whole list when it changes.
//!
//! With `EDEN_NX2_LAUNCHER_PAD_IDLE` on (default off) the thread stops while a game runs: it drops
//! the gilrs context, which ends gilrs' own polling thread (gilrs-core's Windows backend reads
//! every pad about every 8 ms on a thread of its own), and waits until the game has exited. Then
//! it opens a new context, reads the pads once and emits the list if it changed meanwhile.

use std::sync::{Condvar, Mutex, MutexGuard};
use std::time::Duration;

use gilrs::{EventType, Gilrs, PowerInfo};
use tauri::{AppHandle, Emitter, Manager};

use crate::state::AppState;
use crate::types::{Controller, ControllerKind};

pub const EVENT: &str = "controller://changed";

fn kind_of(vendor: Option<u16>, name: &str) -> ControllerKind {
    match vendor {
        Some(0x054c) => return ControllerKind::Playstation,
        Some(0x045e) => return ControllerKind::Xbox,
        Some(0x057e) => return ControllerKind::Switch,
        _ => {}
    }
    let n = name.to_ascii_lowercase();
    if n.contains("xbox") || n.contains("xinput") {
        ControllerKind::Xbox
    } else if n.contains("dualsense") || n.contains("dualshock") || n.contains("playstation") || n.contains("ps4") || n.contains("ps5") {
        ControllerKind::Playstation
    } else if n.contains("pro controller") || n.contains("joy-con") || n.contains("nintendo") {
        ControllerKind::Switch
    } else {
        ControllerKind::Generic
    }
}

fn snapshot(gilrs: &Gilrs) -> Vec<Controller> {
    gilrs
        .gamepads()
        .map(|(id, pad)| Controller {
            id: usize::from(id).to_string(),
            name: pad.name().to_string(),
            kind: kind_of(pad.vendor_id(), pad.name()),
            connected: pad.is_connected(),
            battery: match pad.power_info() {
                PowerInfo::Discharging(p) | PowerInfo::Charging(p) => Some(p.min(100)),
                PowerInfo::Charged => Some(100),
                PowerInfo::Wired | PowerInfo::Unknown => None,
            },
        })
        .collect()
}

/// The switch that stops controller polling while a game runs. Read from the launcher's own
/// environment at every launch.
pub const PAD_IDLE_SWITCH: &str = "EDEN_NX2_LAUNCHER_PAD_IDLE";

/// A switch's value as the emulator reads its `EDEN_NX2_*` switches, `strtoul(value, nullptr, 10) != 0`:
/// unset, empty, `0`, `false` and anything that does not start with a number are off.
pub fn switch_on(value: Option<&str>) -> bool {
    let Some(value) = value else { return false };
    let value = value.trim_start_matches(|c: char| matches!(c, ' ' | '\t' | '\n' | '\x0b' | '\x0c' | '\r'));
    let value = value.strip_prefix(['+', '-']).unwrap_or(value);
    // Any digit but 0 makes the number non-zero, an overflow included (strtoul gives ULONG_MAX).
    value.chars().take_while(char::is_ascii_digit).any(|c| c != '0')
}

fn pad_idle_enabled() -> bool {
    let value = std::env::var_os(PAD_IDLE_SWITCH);
    switch_on(value.as_deref().and_then(|v| v.to_str()))
}

/// Whether the controller thread is to stand still. A flag with a condition variable: the thread
/// sleeps its 250 ms on it and is woken at once when a game starts, and waits on it, using no
/// processor time, until the game has exited.
struct Gate {
    paused: Mutex<bool>,
    changed: Condvar,
}

impl Gate {
    const fn new() -> Self {
        Self { paused: Mutex::new(false), changed: Condvar::new() }
    }

    fn lock(&self) -> MutexGuard<'_, bool> {
        self.paused.lock().unwrap_or_else(|e| e.into_inner())
    }

    fn set(&self, paused: bool) {
        let mut guard = self.lock();
        if *guard != paused {
            *guard = paused;
            self.changed.notify_all();
        }
    }

    fn is_paused(&self) -> bool {
        *self.lock()
    }

    /// Sleeps for `timeout`, or less when the gate closes meanwhile. Returns whether it is closed.
    fn sleep_unless_paused(&self, timeout: Duration) -> bool {
        let guard = self.lock();
        let (guard, _) = self
            .changed
            .wait_timeout_while(guard, timeout, |paused| !*paused)
            .unwrap_or_else(|e| e.into_inner());
        *guard
    }

    /// Returns once the gate is open (at once when it already is).
    fn wait_until_resumed(&self) {
        let guard = self.lock();
        let _open = self.changed.wait_while(guard, |paused| *paused).unwrap_or_else(|e| e.into_inner());
    }
}

static GATE: Gate = Gate::new();

/// Holds the controller thread still until it is dropped: dropped when the game exits, or on any
/// way out of the launch (a panic included), so the pads can never stay unpolled.
pub struct IdleGuard {
    gate: &'static Gate,
}

impl IdleGuard {
    fn new(gate: &'static Gate) -> Self {
        gate.set(true);
        Self { gate }
    }
}

impl Drop for IdleGuard {
    fn drop(&mut self) {
        self.gate.set(false);
    }
}

/// Called right after the emulator was started: with `EDEN_NX2_LAUNCHER_PAD_IDLE` on, stops the
/// launcher's controller polling until the returned guard is dropped. `None` (nothing changes)
/// with the switch off.
pub fn idle_while_game_runs() -> Option<IdleGuard> {
    if !pad_idle_enabled() {
        return None;
    }
    log::info!("Controller polling stopped while the game runs ({PAD_IDLE_SWITCH})");
    Some(IdleGuard::new(&GATE))
}

pub fn start(app: AppHandle) {
    let spawned = std::thread::Builder::new().name("controllers".into()).spawn(move || run(app));
    if let Err(e) = spawned {
        log::warn!("Controller watcher not started: {e}");
    }
}

fn open() -> Option<Gilrs> {
    match Gilrs::new() {
        Ok(g) => Some(g),
        Err(gilrs::Error::NotImplemented(g)) => Some(g),
        Err(e) => {
            log::warn!("Controllers unavailable: {e}");
            None
        }
    }
}

/// Reads the pads; stores and emits the list when it differs from `last`.
fn publish(app: &AppHandle, store: &Mutex<Vec<Controller>>, gilrs: &Gilrs, last: &mut Vec<Controller>) {
    let now = snapshot(gilrs);
    if now != *last {
        if let Ok(mut s) = store.lock() {
            *s = now.clone();
        }
        let _ = app.emit(EVENT, &now);
        *last = now;
    }
}

fn run(app: AppHandle) {
    let Some(mut gilrs) = open() else { return };
    let store = app.state::<AppState>().controllers.clone();
    let mut last = snapshot(&gilrs);
    if let Ok(mut s) = store.lock() {
        *s = last.clone();
    }
    let mut ticks: u32 = 0;
    loop {
        if GATE.is_paused() {
            // A game runs (EDEN_NX2_LAUNCHER_PAD_IDLE): drop the context, which stops gilrs' own
            // polling thread, and wait. Pads that came or went meanwhile are reported on return.
            drop(gilrs);
            GATE.wait_until_resumed();
            let Some(reopened) = open() else { return };
            gilrs = reopened;
            publish(&app, &store, &gilrs, &mut last);
            ticks = 0;
            log::info!("Controller polling resumed");
        }
        let mut changed = false;
        while let Some(ev) = gilrs.next_event() {
            if matches!(ev.event, EventType::Connected | EventType::Disconnected) {
                changed = true;
            }
        }
        ticks = ticks.wrapping_add(1);
        // Battery levels move slowly: look every 5 s besides connects and disconnects.
        if changed || ticks % 20 == 0 {
            publish(&app, &store, &gilrs, &mut last);
        }
        GATE.sleep_unless_paused(Duration::from_millis(250));
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn kinds() {
        assert_eq!(kind_of(Some(0x054c), "Wireless Controller"), ControllerKind::Playstation);
        assert_eq!(kind_of(Some(0x045e), "Controller"), ControllerKind::Xbox);
        assert_eq!(kind_of(Some(0x057e), "Pro Controller"), ControllerKind::Switch);
        assert_eq!(kind_of(None, "Xbox Wireless Controller"), ControllerKind::Xbox);
        assert_eq!(kind_of(Some(0x2dc8), "8BitDo"), ControllerKind::Generic);
    }

    #[test]
    fn the_switch_reads_like_strtoul() {
        for off in [None, Some(""), Some("0"), Some("00"), Some("false"), Some("true"), Some("off"), Some("x1"), Some("0x1"), Some("-0"), Some(" 0"), Some("+")] {
            assert!(!switch_on(off), "{off:?}");
        }
        for on in [Some("1"), Some("2"), Some(" 1"), Some("\t1"), Some("+1"), Some("-1"), Some("01"), Some("1abc"), Some("99999999999999999999999")] {
            assert!(switch_on(on), "{on:?}");
        }
    }

    #[test]
    fn a_closed_gate_cuts_the_sleep_short() {
        let gate = Gate::new();
        assert!(!gate.is_paused());
        assert!(!gate.sleep_unless_paused(Duration::from_millis(5)));
        gate.set(true);
        assert!(gate.is_paused());
        // Already closed: returns at once, far under the timeout.
        assert!(gate.sleep_unless_paused(Duration::from_secs(60)));
        gate.set(true);
        assert!(gate.is_paused(), "closing twice is the same");
        gate.set(false);
        assert!(!gate.is_paused());
    }

    #[test]
    fn pausing_wakes_a_sleeper_and_resuming_wakes_a_waiter() {
        let gate = Gate::new();
        std::thread::scope(|s| {
            let sleeper = s.spawn(|| gate.sleep_unless_paused(Duration::from_secs(60)));
            std::thread::sleep(Duration::from_millis(20));
            gate.set(true);
            assert!(sleeper.join().unwrap(), "woken by the pause");

            let waiter = s.spawn(|| gate.wait_until_resumed());
            std::thread::sleep(Duration::from_millis(20));
            gate.set(false);
            waiter.join().unwrap();
        });
        assert!(!gate.is_paused());
        // Open already: waiting returns at once.
        gate.wait_until_resumed();
    }

    #[test]
    fn the_guard_pauses_until_it_is_dropped() {
        static TEST_GATE: Gate = Gate::new();
        assert!(!TEST_GATE.is_paused());
        let guard = IdleGuard::new(&TEST_GATE);
        assert!(TEST_GATE.is_paused());
        drop(guard);
        assert!(!TEST_GATE.is_paused());
    }
}
