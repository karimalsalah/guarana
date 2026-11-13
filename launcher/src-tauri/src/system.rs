//! `detect_system`: what the profiles and the doctor need to know about this PC. Read-only:
//! DXGI adapter enumeration, display settings, power status and a few registry values.

use std::path::Path;

use windows::core::{Interface, PCWSTR};
use windows::Win32::Graphics::Dxgi::Common::DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
use windows::Win32::Graphics::Dxgi::{CreateDXGIFactory1, IDXGIDevice, IDXGIFactory1, IDXGIOutput6};
use windows::Win32::Graphics::Gdi::{EnumDisplaySettingsW, DEVMODEW, ENUM_CURRENT_SETTINGS};
use windows::Win32::System::Power::{GetSystemPowerStatus, SYSTEM_POWER_STATUS};
use windows::Win32::System::SystemInformation::GetPhysicallyInstalledSystemMemory;
use winreg::enums::{HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE, KEY_READ, KEY_WOW64_32KEY, KEY_WOW64_64KEY};
use winreg::RegKey;

use crate::paths;
use crate::types::{
    CpuInfo, DeviceInfo, DisplayInfo, Form, Gpu, OsInfo, PowerInfo, SystemInfo, Vendor,
};
use crate::util;

const DXGI_ADAPTER_FLAG_SOFTWARE: u32 = 2;
const DISPLAY_CLASS: &str =
    r"SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}";

fn hklm() -> RegKey {
    RegKey::predef(HKEY_LOCAL_MACHINE)
}

fn clean(s: String) -> Option<String> {
    let s = s.trim().trim_matches('\0').trim().to_string();
    (!s.is_empty()).then_some(s)
}

fn reg_str(root: &RegKey, path: &str, name: &str) -> Option<String> {
    root.open_subkey_with_flags(path, KEY_READ).ok()?.get_value::<String, _>(name).ok().and_then(clean)
}

fn reg_u32(root: &RegKey, path: &str, name: &str) -> Option<u32> {
    root.open_subkey_with_flags(path, KEY_READ).ok()?.get_value::<u32, _>(name).ok()
}

pub fn detect() -> SystemInfo {
    let mut sys = sysinfo::System::new();
    sys.refresh_memory();
    sys.refresh_cpu();

    let gpus = gpus();
    let primary_gpu = gpus.iter().position(|g| !g.is_integrated).unwrap_or(0);
    let (power, has_battery) = power();
    let ram_bytes = installed_ram_bytes().unwrap_or_else(|| sys.total_memory());

    SystemInfo {
        os: os_info(),
        cpu: cpu_info(&sys),
        ram_gb: util::gib(ram_bytes),
        pagefile_gb: util::gib(sys.total_swap()),
        gpus,
        primary_gpu,
        display: display(),
        power,
        hags: hags(),
        device: device(has_battery),
        vc_redist: vc_redist().installed,
        webview2: webview2(),
        free_disk_gb: util::gib(free_disk_bytes(&paths::user_data_dir()).unwrap_or(0)),
    }
}

fn os_info() -> OsInfo {
    let key = r"SOFTWARE\Microsoft\Windows NT\CurrentVersion";
    let build = reg_str(&hklm(), key, "CurrentBuildNumber")
        .and_then(|b| b.parse::<u32>().ok())
        .unwrap_or(0);
    let mut name = reg_str(&hklm(), key, "ProductName").unwrap_or_else(|| "Windows".to_string());
    // ProductName still says "Windows 10" on Windows 11.
    if build >= 22000 && name.contains("Windows 10") {
        name = name.replacen("Windows 10", "Windows 11", 1);
    }
    let display_version = reg_str(&hklm(), key, "DisplayVersion")
        .or_else(|| reg_str(&hklm(), key, "ReleaseId"))
        .unwrap_or_default();
    OsInfo { name, build, display_version }
}

