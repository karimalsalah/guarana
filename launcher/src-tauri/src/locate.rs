//! `locate_files`, `add_game_folder`, `list_games`: finds keys, firmware and games the user
//! already has. Reads names and, for a few formats, a small header; never a whole file, never a
//! cloud-only (OneDrive placeholder) file, never through a junction or symbolic link. Nothing is
//! copied here: only `import_keys` copies, and only the file the user picked.

use std::collections::{HashMap, HashSet};
use std::fs::{self, File, Metadata};
use std::io::Read;
use std::os::windows::fs::MetadataExt;
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

use walkdir::WalkDir;
use windows::core::PCWSTR;
use windows::Win32::Storage::FileSystem::{GetDriveTypeW, GetLogicalDrives};

use crate::paths;
use crate::types::{Container, Found, FoundFirmware, FoundKey, Game, KeyKind, Progress};

pub const OCARINA_TITLE_ID: &str = "0400486024DEC000";

const FILE_ATTRIBUTE_REPARSE_POINT: u32 = 0x400;
const FILE_ATTRIBUTE_OFFLINE: u32 = 0x1000;
const FILE_ATTRIBUTE_RECALL_ON_OPEN: u32 = 0x4_0000;
const FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS: u32 = 0x40_0000;
const DRIVE_REMOVABLE: u32 = 2;
const DRIVE_FIXED: u32 = 3;

/// Folders never entered: system folders and trees that cannot hold what we look for.
const SKIP_DIRS: [&str; 20] = [
    "windows",
    "program files",
    "program files (x86)",
    "programdata",
    "$recycle.bin",
    "system volume information",
    "recovery",
    "perflogs",
    "windowsapps",
    "msocache",
    "config.msi",
    "appdata",
    "node_modules",
    ".git",
    "onedrivetemp",
    "$windows.~bt",
    "$windows.~ws",
    "$winreagent",
    "intel",
    "amd",
];

/// Program ids from AGENTS.md, for display names.
const KNOWN_TITLES: [(&str, &str); 31] = [
    ("040074A01BF12000", "Donkey Kong Bananza"),
    ("040077201BE50000", "Kirby Air Riders"),
    ("04007C6028AA8000", "Yoshi and the Mysterious Book (demo)"),
    ("0400D4201C6EA000", "Mario Tennis Fever"),
    ("0400E88025B1A000", "Star Fox"),
    ("04007EF01F8AE000", "Fire Emblem: Fortune's Weave"),
    ("0400C3F00006E000", "Mario Kart World"),
    ("04008E001F098000", "Nintendo Switch Sports Resort"),
    ("0400A3A01EE4C000", "Pokémon Pokopia"),
    ("0400A4802437E000", "Drag x Drive Global Jam"),
    ("040041E0212AE000", "Fast Fusion"),
    ("04005B902517C000", "Kirby Air Riders Global Test Ride"),
    ("040057C02159A000", "Nintendo Switch 2 Welcome Tour"),
    ("040004F027560000", "Orbitals Global Friends Pass"),
    ("0400380020056000", "Raiders"),
    ("0400D1D02399A000", "The Duskbloods network test"),
    ("0400486024DEC000", "The Legend of Zelda: Ocarina of Time"),
    ("0400245021D2C000", "Yoshi and the Mysterious Book"),
    ("0400DDB020648000", "Cyberpunk 2077: Ultimate Edition"),
    ("0100606026F12000", "Final Fantasy Resonance"),
    ("010019A01E2F2000", "Metroid Prime 4: Beyond - Nintendo Switch 2 Edition"),
    ("0100152000022000", "Mario Kart 8 Deluxe"),
    ("040032B023CA4000", "Orbitals"),
    ("0400921025B32000", "Sonic Frontiers (Switch 2 Edition)"),
    ("04003F40232DE000", "Hyrule Warriors: Age of Imprisonment"),
    ("040015100B514001", "Super Mario Bros. Wonder - Nintendo Switch 2 Edition + Meetup in Bellabel Park"),
    ("01004D300C5AE000", "Kirby and the Forgotten Land"),
    ("01007EF00011E000", "The Legend of Zelda: Breath of the Wild"),
    ("0100F2C0115B6000", "The Legend of Zelda: Tears of the Kingdom"),
    ("01006D0017F7A000", "Mario & Luigi: Brothership"),
    ("010051F0207B2000", "Tomodachi Life: Living the Dream"),
];

