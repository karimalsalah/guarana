//! `run_doctor` / `apply_fix`. Checks only read. The fixes open an official Windows or vendor
//! page, or act inside Guarana's own folders (stale shader-cache temp files, installing an
//! update); nothing else on the system is changed.

use std::fs;
use std::path::Path;
use std::time::{Duration, SystemTime};

use tauri::{AppHandle, Emitter, Manager};

use crate::state::{self, AppState};
use crate::types::{Check, CheckStatus, Progress, SystemInfo, Vendor};
use crate::util::{self, CmdResult};
use crate::{install, locate, paths, system};

pub const EVENT: &str = "doctor://progress";

/// Oldest Intel driver known to run Guarana well.
const INTEL_KNOWN_GOOD: (u32, u32, u32, u32) = (32, 0, 101, 9175);
const INTEL_DRIVER_PAGE: &str = "https://www.intel.com/content/www/us/en/download/785597/intel-arc-iris-xe-graphics-windows.html";
const AMD_DRIVER_PAGE: &str = "https://www.amd.com/en/support/download/drivers.html";
const NVIDIA_DRIVER_PAGE: &str = "https://www.nvidia.com/en-us/drivers/";
const VC_REDIST_PAGE: &str = "https://learn.microsoft.com/cpp/windows/latest-supported-vc-redist";
const HDR_HELP_PAGE: &str = "https://support.microsoft.com/windows/hdr-settings-in-windows-2d767185-38ec-7fdc-6f97-bbc6c5ef24e6";

/// In report order. `emulator_version` asks GitHub, so it runs alongside the others.
const CHECKS: [(&str, &str); 18] = [
    ("emulator_version", "Guarana is up to date"),
    ("gpu_driver", "GPU driver"),
    ("vulkan", "Vulkan driver"),
    ("vc_redist", "Visual C++ 2015-2022 runtime (x64)"),
    ("avx2", "CPU supports AVX2"),
    ("memory", "Memory and page file"),
    ("disk", "Free disk space for caches"),
    ("power", "Power source and power mode"),
    ("hags", "Hardware-accelerated GPU scheduling"),
    ("intel_shared_memory", "Intel Shared GPU Memory Override"),
    ("windows_hdr", "Windows HDR"),
    ("defender", "Microsoft Defender and the shader cache"),
    ("shader_tmp", "Shader cache temporary files"),
    ("keys", "Keys"),
    ("firmware", "Firmware"),
    ("game_folder", "Game folder"),
    ("ocarina", "The Legend of Zelda: Ocarina of Time"),
    ("controller", "Controller connected"),
];

/// fix id -> (check id it fixes, what it opens or does).
const FIXES: [(&str, &str); 14] = [
    ("install_update", "emulator_version"),
    ("open_driver_page", "gpu_driver"),
    ("open_vulkan_driver_page", "vulkan"),
    ("open_vc_redist", "vc_redist"),
    ("install_vc_redist", "vc_redist"),
    ("open_memory_settings", "memory"),
    ("open_storage_settings", "disk"),
    ("open_power_settings", "power"),
    ("open_graphics_settings", "hags"),
    ("open_intel_graphics", "intel_shared_memory"),
    ("open_hdr_help", "windows_hdr"),
    ("open_windows_security", "defender"),
    ("delete_shader_tmp", "shader_tmp"),
    ("open_bluetooth_settings", "controller"),
];

fn title_of(id: &str) -> &'static str {
    CHECKS.iter().find(|(i, _)| *i == id).map(|(_, t)| *t).unwrap_or("Check")
}

fn check(id: &str, status: CheckStatus, detail: impl Into<String>) -> Check {
    Check::new(id, title_of(id), status, detail)
}

fn driver_page(vendor: Vendor) -> &'static str {
    match vendor {
        Vendor::Intel => INTEL_DRIVER_PAGE,
        Vendor::Amd => AMD_DRIVER_PAGE,
        Vendor::Nvidia => NVIDIA_DRIVER_PAGE,
        Vendor::Other => INTEL_DRIVER_PAGE,
    }
}

fn parse_version4(v: &str) -> Option<(u32, u32, u32, u32)> {
    let mut it = v.split('.').map(|p| p.trim().parse::<u32>());
    Some((it.next()?.ok()?, it.next()?.ok()?, it.next()?.ok()?, it.next()?.ok()?))
}