/// `26100.4351`: the build with its update revision, for reports.
pub fn os_build_full() -> String {
    let key = r"SOFTWARE\Microsoft\Windows NT\CurrentVersion";
    let build = reg_str(&hklm(), key, "CurrentBuildNumber").unwrap_or_default();
    match reg_u32(&hklm(), key, "UBR") {
        Some(ubr) => format!("{build}.{ubr}"),
        None => build,
    }
}

fn cpu_info(sys: &sysinfo::System) -> CpuInfo {
    let name = reg_str(&hklm(), r"HARDWARE\DESCRIPTION\System\CentralProcessor\0", "ProcessorNameString")
        .or_else(|| sys.cpus().first().map(|c| c.brand().trim().to_string()))
        .unwrap_or_else(|| "Unknown CPU".to_string());
    let threads = match sys.cpus().len() {
        0 => std::thread::available_parallelism().map(|n| n.get()).unwrap_or(1),
        n => n,
    };
    let cores = sys.physical_core_count().unwrap_or(threads);
    let (avx2, avx512, hybrid) = cpu_features();
    CpuInfo { name, cores: cores as u32, threads: threads as u32, avx2, avx512, hybrid }
}

#[cfg(target_arch = "x86_64")]
fn cpu_features() -> (bool, bool, bool) {
    use std::arch::x86_64::{__cpuid, __cpuid_count};
    let avx2 = std::arch::is_x86_feature_detected!("avx2");
    let avx512 = std::arch::is_x86_feature_detected!("avx512f");
    // CPUID.(EAX=07H,ECX=0):EDX[15]: the processor has two core types (P-cores and E-cores).
    #[allow(unused_unsafe)]
    let hybrid = unsafe { __cpuid(0).eax >= 7 && (__cpuid_count(7, 0).edx >> 15) & 1 == 1 };
    (avx2, avx512, hybrid)
}

#[cfg(not(target_arch = "x86_64"))]
fn cpu_features() -> (bool, bool, bool) {
    (false, false, false)
}

fn installed_ram_bytes() -> Option<u64> {
    let mut kb = 0u64;
    unsafe { GetPhysicallyInstalledSystemMemory(&mut kb) }.ok()?;
    (kb > 0).then_some(kb * 1024)
}

pub fn available_ram_bytes() -> u64 {
    let mut sys = sysinfo::System::new();
    sys.refresh_memory();
    sys.available_memory()
}

/// What the display class key says about one adapter.
struct RegAdapter {
    vendor_id: u32,
    device_id: u32,
    driver_version: Option<String>,
    driver_date: Option<String>,
    vulkan: bool,
}

fn hex_after(s: &str, tag: &str) -> Option<u32> {
    let start = s.find(tag)? + tag.len();
    let digits: String = s[start..].chars().take_while(|c| c.is_ascii_hexdigit()).collect();
    u32::from_str_radix(&digits, 16).ok()
}

fn adapter_registry() -> Vec<RegAdapter> {
    let Ok(class) = hklm().open_subkey_with_flags(DISPLAY_CLASS, KEY_READ) else {
        return Vec::new();
    };
    let mut out = Vec::new();
    for name in class.enum_keys().flatten() {
        if name.is_empty() || !name.chars().all(|c| c.is_ascii_digit()) {
            continue;
        }
        let Ok(key) = class.open_subkey_with_flags(&name, KEY_READ) else {
            continue;
        };
        let id = key.get_value::<String, _>("MatchingDeviceId").unwrap_or_default().to_ascii_lowercase();
        let (Some(vendor_id), Some(device_id)) = (hex_after(&id, "ven_"), hex_after(&id, "dev_")) else {
            continue;
        };
        out.push(RegAdapter {
            vendor_id,
            device_id,
            driver_version: key.get_value::<String, _>("DriverVersion").ok().and_then(clean),
            driver_date: key.get_value::<String, _>("DriverDate").ok().and_then(clean),
            vulkan: key.get_raw_value("VulkanDriverName").is_ok()
                || key.get_raw_value("VulkanDriverNameWow").is_ok(),
        });
    }
    out
}