/// Emulator folders under `%APPDATA%` checked by the quick scan.
const EMULATORS: [&str; 7] = ["lichii", "eden", "yuzu", "suyu", "citron", "Ryujinx", "sudachi"];

pub struct Root {
    pub path: PathBuf,
    pub max_depth: usize,
    pub hint: String,
}

/// Emulator folders first (small, and where keys and firmware usually are), then their game
/// folders, then Desktop, Documents and Downloads (from the Known Folder API, so OneDrive
/// redirection is followed), then drive-level folders. The time budget is shared in that order.
pub fn quick_roots(known: &[(PathBuf, &str)]) -> Vec<Root> {
    let mut roots: Vec<Root> = Vec::new();
    let mut game_roots: Vec<Root> = Vec::new();
    let appdata = paths::roaming_appdata();
    for emu in EMULATORS {
        let base = appdata.join(emu);
        if !base.is_dir() {
            continue;
        }
        let hint = if emu == "lichii" { "Guarana (already in use)".to_string() } else { emu.to_string() };
        for (sub, depth) in [
            (base.join("keys"), 1),
            (base.join("system"), 1),
            (base.join("nand").join("system").join("Contents").join("registered"), 1),
            (base.join("bis").join("system").join("Contents").join("registered"), 1),
        ] {
            if sub.is_dir() {
                roots.push(Root { path: sub, max_depth: depth, hint: hint.clone() });
            }
        }
        for dir in emulator_game_dirs(&base) {
            game_roots.push(Root { path: dir, max_depth: 3, hint: format!("{emu} game folder") });
        }
    }
    roots.extend(game_roots);
    roots.extend(known.iter().map(|(p, hint)| Root { path: p.clone(), max_depth: 3, hint: hint.to_string() }));
    for drive in drive_roots() {
        for name in ["Switch", "Emulation", "ROMs", "Games"] {
            let p = drive.join(name);
            if p.is_dir() {
                let hint = p.to_string_lossy().into_owned();
                roots.push(Root { path: p, max_depth: 4, hint });
            }
        }
    }
    roots
}

pub fn deep_roots(roots: &[String]) -> Vec<Root> {
    roots
        .iter()
        .map(PathBuf::from)
        .filter(|p| p.is_absolute() && p.is_dir())
        .map(|p| Root { hint: p.to_string_lossy().into_owned(), path: p, max_depth: 12 })
        .collect()
}

/// `X:\` for every fixed or removable drive.
fn drive_roots() -> Vec<PathBuf> {
    drives(true)
}

/// `X:\` of every fixed (internal) drive: what a deep search covers when no folder is given.
pub fn fixed_drives() -> Vec<String> {
    drives(false).into_iter().map(|p| p.to_string_lossy().into_owned()).collect()
}

fn drives(removable: bool) -> Vec<PathBuf> {
    // SAFETY: no arguments; returns a bitmask.
    let mask = unsafe { GetLogicalDrives() };
    let mut out = Vec::new();
    for i in 0..26u32 {
        if mask & (1 << i) == 0 {
            continue;
        }
        let root = format!("{}:\\", (b'A' + i as u8) as char);
        let wide: Vec<u16> = root.encode_utf16().chain(std::iter::once(0)).collect();
        // SAFETY: `wide` is NUL-terminated and outlives the call.
        let kind = unsafe { GetDriveTypeW(PCWSTR(wide.as_ptr())) };
        if kind == DRIVE_FIXED || (removable && kind == DRIVE_REMOVABLE) {
            out.push(PathBuf::from(root));
        }
    }
    out
}

