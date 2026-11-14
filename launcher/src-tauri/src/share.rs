//! "Share my numbers": after a game session with at least 60 s of gameplay data, Home offers to open
//! the device-report issue form with this session's numbers filled in. Nothing is sent: the offer
//! is built from the emulator's own log, the form opens in the browser with text fields only, and the
//! user reviews it, adds the scene and a screenshot, and submits it on GitHub (or not).
//!
//! The same scrubbing as reports applies (user name, profile folder, host name). The link has three
//! tiers: everything (at most `report::MAX_URL` characters), the essentials (at most `SHORT_URL`),
//! and, when even those do not fit, a bare form plus the text for the clipboard.

use serde::{Deserialize, Serialize};
use tauri::{AppHandle, Emitter, Manager};

use crate::autopilot::{self, SessionMetrics};
use crate::report::{self, Scrubber};
use crate::state::{self, AppState};
use crate::types::SystemInfo;
use crate::util::{self, CmdResult};
use crate::{modes, system};

pub const EVENT: &str = "share://changed";
/// The least gameplay the numbers must cover before Home offers them.
pub const MIN_GAMEPLAY_SECS: f64 = 60.0;
/// The second try of the link, then the clipboard.
pub const SHORT_URL: usize = 2000;
const TEMPLATE: &str = "device-report.yml";
/// Fields that may be cut shorter, longest first, to fit the full link.
const SHRINKABLE: &[&str] = &["notes", "settings", "results"];

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct ShareOffer {
    pub at: String,
    pub title_id: Option<String>,
    /// The game's name as the launcher knows it, or its program id.
    pub game: String,
    pub game_fps: f64,
    pub within2: f64,
    pub shown_fps: f64,
    pub p50: f64,
    pub p95: f64,
    pub p99: f64,
    pub gameplay_secs: f64,
    pub fluid: bool,
    pub on_battery: bool,
    pub mode: String,
    pub profile: String,
    /// The emulator build the session ran on, as "version (channel)".
    pub build: String,
}

#[derive(Debug, Clone, Serialize)]
pub struct ShareStatus {
    /// False after "Don't ask again": no card is shown and nothing is kept.
    pub enabled: bool,
    pub offer: Option<ShareOfferView>,
}

#[derive(Debug, Clone, Serialize)]
pub struct ShareOfferView {
    #[serde(flatten)]
    pub offer: ShareOffer,
    /// "Your session: 25 game fps, 93% of frames on time. Share it with the community?"
    pub text: String,
}

#[derive(Debug, Clone, Serialize, PartialEq)]
pub struct ShareLink {
    pub url: String,
    pub url_chars: usize,
    /// `full`, `short` (essentials only) or `clipboard` (the URL opens an empty form).
    pub level: String,
    /// All the numbers and fields as plain text: what goes to the clipboard in `short` and `clipboard`.
    pub text: String,
}

/// "Your session: 25 game fps, 93% of frames on time. Share it with the community?"
pub fn card_text(o: &ShareOffer) -> String {
    format!(
        "Your session: {:.0} game fps, {:.0}% of frames on time. Share it with the community?",
        o.game_fps, o.within2
    )
}

pub fn view(enabled: bool, offer: Option<&ShareOffer>) -> ShareStatus {
    ShareStatus {
        enabled,
        offer: offer.filter(|_| enabled).map(|o| ShareOfferView { text: card_text(o), offer: o.clone() }),
    }
}

/// An offer for a finished session, or none when it was short, crashed or has no numbers.
#[allow(clippy::too_many_arguments)]
pub fn offer_from(
    m: &SessionMetrics,
    exit_code: i32,
    on_battery: bool,
    game: &str,
    mode: &str,
    profile: &str,
    build: &str,
    at: &str,
) -> Option<ShareOffer> {
    if exit_code != 0 || m.gameplay_secs < MIN_GAMEPLAY_SECS {
        return None;
    }
    let shown = m.shown.as_ref()?;
    let game_fps = m.game_fps.filter(|f| *f > 0.0)?;
    Some(ShareOffer {
        at: at.to_string(),
        title_id: m.title_id.clone(),
        game: game.to_string(),
        game_fps,
        within2: shown.within2,
        shown_fps: shown.fps,
        p50: shown.p50,
        p95: shown.p95,
        p99: shown.p99,
        gameplay_secs: m.gameplay_secs,
        fluid: m.fluid_active.unwrap_or(false),
        on_battery,
        mode: mode.to_string(),
        profile: profile.to_string(),
        build: build.to_string(),
    })
}

