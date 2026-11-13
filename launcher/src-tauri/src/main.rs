// Guarana Launcher: the Windows installer, first-run setup, launcher, updater and doctor for
// Guarana. GPL-3.0-or-later.

// No console window in release builds.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

#[cfg(not(windows))]
compile_error!("Guarana Launcher is a Windows application");

mod autopilot;
mod commands;
mod community;
mod controllers;
mod credits;
mod doctor;
mod ini;
mod install;
mod keys;
mod launch;
mod locate;
mod logging;
mod modes;
mod paths;
mod profiles;
mod report;
mod state;
mod system;
mod types;
mod util;
mod warm;

use tauri::Manager;
use windows::Win32::System::Diagnostics::Debug::{SetErrorMode, SEM_FAILCRITICALERRORS, SEM_NOOPENFILEERRORBOX};

fn main() {
    logging::init();
    log::info!("Guarana Launcher {} starting", env!("CARGO_PKG_VERSION"));
    // Scanning drives must never pop up "insert a disk" dialogs.
    // SAFETY: sets this process's error mode; no pointers involved.
    unsafe {
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    }

    let result = tauri::Builder::default()
        // First, so a second start only brings the running launcher to the front.
        .plugin(tauri_plugin_single_instance::init(|app, _args, _cwd| {
            if let Some(window) = app.get_webview_window("main") {
                let _ = window.unminimize();
                let _ = window.show();
                let _ = window.set_focus();
            }
        }))
        .manage(state::AppState::new())
        .setup(|app| {
            let handle = app.handle().clone();
            controllers::start(handle.clone());
            // Community best settings: refreshed in the background at most once a day.
            tauri::async_runtime::spawn(async {
                let _ = community::load(false).await;
            });
            tauri::async_runtime::spawn(async move {
                install::ensure_bundled(&handle).await;
                warm::auto_after_driver_change(handle).await;
            });
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            commands::detect_system,
            commands::list_profiles,
            commands::get_state,
            commands::check_updates,
            commands::install_version,
            commands::set_current_version,
            commands::locate_files,
            commands::import_keys,
            commands::add_game_folder,
            commands::list_games,
            commands::apply_profile,
            commands::run_doctor,
            commands::apply_fix,
            commands::prefetch_caches,
            commands::launch_game,
            commands::collect_report,
            commands::open_path,
            commands::open_url,
            commands::list_controllers,
            commands::warm_shaders,
            commands::stop_warm_shaders,
            commands::pick_folder,
            commands::get_mode,
            commands::set_mode,
            commands::autopilot_undo,
            commands::autopilot_enable,
            commands::ask_ai,
            commands::get_credits,
        ])
        .run(tauri::generate_context!());
    if let Err(e) = result {
        log::error!("Guarana Launcher stopped: {e}");
    }
}