/// Game folders other emulators were told about: yuzu-family `qt-config.ini`
/// (`Paths\gamedirs\N\path=`) and Ryujinx's `Config.json` (`game_dirs`).
fn emulator_game_dirs(base: &Path) -> Vec<PathBuf> {
    let mut out = Vec::new();
    if let Ok(text) = fs::read_to_string(base.join("config").join("qt-config.ini")) {
        for line in text.lines() {
            let Some((key, value)) = line.split_once('=') else { continue };
            let key = key.trim().to_ascii_lowercase();
            if key.starts_with("paths\\gamedirs\\") && key.ends_with("\\path") {
                let v = value.trim().trim_matches('"').replace('/', "\\");
                let p = PathBuf::from(&v);
                if p.is_absolute() && p.is_dir() {
                    out.push(p);
                }
            }
        }
    }
    if let Ok(bytes) = fs::read(base.join("Config.json")) {
        if let Ok(json) = serde_json::from_slice::<serde_json::Value>(&bytes) {
            if let Some(dirs) = json.get("game_dirs").and_then(|d| d.as_array()) {
                for d in dirs.iter().filter_map(|d| d.as_str()) {
                    let p = PathBuf::from(d);
                    if p.is_absolute() && p.is_dir() {
                        out.push(p);
                    }
                }
            }
        }
    }
    out
}

fn attributes(meta: &Metadata) -> u32 {
    meta.file_attributes()
}

/// A OneDrive (or other cloud) placeholder: opening it would download it.
fn is_cloud_only(meta: &Metadata) -> bool {
    attributes(meta) & (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS) != 0
}

fn read_head(path: &Path, max: usize) -> Option<Vec<u8>> {
    let mut f = File::open(path).ok()?;
    let mut buf = vec![0u8; max];
    let mut filled = 0;
    while filled < max {
        match f.read(&mut buf[filled..]) {
            Ok(0) => break,
            Ok(n) => filled += n,
            Err(_) => return None,
        }
    }
    buf.truncate(filled);
    Some(buf)
}

fn le_u32(b: &[u8], at: usize) -> Option<u32> {
    b.get(at..at + 4).map(|s| u32::from_le_bytes([s[0], s[1], s[2], s[3]]))
}

fn le_u64(b: &[u8], at: usize) -> Option<u64> {
    b.get(at..at + 8).map(|s| u64::from_le_bytes([s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7]]))
}

/// NUP: superblock (0x80, "NUP\x01", TOC offset at 0x08, package count at 0x48), then the TOC
/// header (0x30) and 0x40-byte package records (title id at +0, kind at +0x18; 0 = base).
/// Layout from `src/core/file_sys/nup_container.cpp`.
fn nup_title_id(path: &Path) -> Option<u64> {
    let mut f = File::open(path).ok()?;
    let mut sb = [0u8; 0x80];
    f.read_exact(&mut sb).ok()?;
    if &sb[0..4] != b"NUP\x01" {
        return None;
    }
    let toc_offset = le_u64(&sb, 0x08)?;
    let count = le_u32(&sb, 0x48)?.min(64) as usize;
    if count == 0 || toc_offset < 0x80 {
        return None;
    }
    use std::io::{Seek, SeekFrom};
    f.seek(SeekFrom::Start(toc_offset)).ok()?;
    let mut toc = vec![0u8; 0x30 + count * 0x40];
    f.read_exact(&mut toc).ok()?;
    if le_u32(&toc, 0)? != 0x0143_4F54 {
        return None;
    }
    let records: Vec<(u64, u8)> = (0..count)
        .filter_map(|i| {
            let at = 0x30 + i * 0x40;
            Some((le_u64(&toc, at)?, *toc.get(at + 0x18)?))
        })
        .collect();
    records.iter().find(|(_, kind)| *kind == 0).or(records.first()).map(|(id, _)| *id).filter(|id| *id != 0)
}

/// NSP (PFS0): the ticket's file name is the rights id, whose first 16 hex digits are the
/// title id. Only the header and its name table are read.
fn nsp_title_id(path: &Path) -> Option<u64> {
    let head = read_head(path, 16 * 1024)?;
    if head.get(0..4)? != b"PFS0" {
        return None;
    }
    let n = le_u32(&head, 4)? as usize;
    let strings = le_u32(&head, 8)? as usize;
    if n == 0 || n > 1024 {
        return None;
    }
    let table_start = 0x10 + n * 0x18;
    let table = head.get(table_start..table_start + strings)?;
    for i in 0..n {
        let off = le_u32(&head, 0x10 + i * 0x18 + 0x10)? as usize;
        let rest = table.get(off..)?;
        let end = rest.iter().position(|&c| c == 0).unwrap_or(rest.len());
        let name = String::from_utf8_lossy(&rest[..end]).to_ascii_lowercase();
        if name.ends_with(".tik") && name.len() >= 36 {
            if let Ok(id) = u64::from_str_radix(&name[..16], 16) {
                return Some(id);
            }
        }
    }
    None
}

