//! `collect_report`: zips the emulator's logs, the launcher's log and the system info, with the
//! user name, profile folder and host name scrubbed from every text file, and builds a prefilled
//! GitHub issue URL. Nothing is sent: the UI opens the URL and the user reviews the issue and
//! attaches the zip.

use std::fs::{self, File};
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::Path;

use percent_encoding::{utf8_percent_encode, AsciiSet, NON_ALPHANUMERIC};
use tauri::{AppHandle, Manager};
use zip::write::SimpleFileOptions;

use crate::state::{self, AppState};
use crate::types::{Form, ReportResult, SystemInfo, Vendor};
use crate::util::{self, CmdResult, Context};
use crate::{locate, paths, system};

const ISSUES: &str = "https://github.com/karimalsalah/guarana/issues/new";
const MAX_URL: usize = 7000;
const LOG_HEAD: u64 = 256 << 10;
const LOG_TAIL: u64 = 16 << 20;
/// Characters left as they are in query values (RFC 3986 unreserved).
const QUERY: &AsciiSet = &NON_ALPHANUMERIC.remove(b'-').remove(b'.').remove(b'_').remove(b'~');

/// Case-insensitive (ASCII) replacement; `whole_word` needs a non-word character on both sides.
fn replace_ci(hay: &str, needle: &str, repl: &str, whole_word: bool) -> String {
    if needle.is_empty() || hay.len() < needle.len() {
        return hay.to_string();
    }
    let h = hay.as_bytes();
    let n = needle.as_bytes();
    let word = |b: u8| b.is_ascii_alphanumeric() || b == b'_';
    let mut out = String::with_capacity(hay.len());
    let (mut last, mut i) = (0, 0);
    while i + n.len() <= h.len() {
        let hit = h[i..i + n.len()].eq_ignore_ascii_case(n)
            && (!whole_word || ((i == 0 || !word(h[i - 1])) && (i + n.len() == h.len() || !word(h[i + n.len()]))));
        if hit {
            // `needle` is valid UTF-8 and its non-ASCII bytes matched exactly, so both ends of
            // the match are character boundaries.
            out.push_str(&hay[last..i]);
            out.push_str(repl);
            i += n.len();
            last = i;
        } else {
            i += 1;
        }
    }
    out.push_str(&hay[last..]);
    out
}

pub struct Scrubber {
    rules: Vec<(String, String, bool)>,
}

impl Scrubber {
    pub fn new() -> Self {
        let mut rules: Vec<(String, String, bool)> = Vec::new();
        if let Some(profile) = paths::user_profile() {
            let p = profile.to_string_lossy().into_owned();
            for variant in [p.clone(), p.replace('\\', "/"), p.replace('\\', "\\\\")] {
                rules.push((variant, "%USERPROFILE%".into(), false));
            }
        }
        if let Ok(user) = std::env::var("USERNAME") {
            let user = user.trim().to_string();
            if user.len() >= 2 {
                for (sep, esc) in [("\\", "\\"), ("/", "/"), ("\\\\", "\\\\")] {
                    rules.push((format!("{sep}Users{esc}{user}"), format!("{sep}Users{esc}%USERNAME%"), false));
                }
            }
            if user.len() >= 3 {
                rules.push((user, "%USERNAME%".into(), true));
            }
        }
        if let Ok(host) = std::env::var("COMPUTERNAME") {
            let host = host.trim().to_string();
            if host.len() >= 2 {
                rules.push((host, "%COMPUTERNAME%".into(), true));
            }
        }
        rules.sort_by(|a, b| b.0.len().cmp(&a.0.len()));
        Self { rules }
    }

    pub fn apply(&self, text: &str) -> String {
        self.rules.iter().fold(text.to_string(), |t, (needle, repl, whole)| replace_ci(&t, needle, repl, *whole))
    }
}