/// Days since a registry `DriverDate` (`M-D-YYYY`).
fn driver_age_days(date: &str) -> Option<u64> {
    let mut it = date.split(['-', '/']).map(|p| p.trim().parse::<i64>());
    let (m, d, y) = (it.next()?.ok()?, it.next()?.ok()?, it.next()?.ok()?);
    if !(1..=12).contains(&m) || !(1..=31).contains(&d) || y < 2000 {
        return None;
    }
    // Days from civil (H. Hinnant), then the difference to today.
    let (yy, mm) = if m <= 2 { (y - 1, m + 9) } else { (y, m - 3) };
    let era = if yy >= 0 { yy } else { yy - 399 } / 400;
    let yoe = yy - era * 400;
    let doy = (153 * mm + 2) / 5 + d - 1;
    let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    let days = era * 146_097 + doe - 719_468;
    let now = SystemTime::now().duration_since(SystemTime::UNIX_EPOCH).ok()?.as_secs() as i64 / 86_400;
    (now >= days).then(|| (now - days) as u64)
}

fn gpu_driver(sys: &SystemInfo) -> Check {
    let Some(gpu) = sys.primary() else {
        return check("gpu_driver", CheckStatus::Fail, "No GPU found through DirectX.");
    };
    let page_fix = |c: Check| c.with_fix("open_driver_page", "Open the driver download page");
    let version = if gpu.driver_version.is_empty() { "unknown".to_string() } else { gpu.driver_version.clone() };
    let age = gpu.driver_date.as_deref().and_then(driver_age_days);
    match gpu.vendor {
        Vendor::Intel => match parse_version4(&gpu.driver_version) {
            Some(v) if v >= INTEL_KNOWN_GOOD => {
                check("gpu_driver", CheckStatus::Ok, format!("{}: driver {version}, at or above the known-good 32.0.101.9175.", gpu.name))
            }
            Some(_) => page_fix(check(
                "gpu_driver",
                CheckStatus::Warn,
                format!("{}: driver {version} is older than the known-good 32.0.101.9175. Update from Intel's site.", gpu.name),
            )),
            None => page_fix(check("gpu_driver", CheckStatus::Info, format!("{}: driver {version}.", gpu.name))),
        },
        _ => match age {
            Some(days) if days > 365 => page_fix(check(
                "gpu_driver",
                CheckStatus::Warn,
                format!("{}: driver {version} is {} months old. A newer one usually fixes Vulkan problems.", gpu.name, days / 30),
            )),
            _ => page_fix(check("gpu_driver", CheckStatus::Ok, format!("{}: driver {version}.", gpu.name))),
        },
    }
}

fn vulkan(sys: &SystemInfo) -> Check {
    let loader = system::vulkan_loader_present();
    let gpu = sys.primary();
    let icd = gpu.map(|g| g.vulkan).unwrap_or(false);
    let fix = |c: Check| c.with_fix("open_vulkan_driver_page", "Get the GPU driver");
    match (loader, icd) {
        (true, true) => check("vulkan", CheckStatus::Ok, "The Vulkan loader and the GPU's Vulkan driver are installed."),
        (false, _) => fix(check(
            "vulkan",
            CheckStatus::Fail,
            "vulkan-1.dll is missing. The GPU driver installs it: install the vendor's driver.",
        )),
        (true, false) => fix(check(
            "vulkan",
            CheckStatus::Fail,
            "No Vulkan driver is registered for the GPU. Install the vendor's own driver (not the one from Windows Update).",
        )),
    }
}

fn vc_redist() -> Check {
    let vc = system::vc_redist();
    // The installer ships in the Guarana folder: one click installs it (Windows asks for permission).
    let fix = |c: Check| match locate_vc_redist() {
        Some(_) => c.with_fix("install_vc_redist", "Install it (Windows will ask to allow it)"),
        None => c.with_fix("open_vc_redist", "Open Microsoft's download page"),
    };
    match (vc.installed, vc.version) {
        (false, _) => fix(check("vc_redist", CheckStatus::Fail, "Not installed. Guarana needs the x64 Visual C++ 2015-2022 runtime.")),
        // Builds from Visual Studio 17.10 and later need runtime 14.40 or newer.
        (true, Some((maj, min, bld))) if maj == 14 && min < 40 => fix(check(
            "vc_redist",
            CheckStatus::Warn,
            format!("Version {maj}.{min}.{bld} is older than 14.40, which current builds need. Install the latest."),
        )),
        (true, Some((maj, min, bld))) => check("vc_redist", CheckStatus::Ok, format!("Version {maj}.{min}.{bld}.")),
        (true, None) => check("vc_redist", CheckStatus::Ok, "Installed."),
    }
}