/// `[0400486024DEC000]` (or a bare 16-digit id starting 01 or 04) in a file name.
pub fn title_id_from_name(name: &str) -> Option<u64> {
    let bytes = name.as_bytes();
    let mut i = 0;
    while i + 16 <= bytes.len() {
        let run = &bytes[i..i + 16];
        let before_ok = i == 0 || !bytes[i - 1].is_ascii_alphanumeric();
        let after_ok = i + 16 == bytes.len() || !bytes[i + 16].is_ascii_alphanumeric();
        if before_ok && after_ok && run.iter().all(|c| c.is_ascii_hexdigit()) && (run.starts_with(b"01") || run.starts_with(b"04")) {
            return u64::from_str_radix(std::str::from_utf8(run).ok()?, 16).ok();
        }
        i += 1;
    }
    None
}

fn known_title(id: &str) -> Option<&'static str> {
    KNOWN_TITLES.iter().find(|(k, _)| k.eq_ignore_ascii_case(id)).map(|(_, v)| *v)
}

/// "The_Legend_of_Zelda [0400486024DEC000][v0]" -> "The Legend of Zelda".
fn clean_title(stem: &str) -> String {
    let mut out = String::new();
    let mut depth = 0i32;
    for c in stem.chars() {
        match c {
            '[' | '(' | '{' => depth += 1,
            ']' | ')' | '}' => depth = (depth - 1).max(0),
            _ if depth == 0 => out.push(if c == '_' { ' ' } else { c }),
            _ => {}
        }
    }
    let t = out.split_whitespace().collect::<Vec<_>>().join(" ");
    let t = t.trim_matches(|c: char| c == '-' || c == '.' || c.is_whitespace()).to_string();
    if t.is_empty() {
        stem.to_string()
    } else {
        t
    }
}

fn container_of(path: &Path) -> Option<Container> {
    let ext = path.extension()?.to_string_lossy().to_ascii_lowercase();
    Some(match ext.as_str() {
        "nup" => Container::Nup,
        "nsp" => Container::Nsp,
        "xci" => Container::Xci,
        "nx" => Container::Nx,
        "nsz" | "xcz" => Container::Other,
        _ => return None,
    })
}

/// A game file, or `None` for anything else. `cache` lets a rescan reuse what an earlier scan
/// read from the same file (same path and size).
pub fn identify_game(path: &Path, meta: &Metadata, cache: &HashMap<String, Game>) -> Option<Game> {
    let container = container_of(path)?;
    let path_s = path.to_string_lossy().into_owned();
    if let Some(g) = cache.get(&path_s.to_ascii_lowercase()) {
        if g.size_bytes == meta.len() {
            return Some(g.clone());
        }
    }
    let stem = path.file_stem().map(|s| s.to_string_lossy().into_owned()).unwrap_or_default();
    let from_header = if is_cloud_only(meta) {
        None
    } else {
        match container {
            Container::Nup => nup_title_id(path),
            Container::Nsp => nsp_title_id(path),
            _ => None,
        }
    };
    let id = from_header.or_else(|| title_id_from_name(&stem));
    let title_id = id.map(|v| format!("{v:016X}")).unwrap_or_default();
    let title = known_title(&title_id).map(str::to_string).unwrap_or_else(|| clean_title(&stem));
    Some(Game {
        is_ocarina: title_id.eq_ignore_ascii_case(OCARINA_TITLE_ID),
        title_id,
        title,
        path: path_s,
        container,
        size_bytes: meta.len(),
    })
}

