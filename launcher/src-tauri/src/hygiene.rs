//! Settings hygiene (issue #125), first slice: what the doctor's `switch_file`, `settings_file`,
//! `cache_files` and `user_folder` rows find. Every rule here only reports. Nothing is fixed,
//! written, renamed or deleted; the fixes come in later slices of #125.
//!
//! The rules are pure functions over the text, bytes and names they are handed, so the tests run
//! on fixture strings. The readers at the end open only Guarana's own settings files
//! (`nx2_switches.txt`, `config\qt-config.ini`, `config\custom\*.ini`, the same two files in
//! `%APPDATA%\lichii`), the names, sizes and first bytes of shader-cache files, the installed build's
//! `build-facts.json`, and the names of `EDEN_NX2_*` environment variables. `may_read` refuses any
//! path under `keys`, `nand`, `sdmc`, `load` or `dump`; nothing here opens firmware, a game file or
//! a save.

use std::collections::BTreeMap;
use std::fs;
use std::io::Read;
use std::path::{Component, Path, PathBuf};
use std::time::{Duration, SystemTime};

use serde::Deserialize;
use winreg::enums::{HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE, KEY_READ};
use winreg::RegKey;

use crate::state::{self, Persisted};
use crate::types::{CheckStatus, Gpu, SystemInfo, ToggleValue, Vendor};
use crate::{modes, paths, profiles, util};

/// What one rule found. `rule` is the rule's id in the #125 design (C1, C2, ...).
#[derive(Debug, Clone, PartialEq)]
pub struct Finding {
    pub rule: &'static str,
    pub status: CheckStatus,
    pub text: String,
}

fn finding(rule: &'static str, status: CheckStatus, text: impl Into<String>) -> Finding {
    Finding { rule, status, text: text.into() }
}

const EXTRAPOLATE: &str = "EDEN_NX2_EXTRAPOLATE";
const FLUID: &str = "EDEN_NX2_FLUID";
/// The block Guarana's own settings page writes into `nx2_switches.txt`
/// (`src/common/guarana_modes.cpp`).
const MODE_BLOCK_BEGIN: &str = "# >>> Guarana mode";
const MODE_BLOCK_END: &str = "# <<< Guarana mode";
const RENDERER: &str = "Renderer";

// ---------------------------------------------------------------------------------------------
// Switch files

/// One `NAME=VALUE` line as the emulator's loader reads it (`LoadNx2Switches`,
/// `src/lichii/nx2_switches.h`): spaces, tabs and CR trimmed, lines starting with `#` skipped, a
/// value wholly in double quotes unquoted. Nothing else is stripped: a ` # note` after a value is
/// part of the value, and a byte-order mark is part of the first line's name.
struct SwitchLine<'a> {
    /// 1-based.
    number: usize,
    raw: &'a str,
    name: &'a str,
    value: &'a str,
    quoted: bool,
}

fn is_blank(c: char) -> bool {
    c == ' ' || c == '\t' || c == '\r'
}

fn switch_lines(text: &str) -> Vec<SwitchLine<'_>> {
    let mut out = Vec::new();
    for (i, line) in text.split('\n').enumerate() {
        let t = line.trim_matches(is_blank);
        if t.is_empty() || t.starts_with('#') {
            continue;
        }
        let Some(eq) = t.find('=') else { continue };
        let name = t[..eq].trim_end_matches(is_blank);
        let mut value = t[eq + 1..].trim_start_matches(is_blank);
        let quoted = value.len() >= 2 && value.starts_with('"') && value.ends_with('"');
        if quoted {
            value = &value[1..value.len() - 1];
        }
        if !name.is_empty() {
            out.push(SwitchLine { number: i + 1, raw: t, name, value, quoted });
        }
    }
    out
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Zone {
    Player,
    Launcher,
    Mode,
}

/// Which block each line (split at `\n`) is in; marker lines belong to their block.
fn zones(text: &str) -> Vec<Zone> {
    let mut zone = Zone::Player;
    let mut out = Vec::new();
    for line in text.split('\n') {
        let t = line.trim();
        if t.starts_with(profiles::BLOCK_BEGIN) {
            zone = Zone::Launcher;
            out.push(zone);
        } else if t.starts_with(MODE_BLOCK_BEGIN) {
            zone = Zone::Mode;
            out.push(zone);
        } else if t.starts_with(profiles::BLOCK_END) || t.starts_with(MODE_BLOCK_END) {
            out.push(zone);
            zone = Zone::Player;
        } else {
            out.push(zone);
        }
    }
    out
}

fn has_launcher_block(text: &str) -> bool {
    text.split('\n').any(|l| l.trim().starts_with(profiles::BLOCK_BEGIN))
}

/// `NAME -> VALUE` inside the launcher's block, as the loader reads them; None without a block.
fn launcher_block(text: &str) -> Option<BTreeMap<String, String>> {
    if !has_launcher_block(text) {
        return None;
    }
    let zones = zones(text);
    let mut out = BTreeMap::new();
    for l in switch_lines(text) {
        if zones.get(l.number - 1) == Some(&Zone::Launcher) {
            out.insert(l.name.to_string(), l.value.to_string());
        }
    }
    Some(out)
}

/// C1, stale block: the launcher's block differs from what this launcher writes for the saved
/// profile and overrides on this PC (`profiles::block_switches`). Typically `EXTRAPOLATE=1` from a
/// launcher before #122 on a machine with an Intel integrated GPU.
fn stale_block(block: &BTreeMap<String, String>, expected: &[(String, String)]) -> Option<Finding> {
    let mut diffs = Vec::new();
    for (k, v) in block {
        match expected.iter().find(|(name, _)| name == k) {
            None => diffs.push(format!("{k}={v} (this launcher leaves it out)")),
            Some((_, e)) if e != v => diffs.push(format!("{k}={v} (this launcher writes {e})")),
            Some(_) => {}
        }
    }
    for (k, e) in expected {
        if !block.contains_key(k) {
            diffs.push(format!("no {k} (this launcher writes {k}={e})"));
        }
    }
    if diffs.is_empty() {
        return None;
    }
    let mut text = format!(
        "The launcher's block in nx2_switches.txt is not what this launcher writes for your saved settings: {}.",
        diffs.join("; ")
    );
    if block.get(EXTRAPOLATE).map(String::as_str) == Some("1") && !expected.iter().any(|(k, _)| k == EXTRAPOLATE) {
        text.push_str(" It comes from an older launcher: with an Intel integrated GPU, EDEN_NX2_EXTRAPOLATE=1 turns off the measured 120 and 144 Hz sets of Ocarina of Time (at 144 Hz, 58.5 frames shown a second instead of 91.5; issue #119).");
    }
    text.push_str(" The launcher rewrites its block each time you start a game from it, so start your next game from the launcher; starting guarana.exe from its own shortcut keeps the old block until then.");
    Some(finding("C1", CheckStatus::Warn, text))
}

/// C1, `FLUID=1`: the block's `EDEN_NX2_FLUID=1` (Even frame pacing) wins over Ocarina of Time's
/// measured `FLUID=2` at every refresh rate, on the integrated GPUs whose profiles turn it on.
/// Report only: leaving the line out would cost every other game; issue #125's emulator slice lets
/// the game's measured value win instead.
fn fluid_over_ocarina(block: &BTreeMap<String, String>, ocarina_in_library: bool, gpus: &[Gpu]) -> Option<Finding> {
    let igpu = gpus.iter().any(|g| g.is_integrated && matches!(g.vendor, Vendor::Intel | Vendor::Amd));
    if block.get(FLUID).map(String::as_str) != Some("1") || !ocarina_in_library || !igpu {
        return None;
    }
    Some(finding(
        "C1",
        CheckStatus::Info,
        "The launcher's block sets EDEN_NX2_FLUID=1 (Even frame pacing). Ocarina of Time has a measured value of its own, EDEN_NX2_FLUID=2, but a line in nx2_switches.txt wins over it, so Ocarina runs with 1. Nothing to do for now: a coming update lets the game's measured value win (issue #125). Turning Even frame pacing off under Home > Advanced gives Ocarina its own value but takes even pacing from your other games.",
    ))
}

