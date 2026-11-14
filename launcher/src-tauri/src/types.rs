//! The types of `launcher/CONTRACT.md`. Field names are serialized exactly as written there
//! (snake_case), so no `rename_all` on the structs; string unions are lowercase enums.

use serde::{Deserialize, Serialize};

/// Payload of `install://progress`, `locate://progress` and `doctor://progress`.
#[derive(Debug, Clone, Serialize)]
pub struct Progress {
    pub stage: String,
    pub done: u64,
    pub total: u64,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub message: Option<String>,
}

impl Progress {
    pub fn new(stage: &str, done: u64, total: u64, message: Option<String>) -> Self {
        Self { stage: stage.to_string(), done, total, message }
    }
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "lowercase")]
pub enum Vendor {
    Intel,
    Amd,
    Nvidia,
    Other,
}

#[derive(Debug, Clone, Serialize)]
pub struct Gpu {
    pub name: String,
    pub vendor: Vendor,
    pub driver_version: String,
    pub is_integrated: bool,
    pub dedicated_mb: u64,
    pub shared_mb: u64,
    pub vulkan: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub arch_hint: Option<String>,
    /// PCI ids and the driver date, used by the doctor; not part of the contract.
    #[serde(skip)]
    pub vendor_id: u32,
    #[serde(skip)]
    pub device_id: u32,
    #[serde(skip)]
    pub driver_date: Option<String>,
}

#[derive(Debug, Clone, Serialize)]
pub struct OsInfo {
    pub name: String,
    pub build: u32,
    pub display_version: String,
}

#[derive(Debug, Clone, Serialize)]
pub struct CpuInfo {
    pub name: String,
    pub cores: u32,
    pub threads: u32,
    pub avx2: bool,
    pub avx512: bool,
    pub hybrid: bool,
}

#[derive(Debug, Clone, Serialize)]
pub struct DisplayInfo {
    pub width: u32,
    pub height: u32,
    pub refresh_hz: u32,
    pub vrr: Option<bool>,
    pub hdr: Option<bool>,
}

#[derive(Debug, Clone, Serialize)]
pub struct PowerInfo {
    pub on_ac: Option<bool>,
    pub battery_percent: Option<u8>,
    pub plan: Option<String>,
}

#[derive(Debug, Clone, Copy, Serialize, PartialEq, Eq)]
#[serde(rename_all = "lowercase")]
pub enum Form {
    Laptop,
    Handheld,
    Desktop,
    Unknown,
}

#[derive(Debug, Clone, Serialize)]
pub struct DeviceInfo {
    pub manufacturer: String,
    pub model: String,
    pub form: Form,
    pub profile_hint: Option<String>,
}

#[derive(Debug, Clone, Serialize)]
pub struct SystemInfo {
    pub os: OsInfo,
    pub cpu: CpuInfo,
    pub ram_gb: f64,
    pub pagefile_gb: f64,
    pub gpus: Vec<Gpu>,
    pub primary_gpu: usize,
    pub display: DisplayInfo,
    pub power: PowerInfo,
    pub hags: Option<bool>,
    pub device: DeviceInfo,
    pub vc_redist: bool,
    pub webview2: Option<String>,
    pub free_disk_gb: f64,
}

impl SystemInfo {
    pub fn primary(&self) -> Option<&Gpu> {
        self.gpus.get(self.primary_gpu)
    }
}

/// `boolean | number | string`. Integers stay integers on the wire.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(untagged)]
pub enum ToggleValue {
    Bool(bool),
    Int(i64),
    Float(f64),
    Str(String),
}

impl ToggleValue {
    pub fn as_bool(&self) -> Option<bool> {
        match self {
            ToggleValue::Bool(b) => Some(*b),
            ToggleValue::Int(i) => Some(*i != 0),
            ToggleValue::Str(s) => match s.to_ascii_lowercase().as_str() {
                "true" | "1" | "on" | "yes" => Some(true),
                "false" | "0" | "off" | "no" => Some(false),
                _ => None,
            },
            ToggleValue::Float(_) => None,
        }
    }

    pub fn as_text(&self) -> String {
        match self {
            ToggleValue::Bool(b) => b.to_string(),
            ToggleValue::Int(i) => i.to_string(),
            ToggleValue::Float(f) => f.to_string(),
            ToggleValue::Str(s) => s.clone(),
        }
    }
}

impl From<bool> for ToggleValue {
    fn from(b: bool) -> Self {
        ToggleValue::Bool(b)
    }
}

impl From<&str> for ToggleValue {
    fn from(s: &str) -> Self {
        ToggleValue::Str(s.to_string())
    }
}

#[derive(Debug, Clone, Copy, Serialize, PartialEq, Eq)]
#[serde(rename_all = "lowercase")]
pub enum ToggleKind {
    Bool,
    Choice,
    /// In the contract; no toggle uses it yet.
    #[allow(dead_code)]
    Number,
}

#[derive(Debug, Clone, Serialize)]
pub struct Toggle {
    pub id: String,
    pub label: String,
    pub help: String,
    pub value: ToggleValue,
    /// The profile's own value: what `value` is without the user's or the mode's changes.
    pub default: ToggleValue,
    pub kind: ToggleKind,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub choices: Option<Vec<String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub experimental: Option<bool>,
}