/// Name and format check of a `*.keys` file: most lines must be `name = hex`.
pub fn key_kind(path: &Path, meta: &Metadata) -> Option<KeyKind> {
    let name = path.file_name()?.to_string_lossy().to_ascii_lowercase();
    if !name.ends_with(".keys") || meta.len() == 0 || meta.len() > 4 << 20 {
        return None;
    }
    let by_name = match name.as_str() {
        "prod.keys" => Some(KeyKind::Prod),
        "title.keys" => Some(KeyKind::Title),
        _ => None,
    };
    if is_cloud_only(meta) {
        return Some(by_name.unwrap_or(KeyKind::Unknown));
    }
    let head = read_head(path, 16 * 1024)?;
    let text = String::from_utf8_lossy(&head);
    let (mut lines, mut good, mut prod_names, mut title_names) = (0usize, 0usize, 0usize, 0usize);
    for line in text.lines().take(400) {
        let t = line.trim().trim_start_matches('\u{feff}');
        if t.is_empty() || t.starts_with(';') || t.starts_with('#') || t.starts_with('[') {
            continue;
        }
        lines += 1;
        let Some((k, v)) = t.split_once('=') else { continue };
        let (k, v) = (k.trim(), v.trim());
        let name_ok = !k.is_empty() && k.chars().all(|c| c.is_ascii_alphanumeric() || c == '_');
        let value_ok = v.len() >= 2 && v.chars().all(|c| c.is_ascii_hexdigit());
        if name_ok && value_ok {
            good += 1;
            let kl = k.to_ascii_lowercase();
            if kl.contains("header_key") || kl.contains("master_key") || kl.contains("key_area_key") || kl.contains("titlekek") {
                prod_names += 1;
            }
            if k.len() == 32 && k.chars().all(|c| c.is_ascii_hexdigit()) {
                title_names += 1;
            }
        }
    }
    // A cut-off last line is allowed for: 80 % of the lines must match.
    if good == 0 || good * 10 < lines * 8 {
        return None;
    }
    Some(by_name.unwrap_or(if prod_names > 0 {
        KeyKind::Prod
    } else if title_names == good {
        KeyKind::Title
    } else {
        KeyKind::Unknown
    }))
}

/// A firmware version in the folder name or its parents ("Firmware 20.1.0").
fn version_in_path(path: &Path) -> Option<String> {
    for comp in path.components().rev().take(3) {
        let s = comp.as_os_str().to_string_lossy();
        let chars: Vec<char> = s.chars().collect();
        let mut i = 0;
        while i < chars.len() {
            if chars[i].is_ascii_digit() && (i == 0 || !chars[i - 1].is_ascii_alphanumeric()) {
                let mut j = i;
                let mut dots = 0;
                while j < chars.len() && (chars[j].is_ascii_digit() || (chars[j] == '.' && j + 1 < chars.len() && chars[j + 1].is_ascii_digit())) {
                    if chars[j] == '.' {
                        dots += 1;
                    }
                    j += 1;
                }
                if dots == 2 {
                    return Some(chars[i..j].iter().collect());
                }
                i = j.max(i + 1);
            } else {
                i += 1;
            }
        }
    }
    None
}

pub struct Scanner<'a> {
    deadline: Instant,
    pub found: Found,
    seen: HashSet<String>,
    nca_counts: HashMap<PathBuf, (u32, u32, String)>,
    progress: &'a dyn Fn(Progress),
    last_emit: Instant,
    pub timed_out: bool,
    games_only: bool,
    cache: HashMap<String, Game>,
}

impl<'a> Scanner<'a> {
    pub fn new(budget: Duration, progress: &'a dyn Fn(Progress), games_only: bool, cache: HashMap<String, Game>) -> Self {
        Self {
            deadline: Instant::now() + budget,
            found: Found::default(),
            seen: HashSet::new(),
            nca_counts: HashMap::new(),
            progress,
            last_emit: Instant::now(),
            timed_out: false,
            games_only,
            cache,
        }
    }

    fn skip_dir(name: &str) -> bool {
        let l = name.to_ascii_lowercase();
        SKIP_DIRS.contains(&l.as_str()) || l.starts_with('$')
    }