/// A log's first 256 KiB (the build line) and last 16 MiB.
fn read_log(path: &Path) -> Option<String> {
    let mut f = File::open(path).ok()?;
    let len = f.metadata().ok()?.len();
    let mut bytes = Vec::new();
    if len <= LOG_HEAD + LOG_TAIL {
        f.read_to_end(&mut bytes).ok()?;
    } else {
        let mut head = vec![0u8; LOG_HEAD as usize];
        f.read_exact(&mut head).ok()?;
        f.seek(SeekFrom::Start(len - LOG_TAIL)).ok()?;
        let mut tail = Vec::new();
        f.read_to_end(&mut tail).ok()?;
        bytes.extend_from_slice(&head);
        bytes.extend_from_slice(format!("\n[... {} bytes left out ...]\n", len - LOG_HEAD - LOG_TAIL).as_bytes());
        bytes.extend_from_slice(&tail);
    }
    Some(String::from_utf8_lossy(&bytes).into_owned())
}

/// The program id of the last `Booting game: <id>` line.
fn last_booted_title(log: &str) -> Option<String> {
    log.lines().rev().find_map(|line| {
        let at = line.find("Booting game:")?;
        let rest = &line[at + "Booting game:".len()..];
        let id: String = rest
            .trim_start()
            .trim_start_matches("0x")
            .chars()
            .take_while(|c| c.is_ascii_hexdigit())
            .collect();
        (id.len() == 16).then(|| id.to_ascii_uppercase())
    })
}

/// The issue forms' Graphics card choices.
fn gpu_choice(sys: &SystemInfo) -> &'static str {
    let Some(g) = sys.primary() else { return "Other" };
    let n = g.name.to_ascii_lowercase();
    let arch = g.arch_hint.as_deref().unwrap_or("");
    match (g.vendor, g.is_integrated) {
        (Vendor::Intel, true) => match arch {
            "xe-lpg-plus" | "xe2-lpg" => "Intel Arc 140T / 140V (Arrow Lake H, Lunar Lake)",
            "xe-lpg" => "Intel Arc (Meteor Lake)",
            "xe3-lpg" => "Intel Arc (Panther Lake)",
            _ => "Other Intel integrated (Iris Xe, UHD)",
        },
        (Vendor::Intel, false) => "Intel Arc discrete (A or B series)",
        (Vendor::Amd, true) => {
            if sys.device.form == Form::Handheld && (n.contains("z1") || n.contains("z2") || n == "amd radeon(tm) graphics" || n == "amd radeon graphics") {
                "AMD Ryzen Z1 / Z2 (handheld)"
            } else if n.contains("780m") || n.contains("760m") {
                "AMD Radeon 780M / 760M"
            } else if n.contains("880m") || n.contains("890m") {
                "AMD Radeon 880M / 890M"
            } else {
                "Other AMD integrated"
            }
        }
        (Vendor::Amd, false) => {
            if n.contains("rx 9") {
                "AMD RX 9000 series"
            } else if n.contains("rx 7") {
                "AMD RX 7000 series"
            } else {
                "AMD RX 6000 series or older"
            }
        }
        (Vendor::Nvidia, _) => {
            if n.contains("rtx 50") {
                "NVIDIA RTX 50 series"
            } else if n.contains("rtx 40") {
                "NVIDIA RTX 40 series"
            } else {
                "NVIDIA RTX 30 series or older"
            }
        }
        (Vendor::Other, _) => "Other",
    }
}

/// The issue forms' Game choices.
fn game_choice(title_id: Option<&str>) -> &'static str {
    match title_id.map(|t| t.to_ascii_uppercase()).as_deref() {
        Some("0400486024DEC000") => "Ocarina of Time",
        Some("0400C3F00006E000") => "Mario Kart World",
        Some("040074A01BF12000") => "Donkey Kong Bananza",
        Some("04007EF01F8AE000") => "Fire Emblem",
        Some("0400380020056000") => "Splatoon Raiders",
        Some("0400A3A01EE4C000") => "Pokopia",
        Some("04003F40232DE000") => "Hyrule Warriors",
        Some("040057C02159A000") => "Welcome Tour",
        _ => "Other / not listed",
    }
}

fn form_choice(form: Form) -> &'static str {
    match form {
        Form::Laptop => "Laptop",
        Form::Handheld => "Handheld",
        Form::Desktop | Form::Unknown => "Desktop",
    }
}

fn os_line(sys: &SystemInfo) -> String {
    format!("{} {} ({})", sys.os.name, sys.os.display_version, system::os_build_full())
}

