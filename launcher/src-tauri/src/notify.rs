//! Windows notifications (tauri-plugin-notification) when shaders are ready or an update is
//! installed. Off until the user turns them on (Home > Advanced), and not sent while the launcher
//! is the active window, which already shows the result.

use serde::Serialize;
use tauri::{AppHandle, Manager};
use tauri_plugin_notification::NotificationExt;

use crate::state::AppState;
use crate::util::CmdResult;
use crate::warm::WarmResult;

/// `get_notifications` / `set_notifications`.
#[derive(Debug, Clone, Serialize)]
pub struct Notifications {
    pub enabled: bool,
}

pub fn status(app: &AppHandle) -> Notifications {
    Notifications { enabled: app.state::<AppState>().load().notifications }
}

pub fn set_enabled(app: &AppHandle, enabled: bool) -> CmdResult<Notifications> {
    app.state::<AppState>().update(|s| s.notifications = enabled)?;
    if enabled {
        // Shown even in front: it answers the switch and makes Windows list Guarana's notifications.
        show(app, "Notifications are on", "Guarana tells you here when shaders are ready or an update is installed.");
    }
    Ok(status(app))
}

fn show(app: &AppHandle, title: &str, body: &str) {
    if let Err(e) = app.notification().builder().title(title).body(body).show() {
        log::warn!("Notification not shown: {e}");
    }
}

fn send(app: &AppHandle, title: &str, body: &str) {
    if !app.state::<AppState>().load().notifications {
        return;
    }
    let in_front = app.get_webview_window("main").and_then(|w| w.is_focused().ok()).unwrap_or(false);
    if !in_front {
        show(app, title, body);
    }
}

fn game_title(app: &AppHandle, title_id: &str) -> String {
    app.state::<AppState>()
        .load()
        .games
        .iter()
        .find(|g| g.title_id.eq_ignore_ascii_case(title_id))
        .map(|g| g.title.clone())
        .unwrap_or_else(|| title_id.to_ascii_uppercase())
}

fn minutes(seconds: f64) -> String {
    let s = seconds.max(0.0).round() as u64;
    if s < 60 {
        format!("{s} s")
    } else {
        format!("{} min {} s", s / 60, s % 60)
    }
}

/// After one game's shader warm-up ("Prepare shaders", or the autopilot's).
pub fn warm_finished(app: &AppHandle, r: &WarmResult) {
    let game = game_title(app, &r.title_id);
    let (title, body) = match r.status.as_str() {
        "done" => ("Shaders are ready", format!("{game}: {} built in {}.", r.built, minutes(r.seconds))),
        "incomplete" => ("Shaders partly ready", format!("{game}: {} left. Prepare shaders again to finish.", r.left)),
        "nothing built" => ("Shader warm-up finished", format!("{game}: nothing could be built ahead. It builds shaders as you play.")),
        "failed" => ("Shader warm-up failed", format!("{game}: {}", r.message.as_deref().unwrap_or("guarana-cli stopped"))),
        _ => return,
    };
    send(app, title, &body);
}

/// After the automatic warm-up of every cached game for a new GPU driver.
pub fn driver_warm_finished(app: &AppHandle, ready: usize) {
    if ready > 0 {
        let body = if ready == 1 { "1 game has its shaders ready again.".to_string() } else { format!("{ready} games have their shaders ready again.") };
        send(app, "Shaders rebuilt for your GPU driver", &body);
    }
}

pub fn update_installed(app: &AppHandle, version: &str) {
    send(app, &format!("Guarana {version} is installed"), "Your next game starts with it.");
}