    pub fn scan(&mut self, root: &Root, index: usize, total: usize) {
        if self.timed_out {
            return;
        }
        let mut it = WalkDir::new(&root.path)
            .max_depth(root.max_depth)
            .follow_links(false)
            .into_iter()
            .filter_entry(|e| {
                if e.depth() == 0 {
                    return true;
                }
                if e.path_is_symlink() {
                    return false;
                }
                if e.file_type().is_dir() {
                    let reparse = e.metadata().map(|m| m.file_attributes() & FILE_ATTRIBUTE_REPARSE_POINT != 0).unwrap_or(true);
                    return !reparse && !Self::skip_dir(&e.file_name().to_string_lossy());
                }
                true
            });
        while let Some(item) = it.next() {
            if Instant::now() >= self.deadline {
                self.timed_out = true;
                break;
            }
            let Ok(entry) = item else { continue };
            let name = entry.file_name().to_string_lossy().to_ascii_lowercase();
            if entry.file_type().is_dir() {
                self.found.scanned_dirs += 1;
                if entry.depth() > 0 && name.ends_with(".nca") {
                    // Ryujinx keeps each NCA as a folder holding a file named 00.
                    self.count_nca(entry.path(), &name, &root.hint);
                    it.skip_current_dir();
                }
                if self.last_emit.elapsed() >= Duration::from_millis(250) {
                    (self.progress)(Progress::new(
                        "scan",
                        index as u64,
                        total as u64,
                        Some(format!("{} folders: {}", self.found.scanned_dirs, root.hint)),
                    ));
                    self.last_emit = Instant::now();
                }
                continue;
            }
            if !entry.file_type().is_file() {
                continue;
            }
            let Ok(meta) = entry.metadata() else { continue };
            self.classify(entry.path(), &name, &meta, &root.hint);
        }
        self.flush_firmware();
    }

    fn count_nca(&mut self, path: &Path, lower_name: &str, hint: &str) {
        let Some(parent) = path.parent() else { return };
        let slot = self.nca_counts.entry(parent.to_path_buf()).or_insert((0, 0, hint.to_string()));
        slot.0 += 1;
        if lower_name.ends_with(".cnmt.nca") {
            slot.1 += 1;
        }
    }

    fn classify(&mut self, path: &Path, lower_name: &str, meta: &Metadata, hint: &str) {
        if lower_name.ends_with(".nca") {
            if !self.games_only {
                self.count_nca(path, lower_name, hint);
            }
            return;
        }
        let key = path.to_string_lossy().to_ascii_lowercase();
        if self.seen.contains(&key) {
            return;
        }
        if let Some(game) = identify_game(path, meta, &self.cache) {
            self.seen.insert(key);
            self.found.games.push(game);
            return;
        }
        if self.games_only {
            return;
        }
        if lower_name.ends_with(".keys") {
            if let Some(kind) = key_kind(path, meta) {
                self.seen.insert(key);
                self.found.keys.push(FoundKey {
                    path: path.to_string_lossy().into_owned(),
                    kind,
                    source_hint: hint.to_string(),
                });
            }
        }
    }

    /// Folders with many NCAs and many metadata NCAs: a firmware dump or an installed firmware.
    fn flush_firmware(&mut self) {
        for (dir, (ncas, cnmts, hint)) in self.nca_counts.drain() {
            if ncas < 40 || cnmts < 10 {
                continue;
            }
            let key = dir.to_string_lossy().to_ascii_lowercase();
            if !self.seen.insert(key) {
                continue;
            }
            self.found.firmware.push(FoundFirmware {
                version: version_in_path(&dir),
                path: dir.to_string_lossy().into_owned(),
                source_hint: hint,
            });
        }
    }
}

/// Games in `folder` (four levels deep, 20 s at most), reusing `cache` where it can.
pub fn scan_game_folders(folders: &[String], cache: &[Game]) -> Vec<Game> {
    let cache: HashMap<String, Game> = cache.iter().map(|g| (g.path.to_ascii_lowercase(), g.clone())).collect();
    let noop = |_: Progress| {};
    let mut scanner = Scanner::new(Duration::from_secs(20), &noop, true, cache);
    let roots: Vec<Root> = folders
        .iter()
        .map(PathBuf::from)
        .filter(|p| p.is_dir())
        .map(|p| Root { hint: p.to_string_lossy().into_owned(), path: p, max_depth: 4 })
        .collect();
    for (i, root) in roots.iter().enumerate() {
        scanner.scan(root, i, roots.len());
    }
    let mut games = scanner.found.games;
    games.sort_by(|a, b| a.title.to_lowercase().cmp(&b.title.to_lowercase()).then(a.path.cmp(&b.path)));
    games
}