/// Vulkan ICDs registered the older way, outside the adapter's driver key.
fn khronos_icds_registered() -> bool {
    hklm()
        .open_subkey_with_flags(r"SOFTWARE\Khronos\Vulkan\Drivers", KEY_READ)
        .map(|k| k.enum_values().next().is_some())
        .unwrap_or(false)
}

pub fn vulkan_loader_present() -> bool {
    std::env::var_os("SystemRoot")
        .map(|root| Path::new(&root).join("System32").join("vulkan-1.dll").is_file())
        .unwrap_or(false)
}

fn vendor_of(id: u32) -> Vendor {
    match id {
        0x8086 => Vendor::Intel,
        0x1002 | 0x1022 => Vendor::Amd,
        0x10de => Vendor::Nvidia,
        _ => Vendor::Other,
    }
}

fn is_integrated(vendor: Vendor, device_id: u32, name: &str, dedicated: u64) -> bool {
    let n = name.to_ascii_lowercase();
    match vendor {
        // Discrete Intel: DG1, Alchemist (Arc A), Battlemage (Arc B5xx/B7xx). Everything else,
        // including Panther Lake's Arc B3xx, is integrated.
        Vendor::Intel => !matches!(device_id, 0x4905..=0x4908 | 0x5690..=0x56ff | 0xe200..=0xe2ff),
        Vendor::Amd => {
            let discrete = n.contains("rx ")
                || n.contains("rx-")
                || n.contains("radeon pro w")
                || n.contains("radeon pro v")
                || n.contains("firepro")
                || n.contains("instinct")
                || n.contains("radeon vii");
            !discrete
        }
        Vendor::Nvidia => false,
        Vendor::Other => dedicated < 1024,
    }
}

fn arch_hint(vendor: Vendor, device_id: u32, name: &str) -> Option<String> {
    let n = name.to_ascii_lowercase();
    let has = |needles: &[&str]| needles.iter().any(|x| n.contains(x));
    let hint = match vendor {
        Vendor::Intel => {
            if has(&["140v", "130v"]) || (0x6420..=0x64ff).contains(&device_id) {
                "xe2-lpg"
            } else if has(&["140t", "130t"]) || matches!(device_id, 0x7d41 | 0x7d51 | 0x7d67 | 0x7dd1) {
                "xe-lpg-plus"
            } else if has(&["b390", "b370"]) || (0xb080..=0xb0ff).contains(&device_id) {
                "xe3-lpg"
            } else if matches!(device_id, 0x7d40 | 0x7d45 | 0x7d55 | 0x7d60 | 0x7dd5) {
                "xe-lpg"
            } else if (0x5690..=0x56ff).contains(&device_id) {
                "xe-hpg"
            } else if (0xe200..=0xe2ff).contains(&device_id) {
                "xe2-hpg"
            } else if n.contains("iris(r) xe") || n.contains("iris xe") {
                "xe-lp"
            } else {
                return None;
            }
        }
        Vendor::Amd => {
            if has(&["8060s", "8050s", "8040s", "890m", "880m", "860m", "840m", "820m", "z2 extreme"])
                || matches!(device_id, 0x150e | 0x1586 | 0x1114)
            {
                "rdna3.5"
            } else if has(&["780m", "760m", "740m", "z1"]) || matches!(device_id, 0x15bf | 0x15c8) {
                "rdna3"
            } else if has(&["680m", "660m"]) || matches!(device_id, 0x1681 | 0x163f) {
                "rdna2"
            } else if has(&["rx 9"]) {
                "rdna4"
            } else if has(&["rx 7"]) {
                "rdna3"
            } else if has(&["rx 6"]) {
                "rdna2"
            } else {
                return None;
            }
        }
        Vendor::Nvidia => {
            if has(&["rtx 50", "rtx pro"]) {
                "blackwell"
            } else if has(&["rtx 40", "rtx 2000 ada", "rtx 4000 ada"]) {
                "ada"
            } else if has(&["rtx 30", "rtx a"]) {
                "ampere"
            } else if has(&["rtx 20", "gtx 16"]) {
                "turing"
            } else {
                return None;
            }
        }
        Vendor::Other => return None,
    };
    Some(hint.to_string())
}