/// `vc_redist.x64.exe` as shipped in the installed Guarana folder: beside the launcher, in the
/// launcher's data folder, or in the current (or any installed) emulator version folder.
pub fn locate_vc_redist() -> Option<std::path::PathBuf> {
    const NAME: &str = "vc_redist.x64.exe";
    let mut dirs: Vec<std::path::PathBuf> = Vec::new();
    if let Some(cur) = state::read_current() {
        let p = std::path::PathBuf::from(cur.path);
        dirs.push(p.clone());
        if let Some(parent) = p.parent() {
            dirs.push(parent.to_path_buf());
        }
    }
    if let Some(dir) = std::env::current_exe().ok().and_then(|e| e.parent().map(|p| p.to_path_buf())) {
        dirs.push(dir.join("resources"));
        dirs.push(dir);
    }
    dirs.push(paths::install_dir());
    if let Ok(entries) = fs::read_dir(paths::versions_dir()) {
        for e in entries.flatten() {
            dirs.push(e.path());
            if let Ok(inner) = fs::read_dir(e.path()) {
                dirs.extend(inner.flatten().filter(|i| i.path().is_dir()).map(|i| i.path()));
            }
        }
    }
    dirs.into_iter().map(|d| d.join(NAME)).find(|p| p.is_file() && p.metadata().map(|m| m.len() > (1 << 20)).unwrap_or(false))
}

/// What the runtime installer's exit code means: Ok(message) or Err(message).
fn vc_redist_outcome(code: i32) -> Result<&'static str, String> {
    match code {
        0 => Ok("Installed."),
        3010 => Ok("Installed. Restart Windows to finish."),
        1638 => Ok("A newer runtime is already installed."),
        1223 => Err("Windows asked for permission and it was not given. Nothing was changed.".into()),
        other => Err(format!("The runtime installer stopped with code {other}. Nothing else was changed.")),
    }
}

/// Runs the shipped installer with `/install /quiet /norestart` after the user's click. The
/// elevation is Windows' own consent prompt (`-Verb RunAs`); nothing is elevated silently.
fn install_vc_redist() -> CmdResult<String> {
    use std::os::windows::process::CommandExt;
    let exe = locate_vc_redist().ok_or("The runtime installer is not in the Guarana folder; use Microsoft's page instead")?;
    let quoted = exe.to_string_lossy().replace('\'', "''");
    let script = format!(
        "$ErrorActionPreference='Stop'; try {{ $p = Start-Process -FilePath '{quoted}' -ArgumentList '/install','/quiet','/norestart' -Verb RunAs -Wait -PassThru; exit $p.ExitCode }} catch {{ exit 1223 }}"
    );
    let status = std::process::Command::new("powershell.exe")
        .args(["-NoProfile", "-NonInteractive", "-WindowStyle", "Hidden", "-Command", &script])
        .creation_flags(0x0800_0000)
        .status()
        .map_err(|e| format!("Could not start the installer: {e}"))?;
    let code = status.code().unwrap_or(-1);
    log::info!("Doctor: vc_redist.x64.exe exited with {code}");
    vc_redist_outcome(code).map(|m| m.to_string())
}

fn avx2(sys: &SystemInfo) -> Check {
    if sys.cpu.avx2 {
        check("avx2", CheckStatus::Ok, format!("{} supports AVX2.", sys.cpu.name))
    } else {
        check(
            "avx2",
            CheckStatus::Fail,
            format!("{} has no AVX2. Guarana's x86-64 builds need it (Intel Haswell, AMD Zen or newer).", sys.cpu.name),
        )
    }
}

