// Guarana Launcher: the Windows installer, first-run setup, launcher, updater and doctor for
// Guarana. GPL-3.0-or-later.

// No console window in release builds.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

#[cfg(not(windows))]
compile_error!("Guarana Launcher is a Windows application");

mod args;
mod autopilot;
mod boot;
mod booster;
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
mod notify;
mod open;
mod paths;
mod profiles;
mod report;
mod share;
mod state;
mod system;
mod taskbar;
mod types;
mod util;
mod warm;

use tauri::Manager;
use windows::Win32::System::Diagnostics::Debug::{SetErrorMode, SEM_FAILCRITICALERRORS, SEM_NOOPENFILEERRORBOX};

fn main() {
    // The Game Booster's elevated helper: the launcher started again with --booster-helper through
    // ShellExecuteEx "runas". It does only its small elevated task and exits, before Tauri or the
    // single-instance plugin (which would otherwise treat it as a second launcher) start.
    let args: Vec<String> = std::env::args().collect();
    if args.get(1).map(String::as_str) == Some(booster::HELPER_FLAG) {
        std::process::exit(booster::run_helper(&args[2..]));
    }

    logging::init();
    log::info!("Guarana Launcher {} starting", env!("CARGO_PKG_VERSION"));
    // Scanning drives must never pop up "insert a disk" dialogs.
    // SAFETY: sets this process's error mode; no pointers involved.
    unsafe {
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    }

    let result = tauri::Builder::default()
        // First, so a second start only hands its command line to the running launcher: a game
        // file goes to `open`, a guarana:// link to the deep-link plugin (the `deep-link` feature).
        .plugin(tauri_plugin_single_instance::init(|app, args, cwd| {
            if let Some(window) = app.get_webview_window("main") {
                let _ = window.unminimize();
                let _ = window.show();
                let _ = window.set_focus();
            }
            open::from_args(app, &args, std::path::Path::new(&cwd));
        }))
        .plugin(tauri_plugin_deep_link::init())
        .plugin(tauri_plugin_notification::init())
        .manage(state::AppState::new())
        .setup(|app| {
            let handle = app.handle().clone();
            open::init(&handle);
            controllers::start(handle.clone());
            // Community best settings: refreshed in the background at most once a day.
            tauri::async_runtime::spawn(async {
                let _ = community::load(false).await;
            });
            // A boosted session left behind by a crash: put its system changes back.
            let recover = handle.clone();
            tauri::async_runtime::spawn_blocking(move || booster::recover_after_crash(&recover));
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
            commands::get_share_status,
            commands::share_open,
            commands::share_dismiss,
            commands::share_set_prompt,
            commands::take_launch_request,
            commands::get_notifications,
            commands::set_notifications,
            commands::get_booster,
            commands::set_booster_level,
            commands::booster_undo_all,
        ])
        .run(tauri::generate_context!());
    if let Err(e) = result {
        log::error!("Guarana Launcher stopped: {e}");
    }
}