fn summary(sys: &SystemInfo, kind: &str, build: &str, profile: &str, game: Option<&str>, note: &str, zip_name: &str) -> String {
    let gpu = sys
        .primary()
        .map(|g| {
            format!(
                "{}, driver {}, {}, Vulkan {}",
                g.name,
                g.driver_version,
                if g.is_integrated { "integrated" } else { "dedicated" },
                if g.vulkan { "yes" } else { "no" }
            )
        })
        .unwrap_or_else(|| "unknown".into());
    let power = match sys.power.on_ac {
        Some(true) => "plugged in",
        Some(false) => "on battery",
        None => "unknown",
    };
    let mut s = format!(
        "### Guarana {kind} report\n\n\
         - Build: {build}\n\
         - Launcher: {}\n\
         - OS: {}\n\
         - CPU: {} ({} cores, {} threads, AVX2 {})\n\
         - GPU: {gpu}\n\
         - RAM: {:.0} GB, page file {:.1} GB\n\
         - Display: {}x{} at {} Hz, HDR {}\n\
         - Device: {} {} ({:?})\n\
         - Power: {power}, {}\n\
         - Profile: {profile}\n",
        env!("CARGO_PKG_VERSION"),
        os_line(sys),
        sys.cpu.name,
        sys.cpu.cores,
        sys.cpu.threads,
        if sys.cpu.avx2 { "yes" } else { "no" },
        sys.ram_gb,
        sys.pagefile_gb,
        sys.display.width,
        sys.display.height,
        sys.display.refresh_hz,
        match sys.display.hdr {
            Some(true) => "on",
            Some(false) => "off",
            None => "unknown",
        },
        sys.device.manufacturer,
        sys.device.model,
        sys.device.form,
        sys.power.plan.as_deref().unwrap_or("plan unknown"),
    );
    if let Some(g) = game {
        s.push_str(&format!("- Last game booted: {g}\n"));
    }
    if !note.is_empty() {
        s.push_str(&format!("\n{note}\n"));
    }
    s.push_str(&format!("\nLogs and system details: `{zip_name}` (made by the launcher; user and PC names removed).\n"));
    s
}