/// C2: the launcher's block and the settings page's block name the same switch. The loader keeps
/// the line lower in the file.
fn same_switch_in_both_blocks(text: &str) -> Option<Finding> {
    let zones = zones(text);
    let mut launcher: BTreeMap<&str, (usize, &str)> = BTreeMap::new();
    let mut mode: BTreeMap<&str, (usize, &str)> = BTreeMap::new();
    for l in switch_lines(text) {
        match zones.get(l.number - 1) {
            Some(Zone::Launcher) => {
                launcher.insert(l.name, (l.number, l.value));
            }
            Some(Zone::Mode) => {
                mode.insert(l.name, (l.number, l.value));
            }
            _ => {}
        }
    }
    let mut both = Vec::new();
    for (name, (ln, lv)) in &launcher {
        if let Some((mn, mv)) = mode.get(name) {
            let used = if ln > mn { format!("{lv}, the launcher's") } else { format!("{mv}, the settings page's") };
            both.push(format!("{name} (Guarana uses {used})"));
        }
    }
    if both.is_empty() {
        return None;
    }
    Some(finding(
        "C2",
        CheckStatus::Warn,
        format!(
            "Two blocks in nx2_switches.txt set the same switch: the launcher's and the one Guarana's settings page writes. Guarana uses the line lower in the file, so the settings page can show a value that is not the one in use: {}. Set it in one place only, the launcher (Home > Advanced) or Guarana's settings page.",
            both.join("; ")
        ),
    ))
}

/// A value that is a number or a single word (`igpu`, `1`, `0.75`), not a path or free text.
fn plain_word(s: &str) -> bool {
    !s.is_empty() && s.chars().all(|c| c.is_ascii_alphanumeric() || matches!(c, '_' | '.' | '+' | '-'))
}

/// Where an unquoted ` # note` starts in a value: a `#` right after a space or tab.
fn note_start(value: &str) -> Option<usize> {
    value
        .char_indices()
        .find(|&(i, c)| c == '#' && i > 0 && value[..i].ends_with(|p: char| p == ' ' || p == '\t'))
        .map(|(i, _)| i)
}

/// C3: lines the loader reads wrongly. A byte-order mark becomes part of the first line's name; an
/// unquoted ` # note` becomes part of the value. A note is flagged only after a number or a single
/// word (or, with the installed build's switch registry, for a switch whose type is `number` or
/// `word`): `EDEN_NX2_PIPELINE_STATS=C:\runs #2` is a folder name, not a note. With the registry,
/// an `EDEN_NX2_*` name it does not list is reported too.
fn misread_lines(text: &str, registry: Option<&BTreeMap<String, String>>) -> Vec<Finding> {
    let mut out = Vec::new();
    let lines = switch_lines(text);
    if text.starts_with('\u{feff}') {
        // A comment line holding an `=` is read as a name too, but no switch is lost there.
        let first = lines.first().filter(|l| l.number == 1 && !l.name.trim_start_matches('\u{feff}').starts_with('#'));
        match first {
            Some(l) => out.push(finding(
                "C3",
                CheckStatus::Warn,
                format!(
                    "nx2_switches.txt starts with a byte-order mark, an invisible character some editors add when they save. Guarana reads it as part of the first line's name, so {} on line 1 does nothing. Save the file as UTF-8 without it (Notepad: Save as, Encoding: UTF-8).",
                    l.name.trim_start_matches('\u{feff}')
                ),
            )),
            None => out.push(finding(
                "C3",
                CheckStatus::Info,
                "nx2_switches.txt starts with a byte-order mark, an invisible character some editors add when they save. It is harmless while the first line is a comment or blank, but a switch on line 1 would be ignored. Save the file as UTF-8 without it (Notepad: Save as, Encoding: UTF-8).",
            )),
        }
    }
    for l in &lines {
        if l.quoted {
            continue;
        }
        let Some(at) = note_start(l.value) else { continue };
        let head = l.value[..at].trim_end_matches(is_blank);
        let kind = registry.and_then(|r| r.get(l.name)).map(String::as_str);
        let flagged = match kind {
            Some(kind) => kind == "number" || kind == "word",
            None => plain_word(head),
        };
        if !flagged {
            continue;
        }
        // Number switches are read with `strtoul`/`strtod`, which stop at the blank: `1 # note`
        // still gives 1. A word is compared whole (`POWER_POLICY=igpu # x` is not `igpu`).
        let number = kind.map_or_else(|| head.parse::<f64>().is_ok() && head.bytes().any(|b| b.is_ascii_digit()), |k| k == "number");
        if number {
            out.push(finding(
                "C3",
                CheckStatus::Info,
                format!(
                    "nx2_switches.txt line {}: \"{}\". Guarana takes everything after the = as the value, the note too, so it reads \"{}\". A switch that reads a number stops at the blank and still gets {head}, but one that compares the whole text does not match it. Put the note on a line of its own that starts with #.",
                    l.number, l.raw, l.value
                ),
            ));
        } else {
            out.push(finding(
                "C3",
                CheckStatus::Warn,
                format!(
                    "nx2_switches.txt line {}: \"{}\". Guarana takes everything after the = as the value, the note too, so it reads \"{}\", which is not \"{head}\", and the switch does nothing. Put the note on a line of its own that starts with #.",
                    l.number, l.raw, l.value
                ),
            ));
        }
    }
    if let Some(known) = registry {
        for l in &lines {
            let name = l.name.trim_start_matches('\u{feff}');
            if name.starts_with("EDEN_NX2_") && !known.contains_key(name) {
                out.push(finding(
                    "C3",
                    CheckStatus::Info,
                    format!(
                        "nx2_switches.txt line {}: {name} is not a switch the installed Guarana knows, so it does nothing (a typo, or a switch an older version had).",
                        l.number
                    ),
                ));
            }
        }
    }
    out
}

/// C4: `EDEN_NX2_*` variables Windows keeps for the user or the whole system. They win over every
/// switch file and every launcher setting (`src/lichii/nx2_switches.h`).
fn persistent_variables(user: &[(String, String)], system: &[(String, String)]) -> Vec<Finding> {
    let list = |vars: &[(String, String)]| -> Vec<String> {
        vars.iter()
            .filter(|(n, _)| n.to_ascii_uppercase().starts_with("EDEN_NX2_"))
            .map(|(n, v)| format!("{n}={v}"))
            .collect()
    };
    let mut out = Vec::new();
    let mine = list(user);
    if !mine.is_empty() {
        out.push(finding(
            "C4",
            CheckStatus::Warn,
            format!(
                "Windows keeps {} as environment variable(s) of your account. An environment variable wins over every switch file and every launcher setting, in every game. If you did not set it on purpose, remove it: Start, type \"Edit environment variables for your account\".",
                mine.join(", ")
            ),
        ));
    }
    let all = list(system);
    if !all.is_empty() {
        out.push(finding(
            "C4",
            CheckStatus::Warn,
            format!(
                "Windows keeps {} as system-wide environment variable(s). An environment variable wins over every switch file and every launcher setting, in every game. If nobody set it on purpose, an administrator can remove it: Start, type \"Edit the system environment variables\".",
                all.join(", ")
            ),
        ));
    }
    out
}

/// C5: a launcher block nobody rewrites: Custom mode without a saved profile this launcher knows
/// (`launch.rs` rewrites the block only from a known one).
fn orphan_block(has_block: bool, mode: &str, profile_known: bool) -> Option<Finding> {
    if !has_block || mode != modes::CUSTOM || profile_known {
        return None;
    }
    Some(finding(
        "C5",
        CheckStatus::Warn,
        "The launcher's block in nx2_switches.txt is no longer rewritten: the launcher is in Custom mode without a profile it knows, so the block's lines keep applying as an earlier launcher wrote them. Pick a mode on Home, or apply a profile under Home > Advanced, and the launcher writes the block again.",
    ))
}