fn memory(sys: &SystemInfo) -> Check {
    let pf = system::pagefile_config();
    let mut status = CheckStatus::Ok;
    let mut notes = vec![format!("{:.0} GB RAM, page file {:.1} GB now", sys.ram_gb, sys.pagefile_gb)];
    if sys.ram_gb < 7.5 {
        status = CheckStatus::Fail;
        notes.push("Switch 2 games want 16 GB of RAM; 8 GB or less will run out of memory".into());
    } else if sys.ram_gb < 15.0 {
        status = CheckStatus::Warn;
        notes.push("Switch 2 games want 16 GB of RAM".into());
    }
    match pf {
        Some(p) if p.none => {
            status = worse(status, CheckStatus::Warn);
            notes.push("there is no page file: let Windows manage one so memory can grow".into());
        }
        Some(p) if !p.system_managed && p.custom_max_mb < 16 * 1024 && sys.ram_gb < 32.0 => {
            status = worse(status, CheckStatus::Warn);
            notes.push(format!(
                "the page file is capped at {:.1} GB: set it to system managed so it can grow",
                p.custom_max_mb as f64 / 1024.0
            ));
        }
        Some(_) => notes.push("the page file can grow".into()),
        None => {}
    }
    let c = check("memory", status, format!("{}.", notes.join("; ")));
    if status == CheckStatus::Ok {
        c
    } else {
        c.with_fix("open_memory_settings", "Open System settings")
    }
}

fn worse(a: CheckStatus, b: CheckStatus) -> CheckStatus {
    let rank = |s: CheckStatus| match s {
        CheckStatus::Info => 0,
        CheckStatus::Ok => 1,
        CheckStatus::Warn => 2,
        CheckStatus::Fail => 3,
    };
    if rank(a) >= rank(b) {
        a
    } else {
        b
    }
}

fn disk() -> Check {
    let free = system::free_disk_bytes(&paths::user_data_dir()).unwrap_or(0);
    let gb = util::gib(free);
    let fix = |c: Check| c.with_fix("open_storage_settings", "Open Storage settings");
    if free == 0 {
        check("disk", CheckStatus::Info, "Could not read the free space.")
    } else if gb < 5.0 {
        fix(check("disk", CheckStatus::Fail, format!("{gb:.1} GB free where the caches live. Shader caches need a few GB per game.")))
    } else if gb < 20.0 {
        fix(check("disk", CheckStatus::Warn, format!("{gb:.1} GB free where the caches live. 20 GB or more avoids surprises.")))
    } else {
        check("disk", CheckStatus::Ok, format!("{gb:.0} GB free where the caches live."))
    }
}

fn power(sys: &SystemInfo) -> Check {
    let fix = |c: Check| c.with_fix("open_power_settings", "Open Power settings");
    let mode = system::power_mode(sys.power.on_ac);
    let plan = sys.power.plan.clone().unwrap_or_else(|| "unknown".into());
    match (sys.power.on_ac, mode) {
        (Some(false), _) => fix(check(
            "power",
            CheckStatus::Warn,
            format!(
                "On battery{}. GPUs run slower on battery: plug in for the best frame rate, or use the Low power profile. Plan: {plan}.",
                sys.power.battery_percent.map(|p| format!(" ({p}%)")).unwrap_or_default()
            ),
        )),
        (_, Some("Best power efficiency")) => fix(check(
            "power",
            CheckStatus::Warn,
            "Windows power mode is Best power efficiency, which holds the CPU and GPU back. Balanced or Best performance plays better.",
        )),
        _ => check("power", CheckStatus::Ok, format!("Plugged in or a desktop. Plan: {plan}.")),
    }
}

fn hags(sys: &SystemInfo) -> Check {
    let state = match sys.hags {
        Some(true) => "on",
        Some(false) => "off",
        None => "not reported by the driver",
    };
    check("hags", CheckStatus::Info, format!("Hardware-accelerated GPU scheduling is {state}. Neither setting is required; it is shown for reports."))
        .with_fix("open_graphics_settings", "Open Graphics settings")
}

fn intel_shared_memory(sys: &SystemInfo) -> Check {
    match sys.primary() {
        Some(g) if g.vendor == Vendor::Intel && g.is_integrated => check(
            "intel_shared_memory",
            CheckStatus::Info,
            format!(
                "The iGPU can use up to {:.1} GB of RAM now. On Core Ultra laptops, Intel Graphics Software has a Shared GPU Memory Override that raises it; more helps the larger games.",
                g.shared_mb as f64 / 1024.0
            ),
        )
        .with_fix("open_intel_graphics", "Get Intel Graphics Software"),
        _ => check("intel_shared_memory", CheckStatus::Info, "Not an Intel integrated GPU: nothing to set."),
    }
}

