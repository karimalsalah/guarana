//! `list_controllers` and `controller://changed`: a thread owns the gilrs context (Windows.Gaming.Input),
//! keeps the connected pads in `AppState::controllers` and emits the whole list when it changes.

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

pub fn start(app: AppHandle) {
    let spawned = std::thread::Builder::new().name("controllers".into()).spawn(move || run(app));
    if let Err(e) = spawned {
        log::warn!("Controller watcher not started: {e}");
    }
}

fn run(app: AppHandle) {
    let mut gilrs = match Gilrs::new() {
        Ok(g) => g,
        Err(gilrs::Error::NotImplemented(g)) => g,
        Err(e) => {
            log::warn!("Controllers unavailable: {e}");
            return;
        }
    };
    let store = app.state::<AppState>().controllers.clone();
    let mut last = snapshot(&gilrs);
    if let Ok(mut s) = store.lock() {
        *s = last.clone();
    }
    let mut ticks: u32 = 0;
    loop {
        let mut changed = false;
        while let Some(ev) = gilrs.next_event() {
            if matches!(ev.event, EventType::Connected | EventType::Disconnected) {
                changed = true;
            }
        }
        ticks = ticks.wrapping_add(1);
        // Battery levels move slowly: look every 5 s besides connects and disconnects.
        if changed || ticks % 20 == 0 {
            let now = snapshot(&gilrs);
            if now != last {
                if let Ok(mut s) = store.lock() {
                    *s = now.clone();
                }
                let _ = app.emit(EVENT, &now);
                last = now;
            }
        }
        std::thread::sleep(Duration::from_millis(250));
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
}