/// The user-mode driver version DXGI reports, e.g. `32.0.101.6913`.
fn format_umd(v: i64) -> String {
    let v = v as u64;
    format!("{}.{}.{}.{}", (v >> 48) & 0xffff, (v >> 32) & 0xffff, (v >> 16) & 0xffff, v & 0xffff)
}

fn gpus() -> Vec<Gpu> {
    let registry = adapter_registry();
    let khronos = khronos_icds_registered();
    let mut out: Vec<Gpu> = Vec::new();
    // SAFETY: plain COM calls on interfaces this function owns; DXGI needs no apartment.
    unsafe {
        let Ok(factory) = CreateDXGIFactory1::<IDXGIFactory1>() else {
            log::warn!("CreateDXGIFactory1 failed; no GPU list");
            return out;
        };
        for index in 0..16u32 {
            let Ok(adapter) = factory.EnumAdapters1(index) else {
                break; // DXGI_ERROR_NOT_FOUND past the last adapter
            };
            let Ok(desc) = adapter.GetDesc1() else {
                continue;
            };
            if desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE != 0 || desc.VendorId == 0x1414 {
                continue; // WARP / Microsoft Basic Render Driver
            }
            let len = desc.Description.iter().position(|&c| c == 0).unwrap_or(desc.Description.len());
            let name = String::from_utf16_lossy(&desc.Description[..len]).trim().to_string();
            let umd = adapter.CheckInterfaceSupport(&IDXGIDevice::IID).ok().map(format_umd);
            let reg = registry.iter().find(|r| r.vendor_id == desc.VendorId && r.device_id == desc.DeviceId);
            let vendor = vendor_of(desc.VendorId);
            let dedicated_mb = (desc.DedicatedVideoMemory as u64) >> 20;
            let gpu = Gpu {
                vendor,
                driver_version: umd
                    .or_else(|| reg.and_then(|r| r.driver_version.clone()))
                    .unwrap_or_default(),
                is_integrated: is_integrated(vendor, desc.DeviceId, &name, dedicated_mb),
                dedicated_mb,
                shared_mb: (desc.SharedSystemMemory as u64) >> 20,
                vulkan: reg.map(|r| r.vulkan).unwrap_or(false) || khronos,
                arch_hint: arch_hint(vendor, desc.DeviceId, &name),
                vendor_id: desc.VendorId,
                device_id: desc.DeviceId,
                driver_date: reg.and_then(|r| r.driver_date.clone()),
                name,
            };
            // The same adapter can be listed once per LUID on some hybrid setups.
            if !out.iter().any(|g| g.vendor_id == gpu.vendor_id && g.device_id == gpu.device_id && g.name == gpu.name) {
                out.push(gpu);
            }
        }
    }
    out
}

fn display() -> DisplayInfo {
    let mut dm = DEVMODEW { dmSize: std::mem::size_of::<DEVMODEW>() as u16, ..Default::default() };
    // SAFETY: `dm` is a valid DEVMODEW with dmSize set, as EnumDisplaySettingsW requires.
    let ok = unsafe { EnumDisplaySettingsW(PCWSTR::null(), ENUM_CURRENT_SETTINGS, &mut dm) }.as_bool();
    if !ok {
        return DisplayInfo { width: 0, height: 0, refresh_hz: 0, vrr: None, hdr: None };
    }
    DisplayInfo {
        width: dm.dmPelsWidth,
        height: dm.dmPelsHeight,
        refresh_hz: dm.dmDisplayFrequency,
        vrr: None,
        hdr: primary_display_hdr().map(|h| h.on),
    }
}