fn results_text(o: &ShareOffer) -> String {
    // Exactly one "N fps" figure, and no "p50"/"p99" or "frames per second": the report bot reads the
    // Results text for the game fps and its frame times, and refuses text with several figures. The
    // shown-frame intervals are not game frame times, so they go to the notes instead.
    format!(
        "Average {:.1} fps (game fps: the frames the game renders, generated frames not counted). \
         {:.0}% of shown frames within 2 ms of the median; even frame pacing {}. Game: {}.\n\
         Averaged over {:.0} s of play (10 s windows of the emulator's own log, boot window left out). \
         Read by the Guarana launcher, not measured with tools/bench; the scene was not checked.",
        o.game_fps,
        o.within2,
        if o.fluid { "on" } else { "off" },
        o.game,
        o.gameplay_secs
    )
}

fn results_short(o: &ShareOffer) -> String {
    format!(
        "Average {:.1} fps (game fps), {:.0}% of shown frames within 2 ms of the median, even pacing {}, {:.0} s of play. {}. Launcher log, scene not checked.",
        o.game_fps,
        o.within2,
        if o.fluid { "on" } else { "off" },
        o.gameplay_secs,
        o.game
    )
}

/// The intervals between shown frames (not the game's frame times) for the notes.
fn intervals_text(o: &ShareOffer) -> String {
    format!(
        "Shown-frame intervals: median {:.1} ms, 95th percentile {:.1} ms, 99th percentile {:.1} ms, shown at {:.1} per second.",
        o.p50, o.p95, o.p99, o.shown_fps
    )
}