/// What the `switch_file` row reads.
pub struct SwitchFileInput<'a> {
    /// The user folder's `nx2_switches.txt`; None when there is none.
    pub text: Option<&'a str>,
    /// `profiles::block_switches` for the saved profile and overrides; None when there is no saved
    /// profile this launcher knows, or its overrides do not apply.
    pub expected: Option<&'a [(String, String)]>,
    pub mode: &'a str,
    pub profile_known: bool,
    pub ocarina_in_library: bool,
    pub gpus: &'a [Gpu],
    /// The installed build's switch registry (`build-facts.json`): name -> value type.
    pub registry: Option<&'a BTreeMap<String, String>>,
    pub user_env: &'a [(String, String)],
    pub system_env: &'a [(String, String)],
    /// `<version>\user` when the installed build runs portable.
    pub portable_dir: Option<&'a Path>,
}

pub fn switch_file(i: &SwitchFileInput) -> Vec<Finding> {
    let mut out = Vec::new();
    if let Some(dir) = i.portable_dir {
        out.push(finding(
            "portable",
            CheckStatus::Warn,
            format!(
                "Guarana runs portable: it reads its settings from {}, not from the folder the launcher writes to, so the launcher's profiles and these checks do not reach the files it uses.",
                dir.display()
            ),
        ));
    }
    if let Some(text) = i.text {
        let block = launcher_block(text);
        if let (Some(block), Some(expected)) = (&block, i.expected) {
            out.extend(stale_block(block, expected));
        }
        if let Some(block) = &block {
            out.extend(fluid_over_ocarina(block, i.ocarina_in_library, i.gpus));
        }
        out.extend(same_switch_in_both_blocks(text));
        out.extend(misread_lines(text, i.registry));
        out.extend(orphan_block(block.is_some(), i.mode, i.profile_known));
    }
    out.extend(persistent_variables(i.user_env, i.system_env));
    out
}

// ---------------------------------------------------------------------------------------------
// Settings files

/// `(section, key, value)` for every `key=value` line of an ini file (SimpleIni: `;` and `#`
/// comments, `[section]` headers).
fn ini_entries(text: &str) -> Vec<(String, String, String)> {
    let mut section = String::new();
    let mut out = Vec::new();
    for line in text.trim_start_matches('\u{feff}').lines() {
        let t = line.trim();
        if t.is_empty() || t.starts_with(';') || t.starts_with('#') {
            continue;
        }
        if t.starts_with('[') && t.ends_with(']') && t.len() >= 2 {
            section = t[1..t.len() - 1].trim().to_string();
            continue;
        }
        if let Some((k, v)) = t.split_once('=') {
            out.push((section.clone(), k.trim().to_string(), v.trim().to_string()));
        }
    }
    out
}

fn ini_get<'a>(entries: &'a [(String, String, String)], section: &str, key: &str) -> Option<&'a str> {
    entries
        .iter()
        .find(|(s, k, _)| s.eq_ignore_ascii_case(section) && k.eq_ignore_ascii_case(key))
        .map(|(_, _, v)| v.as_str())
}

/// The two `[Renderer]` cache settings (`src/common/settings.h`), their label in Guarana's settings
/// and what turning them off costs.
const CACHE_KEYS: [(&str, &str, &str); 2] = [
    (
        "use_disk_shader_cache",
        "Use persistent pipeline cache",
        "every start of a game compiles its shaders from nothing, and Prepare shaders has nothing to fill",
    ),
    (
        "use_vulkan_driver_pipeline_cache",
        "Use Vulkan pipeline cache",
        "the GPU driver's pipeline cache is not kept between starts, so shaders compile again at every start",
    ),
];

/// C11: the cache settings a `qt-config.ini` turns off, or, `per_game`, a game's
/// `config\custom\<id>.ini` (where a value counts only when it does not follow the global one).
/// The emulator reads a value only when its `\default` is false (`Config::ReadSettingGeneric`).
pub fn cache_keys_off(text: &str, per_game: bool) -> Vec<&'static str> {
    let e = ini_entries(text);
    let mut out = Vec::new();
    for (key, _, _) in CACHE_KEYS {
        let own = !per_game
            || ini_get(&e, RENDERER, &format!("{key}\\use_global")) == Some("false")
            || ini_get(&e, RENDERER, &format!("{key}\\global")) == Some("false");
        let set = ini_get(&e, RENDERER, &format!("{key}\\default")) == Some("false");
        let off = ini_get(&e, RENDERER, key).is_some_and(|v| v.eq_ignore_ascii_case("false"));
        if own && set && off {
            out.push(key);
        }
    }
    out
}

fn cache_keys_finding(file: &str, per_game: bool, keys: &[&str]) -> Option<Finding> {
    if keys.is_empty() {
        return None;
    }
    let mut parts = Vec::new();
    for key in keys {
        if let Some((k, label, cost)) = CACHE_KEYS.iter().find(|(k, _, _)| k == key) {
            parts.push(format!("\"{label}\" ({k}=false): {cost}"));
        }
    }
    let where_ = if per_game { "in the game's Properties" } else { "in Guarana's Configure window" };
    let (turns, it) = match (per_game, parts.len() > 1) {
        (true, true) => ("turn", "them"),
        (true, false) => ("turn", "it"),
        (false, true) => ("turns", "them"),
        (false, false) => ("turns", "it"),
    };
    Some(finding(
        "C11",
        CheckStatus::Warn,
        format!(
            "{file} {turns} off {}. Turn {it} back on {where_}, unless you turned {it} off on purpose to work around a driver problem.",
            parts.join("; ")
        ),
    ))
}

/// C17: launcher overrides naming a toggle this launcher does not have (saved by a newer launcher
/// before a rollback). Applying them fails with "Unknown setting".
fn unknown_overrides(overrides: &BTreeMap<String, ToggleValue>, known: &BTreeMap<String, ToggleValue>) -> Option<Finding> {
    let unknown: Vec<&str> = overrides.keys().filter(|k| !known.contains_key(*k)).map(String::as_str).collect();
    if unknown.is_empty() {
        return None;
    }
    Some(finding(
        "C17",
        CheckStatus::Warn,
        format!(
            "The launcher's saved settings name {}, which this launcher does not have (a newer launcher saved them), so applying them stops with \"Unknown setting\". Pick a mode on Home, or apply a profile again under Home > Advanced.",
            unknown.join(", ")
        ),
    ))
}

/// What the `settings_file` row reads.
pub struct SettingsInput<'a> {
    pub global: Option<&'a str>,
    /// `(file name, text)` of each `config\custom\*.ini`.
    pub per_game: &'a [(String, String)],
    pub overrides: &'a BTreeMap<String, ToggleValue>,
    /// The saved profile's toggles (`profiles::defaults_of`); None without a saved profile.
    pub known_toggles: Option<&'a BTreeMap<String, ToggleValue>>,
}

pub fn settings_file(i: &SettingsInput) -> Vec<Finding> {
    let mut out = Vec::new();
    if let Some(text) = i.global {
        out.extend(cache_keys_finding("qt-config.ini", false, &cache_keys_off(text, false)));
    }
    for (name, text) in i.per_game {
        out.extend(cache_keys_finding(&format!("One game's own settings (config\\custom\\{name})"), true, &cache_keys_off(text, true)));
    }
    if let Some(known) = i.known_toggles {
        out.extend(unknown_overrides(i.overrides, known));
    }
    out
}

// ---------------------------------------------------------------------------------------------
// Shader cache files

/// The per-title log (`src/video_core/shader_cache_v2/cache.h`).
const CACHE_LOG: &str = "lichii-cache.v2.bin";
/// The magic a log starts with; the `u32` format version follows it (`cache_format.h`).
const LOG_MAGIC: &[u8; 8] = b"LXSHCACH";
/// The driver pipeline cache file holds only its header up to this size (8-byte magic and a
/// `u32` version, `vk_pipeline_cache.cpp`).
const DRIVER_FILE_HEADER: u64 = 12;
/// "PGP2", the precache lists' magic (`shader_warm_builder.cpp`).
const PRECACHE_LIST_MAGIC: u32 = 0x3250_4750;
const PRECACHE_LISTS: [&str; 2] = ["precache-graphics.bin", "precache-graphics-libraries.bin"];
const WARM_MARKER: &str = "warm-compute.done";