pub struct HdrInfo {
    /// Windows runs the display in HDR (the output's color space is PQ / BT.2020).
    pub on: bool,
    /// The panel reports a peak of 400 nits or more (DisplayHDR 400 and up), or HDR is on.
    pub capable: bool,
    pub max_nits: f32,
}

/// HDR state of the primary display (the output at desktop origin), from
/// `IDXGIOutput6::GetDesc1`. Read-only: never changes the Windows setting.
pub fn primary_display_hdr() -> Option<HdrInfo> {
    let mut fallback = None;
    // SAFETY: COM calls on interfaces this function owns.
    unsafe {
        let factory = CreateDXGIFactory1::<IDXGIFactory1>().ok()?;
        for a in 0..16u32 {
            let Ok(adapter) = factory.EnumAdapters1(a) else { break };
            for o in 0..16u32 {
                let Ok(output) = adapter.EnumOutputs(o) else { break };
                let Ok(output6) = output.cast::<IDXGIOutput6>() else { continue };
                let Ok(desc) = output6.GetDesc1() else { continue };
                let on = desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
                let info = HdrInfo { on, capable: on || desc.MaxLuminance >= 400.0, max_nits: desc.MaxLuminance };
                let r = desc.DesktopCoordinates;
                if desc.AttachedToDesktop.as_bool() && r.left == 0 && r.top == 0 {
                    return Some(info);
                }
                if fallback.is_none() {
                    fallback = Some(info);
                }
            }
        }
    }
    fallback
}

/// (power info, whether the machine has a system battery).
fn power() -> (PowerInfo, Option<bool>) {
    let mut s = SYSTEM_POWER_STATUS::default();
    // SAFETY: plain out-parameter call.
    if unsafe { GetSystemPowerStatus(&mut s) }.is_err() {
        return (PowerInfo { on_ac: None, battery_percent: None, plan: power_plan(None) }, None);
    }
    let on_ac = match s.ACLineStatus {
        0 => Some(false),
        1 => Some(true),
        _ => None,
    };
    let has_battery = match s.BatteryFlag {
        255 => None,
        flag => Some(flag & 128 == 0),
    };
    let battery_percent = (has_battery == Some(true) && s.BatteryLifePercent <= 100).then_some(s.BatteryLifePercent);
    (PowerInfo { on_ac, battery_percent, plan: power_plan(on_ac) }, has_battery)
}

fn known_scheme(guid: &str) -> Option<&'static str> {
    Some(match guid.to_ascii_lowercase().as_str() {
        "381b4222-f694-41f0-9685-ff5bb260df2e" => "Balanced",
        "8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c" => "High performance",
        "a1841308-3541-4fab-bc81-f71556f20b4a" => "Power saver",
        "e9a42b02-d5df-448d-aa00-03f14749eb61" => "Ultimate Performance",
        _ => return None,
    })
}

/// The power mode slider (Settings > System > Power), stored as an overlay scheme.
fn overlay_name(guid: &str) -> Option<&'static str> {
    Some(match guid.to_ascii_lowercase().as_str() {
        "961cc777-2547-4f9d-8174-7d86181b8a7a" => "Best power efficiency",
        "00000000-0000-0000-0000-000000000000" => "Balanced",
        "3af9b8d9-7c97-431d-ad78-34a8bfea439f" => "Better performance",
        "ded574b5-45a0-4f42-8737-46345c09c238" => "Best performance",
        _ => return None,
    })
}