type Fields = Vec<(&'static str, String)>;

/// The form's text fields by id. The scene, the screenshot and the choices (form factor, graphics
/// card, game, power source) are left to the user: only text fields are filled.
fn fields(sys: &SystemInfo, o: &ShareOffer, launcher_version: &str, scrub: &Scrubber, short: bool) -> Fields {
    let device = format!("{} {}", sys.device.manufacturer, sys.device.model).trim().to_string();
    let gpu = sys.primary();
    let gpu_name = gpu.map(|g| g.name.clone()).unwrap_or_default();
    let device_line = if device.is_empty() { gpu_name.clone() } else { device };
    let os = format!("{} {} ({})", sys.os.name, sys.os.display_version, system::os_build_full());
    let power = format!(
        "{}{}",
        sys.power.plan.clone().unwrap_or_else(|| "plan unknown".into()),
        if o.on_battery { ", on battery" } else { ", plugged in" }
    );
    let mut f: Fields = vec![
        ("device", device_line),
        ("cpu", sys.cpu.name.clone()),
        // The version alone: the report bot reads the first version number in this field.
        ("driver", gpu.map(|g| g.driver_version.clone()).unwrap_or_default()),
        ("ram", format!("{:.0} GB, page file {:.1} GB", sys.ram_gb, sys.pagefile_gb)),
        ("os", os),
        ("power_mode", power),
        ("build", o.build.clone()),
    ];
    if short {
        f.push(("results", results_short(o)));
    } else {
        f.push((
            "settings",
            format!(
                "Guarana launcher {launcher_version}, mode {}, profile {}. The launcher does not know about settings changed by hand or \
                 EDEN_NX2_* switches set elsewhere: add them here, or write none.",
                o.mode, o.profile
            ),
        ));
        f.push(("results", results_text(o)));
        f.push((
            "notes",
            format!(
                "Filled in by the Guarana launcher's \"Share my numbers\" from the last session's log; nothing was sent automatically. \
                 GPU: {gpu_name}. Display {}x{} at {} Hz. {} \
                 Still to add by hand: the scene, a screenshot of it, the form factor, graphics card, game and power source.",
                sys.display.width,
                sys.display.height,
                sys.display.refresh_hz,
                intervals_text(o)
            ),
        ));
    }
    f.into_iter().map(|(k, v)| (k, scrub.apply(&v))).collect()
}

/// The fields as the plain text the user can paste into the form.
pub fn clipboard_text(fields: &Fields) -> String {
    fields.iter().map(|(k, v)| format!("{k}: {v}")).collect::<Vec<_>>().join("\n")
}

/// The link in its three tiers.
pub fn link(title: &str, full: Fields, short: Fields) -> ShareLink {
    let text = clipboard_text(&full);
    let url = report::issue_url_limit(TEMPLATE, title, full, SHRINKABLE, report::MAX_URL);
    if url.len() <= report::MAX_URL {
        return ShareLink { url_chars: url.len(), url, level: "full".into(), text };
    }
    let url = report::issue_url_limit(TEMPLATE, title, short, &["results"], SHORT_URL);
    if url.len() <= SHORT_URL {
        return ShareLink { url_chars: url.len(), url, level: "short".into(), text };
    }
    let url = format!("{}?template={TEMPLATE}", report::ISSUES);
    ShareLink { url_chars: url.len(), url, level: "clipboard".into(), text }
}

/// The offer's link on this PC.
pub fn build_link(sys: &SystemInfo, o: &ShareOffer) -> ShareLink {
    let scrub = Scrubber::new();
    let launcher = env!("CARGO_PKG_VERSION");
    let full = fields(sys, o, launcher, &scrub, false);
    let short = fields(sys, o, launcher, &scrub, true);
    let device = format!("{} {}", sys.device.manufacturer, sys.device.model).trim().to_string();
    let gpu = sys.primary().map(|g| g.name.clone()).unwrap_or_default();
    let title = scrub.apply(&if device.is_empty() { format!("[Device] {gpu}") } else { format!("[Device] {device}, {gpu}") });
    link(&title, full, short)
}

// ---- state and the session hook -----------------------------------------------------------------------

pub fn status(app: &AppHandle) -> ShareStatus {
    let p = app.state::<AppState>().load();
    view(!p.share_prompt_off, p.share_offer.as_ref())
}

/// Called when a game started by the launcher exits (before the autopilot). Blocking.
pub fn after_session(app: &AppHandle, info: &autopilot::SessionInfo) {
    let st = app.state::<AppState>();
    let persisted = st.load();
    if persisted.share_prompt_off || info.exit_code != 0 {
        return;
    }
    let Some((text, key)) = autopilot::last_session_log(info.seconds as u64 + 600) else { return };
    if persisted.share_session_key.as_deref() == Some(key.as_str()) {
        return;
    }
    let mut m = autopilot::parse_log(&text);
    if m.title_id.is_none() {
        m.title_id = info.title_id.clone();
    }
    let game = m
        .title_id
        .as_deref()
        .map(|id| {
            persisted
                .games
                .iter()
                .find(|g| g.title_id.eq_ignore_ascii_case(id))
                .map(|g| g.title.clone())
                .unwrap_or_else(|| match id {
                    crate::locate::OCARINA_TITLE_ID => "The Legend of Zelda: Ocarina of Time".to_string(),
                    other => other.to_string(),
                })
        })
        .unwrap_or_else(|| "unknown game".into());
    let build = state::current_version(&persisted)
        .map(|v| format!("{} ({})", v.version, v.channel))
        .unwrap_or_else(|| "unknown".into());
    let mode = modes::current(&persisted);
    let profile = persisted.profile_id.clone().unwrap_or_else(|| "none".into());
    let offer = offer_from(&m, info.exit_code, info.launch_on_battery, &game, &mode, &profile, &build, &util::now_rfc3339());
    log::info!("Share my numbers: {}", if offer.is_some() { "offered" } else { "no offer (short session or no numbers)" });
    let _ = st.update(|s| {
        s.share_session_key = Some(key.clone());
        if offer.is_some() {
            s.share_offer = offer.clone();
        }
    });
    let _ = app.emit(EVENT, status(app));
}

pub fn dismiss(app: &AppHandle) -> CmdResult<ShareStatus> {
    app.state::<AppState>().update(|s| s.share_offer = None)?;
    let st = status(app);
    let _ = app.emit(EVENT, &st);
    Ok(st)
}

pub fn set_prompt(app: &AppHandle, enabled: bool) -> CmdResult<ShareStatus> {
    app.state::<AppState>().update(|s| {
        s.share_prompt_off = !enabled;
        if !enabled {
            s.share_offer = None;
        }
    })?;
    let st = status(app);
    let _ = app.emit(EVENT, &st);
    Ok(st)
}

/// The link for the pending offer. The offer stays until the UI dismisses it.
pub fn open_link(app: &AppHandle) -> CmdResult<ShareLink> {
    let st = app.state::<AppState>();
    let offer = st.load().share_offer.ok_or("There is nothing to share right now")?;
    Ok(build_link(&st.system(), &offer))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::autopilot::Shown;
    use crate::types::{CpuInfo, DeviceInfo, DisplayInfo, Form, Gpu, OsInfo, PowerInfo, Vendor};

    fn metrics(secs: f64, fps: Option<f64>, within: f64) -> SessionMetrics {
        SessionMetrics {
            title_id: Some("0400486024DEC000".into()),
            shown: Some(Shown { fps: 49.6, p50: 16.7, p95: 33.4, p99: 100.1, within2: within, windows: 6 }),
            game_fps: fps,
            fluid_active: Some(true),
            gameplay_secs: secs,
            ..Default::default()
        }
    }

    fn offer() -> ShareOffer {
        offer_from(&metrics(75.0, Some(25.2), 93.4), 0, false, "The Legend of Zelda: Ocarina of Time", "auto", "intel-arc-igpu", "20.1.0 (stable)", "2026-11-20T10:00:00Z").unwrap()
    }

    fn sys() -> SystemInfo {
        SystemInfo {
            os: OsInfo { name: "Windows 11".into(), build: 26100, display_version: "24H2".into() },
            cpu: CpuInfo { name: "Intel(R) Core(TM) Ultra 9 285H".into(), cores: 16, threads: 16, avx2: true, avx512: false, hybrid: true },
            ram_gb: 32.0,
            pagefile_gb: 8.0,
            gpus: vec![Gpu {
                name: "Intel(R) Arc(TM) 140T GPU (16GB)".into(),
                vendor: Vendor::Intel,
                driver_version: "32.0.101.9175".into(),
                is_integrated: true,
                dedicated_mb: 128,
                shared_mb: 16000,
                vulkan: true,
                arch_hint: Some("xe-lpg-plus".into()),
                family: None,
                vendor_id: 0x8086,
                device_id: 0,
                driver_date: None,
            }],
            primary_gpu: 0,
            display: DisplayInfo { width: 2880, height: 1800, refresh_hz: 120, vrr: None, hdr: None },
            power: PowerInfo { on_ac: Some(true), battery_percent: None, plan: Some("Best performance".into()) },
            hags: None,
            device: DeviceInfo { manufacturer: "ASUS".into(), model: "Zenbook S 14".into(), form: Form::Laptop, profile_hint: None },
            vc_redist: true,
            webview2: None,
            free_disk_gb: 100.0,
        }
    }

    #[test]
    fn the_card_says_what_the_session_showed() {
        assert_eq!(card_text(&offer()), "Your session: 25 game fps, 93% of frames on time. Share it with the community?");
    }

    #[test]
    fn only_a_minute_of_good_data_is_offered() {
        let ok = |m: &SessionMetrics, code: i32| offer_from(m, code, false, "g", "auto", "p", "b", "t");
        assert!(ok(&metrics(60.0, Some(30.0), 90.0), 0).is_some());
        assert!(ok(&metrics(59.9, Some(30.0), 90.0), 0).is_none(), "under 60 s");
        assert!(ok(&metrics(300.0, Some(30.0), 90.0), 1).is_none(), "crashed");
        assert!(ok(&metrics(300.0, None, 90.0), 0).is_none(), "no game fps");
        assert!(ok(&metrics(300.0, Some(0.0), 90.0), 0).is_none());
        let mut no_shown = metrics(300.0, Some(30.0), 90.0);
        no_shown.shown = None;
        assert!(ok(&no_shown, 0).is_none());
    }

    #[test]
    fn a_stopped_prompt_shows_no_card() {
        let o = offer();
        assert!(view(true, Some(&o)).offer.is_some());
        assert!(view(false, Some(&o)).offer.is_none());
        assert!(view(true, None).offer.is_none());
    }

    #[test]
    fn the_link_fills_only_text_fields_and_leaves_the_scene_empty() {
        let l = build_link(&sys(), &offer());
        assert_eq!(l.level, "full");
        assert!(l.url.starts_with("https://github.com/karimalsalah/guarana/issues/new?template=device-report.yml&title="));
        assert!(l.url_chars <= report::MAX_URL && l.url_chars == l.url.len());
        for id in ["device", "cpu", "driver", "ram", "os", "power_mode", "build", "settings", "results", "notes"] {
            assert!(l.url.contains(&format!("&{id}=")), "missing {id}");
        }
        for id in ["scene", "screenshot", "game", "gpu", "form_factor", "power_source", "bench", "credit"] {
            assert!(!l.url.contains(&format!("&{id}=")), "{id} must be left to the user");
        }
        assert!(l.url.contains("25.2") && l.url.contains("20.1.0"));
        // The driver field holds the version alone; the results read as one fps figure.
        assert!(l.text.contains("driver: 32.0.101.9175
"), "{}", l.text);
        let results = results_text(&offer());
        assert_eq!(results.matches(" fps").count(), 2, "{results}"); // "25.2 fps (game fps: ..."
        assert!(results.contains("25.2 fps") && !results.contains("49.6") && !results.contains("p50") && !results.contains("p99"));
        assert!(!results.contains("frames per second") && !results.contains("frames/s"));
    }

    #[test]
    fn names_and_paths_are_scrubbed() {
        let scrub = Scrubber::for_names("alice", "ALICE-PC");
        let mut s = sys();
        s.device.model = "Zenbook of ALICE-PC".into();
        let mut o = offer();
        o.profile = r"C:\Users\alice\profiles".into();
        let f = fields(&s, &o, "0.1.0", &scrub, false);
        let all = clipboard_text(&f);
        assert!(!all.to_lowercase().contains("alice"), "{all}");
        assert!(all.contains("%COMPUTERNAME%"));
    }

    #[test]
    fn long_fields_fall_back_to_the_short_link_then_the_clipboard() {
        let scrub = Scrubber::for_names("alice", "ALICE-PC");
        let o = offer();
        // A very long free-text field: the full link is shrunk to fit 7000.
        let mut s = sys();
        let mut long = o.clone();
        long.game = "g".repeat(9000);
        let l = link("[Device] t", fields(&s, &long, "0.1.0", &scrub, false), fields(&s, &long, "0.1.0", &scrub, true));
        assert_eq!(l.level, "full");
        assert!(l.url.len() <= report::MAX_URL, "{}", l.url.len());
        // Fields that only the short tier can carry: essentials still over 2000 -> clipboard.
        s.device.model = "y".repeat(7500);
        let full = fields(&s, &o, "0.1.0", &scrub, false);
        let short = fields(&s, &o, "0.1.0", &scrub, true);
        let l = link("[Device] t", full.clone(), short);
        assert_eq!(l.level, "clipboard");
        assert_eq!(l.url, "https://github.com/karimalsalah/guarana/issues/new?template=device-report.yml");
        assert!(l.text.contains(&"y".repeat(7500)), "the clipboard keeps everything");
    }

    #[test]
    fn the_short_tier_stays_within_2000_characters() {
        let scrub = Scrubber::for_names("alice", "ALICE-PC");
        let o = offer();
        let mut s = sys();
        // Over 7000 characters with the long device name, but the essentials (with a short one) fit 2000.
        s.device.model = "z".repeat(7500);
        let full = fields(&s, &o, "0.1.0", &scrub, false);
        let mut short = fields(&s, &o, "0.1.0", &scrub, true);
        short.iter_mut().find(|(k, _)| *k == "device").unwrap().1 = "ASUS Zenbook".into();
        let l = link("[Device] t", full, short);
        assert_eq!(l.level, "short");
        assert!(l.url.len() <= SHORT_URL, "{}", l.url.len());
    }
}