/// The format version in a shader cache log's first 12 bytes; None when it is not a log.
pub fn log_format(header: &[u8]) -> Option<u32> {
    if header.len() < 12 || &header[..8] != LOG_MAGIC {
        return None;
    }
    Some(u32::from_le_bytes([header[8], header[9], header[10], header[11]]))
}

/// The device id in `warm-compute.done`: `v2 <size> <hash> <entries> <driver> <device> ...`.
pub fn marker_device_from_done(text: &str) -> Option<u32> {
    let mut it = text.split_whitespace();
    if it.next()? != "v2" {
        return None;
    }
    it.nth(4)?.parse().ok()
}

/// The device id in a precache list's header: `u32` magic, driver, device id.
pub fn marker_device_from_list(head: &[u8]) -> Option<u32> {
    if head.len() < 12 || u32::from_le_bytes([head[0], head[1], head[2], head[3]]) != PRECACHE_LIST_MAGIC {
        return None;
    }
    Some(u32::from_le_bytes([head[8], head[9], head[10], head[11]]))
}

/// `N` of a driver pipeline cache file `vulkan_pipelines.fork-v<N>.bin`.
pub fn driver_file_version(name: &str) -> Option<u32> {
    name.strip_prefix("vulkan_pipelines.fork-v")?.strip_suffix(".bin")?.parse().ok()
}

/// What the `cache_files` row knows of one title's folder in `cache\shader`.
#[derive(Debug, Clone, Default)]
pub struct TitleCache {
    /// The folder's name, upper case.
    pub title: String,
    pub log_header: Option<Vec<u8>>,
    /// Any of the precache lists or `warm-compute.done` is there.
    pub has_markers: bool,
    pub marker_device: Option<u32>,
    /// Size of the newest `vulkan_pipelines.fork-v<N>.bin`; None when there is none.
    pub driver_file_bytes: Option<u64>,
    /// `*.migrating` files older than an hour: (name, size).
    pub leftovers: Vec<(String, u64)>,
}

/// C15: markers say the driver cache holds the warm-up's pipelines, but the file is missing or
/// holds only its header. Only where the emulator keeps that file: the markers' device is an Intel
/// GPU of this PC and `use_vulkan_driver_pipeline_cache` is on. NVIDIA, AMD and RADV keep their
/// own cache, so there the file never exists and the markers are right.
fn markers_without_driver_cache(t: &TitleCache, gpus: &[Gpu], driver_cache_on: bool) -> bool {
    let intel = t.marker_device.is_some_and(|d| gpus.iter().any(|g| g.vendor == Vendor::Intel && g.device_id == d));
    t.has_markers && driver_cache_on && intel && t.driver_file_bytes.map_or(true, |b| b <= DRIVER_FILE_HEADER)
}

/// C14 (a log of another format than the installed build's, from `build-facts.json`; without it,
/// nothing is compared), C15 and C16 (leftover `*.migrating` files).
pub fn cache_files(titles: &[TitleCache], installed_format: Option<u32>, gpus: &[Gpu], driver_cache_on: bool) -> Vec<Finding> {
    let mut older = Vec::new();
    let mut newer = Vec::new();
    let mut unbuilt = Vec::new();
    let mut leftovers = Vec::new();
    let mut leftover_bytes = 0u64;
    for t in titles {
        if let (Some(installed), Some(found)) = (installed_format, t.log_header.as_deref().and_then(log_format)) {
            if found < installed {
                older.push(format!("{} (format {found})", t.title));
            } else if found > installed {
                newer.push(format!("{} (format {found})", t.title));
            }
        }
        if markers_without_driver_cache(t, gpus, driver_cache_on) {
            unbuilt.push(t.title.clone());
        }
        for (name, bytes) in &t.leftovers {
            leftovers.push(format!("{}\\{name}", t.title));
            leftover_bytes += bytes;
        }
    }
    let mut out = Vec::new();
    if let (false, Some(installed)) = (older.is_empty(), installed_format) {
        out.push(finding(
            "C14",
            CheckStatus::Warn,
            format!(
                "The shader cache of {} is in an older format than the one the installed Guarana writes (format {installed}). Guarana does not use it, so every start of that game compiles its shaders from nothing. Nothing was changed: a coming launcher update sets such a file aside (renamed, never deleted). Until then, with Guarana closed, you can rename {CACHE_LOG} in that game's cache\\shader folder to {CACHE_LOG}.format<N>.",
                older.join(", ")
            ),
        ));
    }
    if let (false, Some(installed)) = (newer.is_empty(), installed_format) {
        out.push(finding(
            "C14",
            CheckStatus::Info,
            format!(
                "The shader cache of {} was written by a newer Guarana than the installed one (format {installed}). It is left as it is and is used again after you update.",
                newer.join(", ")
            ),
        ));
    }
    if !unbuilt.is_empty() {
        out.push(finding(
            "C15",
            CheckStatus::Warn,
            format!(
                "Prepare shaders marked {} as done, but the GPU driver's pipeline cache file (vulkan_pipelines.fork-v*.bin) is missing or empty, so the shaders it lists are not there and the first play compiles them (issue #106). Run Prepare shaders on the game card again to have them ready before you play.",
                unbuilt.join(", ")
            ),
        ));
    }
    if !leftovers.is_empty() {
        out.push(finding(
            "C16",
            CheckStatus::Info,
            format!(
                "{} file(s) of a cache migration that was cut short ({}): {}. Each was being written from a cache file that is still there, so deleting it loses nothing; with Guarana closed you can delete them to free the space.",
                leftovers.len(),
                util::human_bytes(leftover_bytes),
                leftovers.join(", ")
            ),
        ));
    }
    out
}

// ---------------------------------------------------------------------------------------------
// The user folder after the rename (issue #107)

/// `[section] key` of each setting in a `qt-config.ini` that is not at its default and is not one
/// the first-run seed or the launcher writes (`profiles::FIRST_RUN_SEEDS`, a seed only while it
/// still has the seed's value, and `profiles::LAUNCHER_INI_KEYS`). A key without a `\default`
/// beside it is not a setting (window layout, recent files) and is left out.
pub fn chosen_settings(text: &str) -> Vec<String> {
    let e = ini_entries(text);
    let mut out = Vec::new();
    for (section, key, value) in &e {
        let Some(base) = key.strip_suffix("\\default") else { continue };
        if value != "false" {
            continue;
        }
        let current = ini_get(&e, section, base).unwrap_or("");
        if section.eq_ignore_ascii_case(RENDERER) {
            let seeded = profiles::FIRST_RUN_SEEDS.iter().any(|(k, v)| k.eq_ignore_ascii_case(base) && *v == current);
            let launcher = profiles::LAUNCHER_INI_KEYS.iter().any(|k| k.eq_ignore_ascii_case(base));
            if seeded || launcher {
                continue;
            }
        }
        out.push(format!("[{section}] {base}"));
    }
    out
}

/// `NAME=VALUE` lines in a switch file outside the launcher's block.
fn own_switch_lines(text: &str) -> usize {
    let zones = zones(text);
    switch_lines(text).iter().filter(|l| zones.get(l.number - 1) != Some(&Zone::Launcher)).count()
}

/// What the `user_folder` row reads. The saves themselves are never looked at: only whether
/// `lichii\nand` or `lichii\sdmc` exist.
pub struct UserFolderInput<'a> {
    pub guarana_dir_exists: bool,
    pub lichii_config: Option<&'a str>,
    pub guarana_config: Option<&'a str>,
    pub lichii_switches: Option<&'a str>,
    pub guarana_switches: Option<&'a str>,
    pub lichii_has_save_folders: bool,
}