pub fn locate(roots: &[Root], budget: Duration, progress: &dyn Fn(Progress)) -> Found {
    let started = Instant::now();
    let mut scanner = Scanner::new(budget, progress, false, HashMap::new());
    for (i, root) in roots.iter().enumerate() {
        scanner.scan(root, i, roots.len());
    }
    progress(Progress::new(
        "done",
        roots.len() as u64,
        roots.len() as u64,
        Some(if scanner.timed_out { "Stopped at the time limit".to_string() } else { "Done".to_string() }),
    ));
    let mut found = scanner.found;
    found.seconds = started.elapsed().as_secs_f64();
    found
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn title_ids_from_names() {
        assert_eq!(title_id_from_name("Zelda OoT [0400486024DEC000][v0]"), Some(0x0400486024DEC000));
        assert_eq!(title_id_from_name("game_0100152000022000_base"), Some(0x0100152000022000));
        assert_eq!(title_id_from_name("deadbeefdeadbeef"), None);
        assert_eq!(title_id_from_name("x0400486024DEC000"), None);
    }

    #[test]
    fn titles_are_cleaned() {
        assert_eq!(clean_title("The_Legend_of_Zelda [0400486024DEC000][v0]"), "The Legend of Zelda");
        assert_eq!(clean_title("[0400486024DEC000]"), "[0400486024DEC000]");
    }

    #[test]
    fn firmware_versions_from_paths() {
        assert_eq!(version_in_path(Path::new(r"D:\Switch\Firmware 20.1.0")).as_deref(), Some("20.1.0"));
        assert_eq!(version_in_path(Path::new(r"D:\Switch\fw\21.0.0\files")).as_deref(), Some("21.0.0"));
        assert_eq!(version_in_path(Path::new(r"C:\x\registered")), None);
    }

    #[test]
    fn keys_format_check() {
        let dir = std::env::temp_dir().join(format!("guarana-keys-test-{}", std::process::id()));
        fs::create_dir_all(&dir).unwrap();
        let prod = dir.join("prod.keys");
        fs::write(&prod, "header_key = 00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff\nmaster_key_00 = 00112233445566778899aabbccddeeff\n").unwrap();
        assert_eq!(key_kind(&prod, &fs::metadata(&prod).unwrap()), Some(KeyKind::Prod));
        let title = dir.join("my.keys");
        fs::write(&title, "01000000000100000000000000000000 = 00112233445566778899aabbccddeeff\n").unwrap();
        assert_eq!(key_kind(&title, &fs::metadata(&title).unwrap()), Some(KeyKind::Title));
        let junk = dir.join("notes.keys");
        fs::write(&junk, "hello world\nthis is not a keys file\n").unwrap();
        assert_eq!(key_kind(&junk, &fs::metadata(&junk).unwrap()), None);
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn nsp_header_title_id() {
        let dir = std::env::temp_dir().join(format!("guarana-nsp-test-{}", std::process::id()));
        fs::create_dir_all(&dir).unwrap();
        let names = b"0400486024dec0000000000000000000.tik\0abc.cnmt.nca\0";
        let mut data = Vec::new();
        data.extend_from_slice(b"PFS0");
        data.extend_from_slice(&2u32.to_le_bytes());
        data.extend_from_slice(&(names.len() as u32).to_le_bytes());
        data.extend_from_slice(&0u32.to_le_bytes());
        for (i, off) in [0u32, 37].iter().enumerate() {
            data.extend_from_slice(&(i as u64 * 16).to_le_bytes());
            data.extend_from_slice(&16u64.to_le_bytes());
            data.extend_from_slice(&off.to_le_bytes());
            data.extend_from_slice(&0u32.to_le_bytes());
        }
        data.extend_from_slice(names);
        data.extend_from_slice(&[0u8; 32]);
        let nsp = dir.join("oot.nsp");
        fs::write(&nsp, &data).unwrap();
        assert_eq!(nsp_title_id(&nsp), Some(0x0400486024DEC000));
        let game = identify_game(&nsp, &fs::metadata(&nsp).unwrap(), &HashMap::new()).unwrap();
        assert!(game.is_ocarina);
        assert_eq!(game.title, "The Legend of Zelda: Ocarina of Time");
        let _ = fs::remove_dir_all(&dir);
    }
}
