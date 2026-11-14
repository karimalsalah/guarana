//! Games the launcher is asked to start from outside: a file Explorer opens with Guarana (or a
//! shortcut or second start passes), and `guarana://` links (tauri-plugin-deep-link). Each request
//! is checked against the disk and the library on the blocking pool, kept in `AppState` and
//! announced with `launch://request`; the UI takes it with `take_launch_request` and starts it on
//! its launch screen, through `launch_game` like any other game.

use std::collections::HashMap;
use std::fs::{self, Metadata};
use std::path::Path;

use serde::Serialize;
use tauri::{AppHandle, Emitter, Manager};
use tauri_plugin_deep_link::DeepLinkExt;

use crate::args::{self, Link};
use crate::locate;
use crate::paths;
use crate::state::{AppState, Persisted};
use crate::types::{Container, Game};

pub const EVENT: &str = "launch://request";

/// What `take_launch_request` returns: the game to start, or why it cannot be.
#[derive(Debug, Clone, Serialize)]
pub struct LaunchRequest {
    /// `file` (Explorer, a shortcut or the command line) or `link` (`guarana://`).
    pub source: String,
    pub game: Option<Game>,
    pub error: Option<String>,
}

impl LaunchRequest {
    fn new(source: &str, result: Result<Game, String>) -> Self {
        let (game, error) = match result {
            Ok(g) => (Some(g), None),
            Err(e) => (None, Some(e)),
        };
        Self { source: source.to_string(), game, error }
    }
}

/// At start-up: the link or file this process was started with, and links that arrive later.
pub fn init(app: &AppHandle) {
    let handle = app.clone();
    app.deep_link().on_open_url(move |event| {
        if let Some(url) = event.urls().into_iter().next() {
            from_link(&handle, url.as_str().to_string());
        }
    });
    // The plugin read the start link before this listener existed.
    match app.deep_link().get_current() {
        Ok(Some(urls)) => {
            if let Some(url) = urls.first() {
                from_link(app, url.as_str().to_string());
            }
        }
        Ok(None) => {}
        Err(e) => log::warn!("Could not read the link Guarana was started with: {e}"),
    }
    let argv: Vec<String> = std::env::args_os().map(|a| a.to_string_lossy().into_owned()).collect();
    from_args(app, &argv, &std::env::current_dir().unwrap_or_default());
    #[cfg(debug_assertions)]
    register_for_development(app);
}

/// The game file in a command line (`std::env::args_os` order), if any. Called from the
/// single-instance callback, which runs while the second process waits for it, so the disk is
/// only read on the blocking pool. Links in it reach `on_open_url` instead.
pub fn from_args(app: &AppHandle, argv: &[String], cwd: &Path) {
    let Some(path) = args::game_file_arg(argv, cwd) else { return };
    let app = app.clone();
    tauri::async_runtime::spawn_blocking(move || {
        let result = resolve_file(&path, &app.state::<AppState>().load());
        queue(&app, LaunchRequest::new("file", result));
    });
}

fn from_link(app: &AppHandle, url: String) {
    let app = app.clone();
    tauri::async_runtime::spawn_blocking(move || {
        let result = args::parse_link(&url).and_then(|link| resolve_link(link, &app.state::<AppState>().load()));
        queue(&app, LaunchRequest::new("link", result));
    });
}

fn queue(app: &AppHandle, request: LaunchRequest) {
    match (&request.game, &request.error) {
        (Some(g), _) => log::info!("Asked to start {} ({}) from a {}", g.title, g.path, request.source),
        (None, Some(e)) => log::warn!("Refused a {} request: {e}", request.source),
        _ => {}
    }
    let st = app.state::<AppState>();
    *st.launch_request.lock().unwrap_or_else(|e| e.into_inner()) = Some(request);
    let _ = app.emit(EVENT, ());
}

/// `take_launch_request`: the newest request, once.
pub fn take(app: &AppHandle) -> Option<LaunchRequest> {
    app.state::<AppState>().launch_request.lock().unwrap_or_else(|e| e.into_inner()).take()
}

/// A file from the command line: it must be a file; its library entry names it when it has one.
pub fn resolve_file(path: &Path, persisted: &Persisted) -> Result<Game, String> {
    let meta = fs::metadata(path)
        .ok()
        .filter(Metadata::is_file)
        .ok_or_else(|| format!("{} is not there any more", path.display()))?;
    Ok(library_entry(path, persisted).unwrap_or_else(|| identify(path, &meta)))
}

/// A link: a title id must be in the library; a path must be inside one of the game folders the
/// user added (checked before the disk is touched) and be a file there, not a link to one.
pub fn resolve_link(link: Link, persisted: &Persisted) -> Result<Game, String> {
    match link {
        Link::TitleId(tid) => persisted
            .games
            .iter()
            .find(|g| g.title_id.eq_ignore_ascii_case(&tid) && Path::new(&g.path).is_file())
            .cloned()
            .ok_or_else(|| format!("No game with title id {} in your library", tid.to_ascii_uppercase())),
        Link::Path(path) => {
            if !persisted.game_folders.iter().any(|f| paths::is_within(&path, Path::new(f))) {
                return Err(format!(
                    "{} is not in one of your game folders. Links start games from those only; open other files from Explorer.",
                    path.display()
                ));
            }
            let meta = fs::symlink_metadata(&path)
                .ok()
                .filter(|m| m.file_type().is_file())
                .ok_or_else(|| format!("{} is not there", path.display()))?;
            Ok(library_entry(&path, persisted).unwrap_or_else(|| identify(&path, &meta)))
        }
    }
}