/// C12: the copy from `%APPDATA%\lichii` did not happen, or happened into an empty folder: Lichii's
/// files hold settings or switches the player chose and Guarana's hold none. Before the emulator
/// has made a `guarana` folder at all there is nothing to report: its first start copies `lichii`.
pub fn user_folder(i: &UserFolderInput) -> Vec<Finding> {
    if !i.guarana_dir_exists {
        return Vec::new();
    }
    let mut missing = Vec::new();
    let theirs = i.lichii_config.map(chosen_settings).unwrap_or_default();
    let ours = i.guarana_config.map(chosen_settings).unwrap_or_default();
    if !theirs.is_empty() && ours.is_empty() {
        missing.push(format!(
            "%APPDATA%\\lichii\\config\\qt-config.ini has {} setting(s) changed from their defaults and Guarana's own qt-config.ini has none",
            theirs.len()
        ));
    }
    let their_switches = i.lichii_switches.map(own_switch_lines).unwrap_or(0);
    let our_switches = i.guarana_switches.map(own_switch_lines).unwrap_or(0);
    let switches_missing = their_switches > 0 && our_switches == 0;
    if switches_missing {
        missing.push(format!(
            "%APPDATA%\\lichii\\nx2_switches.txt has {their_switches} switch line(s) of yours and Guarana's own has none"
        ));
    }
    if missing.is_empty() {
        return Vec::new();
    }
    let mut text = format!(
        "Your Lichii settings may not have been carried over to Guarana (issue #107): {}. Guarana copies the lichii folder only on a start when no guarana folder exists yet, and never again after that. Nothing was changed: a coming launcher update offers to copy the settings over (never the folders where saves are kept). Until then, set them again in Guarana.",
        missing.join(", and ")
    );
    if switches_missing {
        text.push_str(" With Guarana closed, you can copy your own lines from lichii's nx2_switches.txt into Guarana's, outside the launcher's block.");
    }
    if i.lichii_has_save_folders {
        text.push_str(" %APPDATA%\\lichii also has a nand or sdmc folder, where Lichii kept saves; this check does not look inside them.");
    }
    vec![finding("C12", CheckStatus::Warn, text)]
}

// ---------------------------------------------------------------------------------------------
// What the installed build says about itself

/// `build-facts.json` beside the installed emulator (issue #125's emulator slice writes it at
/// build time). The fields this slice reads; a build without the file gets C3's registry rule and
/// C14 not at all.
#[derive(Debug, Default, Deserialize)]
#[serde(default)]
pub struct BuildFacts {
    /// The shader cache log format the build writes (`cache_format.h` `FormatVersion`).
    pub shader_cache_format: Option<u32>,
    /// Every switch the build knows: name -> value type (`number`, `word`, `path`, `text`).
    pub switches: BTreeMap<String, String>,
}

pub fn parse_build_facts(text: &str) -> Option<BuildFacts> {
    serde_json::from_str(text).ok()
}

// ---------------------------------------------------------------------------------------------
// Readers: only Guarana's own files

/// Folders of the user folder that hold keys, firmware, saves, mods and dumps: never read here.
const NEVER_READ: [&str; 5] = ["keys", "nand", "sdmc", "load", "dump"];
/// A settings file larger than this is not read.
const MAX_TEXT: u64 = 4 << 20;

/// Whether this module may open `path`: inside `root`, with no `..` and no folder named in
/// `NEVER_READ` on the way.
pub fn may_read(path: &Path, root: &Path) -> bool {
    let Ok(rel) = path.strip_prefix(root) else { return false };
    let mut any = false;
    for c in rel.components() {
        match c {
            Component::Normal(part) => {
                let part = part.to_string_lossy();
                if NEVER_READ.iter().any(|n| part.eq_ignore_ascii_case(n)) {
                    return false;
                }
                any = true;
            }
            _ => return false,
        }
    }
    any
}

fn read_text(path: &Path, root: &Path) -> Option<String> {
    if !may_read(path, root) {
        return None;
    }
    let meta = fs::metadata(path).ok()?;
    if !meta.is_file() || meta.len() > MAX_TEXT {
        return None;
    }
    fs::read(path).ok().map(|b| String::from_utf8_lossy(&b).into_owned())
}

/// The first `n` bytes of a file.
fn read_head(path: &Path, root: &Path, n: u64) -> Option<Vec<u8>> {
    if !may_read(path, root) {
        return None;
    }
    let mut head = Vec::new();
    fs::File::open(path).ok()?.take(n).read_to_end(&mut head).ok()?;
    Some(head)
}

fn installed_dir() -> Option<PathBuf> {
    state::read_current().map(|c| PathBuf::from(c.path))
}

fn build_facts() -> Option<BuildFacts> {
    let dir = installed_dir()?;
    parse_build_facts(&read_text(&dir.join("build-facts.json"), &dir)?)
}

/// `EDEN_NX2_*` values under `key` (read only).
fn switch_variables(root: &RegKey, path: &str) -> Vec<(String, String)> {
    let Ok(key) = root.open_subkey_with_flags(path, KEY_READ) else { return Vec::new() };
    let mut out = Vec::new();
    for item in key.enum_values() {
        let Ok((name, _)) = item else { continue };
        if name.to_ascii_uppercase().starts_with("EDEN_NX2_") {
            let value: String = key.get_value(&name).unwrap_or_default();
            out.push((name, value));
        }
    }
    out
}

pub fn switch_file_now(persisted: &Persisted, sys: &SystemInfo) -> Vec<Finding> {
    let user = paths::user_data_dir();
    let text = read_text(&paths::user_switches(), &user);
    let known = persisted.profile_id.as_deref().filter(|p| profiles::is_known(p));
    let expected = known.and_then(|p| profiles::block_switches(p, &persisted.overrides, &sys.gpus).ok());
    let facts = build_facts();
    let registry = facts.as_ref().map(|f| &f.switches).filter(|s| !s.is_empty());
    let user_env = switch_variables(&RegKey::predef(HKEY_CURRENT_USER), "Environment");
    let system_env = switch_variables(
        &RegKey::predef(HKEY_LOCAL_MACHINE),
        r"SYSTEM\CurrentControlSet\Control\Session Manager\Environment",
    );
    let portable = installed_dir().map(|d| d.join("user")).filter(|d| d.is_dir());
    let mode = modes::current(persisted);
    switch_file(&SwitchFileInput {
        text: text.as_deref(),
        expected: expected.as_deref(),
        mode: &mode,
        profile_known: known.is_some(),
        ocarina_in_library: persisted.games.iter().any(|g| g.is_ocarina),
        gpus: &sys.gpus,
        registry,
        user_env: &user_env,
        system_env: &system_env,
        portable_dir: portable.as_deref(),
    })
}

fn global_config_text() -> Option<String> {
    read_text(&paths::qt_config(), &paths::user_data_dir())
}

pub fn settings_file_now(persisted: &Persisted) -> Vec<Finding> {
    let user = paths::user_data_dir();
    let global = global_config_text();
    let mut per_game = Vec::new();
    if let Ok(entries) = fs::read_dir(paths::config_dir().join("custom")) {
        for e in entries.flatten() {
            let name = e.file_name().to_string_lossy().into_owned();
            if !name.to_ascii_lowercase().ends_with(".ini") {
                continue;
            }
            if let Some(text) = read_text(&e.path(), &user) {
                per_game.push((name, text));
            }
        }
    }
    let known = persisted.profile_id.as_deref().and_then(profiles::defaults_of);
    settings_file(&SettingsInput {
        global: global.as_deref(),
        per_game: &per_game,
        overrides: &persisted.overrides,
        known_toggles: known.as_ref(),
    })
}