#[derive(Debug, Clone, Serialize)]
pub struct Profile {
    pub id: String,
    pub name: String,
    pub description: String,
    pub recommended: bool,
    pub toggles: Vec<Toggle>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct InstalledVersion {
    pub version: String,
    pub channel: String,
    pub path: String,
    pub installed_at: String,
}

#[derive(Debug, Clone, Serialize)]
pub struct LauncherState {
    pub install_dir: String,
    pub user_data_dir: String,
    pub installed: Vec<InstalledVersion>,
    pub current: Option<String>,
    pub profile_id: Option<String>,
    pub first_run: bool,
    pub keys_present: bool,
    pub firmware_present: bool,
    pub game_folders: Vec<String>,
}

#[derive(Debug, Clone, Serialize)]
pub struct UpdateInfo {
    pub current: Option<String>,
    pub latest: String,
    pub channel: String,
    pub published_at: String,
    pub notes_md: String,
    pub size_bytes: u64,
    pub sha256: String,
    pub update_available: bool,
}

#[derive(Debug, Clone, Serialize)]
pub struct InstallResult {
    pub version: String,
    pub path: String,
    pub verified_sha256: bool,
    pub seconds: f64,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "lowercase")]
pub enum KeyKind {
    Prod,
    Title,
    Unknown,
}

#[derive(Debug, Clone, Serialize)]
pub struct FoundKey {
    pub path: String,
    pub kind: KeyKind,
    pub source_hint: String,
}

#[derive(Debug, Clone, Serialize)]
pub struct FoundFirmware {
    pub path: String,
    pub version: Option<String>,
    pub source_hint: String,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "lowercase")]
pub enum Container {
    Nup,
    Nsp,
    Xci,
    Nx,
    Other,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct Game {
    pub title_id: String,
    pub title: String,
    pub path: String,
    pub container: Container,
    pub size_bytes: u64,
    pub is_ocarina: bool,
}

#[derive(Debug, Clone, Serialize, Default)]
pub struct Found {
    pub keys: Vec<FoundKey>,
    pub firmware: Vec<FoundFirmware>,
    pub games: Vec<Game>,
    pub scanned_dirs: u64,
    pub seconds: f64,
}

#[derive(Debug, Clone, Serialize)]
pub struct ImportResult {
    pub ok: bool,
    pub dest: String,
    pub message: String,
}

#[derive(Debug, Clone, Serialize)]
pub struct AppliedProfile {
    pub profile_id: String,
    pub switches_path: String,
    pub config_path: String,
    pub changed: Vec<String>,
}

#[derive(Debug, Clone, Copy, Serialize, PartialEq, Eq)]
#[serde(rename_all = "lowercase")]
pub enum CheckStatus {
    Ok,
    Warn,
    Fail,
    Info,
}

#[derive(Debug, Clone, Serialize)]
pub struct Check {
    pub id: String,
    pub title: String,
    pub status: CheckStatus,
    pub detail: String,
    pub fix_id: Option<String>,
    pub fix_label: Option<String>,
}

impl Check {
    pub fn new(id: &str, title: &str, status: CheckStatus, detail: impl Into<String>) -> Self {
        Self {
            id: id.to_string(),
            title: title.to_string(),
            status,
            detail: detail.into(),
            fix_id: None,
            fix_label: None,
        }
    }

    pub fn with_fix(mut self, fix_id: &str, fix_label: &str) -> Self {
        self.fix_id = Some(fix_id.to_string());
        self.fix_label = Some(fix_label.to_string());
        self
    }
}

#[derive(Debug, Clone, Copy, Serialize, PartialEq, Eq)]
#[serde(rename_all = "lowercase")]
pub enum ControllerKind {
    Xbox,
    Playstation,
    Switch,
    Generic,
}

#[derive(Debug, Clone, Serialize, PartialEq)]
pub struct Controller {
    pub id: String,
    pub name: String,
    pub kind: ControllerKind,
    pub connected: bool,
    pub battery: Option<u8>,
}

#[derive(Debug, Clone, Serialize)]
pub struct PrefetchResult {
    pub bytes_warmed: u64,
}

#[derive(Debug, Clone, Serialize)]
pub struct LaunchResult {
    pub pid: u32,
    /// The emulator writes its boot to the launcher (`game://boot`): a game in a launcher session.
    pub follows_boot: bool,
}

/// Payload of `game://exited`.
#[derive(Debug, Clone, Serialize)]
pub struct GameExited {
    pub code: i32,
    pub seconds: f64,
}

/// Payload of `game://boot` (`boot.rs`): where the game's boot is. `phase` is `starting`,
/// `loading`, `shaders`, `precache`, `launching`, `first-frame`, `shown` (in that order) or
/// `window` (a window of the emulator has the foreground). `done` of `total` are shader counts
/// for `shaders` and `precache`, 0 of 0 otherwise.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct BootProgress {
    pub phase: String,
    pub done: u64,
    pub total: u64,
}

#[derive(Debug, Clone, Serialize)]
pub struct ReportResult {
    pub zip_path: String,
    pub issue_url: String,
    pub summary_md: String,
}