fn windows_hdr() -> Check {
    let fix = |c: Check| c.with_fix("open_hdr_help", "How to turn on HDR");
    match system::primary_display_hdr() {
        Some(h) if h.on => check(
            "windows_hdr",
            CheckStatus::Info,
            format!(
                "Windows HDR is on for the main display (it reports a {:.0}-nit peak to Windows). Games with HDR use it when the profile's HDR switch is on.",
                h.max_nits
            ),
        ),
        Some(h) if h.capable => fix(check(
            "windows_hdr",
            CheckStatus::Info,
            format!("The main display looks HDR-capable (peak about {:.0} nits) but Windows HDR is off. Win+Alt+B turns it on.", h.max_nits),
        )),
        Some(h) => fix(check(
            "windows_hdr",
            CheckStatus::Info,
            format!("The main display reports a peak of about {:.0} nits and Windows HDR is off.", h.max_nits),
        )),
        None => check("windows_hdr", CheckStatus::Info, "Could not read the display's HDR state."),
    }
}

fn defender() -> Check {
    check(
        "defender",
        CheckStatus::Info,
        format!(
            "Real-time scanning can slow the first load of large shader caches. If you want, add {} to Microsoft Defender's exclusions; Guarana never changes this itself.",
            paths::user_data_dir().join("cache").display()
        ),
    )
    .with_fix("open_windows_security", "Open Windows Security")
}

/// `*.tmp` files in the shader cache older than an hour (a cache write cut short).
fn stale_tmp_files() -> Vec<(std::path::PathBuf, u64)> {
    let cutoff = SystemTime::now() - Duration::from_secs(3600);
    walkdir::WalkDir::new(paths::shader_cache_dir())
        .max_depth(2)
        .follow_links(false)
        .into_iter()
        .flatten()
        .filter(|e| e.file_type().is_file())
        .filter(|e| e.file_name().to_string_lossy().to_ascii_lowercase().ends_with(".tmp"))
        .filter_map(|e| {
            let m = e.metadata().ok()?;
            (m.modified().ok()? < cutoff).then(|| (e.path().to_path_buf(), m.len()))
        })
        .collect()
}

fn shader_tmp() -> Check {
    let files = stale_tmp_files();
    if files.is_empty() {
        return check("shader_tmp", CheckStatus::Ok, "No leftover temporary files.");
    }
    let bytes: u64 = files.iter().map(|(_, b)| b).sum();
    check(
        "shader_tmp",
        CheckStatus::Warn,
        format!("{} leftover temporary file(s), {}, from cache writes that were cut short.", files.len(), util::human_bytes(bytes)),
    )
    .with_fix("delete_shader_tmp", "Delete them")
}

fn keys() -> Check {
    if state::keys_present() {
        check("keys", CheckStatus::Ok, format!("prod.keys is in {}.", paths::keys_dir().display()))
    } else {
        check("keys", CheckStatus::Fail, "No prod.keys yet. Use Find my files to import the keys you dumped from your console.")
    }
}

fn firmware() -> Check {
    if state::firmware_present() {
        check("firmware", CheckStatus::Ok, "Firmware is installed.")
    } else {
        check("firmware", CheckStatus::Warn, "No firmware installed. Install the firmware you dumped from your console (Tools > Install Firmware in Guarana).")
    }
}

fn game_folder(app: &AppHandle) -> Check {
    let folders = app.state::<AppState>().load().game_folders;
    let present: Vec<&String> = folders.iter().filter(|f| Path::new(f).is_dir()).collect();
    match (folders.len(), present.len()) {
        (0, _) => check("game_folder", CheckStatus::Warn, "No game folder yet. Add the folder that holds your games."),
        (_, 0) => check("game_folder", CheckStatus::Warn, "The game folders are not reachable (a drive unplugged?)."),
        (_, n) => check("game_folder", CheckStatus::Ok, format!("{n} game folder(s).")),
    }
}

fn ocarina(app: &AppHandle) -> Check {
    let games = app.state::<AppState>().load().games;
    match games.iter().find(|g| g.is_ocarina) {
        Some(g) => check("ocarina", CheckStatus::Ok, format!("Found: {}", g.path)),
        None => check(
            "ocarina",
            CheckStatus::Info,
            format!("Not in the game list ({}). It is the game Guarana is tuned on.", locate::OCARINA_TITLE_ID),
        ),
    }
}