/// The findings, and the installed build's cache format when its `build-facts.json` says it.
pub fn cache_files_now(sys: &SystemInfo) -> (Vec<Finding>, Option<u32>) {
    let user = paths::user_data_dir();
    let installed_format = build_facts().and_then(|f| f.shader_cache_format);
    let driver_cache_on = !global_config_text()
        .map(|t| cache_keys_off(&t, false).contains(&"use_vulkan_driver_pipeline_cache"))
        .unwrap_or(false);
    let cutoff = SystemTime::now() - Duration::from_secs(3600);
    let mut titles = Vec::new();
    let Ok(entries) = fs::read_dir(paths::shader_cache_dir()) else {
        return (Vec::new(), installed_format);
    };
    for e in entries.flatten() {
        let dir = e.path();
        let name = e.file_name().to_string_lossy().into_owned();
        if !dir.is_dir() || name.len() != 16 || !name.chars().all(|c| c.is_ascii_hexdigit()) {
            continue;
        }
        let lists: Vec<PathBuf> = PRECACHE_LISTS.iter().map(|n| dir.join(n)).collect();
        let done = dir.join(WARM_MARKER);
        let mut t = TitleCache {
            title: name.to_ascii_uppercase(),
            log_header: read_head(&dir.join(CACHE_LOG), &user, 12),
            has_markers: done.is_file() || lists.iter().any(|p| p.is_file()),
            ..TitleCache::default()
        };
        t.marker_device = read_text(&done, &user)
            .as_deref()
            .and_then(marker_device_from_done)
            .or_else(|| lists.iter().find_map(|p| read_head(p, &user, 12).as_deref().and_then(marker_device_from_list)));
        let mut newest: Option<(u32, u64)> = None;
        if let Ok(files) = fs::read_dir(&dir) {
            for f in files.flatten() {
                let Ok(meta) = f.metadata() else { continue };
                if !meta.is_file() {
                    continue;
                }
                let fname = f.file_name().to_string_lossy().into_owned();
                if let Some(v) = driver_file_version(&fname) {
                    if newest.map_or(true, |(nv, _)| v > nv) {
                        newest = Some((v, meta.len()));
                    }
                }
                if fname.to_ascii_lowercase().ends_with(".migrating") && meta.modified().is_ok_and(|m| m < cutoff) {
                    t.leftovers.push((fname, meta.len()));
                }
            }
        }
        t.driver_file_bytes = newest.map(|(_, b)| b);
        titles.push(t);
    }
    (cache_files(&titles, installed_format, &sys.gpus, driver_cache_on), installed_format)
}

