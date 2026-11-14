//! What the launcher is asked to open, read without touching the disk: a game file on the command
//! line (Explorer's "Open with Guarana", a shortcut, a second start) and `guarana://` links.
//! `open.rs` then checks the result against the disk and the library.

use std::path::{Path, PathBuf};

use crate::util;

/// The extensions Explorer opens with Guarana (`bundle > fileAssociations` in `tauri.conf.json`),
/// and the only ones a link may name. `.nca` is not in the library scan (`locate::container_of`),
/// which would count firmware files as games.
pub const GAME_EXTENSIONS: [&str; 4] = ["nsp", "xci", "nup", "nca"];

/// The link scheme (`plugins > deep-link > desktop` in `tauri.conf.json`).
pub const SCHEME: &str = "guarana";

/// Longer links are refused before they are parsed.
const MAX_LINK_CHARS: usize = 2048;
const MAX_PATH_CHARS: usize = 1024;

const RESERVED_NAMES: [&str; 22] = [
    "con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8", "com9", "lpt1",
    "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9",
];

pub fn has_game_extension(path: &Path) -> bool {
    path.extension()
        .and_then(|e| e.to_str())
        .map(|e| GAME_EXTENSIONS.iter().any(|g| e.eq_ignore_ascii_case(g)))
        .unwrap_or(false)
}

/// A `guarana:` link. Those reach the deep-link plugin, never the file argument path.
pub fn is_link(arg: &str) -> bool {
    let prefix = format!("{SCHEME}:");
    arg.trim().get(..prefix.len()).map(|p| p.eq_ignore_ascii_case(&prefix)).unwrap_or(false)
}

/// The game file among `args` (as `std::env::args_os` gives them: the program first), made
/// absolute against `cwd`, the starting folder of the process that got them. Flags and
/// `guarana:` links are skipped; the first argument with a game extension wins.
pub fn game_file_arg(args: &[String], cwd: &Path) -> Option<PathBuf> {
    args.iter().skip(1).find_map(|arg| {
        let arg = arg.trim().trim_matches('"').trim();
        if arg.is_empty() || arg.starts_with('-') || is_link(arg) {
            return None;
        }
        let path = Path::new(arg);
        if !has_game_extension(path) {
            return None;
        }
        Some(if path.is_absolute() { path.to_path_buf() } else { cwd.join(path) })
    })
}

/// What a `guarana://` link asks for.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Link {
    /// A game of the library, by title id (16 hex digits, lowercase).
    TitleId(String),
    /// A game file by its full path, checked so far only as text.
    Path(PathBuf),
}

const NOT_A_LINK: &str = "Not a Guarana link. Use guarana://launch/<title id> or guarana://launch?path=<file>";

/// Reads `guarana://launch/<title id>`, `guarana://launch?title_id=<title id>` or
/// `guarana://launch?path=<percent-encoded full path>`. Everything else is refused: another scheme
/// or action, a user name, password, port or fragment, more path segments, unknown or repeated
/// parameters, and both or neither of a title id and a path. In parameters `%20` is a space and
/// `+` stays a plus.
pub fn parse_link(text: &str) -> Result<Link, String> {
    let text = text.trim();
    if text.chars().count() > MAX_LINK_CHARS {
        return Err("The link is too long".into());
    }
    let url = url::Url::parse(text).map_err(|_| NOT_A_LINK.to_string())?;
    if url.scheme() != SCHEME {
        return Err(NOT_A_LINK.into());
    }
    if !url.username().is_empty() || url.password().is_some() || url.port().is_some() || url.fragment().is_some() {
        return Err(NOT_A_LINK.into());
    }
    if !url.host_str().map(|h| h.eq_ignore_ascii_case("launch")).unwrap_or(false) {
        return Err("A Guarana link can only start a game: guarana://launch/...".into());
    }
    let segments: Vec<&str> = url.path().split('/').filter(|s| !s.is_empty()).collect();
    let in_path = match segments.as_slice() {
        [] => None,
        [one] => Some(decode(one)?),
        _ => return Err(NOT_A_LINK.into()),
    };
    let mut title_id = None;
    let mut path = None;
    for (key, value) in query_params(url.query().unwrap_or(""))? {
        let slot = match key.as_str() {
            "title_id" => &mut title_id,
            "path" => &mut path,
            other => return Err(format!("Unknown link parameter {other}: use title_id or path")),
        };
        if slot.replace(value).is_some() {
            return Err(format!("The link gives {key} twice"));
        }
    }
    match (in_path, title_id, path) {
        (Some(t), None, None) | (None, Some(t), None) => util::normalize_title_id(&t)
            .map(Link::TitleId)
            .ok_or_else(|| format!("{} is not a title id (16 hex digits)", t.trim())),
        (None, None, Some(p)) => check_link_path(&p).map(Link::Path),
        (None, None, None) => Err("The link names no game: add a title id or a path".into()),
        _ => Err("The link names more than one game: give a title id or a path".into()),
    }
}