fn power_plan(on_ac: Option<bool>) -> Option<String> {
    let schemes = hklm()
        .open_subkey_with_flags(r"SYSTEM\CurrentControlSet\Control\Power\User\PowerSchemes", KEY_READ)
        .ok()?;
    let active = schemes.get_value::<String, _>("ActivePowerScheme").ok()?;
    let name = known_scheme(&active).map(str::to_string).or_else(|| {
        let raw = schemes.open_subkey_with_flags(&active, KEY_READ).ok()?.get_value::<String, _>("FriendlyName").ok()?;
        // "@%SystemRoot%\system32\powrprof.dll,-14,Balanced": the text after the last comma.
        clean(raw.rsplit(',').next().unwrap_or(&raw).to_string())
    })?;
    let overlay_value = if on_ac == Some(false) { "ActiveOverlayDcPowerScheme" } else { "ActiveOverlayAcPowerScheme" };
    let overlay = schemes.get_value::<String, _>(overlay_value).ok().and_then(|g| overlay_name(&g));
    Some(match overlay {
        Some(mode) => format!("{name}, power mode {mode}"),
        None => name,
    })
}

/// The power mode slider alone, for the doctor ("Best power efficiency" etc.).
pub fn power_mode(on_ac: Option<bool>) -> Option<&'static str> {
    let schemes = hklm()
        .open_subkey_with_flags(r"SYSTEM\CurrentControlSet\Control\Power\User\PowerSchemes", KEY_READ)
        .ok()?;
    let value = if on_ac == Some(false) { "ActiveOverlayDcPowerScheme" } else { "ActiveOverlayAcPowerScheme" };
    overlay_name(&schemes.get_value::<String, _>(value).ok()?)
}

fn hags() -> Option<bool> {
    match reg_u32(&hklm(), r"SYSTEM\CurrentControlSet\Control\GraphicsDrivers", "HwSchMode")? {
        2 => Some(true),
        1 => Some(false),
        _ => None,
    }
}

fn is_placeholder(s: &str) -> bool {
    let l = s.to_ascii_lowercase();
    l.is_empty()
        || l.contains("to be filled")
        || l.contains("default string")
        || l == "system product name"
        || l == "system version"
        || l == "not applicable"
        || l == "none"
}

/// (form, profile hint) for devices whose BIOS strings we recognise.
fn known_device(manufacturer: &str, model: &str, family: &str) -> Option<(Form, &'static str)> {
    let m = manufacturer.to_ascii_lowercase();
    let s = format!("{} {}", model, family).to_ascii_lowercase();
    let has = |x: &str| s.contains(x);
    let hit = if m.contains("micro-star") || m.starts_with("msi") {
        if has("claw 8 ai") {
            "msi-claw-8-ai-plus"
        } else if has("claw 7 ai") {
            "msi-claw-7-ai-plus"
        } else if has("claw") {
            "msi-claw-a1m"
        } else {
            return None;
        }
    } else if m.contains("asus") {
        if has("xbox ally x") || has("rc73xa") {
            "rog-xbox-ally-x"
        } else if has("xbox ally") || has("rc73ya") {
            "rog-xbox-ally"
        } else if has("ally x") || has("rc72la") {
            "rog-ally-x"
        } else if has("rog ally") || has("rc71l") {
            "rog-ally"
        } else {
            return None;
        }
    } else if m.contains("lenovo") {
        if has("legion go s") || has("83l3") || has("83n6") || has("83q2") || has("83q3") {
            "legion-go-s"
        } else if has("legion go 2") || has("83n0") || has("83n1") {
            "legion-go-2"
        } else if has("legion go") || has("83e1") {
            "legion-go"
        } else {
            return None;
        }
    } else if m.contains("valve") {
        if has("galileo") {
            "steam-deck-oled"
        } else if has("jupiter") {
            "steam-deck-lcd"
        } else {
            "steam-deck"
        }
    } else if m.contains("ayaneo") {
        "ayaneo"
    } else if m == "gpd" || m.starts_with("gpd ") {
        if has("win") || has("g1618") || has("g1617") {
            "gpd-win"
        } else {
            return None;
        }
    } else if m.contains("one-netbook") || has("onexplayer") || has("onexfly") {
        "onexplayer"
    } else if m.contains("zotac") && has("zone") {
        "zotac-zone"
    } else if m.contains("ayn") && has("loki") {
        "ayn-loki"
    } else {
        return None;
    };
    Some((Form::Handheld, hit))
}