fn controller(app: &AppHandle) -> Check {
    let pads = app.state::<AppState>().controllers.lock().map(|c| c.clone()).unwrap_or_default();
    let connected: Vec<String> = pads.iter().filter(|c| c.connected).map(|c| c.name.clone()).collect();
    if connected.is_empty() {
        check("controller", CheckStatus::Info, "No controller connected. Keyboard works too; connect a controller over USB or Bluetooth.")
            .with_fix("open_bluetooth_settings", "Open Bluetooth settings")
    } else {
        check("controller", CheckStatus::Ok, connected.join(", "))
    }
}

async fn emulator_version(app: &AppHandle) -> Check {
    let installed = state::current_version(&app.state::<AppState>().load());
    if installed.is_none() {
        return check("emulator_version", CheckStatus::Fail, "Guarana is not installed yet.").with_fix("install_update", "Install");
    }
    match tokio::time::timeout(Duration::from_secs(20), install::check_updates(app, None)).await {
        Ok(Ok(u)) if u.update_available => check(
            "emulator_version",
            CheckStatus::Warn,
            format!("{} is installed; {} is available.", u.current.unwrap_or_default(), u.latest),
        )
        .with_fix("install_update", "Update"),
        Ok(Ok(u)) => check("emulator_version", CheckStatus::Ok, format!("{} is the latest {}.", u.current.unwrap_or_default(), u.channel)),
        Ok(Err(e)) => check("emulator_version", CheckStatus::Info, format!("Could not check for updates: {e}")),
        Err(_) => check("emulator_version", CheckStatus::Info, "Could not check for updates: GitHub did not answer in time."),
    }
}

fn blocking_check(app: &AppHandle, id: &str, sys: &SystemInfo) -> Check {
    match id {
        "gpu_driver" => gpu_driver(sys),
        "vulkan" => vulkan(sys),
        "vc_redist" => vc_redist(),
        "avx2" => avx2(sys),
        "memory" => memory(sys),
        "disk" => disk(),
        "power" => power(sys),
        "hags" => hags(sys),
        "intel_shared_memory" => intel_shared_memory(sys),
        "windows_hdr" => windows_hdr(),
        "defender" => defender(),
        "shader_tmp" => shader_tmp(),
        "keys" => keys(),
        "firmware" => firmware(),
        "game_folder" => game_folder(app),
        "ocarina" => ocarina(app),
        "controller" => controller(app),
        other => check(other, CheckStatus::Info, "Unknown check"),
    }
}

async fn one(app: &AppHandle, id: &str, sys: &SystemInfo) -> Check {
    if id == "emulator_version" {
        return emulator_version(app).await;
    }
    let (app2, id2, sys2) = (app.clone(), id.to_string(), sys.clone());
    tauri::async_runtime::spawn_blocking(move || blocking_check(&app2, &id2, &sys2))
        .await
        .unwrap_or_else(|e| check(id, CheckStatus::Info, format!("The check failed to run: {e}")))
}

async fn system_info(app: &AppHandle) -> CmdResult<SystemInfo> {
    let app2 = app.clone();
    tauri::async_runtime::spawn_blocking(move || app2.state::<AppState>().system())
        .await
        .map_err(|e| e.to_string())
}

pub async fn run(app: &AppHandle) -> CmdResult<Vec<Check>> {
    let app2 = app.clone();
    let version = tauri::async_runtime::spawn(async move { emulator_version(&app2).await });
    let sys = system_info(app).await?;
    let total = CHECKS.len() as u64;
    let mut out = Vec::with_capacity(CHECKS.len());
    let mut version_at = None;
    for (i, (id, title)) in CHECKS.iter().enumerate() {
        let _ = app.emit(EVENT, Progress::new(id, i as u64, total, Some(title.to_string())));
        if *id == "emulator_version" {
            version_at = Some(out.len());
            out.push(check(id, CheckStatus::Info, "Checking..."));
        } else {
            out.push(one(app, id, &sys).await);
        }
    }
    if let Some(at) = version_at {
        out[at] = version
            .await
            .unwrap_or_else(|e| check("emulator_version", CheckStatus::Info, format!("Could not check for updates: {e}")));
    }
    let _ = app.emit(EVENT, Progress::new("done", total, total, None));
    Ok(out)
}