fn query_params(query: &str) -> Result<Vec<(String, String)>, String> {
    query
        .split('&')
        .filter(|pair| !pair.is_empty())
        .map(|pair| -> Result<(String, String), String> {
            let (key, value) = pair.split_once('=').unwrap_or((pair, ""));
            Ok((decode(key)?, decode(value)?))
        })
        .collect()
}

/// Percent-decoding only (`+` is not a space), to UTF-8 without control characters.
fn decode(text: &str) -> Result<String, String> {
    let decoded = percent_encoding::percent_decode_str(text)
        .decode_utf8()
        .map_err(|_| "The link is not valid text".to_string())?;
    if decoded.chars().any(char::is_control) {
        return Err("The link has control characters".into());
    }
    Ok(decoded.into_owned())
}

/// A link's path, checked as text before anything opens it: a full path on a drive letter
/// (`D:\Games\game.nsp`, `/` taken as `\`), no network (`\\server\share`) or device (`\\?\`,
/// `\\.\`) path, no `.` or `..`, no second `:` (an alternate data stream), no character or device
/// name Windows reserves, and one of the game extensions.
pub fn check_link_path(text: &str) -> Result<PathBuf, String> {
    let path = text.trim().replace('/', "\\");
    if path.is_empty() || path.chars().count() > MAX_PATH_CHARS {
        return Err("The link's path is empty or too long".into());
    }
    let b = path.as_bytes();
    if !(b.len() > 3 && b[0].is_ascii_alphabetic() && b[1] == b':' && b[2] == b'\\') {
        return Err("A link can only name a file on a drive, like D:\\Games\\game.nsp".into());
    }
    for part in path[3..].split('\\') {
        let bad_char = part.chars().any(|c| c.is_control() || matches!(c, ':' | '<' | '>' | '"' | '|' | '?' | '*'));
        let stem = part.split('.').next().unwrap_or("").trim_end().to_ascii_lowercase();
        if part.is_empty()
            || part == "."
            || part == ".."
            || part.ends_with('.')
            || part.ends_with(' ')
            || bad_char
            || RESERVED_NAMES.contains(&stem.as_str())
        {
            return Err(format!("{path} is not a path a link may open"));
        }
    }
    let path = PathBuf::from(path);
    if !has_game_extension(&path) {
        return Err("A link can only open .nsp, .xci, .nup and .nca files".into());
    }
    Ok(path)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn args(list: &[&str]) -> Vec<String> {
        list.iter().map(|s| s.to_string()).collect()
    }

    fn link_path(p: &str) -> Result<Link, String> {
        Ok(Link::Path(PathBuf::from(p)))
    }

    #[test]
    fn file_argument_from_explorer() {
        let cwd = Path::new(r"C:\Program Files\Somewhere");
        let got = game_file_arg(&args(&[r"C:\Guarana\guarana-launcher.exe", r"D:\Games\Zelda [0400486024DEC000].nsp"]), cwd);
        assert_eq!(got, Some(PathBuf::from(r"D:\Games\Zelda [0400486024DEC000].nsp")));
        // Any of the four extensions, in any case; quotes a shortcut left are dropped.
        for name in [r"D:\g.XCI", r"D:\g.Nup", r"D:\g.nca", r#""D:\g.nsp""#] {
            assert!(game_file_arg(&args(&["launcher.exe", name]), cwd).is_some(), "{name}");
        }
    }

    #[test]
    fn file_argument_skips_the_program_flags_links_and_other_files() {
        let cwd = Path::new(r"D:\Games");
        assert_eq!(game_file_arg(&args(&[r"D:\Games\launcher.nsp"]), cwd), None, "the program itself");
        assert_eq!(game_file_arg(&args(&["launcher.exe"]), cwd), None);
        assert_eq!(game_file_arg(&args(&["launcher.exe", "--fullscreen", r"D:\notes.txt", "game.exe"]), cwd), None);
        assert_eq!(game_file_arg(&args(&["launcher.exe", r"D:\Games\game.nsp.exe"]), cwd), None);
        // Links go to the deep-link plugin; the file path must never take them as well.
        assert_eq!(game_file_arg(&args(&["launcher.exe", "guarana://launch?path=D:%5Cg.nsp"]), cwd), None);
        assert_eq!(game_file_arg(&args(&["launcher.exe", "GUARANA://launch/x.nsp"]), cwd), None);
        assert_eq!(
            game_file_arg(&args(&["launcher.exe", "-v", "guarana:x.nsp", r"E:\b.xci", r"E:\c.nsp"]), cwd),
            Some(PathBuf::from(r"E:\b.xci")),
            "the first game file wins"
        );
    }

    #[test]
    fn relative_file_arguments_follow_the_starting_folder() {
        let cwd = Path::new(r"D:\Games");
        assert_eq!(game_file_arg(&args(&["launcher.exe", r"sub\game.nsp"]), cwd), Some(PathBuf::from(r"D:\Games\sub\game.nsp")));
        assert_eq!(game_file_arg(&args(&["launcher.exe", r"\game.nsp"]), cwd), Some(PathBuf::from(r"D:\game.nsp")));
    }

    #[test]
    fn link_kinds() {
        assert!(is_link("guarana://launch/0400486024DEC000"));
        assert!(is_link("  Guarana:x"));
        assert!(!is_link("guaran://x"));
        assert!(!is_link(r"D:\guarana\game.nsp"));
        assert!(!is_link("g"));
    }

    #[test]
    fn links_by_title_id() {
        let oot = Ok(Link::TitleId("0400486024dec000".into()));
        assert_eq!(parse_link("guarana://launch/0400486024DEC000"), oot);
        assert_eq!(parse_link("guarana://launch/0400486024DEC000/"), oot);
        assert_eq!(parse_link("guarana://launch?title_id=0400486024dec000"), oot);
        assert_eq!(parse_link("guarana://launch/?title_id=0x0400486024DEC000"), oot);
        assert_eq!(parse_link("GUARANA://LAUNCH/0400486024DEC000"), oot);
        assert!(parse_link("guarana://launch/0400486024DEC00").is_err(), "15 digits");
        assert!(parse_link("guarana://launch/0400486024DEC00G").is_err());
        assert!(parse_link("guarana://launch?title_id=").is_err());
    }

    #[test]
    fn links_by_path() {
        assert_eq!(parse_link("guarana://launch?path=D%3A%5CGames%5CZelda.nsp"), link_path(r"D:\Games\Zelda.nsp"));
        assert_eq!(parse_link(r"guarana://launch?path=D:\Games\Zelda.NSP"), link_path(r"D:\Games\Zelda.NSP"));
        assert_eq!(parse_link("guarana://launch?path=D:/Games/Zelda.xci"), link_path(r"D:\Games\Zelda.xci"));
        // %20 is a space and + a plus; a raw space is encoded by the URL parser first.
        assert_eq!(parse_link("guarana://launch?path=D:%5CMy%20Games%5CA+B.nsp"), link_path(r"D:\My Games\A+B.nsp"));
        assert_eq!(parse_link(r"guarana://launch?path=D:\My Games\A%2BB.nup"), link_path(r"D:\My Games\A+B.nup"));
        assert_eq!(parse_link("guarana://launch?path=D:%5CSpi%C3%A9le%5Cg.nca"), link_path("D:\\Spi\u{e9}le\\g.nca"));
    }

    #[test]
    fn links_refused() {
        for bad in [
            "",
            "launch/0400486024DEC000",
            "https://launch/0400486024DEC000",
            "guaranax://launch/0400486024DEC000",
            "guarana:launch/0400486024DEC000",
            "guarana:///0400486024DEC000",
            "guarana://run/0400486024DEC000",
            "guarana://play?title_id=0400486024DEC000",
            "guarana://launch",
            "guarana://launch/",
            "guarana://launch/0400486024DEC000/extra",
            "guarana://launch/0400486024DEC000?title_id=0400486024DEC000",
            "guarana://launch?title_id=0400486024DEC000&path=D:%5Cg.nsp",
            "guarana://launch?title_id=0400486024DEC000&title_id=0400486024DEC000",
            "guarana://launch?title_id=0400486024DEC000&exec=calc.exe",
            "guarana://launch?cmd=calc.exe",
            "guarana://launch/0400486024DEC000#x",
            "guarana://user@launch/0400486024DEC000",
            "guarana://user:pw@launch/0400486024DEC000",
            "guarana://launch:80/0400486024DEC000",
            "guarana://launch?path=D:%5Cg.nsp%00.exe",
            "guarana://launch?path=D:%5Cg%FF.nsp",
        ] {
            assert!(parse_link(bad).is_err(), "{bad}");
        }
        let long = format!("guarana://launch?path=D:%5C{}.nsp", "a".repeat(MAX_LINK_CHARS));
        assert!(parse_link(&long).is_err());
    }

    #[test]
    fn link_paths_must_be_plain_drive_paths_to_game_files() {
        assert_eq!(check_link_path(r"D:\Games\g.nsp"), Ok(PathBuf::from(r"D:\Games\g.nsp")));
        assert_eq!(check_link_path(r"c:\g.xci"), Ok(PathBuf::from(r"c:\g.xci")));
        for bad in [
            r"\\server\share\g.nsp",
            r"//server/share/g.nsp",
            r"\\?\D:\Games\g.nsp",
            r"\\.\D:\Games\g.nsp",
            r"\\?\UNC\server\share\g.nsp",
            r"Games\g.nsp",
            r"\Games\g.nsp",
            r"D:Games\g.nsp",
            r"D:\",
            r"D:\Games\..\..\Windows\g.nsp",
            r"D:\Games\.\g.nsp",
            r"D:\Games\\g.nsp",
            r"D:\Games\g.nsp:hidden.nsp",
            r"D:\Games\g.nsp::$DATA",
            r"D:\Games\g*.nsp",
            r"D:\Games\g?.nsp",
            r"D:\Games\a|b.nsp",
            r"D:\Games\g.nsp.",
            r"D:\Games \g.nsp",
            r"D:\Games\NUL.nsp",
            r"D:\COM1\g.nsp",
            r"D:\Games\g.exe",
            r"D:\Games\g.nsp.exe",
            r"D:\Games\g",
            r"D:\Games\nsp",
            "D:\\Games\\g\u{1}.nsp",
        ] {
            assert!(check_link_path(bad).is_err(), "{bad}");
        }
    }
}