fn library_entry(path: &Path, persisted: &Persisted) -> Option<Game> {
    let wanted = path.to_string_lossy().to_lowercase();
    persisted.games.iter().find(|g| g.path.to_lowercase() == wanted).cloned()
}

/// A game file outside the library: what the scan would read from it, or, for an `.nca` (which the
/// scan leaves to firmware), its name.
fn identify(path: &Path, meta: &Metadata) -> Game {
    locate::identify_game(path, meta, &HashMap::new()).unwrap_or_else(|| {
        let stem = path.file_stem().map(|s| s.to_string_lossy().into_owned()).unwrap_or_default();
        let title_id = locate::title_id_from_name(&stem).map(|v| format!("{v:016X}")).unwrap_or_default();
        Game {
            is_ocarina: title_id.eq_ignore_ascii_case(locate::OCARINA_TITLE_ID),
            title_id,
            title: stem,
            path: path.to_string_lossy().into_owned(),
            container: Container::Other,
            size_bytes: meta.len(),
        }
    })
}

/// `tauri dev` runs a program the installer never registered: `guarana://` is pointed at it,
/// unless an installed Guarana already owns the scheme.
#[cfg(debug_assertions)]
fn register_for_development(app: &AppHandle) {
    use winreg::enums::HKEY_CURRENT_USER;
    let owned = winreg::RegKey::predef(HKEY_CURRENT_USER)
        .open_subkey(format!(r"Software\Classes\{}\shell\open\command", args::SCHEME))
        .is_ok();
    if !owned {
        if let Err(e) = app.deep_link().register_all() {
            log::warn!("Could not register {}:// for this build: {e}", args::SCHEME);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::PathBuf;

    fn scratch(tag: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("guarana-open-test-{tag}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        dir
    }

    fn game(path: &Path, title_id: &str, title: &str) -> Game {
        Game {
            title_id: title_id.into(),
            title: title.into(),
            path: path.to_string_lossy().into_owned(),
            container: Container::Nsp,
            size_bytes: 3,
            is_ocarina: false,
        }
    }

    #[test]
    fn link_paths_stay_inside_the_game_folders() {
        let root = scratch("folders");
        let games = root.join("Games");
        let elsewhere = root.join("Downloads");
        fs::create_dir_all(&games).unwrap();
        fs::create_dir_all(&elsewhere).unwrap();
        fs::write(games.join("Zelda [0400486024DEC000].nsp"), b"abc").unwrap();
        fs::write(elsewhere.join("other.nsp"), b"abc").unwrap();
        fs::create_dir_all(games.join("folder.nsp")).unwrap();
        let persisted = Persisted { game_folders: vec![games.to_string_lossy().into_owned()], ..Default::default() };

        let ok = resolve_link(Link::Path(games.join("Zelda [0400486024DEC000].nsp")), &persisted).unwrap();
        assert_eq!(ok.title_id, "0400486024DEC000");
        assert!(ok.is_ocarina);
        assert!(resolve_link(Link::Path(elsewhere.join("other.nsp")), &persisted).unwrap_err().contains("game folders"));
        assert!(resolve_link(Link::Path(games.join("missing.nsp")), &persisted).is_err());
        assert!(resolve_link(Link::Path(games.join("folder.nsp")), &persisted).is_err(), "a folder is not a game");
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn title_id_links_use_the_library() {
        let root = scratch("library");
        let file = root.join("Ocarina.nsp");
        fs::write(&file, b"abc").unwrap();
        let persisted = Persisted {
            games: vec![
                game(&root.join("gone.nsp"), "040074A01BF12000", "Donkey Kong Bananza"),
                game(&file, "0400486024DEC000", "The Legend of Zelda: Ocarina of Time"),
            ],
            ..Default::default()
        };
        let found = resolve_link(Link::TitleId("0400486024dec000".into()), &persisted).unwrap();
        assert_eq!(found.title, "The Legend of Zelda: Ocarina of Time");
        // In the list, but its file is gone.
        assert!(resolve_link(Link::TitleId("040074a01bf12000".into()), &persisted).is_err());
        assert!(resolve_link(Link::TitleId("0400c3f00006e000".into()), &persisted).is_err());
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn files_from_the_command_line() {
        let root = scratch("files");
        let listed = root.join("Listed.nsp");
        let content = root.join("0400486024dec000-program.nca");
        fs::write(&listed, b"abc").unwrap();
        fs::write(&content, b"abc").unwrap();
        let persisted = Persisted { games: vec![game(&listed, "0400486024DEC000", "Listed title")], ..Default::default() };
        // The library entry is matched without regard to case.
        let upper = PathBuf::from(listed.to_string_lossy().to_uppercase());
        assert_eq!(resolve_file(&upper, &persisted).unwrap().title, "Listed title");
        let nca = resolve_file(&content, &persisted).unwrap();
        assert_eq!((nca.container, nca.title_id.as_str(), nca.size_bytes), (Container::Other, "0400486024DEC000", 3));
        assert!(resolve_file(&root.join("missing.xci"), &persisted).is_err());
        assert!(resolve_file(&root, &persisted).is_err());
        let _ = fs::remove_dir_all(&root);
    }
}