fn device(has_battery: Option<bool>) -> DeviceInfo {
    let bios = r"HARDWARE\DESCRIPTION\System\BIOS";
    let get = |name: &str| reg_str(&hklm(), bios, name).filter(|v| !is_placeholder(v)).unwrap_or_default();
    let manufacturer = get("SystemManufacturer");
    let product = get("SystemProductName");
    let version = get("SystemVersion");
    let family = get("SystemFamily");
    // Lenovo keeps the marketing name in SystemVersion ("Legion Go 8APU1") and a type number
    // ("83E1") in SystemProductName.
    let model = if manufacturer.eq_ignore_ascii_case("lenovo") && !version.is_empty() {
        format!("{version} ({product})")
    } else {
        product.clone()
    };
    let known = known_device(&manufacturer, &format!("{product} {version}"), &family);
    let form = match (known, has_battery) {
        (Some((form, _)), _) => form,
        (None, Some(true)) => Form::Laptop,
        (None, Some(false)) => Form::Desktop,
        (None, None) => Form::Unknown,
    };
    DeviceInfo { manufacturer, model, form, profile_hint: known.map(|(_, hint)| hint.to_string()) }
}

pub struct VcRedist {
    pub installed: bool,
    /// (major, minor, build) when the registry has them.
    pub version: Option<(u32, u32, u32)>,
}

pub fn vc_redist() -> VcRedist {
    let path = r"SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64";
    for view in [KEY_WOW64_32KEY, KEY_WOW64_64KEY] {
        let Ok(key) = hklm().open_subkey_with_flags(path, KEY_READ | view) else {
            continue;
        };
        if key.get_value::<u32, _>("Installed").unwrap_or(0) != 1 {
            continue;
        }
        let part = |n: &str| key.get_value::<u32, _>(n).ok();
        let version = match (part("Major"), part("Minor"), part("Bld")) {
            (Some(a), Some(b), Some(c)) => Some((a, b, c)),
            _ => None,
        };
        return VcRedist { installed: true, version };
    }
    // An installer that did not register: the DLLs themselves.
    let system32 = std::env::var_os("SystemRoot").map(|r| Path::new(&r).join("System32"));
    let dlls = system32
        .map(|d| d.join("vcruntime140.dll").is_file() && d.join("msvcp140.dll").is_file() && d.join("vcruntime140_1.dll").is_file())
        .unwrap_or(false);
    VcRedist { installed: dlls, version: None }
}

fn webview2() -> Option<String> {
    let client = r"Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}";
    let hkcu = RegKey::predef(HKEY_CURRENT_USER);
    [
        (hklm(), format!(r"SOFTWARE\WOW6432Node\{client}")),
        (hklm(), format!(r"SOFTWARE\{client}")),
        (hkcu, format!(r"Software\{client}")),
    ]
    .iter()
    .find_map(|(root, path)| reg_str(root, path, "pv").filter(|v| v != "0.0.0.0"))
}

pub struct PageFileConfig {
    /// Windows sizes it (and lets it grow).
    pub system_managed: bool,
    /// No page file on any drive.
    pub none: bool,
    /// Sum of the maximum sizes of custom-sized page files, in MiB.
    pub custom_max_mb: u64,
}

pub fn pagefile_config() -> Option<PageFileConfig> {
    let key = hklm()
        .open_subkey_with_flags(r"SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management", KEY_READ)
        .ok()?;
    let lines: Vec<String> = key.get_value::<Vec<String>, _>("PagingFiles").unwrap_or_default();
    let mut config = PageFileConfig { system_managed: false, none: true, custom_max_mb: 0 };
    for line in lines.iter().map(|l| l.trim()).filter(|l| !l.is_empty()) {
        config.none = false;
        let parts: Vec<&str> = line.split_whitespace().collect();
        if parts[0].starts_with("?:") {
            config.system_managed = true;
            continue;
        }
        match (parts.get(1).and_then(|v| v.parse::<u64>().ok()), parts.get(2).and_then(|v| v.parse::<u64>().ok())) {
            (Some(0), Some(0)) | (None, None) => config.system_managed = true,
            (_, Some(max)) => config.custom_max_mb += max,
            (Some(init), None) => config.custom_max_mb += init,
        }
    }
    Some(config)
}