/// `ISSUES?template=..&title=..&field=..`, shortening the longest free-text field until it fits.
fn issue_url(template: &str, title: &str, mut fields: Vec<(&'static str, String)>, shrinkable: &[&str]) -> String {
    let build = |fields: &[(&'static str, String)]| {
        let mut url = format!("{ISSUES}?template={template}&title={}", utf8_percent_encode(title, QUERY));
        for (k, v) in fields {
            if !v.is_empty() {
                url.push_str(&format!("&{k}={}", utf8_percent_encode(v, QUERY)));
            }
        }
        url
    };
    let mut url = build(&fields);
    while url.len() > MAX_URL {
        let Some(idx) = fields
            .iter()
            .enumerate()
            .filter(|(_, (k, v))| shrinkable.contains(k) && !v.is_empty())
            .max_by_key(|(_, (_, v))| v.len())
            .map(|(i, _)| i)
        else {
            break;
        };
        let v = &mut fields[idx].1;
        let keep = v.chars().count() * 3 / 4;
        *v = v.chars().take(keep).collect::<String>() + " [...]";
        if keep < 40 {
            v.clear();
        }
        url = build(&fields);
    }
    url
}

pub fn collect(app: &AppHandle, kind: &str, note: Option<String>) -> CmdResult<ReportResult> {
    if !matches!(kind, "bug" | "performance" | "crash") {
        return Err(format!("Unknown report kind {kind}: use bug, performance or crash"));
    }
    let st = app.state::<AppState>();
    let sys = st.system();
    let persisted = st.load();
    let scrub = Scrubber::new();
    let note = scrub.apply(note.as_deref().unwrap_or("").trim());
    let build = state::current_version(&persisted)
        .map(|v| format!("{} ({})", v.version, v.channel))
        .unwrap_or_else(|| "not installed".into());
    let profile = persisted.profile_id.clone().unwrap_or_else(|| "none".into());

    let dir = paths::reports_dir();
    fs::create_dir_all(&dir).ctx("Could not create the reports folder")?;
    let zip_name = format!("guarana-report-{}.zip", util::stamp());
    let zip_path = dir.join(&zip_name);
    let file = File::create(&zip_path).ctx("Could not create the report")?;
    let mut zip = zip::ZipWriter::new(file);
    let opts = SimpleFileOptions::default().compression_method(zip::CompressionMethod::Deflated);
    let mut add = |name: &str, text: &str| -> CmdResult<()> {
        zip.start_file(name, opts).ctx("Could not write the report")?;
        zip.write_all(text.as_bytes()).ctx("Could not write the report")
    };

    let mut last_game: Option<String> = None;
    let log_dir = paths::emulator_log_dir();
    for name in ["eden_log.txt", "eden_log.txt.old.txt", "eden_log_2.txt", "eden_log_2.txt.old.txt", "eden_log.txt.old"] {
        if let Some(text) = read_log(&log_dir.join(name)) {
            if last_game.is_none() && name == "eden_log.txt" {
                last_game = last_booted_title(&text);
            }
            add(&format!("logs/{name}"), &scrub.apply(&text))?;
        }
    }
    if let Some(text) = read_log(&paths::kept_game_log()) {
        if last_game.is_none() {
            last_game = last_booted_title(&text);
        }
        add("logs/eden_log.last-game.txt", &scrub.apply(&text))?;
    }
    if let Some(text) = read_log(&paths::launcher_log()) {
        add("logs/launcher.log", &scrub.apply(&text))?;
    }
    let sys_json = serde_json::to_string_pretty(&sys).map_err(|e| e.to_string())?;
    add("system.json", &scrub.apply(&sys_json))?;
    if let Ok(text) = fs::read_to_string(paths::user_switches()) {
        add("settings/nx2_switches.txt", &scrub.apply(&text))?;
    }
    // Only [Renderer] of qt-config.ini: other sections hold paths and account fields.
    if let Ok(text) = fs::read_to_string(paths::qt_config()) {
        let mut inside = false;
        let renderer: Vec<&str> = text
            .lines()
            .filter(|l| {
                let t = l.trim();
                if t.starts_with('[') {
                    inside = t.eq_ignore_ascii_case("[Renderer]");
                }
                inside
            })
            .collect();
        add("settings/qt-config-renderer.ini", &scrub.apply(&renderer.join("\r\n")))?;
    }
    let game_label = last_game.as_deref().map(|id| {
        let title = persisted.games.iter().find(|g| g.title_id.eq_ignore_ascii_case(id)).map(|g| g.title.clone());
        match title {
            Some(t) => format!("{t} ({id})"),
            None if id.eq_ignore_ascii_case(locate::OCARINA_TITLE_ID) => format!("The Legend of Zelda: Ocarina of Time ({id})"),
            None => id.to_string(),
        }
    });
    let summary_md = scrub.apply(&summary(&sys, kind, &build, &profile, game_label.as_deref(), &note, &zip_name));
    add("report.md", &summary_md)?;
    drop(add);
    zip.finish().ctx("Could not finish the report")?;

    let gpu = gpu_choice(&sys);
    let game = game_choice(last_game.as_deref());
    let os_major = if sys.os.build >= 22000 { "Windows 11" } else { "Windows 10" };
    let first_line: String = note.lines().next().unwrap_or("").chars().take(60).collect();
    let logs_text = format!("Attach {zip_name} from %LOCALAPPDATA%\\Guarana\\reports (the launcher made it; user and PC names are removed).");
    let issue_url = match kind {
        "performance" => {
            let gpu_name = sys.primary().map(|g| g.name.clone()).unwrap_or_default();
            let device = format!("{} {}", sys.device.manufacturer, sys.device.model).trim().to_string();
            let toggles = format!("Profile {profile}. Switches and Renderer settings are in the attached zip.");
            issue_url(
                "device-report.yml",
                &scrub.apply(&format!("[Device] {device}, {gpu_name}")),
                vec![
                    ("device", scrub.apply(&device)),
                    ("form_factor", form_choice(sys.device.form).to_string()),
                    ("cpu", sys.cpu.name.clone()),
                    ("gpu", gpu.to_string()),
                    ("driver", sys.primary().map(|g| g.driver_version.clone()).unwrap_or_default()),
                    ("ram", format!("{:.0} GB, page file {:.1} GB", sys.ram_gb, sys.pagefile_gb)),
                    ("os", os_line(&sys)),
                    (
                        "power_source",
                        match sys.power.on_ac {
                            Some(false) => "On battery".to_string(),
                            _ => "Plugged in".to_string(),
                        },
                    ),
                    ("power_mode", sys.power.plan.clone().unwrap_or_default()),
                    ("build", build.clone()),
                    ("game", game.to_string()),
                    ("settings", toggles),
                    ("notes", format!("{note}\n\n{logs_text}").trim().to_string()),
                ],
                &["notes"],
            )
        }
        _ => {
            let (template, prefix) = if kind == "crash" { ("crash.yml", "[Crash]") } else { ("game_bug.yml", "[GBug]") };
            let title = match (game_label.as_deref(), first_line.is_empty()) {
                (Some(g), false) => format!("{prefix} {g}: {first_line}"),
                (Some(g), true) => format!("{prefix} {g}"),
                (None, false) => format!("{prefix} {first_line}"),
                (None, true) => format!("{prefix} "),
            };
            issue_url(
                template,
                &title,
                vec![
                    ("build", build.clone()),
                    ("game", game.to_string()),
                    ("gpu", gpu.to_string()),
                    ("os", os_major.to_string()),
                    ("steps", summary_md.clone()),
                    ("logs", logs_text),
                ],
                &["steps"],
            )
        }
    };
    log::info!("Report written to {}", zip_path.display());
    Ok(ReportResult { zip_path: zip_path.to_string_lossy().into_owned(), issue_url, summary_md })
}

// ---- Ask AI ---------------------------------------------------------------------------------------

pub const DOCS_URL: &str = "https://karimalsalah.github.io/guarana/llms-full.txt";
const URL_LIMIT: usize = 1950;

#[derive(Debug, Clone, serde::Serialize)]
pub struct AskAi {
    pub service: String,
    pub url: String,
    pub prompt: String,
    pub url_chars: usize,
}

fn chat_base(service: &str) -> CmdResult<&'static str> {
    match service {
        "claude" => Ok("https://claude.ai/new?q="),
        "chatgpt" => Ok("https://chatgpt.com/?q="),
        other => Err(format!("Unknown assistant {other}: use claude or chatgpt")),
    }
}

fn clip(s: &str, max: usize) -> String {
    let one: String = s.split_whitespace().collect::<Vec<_>>().join(" ");
    if one.chars().count() <= max {
        one
    } else {
        one.chars().take(max.saturating_sub(1)).collect::<String>() + "\u{2026}"
    }
}

/// The chat link for `head` + the optional lines (dropped from the end until it fits) + `tail`.
/// The whole URL stays within `URL_LIMIT` characters.
fn fit_chat_url(base: &str, head: &str, optional: &[String], tail: &str) -> (String, String) {
    let build = |opt: &[String], tail: &str| {
        let mut prompt = head.to_string();
        for l in opt {
            prompt.push('\n');
            prompt.push_str(l);
        }
        if !tail.is_empty() {
            prompt.push_str("\n\n");
            prompt.push_str(tail);
        }
        let url = format!("{base}{}", utf8_percent_encode(&prompt, QUERY));
        (url, prompt)
    };
    let mut keep = optional.len();
    let mut tail = tail.to_string();
    loop {
        let (url, prompt) = build(&optional[..keep], &tail);
        if url.len() <= URL_LIMIT {
            return (url, prompt);
        }
        if keep > 0 {
            keep -= 1;
        } else if tail.chars().count() > 20 {
            let n = tail.chars().count() * 3 / 4;
            tail = tail.chars().take(n).collect();
        } else {
            return (url, prompt);
        }
    }
}

pub fn ask_ai(app: &AppHandle, service: &str, issues: &[String], question: Option<&str>) -> CmdResult<AskAi> {
    let base = chat_base(service)?;
    let st = app.state::<AppState>();
    let sys = st.system();
    let persisted = st.load();
    let scrub = Scrubber::new();
    let build = state::current_version(&persisted)
        .map(|v| format!("{} ({})", v.version, v.channel))
        .unwrap_or_else(|| "not installed".into());
    let gpu = sys
        .primary()
        .map(|g| format!("{}, driver {}, {}", g.name, g.driver_version, if g.is_integrated { "integrated" } else { "dedicated" }))
        .unwrap_or_else(|| "unknown GPU".into());
    let power = match sys.power.on_ac {
        Some(true) => "plugged in",
        Some(false) => "on battery",
        None => "power unknown",
    };
    let mode = crate::modes::status_with(&sys, &crate::community::cached_or_builtin(), &persisted, None);
    let mut optional = vec![
        format!("Guarana {build}, launcher {}.", env!("CARGO_PKG_VERSION")),
        format!("{}; GPU {gpu}; {power}.", os_line(&sys)),
        format!("CPU {}; {:.0} GB RAM, page file {:.1} GB.", sys.cpu.name, sys.ram_gb, sys.pagefile_gb),
        format!("Mode {} (profile {}).", mode.mode, mode.profile_id),
    ];
    if let Some(l) = &mode.autopilot.latest {
        optional.push(clip(&l.text, 160));
    }
    for i in issues.iter().take(6) {
        optional.push(format!("Doctor: {}", clip(i, 110)));
    }
    let optional: Vec<String> = optional.iter().map(|l| scrub.apply(l)).collect();
    let head = format!("I need help with Guarana, a Switch 2 emulator for Windows. First read its docs at {DOCS_URL} . My setup:");
    let tail = scrub.apply(&clip(question.unwrap_or(""), 240));
    let tail = if tail.is_empty() { "What should I check first?".to_string() } else { format!("My question: {tail}") };
    let (url, prompt) = fit_chat_url(base, &head, &optional, &tail);
    util::open_https(&url)?;
    log::info!("Opened {service} with a {}-character prefilled question", url.len());
    Ok(AskAi { service: service.to_string(), url_chars: url.len(), url, prompt })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn chat_links_fit_and_name_the_docs() {
        let head = format!("I need help. Read {DOCS_URL} . My setup:");
        let optional: Vec<String> = (0..12).map(|i| format!("Doctor: line {i} with some \u{e9}xtra text ##&& {}", "x".repeat(100))).collect();
        let (url, prompt) = fit_chat_url("https://claude.ai/new?q=", &head, &optional, "My question: why?");
        assert!(url.len() <= URL_LIMIT, "{}", url.len());
        assert!(url.starts_with("https://claude.ai/new?q=I%20need%20help"));
        assert!(prompt.contains(DOCS_URL) && prompt.ends_with("My question: why?"));
        let (_, all) = fit_chat_url("https://chatgpt.com/?q=", &head, &optional[..1], "q");
        assert!(all.contains("line 0"));
        let (url, _) = fit_chat_url("https://claude.ai/new?q=", &head, &[], &"\u{e9}".repeat(3000));
        assert!(url.len() <= URL_LIMIT, "{}", url.len());
        assert!(chat_base("claude").is_ok() && chat_base("chatgpt").is_ok() && chat_base("bing").is_err());
    }

    #[test]
    fn clip_collapses_whitespace() {
        assert_eq!(clip("a \n  b", 10), "a b");
        assert_eq!(clip("abcdef", 4), "abc\u{2026}");
    }

    #[test]
    fn replaces_case_insensitively() {
        assert_eq!(replace_ci(r"C:\Users\Yousof\AppData", r"c:\users\yousof", "%USERPROFILE%", false), r"%USERPROFILE%\AppData");
        assert_eq!(replace_ci("host DESKTOP-1 and desktop-12", "DESKTOP-1", "%COMPUTERNAME%", true), "host %COMPUTERNAME% and desktop-12");
        assert_eq!(replace_ci("Bookmark", "mark", "X", true), "Bookmark");
        // ASCII letters fold, other characters must match exactly.
        assert_eq!(replace_ci("JÜRGEN jürgen", "Jürgen", "U", false), "JÜRGEN U");
    }

    #[test]
    fn finds_the_last_booted_game() {
        let log = "[  1.0] Loader <Info> Booting game: 0400C3F00006E000\n[ 90.0] Loader <Info> Booting game: 0400486024DEC000 | something\n";
        assert_eq!(last_booted_title(log).as_deref(), Some("0400486024DEC000"));
    }

    #[test]
    fn urls_stay_short() {
        let url = issue_url("game_bug.yml", "[GBug] x", vec![("steps", "a ".repeat(9000)), ("build", "20.0.2".into())], &["steps"]);
        assert!(url.len() <= MAX_URL, "{}", url.len());
        assert!(url.contains("&build=20.0.2"));
        assert!(url.starts_with("https://github.com/karimalsalah/guarana/issues/new?template=game_bug.yml&title=%5BGBug%5D%20x"));
    }
}