pub fn user_folder_now() -> Vec<Finding> {
    let roaming = paths::roaming_appdata();
    let (guarana, lichii) = (roaming.join("guarana"), roaming.join("lichii"));
    // An installed build from before the rename reads lichii itself (`paths::user_data_dir`):
    // nothing was left behind then.
    if !lichii.is_dir() || paths::user_data_dir() != guarana {
        return Vec::new();
    }
    let lichii_config = read_text(&lichii.join("config").join("qt-config.ini"), &lichii);
    let guarana_config = read_text(&guarana.join("config").join("qt-config.ini"), &guarana);
    let lichii_switches = read_text(&lichii.join("nx2_switches.txt"), &lichii);
    let guarana_switches = read_text(&guarana.join("nx2_switches.txt"), &guarana);
    user_folder(&UserFolderInput {
        guarana_dir_exists: guarana.is_dir(),
        lichii_config: lichii_config.as_deref(),
        guarana_config: guarana_config.as_deref(),
        lichii_switches: lichii_switches.as_deref(),
        guarana_switches: guarana_switches.as_deref(),
        lichii_has_save_folders: lichii.join("nand").is_dir() || lichii.join("sdmc").is_dir(),
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn gpu(vendor: Vendor, integrated: bool, vendor_id: u32, device_id: u32) -> Gpu {
        Gpu {
            name: "GPU".into(),
            vendor,
            driver_version: "1".into(),
            is_integrated: integrated,
            dedicated_mb: if integrated { 128 } else { 12000 },
            shared_mb: 16000,
            vulkan: true,
            arch_hint: None,
            family: None,
            vendor_id,
            device_id,
            driver_date: None,
        }
    }

    /// Panther Lake's 4-core Intel Graphics (issue #119's laptop).
    fn ptl() -> Gpu {
        gpu(Vendor::Intel, true, 0x8086, 0xb0a0)
    }

    fn radeon_780m() -> Gpu {
        gpu(Vendor::Amd, true, 0x1002, 0x15bf)
    }

    fn rtx() -> Gpu {
        gpu(Vendor::Nvidia, false, 0x10de, 0x2820)
    }

    fn block(lines: &[&str]) -> String {
        let mut s = String::from("# mine\r\nEDEN_NX2_SPLIT=2\r\n\r\n# >>> Guarana launcher (profile intel-arc-igpu; this block is rewritten by the launcher, edit lines outside it)\r\n");
        for l in lines {
            s.push_str(l);
            s.push_str("\r\n");
        }
        s.push_str("# <<< Guarana launcher\r\n");
        s
    }

    fn input<'a>(text: &'a str, expected: Option<&'a [(String, String)]>, gpus: &'a [Gpu]) -> SwitchFileInput<'a> {
        SwitchFileInput {
            text: Some(text),
            expected,
            mode: "auto",
            profile_known: true,
            ocarina_in_library: false,
            gpus,
            registry: None,
            user_env: &[],
            system_env: &[],
            portable_dir: None,
        }
    }

    fn rules(found: &[Finding]) -> Vec<&'static str> {
        found.iter().map(|f| f.rule).collect()
    }

    #[test]
    fn a_block_from_a_launcher_before_122_on_an_intel_igpu_is_stale() {
        let gpus = [ptl()];
        let expected = profiles::block_switches("intel-arc-igpu", &BTreeMap::new(), &gpus).unwrap();
        let old = block(&["EDEN_NX2_EXTRAPOLATE=1", "EDEN_NX2_FLUID=1"]);
        let found = switch_file(&input(&old, Some(expected.as_slice()), &gpus));
        assert_eq!(rules(&found), ["C1"], "{found:?}");
        assert_eq!(found[0].status, CheckStatus::Warn);
        assert!(found[0].text.contains("EDEN_NX2_EXTRAPOLATE=1 (this launcher leaves it out)"), "{}", found[0].text);
        assert!(found[0].text.contains("issue #119"), "{}", found[0].text);
        // What this launcher writes now is not reported.
        let now = block(&["EDEN_NX2_FLUID=1"]);
        assert!(switch_file(&input(&now, Some(expected.as_slice()), &gpus)).is_empty());
        // On a PC without an Intel iGPU the launcher still writes EXTRAPOLATE=1.
        let dgpu = [rtx()];
        let expected = profiles::block_switches("intel-arc-igpu", &BTreeMap::new(), &dgpu).unwrap();
        assert!(switch_file(&input(&old, Some(expected.as_slice()), &dgpu)).is_empty());
    }

    #[test]
    fn fluid_one_with_ocarina_on_an_igpu_is_reported() {
        let text = block(&["EDEN_NX2_FLUID=1"]);
        let expected = vec![("EDEN_NX2_FLUID".to_string(), "1".to_string())];
        for gpus in [vec![ptl()], vec![radeon_780m()]] {
            let mut i = input(&text, Some(expected.as_slice()), &gpus);
            i.ocarina_in_library = true;
            let found = switch_file(&i);
            assert_eq!(rules(&found), ["C1"], "{found:?}");
            assert_eq!(found[0].status, CheckStatus::Info);
            assert!(found[0].text.contains("EDEN_NX2_FLUID=2"), "{}", found[0].text);
        }
        // Without Ocarina in the library, or on a dedicated GPU only: nothing.
        let gpus = [ptl()];
        assert!(switch_file(&input(&text, Some(expected.as_slice()), &gpus)).is_empty());
        let dgpu = [rtx()];
        let mut i = input(&text, Some(expected.as_slice()), &dgpu);
        i.ocarina_in_library = true;
        assert!(switch_file(&i).is_empty());
    }

    #[test]
    fn a_byte_order_mark_hides_the_first_switch() {
        let found = misread_lines("\u{feff}EDEN_NX2_FLUID=2\r\nEDEN_NX2_GM=11\r\n", None);
        assert_eq!(rules(&found), ["C3"]);
        assert_eq!(found[0].status, CheckStatus::Warn);
        assert!(found[0].text.contains("so EDEN_NX2_FLUID on line 1 does nothing"), "{}", found[0].text);
        // Before a comment it is harmless for now.
        let comment = misread_lines("\u{feff}# my switches\r\nEDEN_NX2_GM=11\r\n", None);
        assert_eq!(rules(&comment), ["C3"]);
        assert_eq!(comment[0].status, CheckStatus::Info);
        // A comment that holds an = is not a switch either.
        let example = misread_lines("\u{feff}# e.g. EDEN_NX2_GM=11\r\nEDEN_NX2_GM=11\r\n", None);
        assert_eq!(rules(&example), ["C3"]);
        assert_eq!(example[0].status, CheckStatus::Info);
        assert!(misread_lines("EDEN_NX2_GM=11\r\n", None).is_empty());
    }

    #[test]
    fn a_note_after_a_word_or_number_is_part_of_the_value() {
        let found = misread_lines("EDEN_NX2_POWER_POLICY=igpu # x\r\nX=1 # note\r\n# a comment # with hashes\r\n", None);
        assert_eq!(rules(&found), ["C3", "C3"], "{found:?}");
        assert!(found[0].text.contains("line 1") && found[0].text.contains("reads \"igpu # x\""), "{}", found[0].text);
        assert!(found[0].text.contains("does nothing"), "{}", found[0].text);
        assert_eq!(found[0].status, CheckStatus::Warn);
        // A number is still read up to the blank (`strtoul`), so it is only info.
        assert!(found[1].text.contains("line 2") && found[1].text.contains("reads \"1 # note\""), "{}", found[1].text);
        assert!(found[1].text.contains("still gets 1") && !found[1].text.contains("does nothing"), "{}", found[1].text);
        assert_eq!(found[1].status, CheckStatus::Info);
        // A folder name with a # in it is not a note; neither is a quoted value or a # without a blank.
        assert!(misread_lines("EDEN_NX2_PIPELINE_STATS=C:\\runs #2\r\n", None).is_empty());
        assert!(misread_lines("EDEN_NX2_X=\"a # b\"\r\nEDEN_NX2_Y=a#b\r\n", None).is_empty());
        // With the installed build's registry the switch's type decides.
        let mut registry = BTreeMap::new();
        registry.insert("EDEN_NX2_DIR".to_string(), "path".to_string());
        registry.insert("EDEN_NX2_N".to_string(), "number".to_string());
        assert!(misread_lines("EDEN_NX2_DIR=runs # 2\r\n", Some(&registry)).is_empty());
        assert_eq!(rules(&misread_lines("EDEN_NX2_N=3 # three\r\n", Some(&registry))), ["C3"]);
    }

    #[test]
    fn unknown_switch_names_need_the_registry() {
        let text = "EDEN_NX2_FLUDI=2\r\nEDEN_NX2_N=1\r\nOTHER_VAR=1\r\n";
        assert!(misread_lines(text, None).is_empty());
        let mut registry = BTreeMap::new();
        registry.insert("EDEN_NX2_N".to_string(), "number".to_string());
        let found = misread_lines(text, Some(&registry));
        assert_eq!(rules(&found), ["C3"]);
        assert_eq!(found[0].status, CheckStatus::Info);
        assert!(found[0].text.contains("EDEN_NX2_FLUDI"), "{}", found[0].text);
    }

    #[test]
    fn the_same_switch_in_both_blocks_is_reported() {
        let text = "# >>> Guarana mode (written by the Guarana settings page)\r\nEDEN_NX2_FLUID=2\r\n# <<< Guarana mode\r\n# >>> Guarana launcher (profile x)\r\nEDEN_NX2_FLUID=1\r\n# <<< Guarana launcher\r\n";
        let found = same_switch_in_both_blocks(text).unwrap();
        assert_eq!(found.rule, "C2");
        assert!(found.text.contains("EDEN_NX2_FLUID (Guarana uses 1, the launcher's)"), "{}", found.text);
        let apart = "# >>> Guarana mode\r\nEDEN_NX2_FLUID=2\r\n# <<< Guarana mode\r\n# >>> Guarana launcher\r\nEDEN_NX2_GM=11\r\n# <<< Guarana launcher\r\n";
        assert!(same_switch_in_both_blocks(apart).is_none());
    }

    #[test]
    fn a_custom_block_without_a_known_profile_is_orphaned() {
        let text = block(&["EDEN_NX2_FLUID=1"]);
        let gpus = [ptl()];
        let mut i = input(&text, None, &gpus);
        i.mode = "custom";
        i.profile_known = false;
        assert_eq!(rules(&switch_file(&i)), ["C5"]);
        i.profile_known = true;
        assert!(switch_file(&i).is_empty());
        i.mode = "auto";
        i.profile_known = false;
        assert!(switch_file(&i).is_empty());
    }

    #[test]
    fn persistent_switch_variables_are_reported_by_scope() {
        let user = vec![("EDEN_NX2_EXTRAPOLATE".to_string(), "1".to_string()), ("PATH".to_string(), "x".to_string())];
        let system = vec![("eden_nx2_fluid".to_string(), "1".to_string())];
        let found = persistent_variables(&user, &system);
        assert_eq!(rules(&found), ["C4", "C4"]);
        assert!(found[0].text.contains("EDEN_NX2_EXTRAPOLATE=1") && found[0].text.contains("your account"), "{}", found[0].text);
        assert!(!found[0].text.contains("PATH"));
        assert!(found[1].text.contains("system-wide"), "{}", found[1].text);
        assert!(persistent_variables(&[], &[]).is_empty());
    }

    #[test]
    fn cache_settings_turned_off_are_reported() {
        let global = "[Renderer]\r\nuse_disk_shader_cache\\default=false\r\nuse_disk_shader_cache=false\r\nuse_vulkan_driver_pipeline_cache\\default=true\r\nuse_vulkan_driver_pipeline_cache=false\r\n";
        assert_eq!(cache_keys_off(global, false), ["use_disk_shader_cache"]);
        // A game's file counts only where it does not follow the global value.
        let game = "[Renderer]\r\nuse_vulkan_driver_pipeline_cache\\use_global=false\r\nuse_vulkan_driver_pipeline_cache\\default=false\r\nuse_vulkan_driver_pipeline_cache=false\r\nuse_disk_shader_cache\\default=false\r\nuse_disk_shader_cache=false\r\n";
        assert_eq!(cache_keys_off(game, true), ["use_vulkan_driver_pipeline_cache"]);
        assert!(cache_keys_off("[Renderer]\r\nuse_disk_shader_cache\\default=true\r\nuse_disk_shader_cache=true\r\n", false).is_empty());
        let per_game = vec![("0100F2C0115B6000.ini".to_string(), game.to_string())];
        let overrides = BTreeMap::new();
        let found = settings_file(&SettingsInput { global: Some(global), per_game: &per_game, overrides: &overrides, known_toggles: None });
        assert_eq!(rules(&found), ["C11", "C11"]);
        assert!(found[0].text.starts_with("qt-config.ini turns off \"Use persistent pipeline cache\""), "{}", found[0].text);
        assert!(found[1].text.contains("0100F2C0115B6000.ini") && found[1].text.contains("game's Properties"), "{}", found[1].text);
    }

    #[test]
    fn overrides_this_launcher_does_not_know_are_reported() {
        let known = profiles::defaults_of("intel-arc-igpu").unwrap();
        let mut overrides = BTreeMap::new();
        overrides.insert("fluid_presenter".to_string(), ToggleValue::Bool(false));
        assert!(unknown_overrides(&overrides, &known).is_none());
        overrides.insert("from_a_newer_launcher".to_string(), ToggleValue::Bool(true));
        let found = unknown_overrides(&overrides, &known).unwrap();
        assert_eq!(found.rule, "C17");
        assert!(found.text.contains("from_a_newer_launcher") && !found.text.contains("fluid_presenter"), "{}", found.text);
    }

    fn log_header(format: u32) -> Vec<u8> {
        let mut h = LOG_MAGIC.to_vec();
        h.extend_from_slice(&format.to_le_bytes());
        h.extend_from_slice(&[0u8; 16]);
        h
    }

    fn title(name: &str) -> TitleCache {
        TitleCache { title: name.to_string(), ..TitleCache::default() }
    }

    #[test]
    fn log_formats_are_compared_with_the_installed_build() {
        let facts = parse_build_facts(r#"{"shader_cache_format": 2, "seeds": {}}"#).unwrap();
        assert_eq!(facts.shader_cache_format, Some(2));
        assert_eq!(log_format(&log_header(3)), Some(3));
        assert_eq!(log_format(b"NOTALOG!\x02\x00\x00\x00"), None);
        assert_eq!(log_format(&log_header(1)[..10]), None);
        let mut v1 = title("0100F2C0115B6000");
        v1.log_header = Some(log_header(1));
        let mut v3 = title("01007EF00011E000");
        v3.log_header = Some(log_header(3));
        let mut v2 = title("0100000000010000");
        v2.log_header = Some(log_header(2));
        let titles = [v1, v3, v2];
        let found = cache_files(&titles, facts.shader_cache_format, &[], true);
        assert_eq!(rules(&found), ["C14", "C14"], "{found:?}");
        assert_eq!(found[0].status, CheckStatus::Warn);
        assert!(found[0].text.contains("0100F2C0115B6000 (format 1)") && !found[0].text.contains("0100000000010000"), "{}", found[0].text);
        assert_eq!(found[1].status, CheckStatus::Info);
        assert!(found[1].text.contains("01007EF00011E000 (format 3)") && found[1].text.contains("newer Guarana"), "{}", found[1].text);
        // Without build-facts.json nothing is compared.
        assert!(cache_files(&titles, None, &[], true).is_empty());
        assert!(parse_build_facts("not json").is_none());
    }

    #[test]
    fn markers_without_a_driver_cache_are_flagged_only_for_an_intel_device() {
        assert_eq!(marker_device_from_done("v2 1048576 1234567 812 5 45216 3 987654321\n"), Some(0xb0a0));
        assert_eq!(marker_device_from_done("v1 1 2 3 4 5"), None);
        let mut list = PRECACHE_LIST_MAGIC.to_le_bytes().to_vec();
        list.extend_from_slice(&1u32.to_le_bytes());
        list.extend_from_slice(&0x15bfu32.to_le_bytes());
        assert_eq!(marker_device_from_list(&list), Some(0x15bf));
        assert_eq!(marker_device_from_list(&[0u8; 12]), None);
        assert_eq!(driver_file_version("vulkan_pipelines.fork-v7.bin"), Some(7));
        assert_eq!(driver_file_version("vulkan_pipelines.bin"), None);

        let gpus = [ptl(), radeon_780m()];
        let mut intel = title("0100F2C0115B6000");
        intel.has_markers = true;
        intel.marker_device = Some(0xb0a0);
        let found = cache_files(&[intel.clone()], None, &gpus, true);
        assert_eq!(rules(&found), ["C15"]);
        assert!(found[0].text.contains("0100F2C0115B6000"), "{}", found[0].text);
        // A header-only file is as good as none; a filled one is fine.
        intel.driver_file_bytes = Some(12);
        assert_eq!(rules(&cache_files(&[intel.clone()], None, &gpus, true)), ["C15"]);
        intel.driver_file_bytes = Some(48 << 20);
        assert!(cache_files(&[intel.clone()], None, &gpus, true).is_empty());
        // With the driver cache setting off the emulator keeps no file: nothing to say.
        intel.driver_file_bytes = None;
        assert!(cache_files(&[intel], None, &gpus, false).is_empty());
        // AMD's driver keeps its own cache: the file never exists and the markers are right.
        let mut amd = title("0100F2C0115B6000");
        amd.has_markers = true;
        amd.marker_device = Some(0x15bf);
        assert!(cache_files(&[amd], None, &gpus, true).is_empty());
    }

    #[test]
    fn leftover_migration_files_are_reported() {
        let mut t = title("0100F2C0115B6000");
        t.leftovers.push(("lichii-cache.v2.bin.migrating".to_string(), 3 << 20));
        let found = cache_files(&[t], None, &[], true);
        assert_eq!(rules(&found), ["C16"]);
        assert_eq!(found[0].status, CheckStatus::Info);
        assert!(found[0].text.contains("0100F2C0115B6000\\lichii-cache.v2.bin.migrating"), "{}", found[0].text);
    }

    const LICHII_CONFIG: &str = "[Core]\r\nuse_multi_core\\default=false\r\nuse_multi_core=false\r\n[Renderer]\r\nscaling_filter\\default=false\r\nscaling_filter=1\r\n[UI]\r\ngeometry=@ByteArray(xyz)\r\n";
    /// What a first start writes: the seeds, the launcher's keys and defaults.
    const SEEDED_CONFIG: &str = "[Core]\r\nuse_multi_core\\default=true\r\nuse_multi_core=true\r\n[Renderer]\r\nscaling_filter\\default=false\r\nscaling_filter=15\r\ngpu_accuracy\\default=false\r\ngpu_accuracy=0\r\nframe_gen\\default=false\r\nframe_gen=true\r\n";

    #[test]
    fn seeds_launcher_keys_and_defaults_are_not_chosen_settings() {
        assert!(chosen_settings(SEEDED_CONFIG).is_empty(), "{:?}", chosen_settings(SEEDED_CONFIG));
        assert_eq!(chosen_settings(LICHII_CONFIG), ["[Core] use_multi_core", "[Renderer] scaling_filter"]);
    }

    #[test]
    fn settings_left_behind_in_lichii_are_reported() {
        let base = UserFolderInput {
            guarana_dir_exists: true,
            lichii_config: Some(LICHII_CONFIG),
            guarana_config: Some(SEEDED_CONFIG),
            lichii_switches: None,
            guarana_switches: None,
            lichii_has_save_folders: true,
        };
        let found = user_folder(&base);
        assert_eq!(rules(&found), ["C12"]);
        assert!(found[0].text.contains("2 setting(s)") && found[0].text.contains("does not look inside"), "{}", found[0].text);
        assert!(!found[0].text.contains("nx2_switches.txt"), "{}", found[0].text);
        // A Guarana folder without a qt-config.ini is the same case.
        assert_eq!(rules(&user_folder(&UserFolderInput { guarana_config: None, ..base })), ["C12"]);
        // Settings of its own in Guarana: the copy happened, or the player started over.
        let own = "[Core]\r\nuse_multi_core\\default=false\r\nuse_multi_core=false\r\n";
        assert!(user_folder(&UserFolderInput { guarana_config: Some(own), ..base }).is_empty());
        // No guarana folder yet: the emulator's first start copies lichii.
        assert!(user_folder(&UserFolderInput { guarana_dir_exists: false, ..base }).is_empty());
        // Switches: lichii's own lines against a Guarana file with only the launcher's block.
        let only_block = block(&["EDEN_NX2_FLUID=1"]).replace("EDEN_NX2_SPLIT=2\r\n", "");
        let switches = UserFolderInput {
            lichii_config: None,
            lichii_switches: Some("EDEN_NX2_GM=11\r\n"),
            guarana_switches: Some(only_block.as_str()),
            lichii_has_save_folders: false,
            ..base
        };
        let found = user_folder(&switches);
        assert_eq!(rules(&found), ["C12"]);
        assert!(found[0].text.contains("1 switch line(s)") && !found[0].text.contains("nand"), "{}", found[0].text);
        assert!(found[0].text.contains("outside the launcher's block"), "{}", found[0].text);
    }

    #[test]
    fn keys_firmware_saves_and_mods_are_never_read() {
        let root = Path::new(r"C:\Users\p\AppData\Roaming\guarana");
        assert!(may_read(&root.join("config").join("qt-config.ini"), root));
        assert!(may_read(&root.join("nx2_switches.txt"), root));
        assert!(may_read(&root.join(r"cache\shader\0100f2c0115b6000\lichii-cache.v2.bin"), root));
        for p in [r"keys\prod.keys", r"nand\system\save\0000000000000001", r"sdmc\x", r"load\0100F2C0115B6000\mod", r"dump\x", r"NAND\user", r"cache\..\keys\prod.keys"] {
            assert!(!may_read(&root.join(p), root), "{p}");
        }
        assert!(!may_read(Path::new(r"C:\Users\p\Documents\save.bin"), root));
        assert!(!may_read(root, root));
    }
}