/// Free bytes on the volume holding `path`.
pub fn free_disk_bytes(path: &Path) -> Option<u64> {
    let target = path.to_string_lossy().to_ascii_lowercase();
    let disks = sysinfo::Disks::new_with_refreshed_list();
    disks
        .list()
        .iter()
        .filter(|d| target.starts_with(&d.mount_point().to_string_lossy().to_ascii_lowercase()))
        .max_by_key(|d| d.mount_point().as_os_str().len())
        .map(|d| d.available_space())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn umd_version_format() {
        let v: i64 = (32i64 << 48) | (0 << 32) | (101 << 16) | 9175;
        assert_eq!(format_umd(v), "32.0.101.9175");
    }

    #[test]
    fn integrated_heuristics() {
        assert!(is_integrated(Vendor::Intel, 0x7d51, "Intel(R) Arc(TM) 140T GPU (16GB)", 128));
        assert!(is_integrated(Vendor::Intel, 0xb080, "Intel(R) Arc(TM) B390 GPU", 128));
        assert!(!is_integrated(Vendor::Intel, 0xe20b, "Intel(R) Arc(TM) B580 Graphics", 12000));
        assert!(is_integrated(Vendor::Amd, 0x15bf, "AMD Radeon(TM) 780M", 4096));
        assert!(!is_integrated(Vendor::Amd, 0x744c, "AMD Radeon RX 7900 XTX", 24000));
        assert!(!is_integrated(Vendor::Nvidia, 0x2684, "NVIDIA GeForce RTX 4090", 24000));
    }

    #[test]
    fn handhelds() {
        let claw = known_device("Micro-Star International Co., Ltd.", "Claw 8 AI+ A2VM", "");
        assert_eq!(claw, Some((Form::Handheld, "msi-claw-8-ai-plus")));
        let ally = known_device("ASUSTeK COMPUTER INC.", "ROG Ally RC71L_RC71L", "");
        assert_eq!(ally, Some((Form::Handheld, "rog-ally")));
        let allyx = known_device("ASUSTeK COMPUTER INC.", "ROG Ally X RC72LA_RC72LA", "");
        assert_eq!(allyx, Some((Form::Handheld, "rog-ally-x")));
        assert_eq!(known_device("LENOVO", "83E1 Legion Go 8APU1", ""), Some((Form::Handheld, "legion-go")));
        assert_eq!(known_device("Dell Inc.", "XPS 13 9350", ""), None);
    }

    /// Runs the Win32 and DXGI code against this PC: `cargo test -- --ignored --nocapture smoke`.
    #[test]
    #[ignore = "reads this PC's hardware"]
    fn smoke_this_pc() {
        let sys = detect();
        println!("{}", serde_json::to_string_pretty(&sys).unwrap());
        println!("hdr: {:?}", primary_display_hdr().map(|h| (h.on, h.capable, h.max_nits)));
        println!("page file: {:?}", pagefile_config().map(|p| (p.system_managed, p.none, p.custom_max_mb)));
        let vc = vc_redist();
        println!("vc++: {} {:?}", vc.installed, vc.version);
        println!("power mode: {:?}, build {}", power_mode(sys.power.on_ac), os_build_full());
        assert!(sys.os.build > 0);
        assert!(sys.cpu.threads > 0);
    }

    #[test]
    fn hex_ids() {
        assert_eq!(hex_after("pci\\ven_8086&dev_7d51&subsys_00011462", "ven_"), Some(0x8086));
        assert_eq!(hex_after("pci\\ven_8086&dev_7d51&subsys_00011462", "dev_"), Some(0x7d51));
    }
}