fn delete_shader_tmp() -> CmdResult<String> {
    if util::emulator_running() || util::warm_up_running() {
        return Err("Close Guarana (and wait for shader preparation to end) first: it may be writing its cache".into());
    }
    let base = paths::shader_cache_dir();
    let (mut removed, mut busy) = (0, 0);
    for (path, _) in stale_tmp_files() {
        // Only inside the shader cache, only *.tmp; a file still open elsewhere refuses deletion.
        if !paths::is_within(&path, &base) {
            continue;
        }
        match fs::remove_file(&path) {
            Ok(()) => removed += 1,
            Err(_) => busy += 1,
        }
    }
    log::info!("Doctor: deleted {removed} shader-cache temp file(s), {busy} in use");
    Ok(format!("Deleted {removed} file(s){}.", if busy > 0 { format!(", {busy} in use left alone") } else { String::new() }))
}

pub async fn apply_fix(app: &AppHandle, fix_id: &str) -> CmdResult<Check> {
    let check_id = FIXES
        .iter()
        .find(|(f, _)| *f == fix_id)
        .map(|(_, c)| *c)
        .ok_or_else(|| format!("Unknown fix {fix_id}"))?;
    let sys = system_info(app).await?;
    let vendor = sys.primary().map(|g| g.vendor).unwrap_or(Vendor::Other);
    let mut note: Option<String> = None;
    match fix_id {
        "install_update" => {
            let channel = state::current_version(&app.state::<AppState>().load())
                .map(|v| v.channel)
                .unwrap_or_else(|| "stable".into());
            let r = install::install_version(app, "latest", &channel).await?;
            note = Some(format!("Installed {}.", r.version));
        }
        "open_driver_page" | "open_vulkan_driver_page" => util::open_https(driver_page(vendor))?,
        "open_vc_redist" => util::open_https(VC_REDIST_PAGE)?,
        "install_vc_redist" => {
            note = Some(tauri::async_runtime::spawn_blocking(install_vc_redist).await.map_err(|e| e.to_string())??);
        }
        "open_memory_settings" => util::open_windows_page("ms-settings:about")?,
        "open_storage_settings" => util::open_windows_page("ms-settings:storagesense")?,
        "open_power_settings" => util::open_windows_page("ms-settings:powersleep")?,
        "open_graphics_settings" => util::open_windows_page("ms-settings:display-advancedgraphics")?,
        "open_intel_graphics" => util::open_https(INTEL_DRIVER_PAGE)?,
        "open_hdr_help" => util::open_https(HDR_HELP_PAGE)?,
        "open_windows_security" => util::open_windows_page("windowsdefender://threatsettings")?,
        "open_bluetooth_settings" => util::open_windows_page("ms-settings:bluetooth")?,
        "delete_shader_tmp" => {
            note = Some(tauri::async_runtime::spawn_blocking(delete_shader_tmp).await.map_err(|e| e.to_string())??);
        }
        _ => unreachable!("every fix in FIXES is handled"),
    }
    let mut result = one(app, check_id, &sys).await;
    if let Some(n) = note {
        result.detail = format!("{n} {}", result.detail);
    }
    Ok(result)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn intel_versions_compare() {
        assert!(parse_version4("32.0.101.9175").unwrap() >= INTEL_KNOWN_GOOD);
        assert!(parse_version4("32.0.101.6913").unwrap() < INTEL_KNOWN_GOOD);
        assert!(parse_version4("31.0.101.5590").unwrap() < INTEL_KNOWN_GOOD);
        assert!(parse_version4("32.0.102.1000").unwrap() > INTEL_KNOWN_GOOD);
        assert_eq!(parse_version4("garbage"), None);
    }

    #[test]
    fn driver_dates() {
        assert!(driver_age_days("1-1-2001").unwrap() > 9000);
        assert_eq!(driver_age_days("13-1-2024"), None);
    }

    #[test]
    fn runtime_installer_exit_codes() {
        assert_eq!(vc_redist_outcome(0), Ok("Installed."));
        assert!(vc_redist_outcome(3010).unwrap().contains("Restart"));
        assert!(vc_redist_outcome(1638).is_ok());
        assert!(vc_redist_outcome(1223).unwrap_err().contains("not given"));
        assert!(vc_redist_outcome(5).unwrap_err().contains("code 5"));
    }

    #[test]
    fn every_fix_names_a_check() {
        for (_, c) in FIXES {
            assert!(CHECKS.iter().any(|(id, _)| *id == c), "{c}");
        }
    }
}
