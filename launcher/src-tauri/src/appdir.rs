//! The program folder: Guarana runs from `%LOCALAPPDATA%\Guarana\app\`, the same path for every
//! version. Windows keys per-app settings by the program's full path: the microphone prompt
//! (issue #199), the GPU choice in Settings > System > Display > Graphics, Windows Firewall rules
//! and Game Bar's game detection. Run from `versions\<version>\`, every update was a new program to
//! Windows, and all of them started over.
//!
//! `versions\<version>\` stays the installed copy of each version (rollback switches back to one)
//! and `current.json` names the one to run. `app\` mirrors that one in a real folder: the programs
//! Windows knows Guarana by (`guarana.exe`, `guarana-cli.exe` and their old names) are copies, and
//! every other file is a hard link to the version's file (a copy too where the volume has no hard
//! links). A junction or a symbolic link would not do: Windows resolves them, so the process would
//! still be `versions\<version>\guarana.exe` to it. Nor would a hard link to the program: Windows
//! keeps one image of a program file for all its names, and can name a process after the name that
//! image was first loaded from, the version folder's when the program ran from there before.
//!
//! A switch builds the new mirror beside `app\` (`.app-staging-*`) and writes `app-version.json`
//! into it last. It then moves `app\` aside (`.app-old-*`) and the mirror into its place; when the
//! second rename fails, the old folder goes back. So `app\` always holds one whole version. A
//! rename is tried again for two seconds, as a virus scanner can hold a new file for a moment
//! (upstream's `UpdateInstaller::Move`, `src/frontend_common/update_installer.cpp`). Nothing is
//! switched while Guarana or a shader warm-up runs: an update or rollback is current at once, and
//! the next Play or warm-up switches. A start that cannot switch runs the version from its own
//! folder, and the log says why.
//!
//! A `user` folder (portable mode, `src/common/fs/path_util.cpp`) is never mirrored and never
//! deleted. A version folder that has one keeps running from that folder, where the data is. One
//! made in `app\` moves into each new `app\`.

use std::ffi::OsStr;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};
use std::sync::{Mutex, MutexGuard};
use std::time::{Duration, Instant};

use serde::{Deserialize, Serialize};

use crate::types::InstalledVersion;
use crate::{paths, util};

/// What `app\` holds, written into it last.
const MARKER: &str = "app-version.json";
const STAGING: &str = ".app-staging-";
const RETIRED: &str = ".app-old-";
/// The emulator's portable data folder beside its executable.
const USER: &str = "user";

/// Makes one file of a mirror: `(existing, new)`.
type LinkFn<'a> = &'a dyn Fn(&Path, &Path) -> io::Result<()>;
/// Moves a folder: `(from, to)`.
type RenameFn<'a> = &'a dyn Fn(&Path, &Path) -> io::Result<()>;

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
struct Marker {
    version: String,
    channel: String,
    /// With the version and channel, tells a reinstall of the same version from the copy before it.
    installed_at: String,
    /// The version's folder the mirror was made from.
    source: String,
    #[serde(default)]
    made_at: String,
}

impl Marker {
    fn of(v: &InstalledVersion) -> Self {
        Self {
            version: v.version.clone(),
            channel: v.channel.clone(),
            installed_at: v.installed_at.clone(),
            source: v.path.clone(),
            made_at: util::now_rfc3339(),
        }
    }

    fn names(&self, v: &InstalledVersion) -> bool {
        self.version == v.version && self.channel == v.channel && self.installed_at == v.installed_at
    }
}

/// Files a switch linked and copied.
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
struct Mirror {
    linked: u64,
    copied: u64,
}

#[derive(Debug, PartialEq, Eq)]
enum Outcome {
    /// `app\` already held the version.
    Current,
    /// `app\` holds it now.
    Switched(Mirror),
    /// The version's folder has a `user` folder: it runs from there.
    Portable,
}

fn read_marker(app: &Path) -> Option<Marker> {
    serde_json::from_slice(&fs::read(app.join(MARKER)).ok()?).ok()
}

/// Whether `app` holds `v`: its record names `v` and the emulator is there.
fn holds(app: &Path, v: &InstalledVersion) -> bool {
    read_marker(app).is_some_and(|m| m.names(v)) && paths::emulator_exe_in(app).is_some()
}

fn is_portable(version_dir: &Path) -> bool {
    version_dir.join(USER).is_dir()
}

/// The programs Windows keys its settings to, at the top of a version folder: always copied.
fn is_program(name: &OsStr) -> bool {
    [paths::EMULATOR_EXE, paths::EMULATOR_EXE_LEGACY, paths::CLI_EXE, paths::CLI_EXE_LEGACY]
        .iter()
        .any(|p| name.eq_ignore_ascii_case(p))
}

/// Makes `root\app` hold `v`, whose emulator folder is `v.path`. Blocking.
fn switch_in(root: &Path, v: &InstalledVersion, link: LinkFn, rename: RenameFn) -> io::Result<Outcome> {
    let source = Path::new(&v.path);
    if is_portable(source) {
        return Ok(Outcome::Portable);
    }
    let app = root.join(paths::APP_DIR);
    if holds(&app, v) {
        return Ok(Outcome::Current);
    }
    if paths::emulator_exe_in(source).is_none() {
        return Err(io::Error::new(
            io::ErrorKind::NotFound,
            format!("{} has no {}", source.display(), paths::EMULATOR_EXE),
        ));
    }
    let staging = unused(root, STAGING);
    let result = build(source, &staging, v, link).and_then(|mirror| swap(&app, &staging, root, rename).map(|()| mirror));
    if result.is_err() {
        // Links and copies only: the version's own folder keeps every file.
        let _ = fs::remove_dir_all(&staging);
    }
    result.map(Outcome::Switched)
}

/// Mirrors `source` into the new folder `staging` and writes the record last.
fn build(source: &Path, staging: &Path, v: &InstalledVersion, link: LinkFn) -> io::Result<Mirror> {
    let mut mirror = Mirror::default();
    mirror_dir(source, staging, true, link, &mut mirror)?;
    let record = serde_json::to_vec_pretty(&Marker::of(v)).map_err(io::Error::other)?;
    util::write_atomic(&staging.join(MARKER), &record)?;
    Ok(mirror)
}

/// Makes the folder `to` with the folders of `from` and a link (or copy) of each file. At the top
/// it copies the programs and leaves out `user` and a record; links and junctions are skipped, so
/// nothing outside the version's folder is reached.
fn mirror_dir(from: &Path, to: &Path, top: bool, link: LinkFn, mirror: &mut Mirror) -> io::Result<()> {
    fs::create_dir(to)?;
    for entry in fs::read_dir(from)? {
        let entry = entry?;
        let name = entry.file_name();
        if top && (name.eq_ignore_ascii_case(USER) || name.eq_ignore_ascii_case(MARKER)) {
            continue;
        }
        let (src, dst) = (entry.path(), to.join(&name));
        let kind = entry.file_type()?;
        if kind.is_symlink() {
            log::warn!("Program folder: skipped the link {}", src.display());
        } else if kind.is_dir() {
            mirror_dir(&src, &dst, false, link, mirror)?;
        } else if kind.is_file() {
            if !(top && is_program(&name)) && link(&src, &dst).is_ok() {
                mirror.linked += 1;
            } else {
                fs::copy(&src, &dst)?;
                mirror.copied += 1;
            }
        }
    }
    Ok(())
}

/// Moves `staging` to `app`. The previous `app` goes aside first and comes back when the second
/// rename fails; once the new one is in place, the previous one is retired.
fn swap(app: &Path, staging: &Path, root: &Path, rename: RenameFn) -> io::Result<()> {
    let previous = match fs::symlink_metadata(app) {
        Ok(_) => {
            let aside = unused(root, RETIRED);
            rename(app, &aside)?;
            Some(aside)
        }
        Err(_) => None,
    };
    if let Err(e) = rename(staging, app) {
        if let Some(aside) = &previous {
            if let Err(back) = rename(aside, app) {
                // The next start puts it back (`recover_in`); until then Guarana runs from its version folder.
                log::error!("Program folder: {} could not be put back from {}: {back}", app.display(), aside.display());
            }
        }
        return Err(e);
    }
    if let Some(aside) = previous {
        retire(&aside, app, rename);
    }
    Ok(())
}

/// Deletes a folder `app` was before, after moving its `user` folder into `app` when `app` has
/// none. A `user` folder that cannot move keeps the whole folder: it can hold saves and keys.
fn retire(old: &Path, app: &Path, rename: RenameFn) {
    let user = old.join(USER);
    if fs::symlink_metadata(&user).is_ok() {
        let target = app.join(USER);
        if fs::symlink_metadata(&target).is_ok() || rename(&user, &target).is_err() {
            log::warn!("Program folder: kept {} for the user folder in it", old.display());
            return;
        }
        log::info!("Program folder: moved the user folder into {}", app.display());
    }
    if let Err(e) = fs::remove_dir_all(old) {
        log::info!("Program folder: {} goes at the next start: {e}", old.display());
    }
}

/// After a crash: removes unfinished mirrors, puts `app` back when a switch stopped between its two
/// renames, and retires the folders switches left aside. Blocking.
fn recover_in(root: &Path, rename: RenameFn) {
    let Ok(entries) = fs::read_dir(root) else { return };
    let mut retired = Vec::new();
    for entry in entries.flatten() {
        let name = entry.file_name().to_string_lossy().into_owned();
        if name.starts_with(STAGING) {
            // Mirrors never hold a `user` folder; one that does was not made here and stays.
            if fs::symlink_metadata(entry.path().join(USER)).is_err() {
                let _ = fs::remove_dir_all(entry.path());
            }
        } else if name.starts_with(RETIRED) {
            retired.push(entry.path());
        }
    }
    // Oldest first: the last one was `app` most recently.
    retired.sort();
    let app = root.join(paths::APP_DIR);
    if fs::symlink_metadata(&app).is_err() {
        if let Some(last) = retired.pop() {
            match rename(&last, &app) {
                Ok(()) => log::info!("Program folder: put {} back after an interrupted switch", app.display()),
                Err(_) => retired.push(last),
            }
        }
    }
    for old in retired {
        retire(&old, &app, rename);
    }
}

/// A name under `root` that nothing has: `<prefix><time>-<pid>`, with a count when taken.
fn unused(root: &Path, prefix: &str) -> PathBuf {
    let base = format!("{prefix}{}-{}", util::stamp(), std::process::id());
    let mut path = root.join(&base);
    let mut n = 1;
    while fs::symlink_metadata(&path).is_ok() {
        path = root.join(format!("{base}-{n}"));
        n += 1;
    }
    path
}

/// Tries `f` up to `tries` times, `pause` apart. A missing source is not waited for.
fn retry(tries: u32, pause: Duration, mut f: impl FnMut() -> io::Result<()>) -> io::Result<()> {
    let mut n = 1;
    loop {
        match f() {
            Ok(()) => return Ok(()),
            Err(e) if n >= tries || e.kind() == io::ErrorKind::NotFound => return Err(e),
            Err(_) => {
                n += 1;
                std::thread::sleep(pause);
            }
        }
    }
}

fn rename_retry(from: &Path, to: &Path) -> io::Result<()> {
    retry(10, Duration::from_millis(200), || fs::rename(from, to))
}

fn hard_link(existing: &Path, new: &Path) -> io::Result<()> {
    fs::hard_link(existing, new)
}

/// One switch at a time, and none between a start's choice of folder and its process.
static LOCK: Mutex<()> = Mutex::new(());

fn lock() -> MutexGuard<'static, ()> {
    LOCK.lock().unwrap_or_else(|e| e.into_inner())
}

/// What keeps `app\` from being switched now. Any running emulator or warm-up counts, wherever it
/// was started from: a rename that fails is not a safe test, as the rules for renaming a folder
/// with open files in it differ between Windows builds.
fn busy() -> Option<&'static str> {
    if util::emulator_running() {
        Some("Guarana is running")
    } else if util::warm_up_running() {
        Some("shaders are being prepared")
    } else {
        None
    }
}

/// Makes `app\` hold `v` unless something runs, waiting up to `wait` for it to end, and logs what
/// it did. True when `app\` holds `v`. The caller holds the lock.
fn prepare(v: &InstalledVersion, wait: Duration) -> bool {
    let root = paths::install_dir();
    let app = root.join(paths::APP_DIR);
    if is_portable(Path::new(&v.path)) {
        log::info!("Program folder: Guarana {} runs from {}, which has a portable user folder", v.version, v.path);
        return false;
    }
    if holds(&app, v) {
        return true;
    }
    let deadline = Instant::now() + wait;
    while let Some(why) = busy() {
        if Instant::now() >= deadline {
            log::info!("Program folder: {} switches to Guarana {} once nothing runs ({why})", app.display(), v.version);
            return false;
        }
        std::thread::sleep(Duration::from_millis(250));
    }
    let started = Instant::now();
    match switch_in(&root, v, &hard_link, &rename_retry) {
        Ok(Outcome::Switched(m)) => {
            log::info!(
                "Program folder: Guarana {} runs from {} now ({} files linked, {} copied, {} ms)",
                v.version,
                app.display(),
                m.linked,
                m.copied,
                started.elapsed().as_millis()
            );
            true
        }
        Ok(Outcome::Current) => true,
        Ok(Outcome::Portable) => false,
        Err(e) => {
            log::warn!("Program folder: {} not switched to Guarana {}: {e}", app.display(), v.version);
            false
        }
    }
}

/// After an install, a rollback and at launcher start: switches `app\` to `v` now, or leaves it to
/// the next Play or warm-up while Guarana or a warm-up runs. Blocking.
pub fn switch_to(v: &InstalledVersion) {
    let _held = lock();
    prepare(v, Duration::ZERO);
}

/// At launcher start, before anything runs from `app\`: finishes or undoes a switch a crash cut
/// short. Blocking.
pub fn recover() {
    let _held = lock();
    recover_in(&paths::install_dir(), &rename_retry);
}

/// The folder to start `v` from. Keep it until the process has started: no switch moves the
/// folder in between.
pub struct RunDir {
    pub dir: PathBuf,
    /// `dir` is `app\`; otherwise it is the version's own folder.
    pub stable: bool,
    _held: MutexGuard<'static, ()>,
}

/// `app\` when it holds `v` or can be switched to it now, waiting up to `wait` for a stopped
/// warm-up to exit; else `v`'s own folder. Blocking.
pub fn run_dir(v: &InstalledVersion, wait: Duration) -> RunDir {
    let held = lock();
    let stable = prepare(v, wait);
    let dir = if stable { paths::app_dir() } else { PathBuf::from(&v.path) };
    RunDir { dir, stable, _held: held }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::Cell;

    fn scratch(tag: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("guarana-appdir-{tag}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        dir
    }

    fn write(path: &Path, body: &str) {
        fs::create_dir_all(path.parent().unwrap()).unwrap();
        fs::write(path, body).unwrap();
    }

    fn read(path: &Path) -> String {
        fs::read_to_string(path).unwrap_or_default()
    }

    /// `versions\<label>\` holding `files`, installed at `at`.
    fn install(root: &Path, label: &str, at: &str, files: &[(&str, &str)]) -> InstalledVersion {
        let dir = root.join("versions").join(label);
        let _ = fs::remove_dir_all(&dir);
        for (rel, body) in files {
            write(&dir.join(rel), body);
        }
        InstalledVersion {
            version: label.into(),
            channel: "stable".into(),
            path: dir.to_string_lossy().into_owned(),
            installed_at: at.into(),
        }
    }

    fn plain_rename(from: &Path, to: &Path) -> io::Result<()> {
        fs::rename(from, to)
    }

    fn switch(root: &Path, v: &InstalledVersion) -> io::Result<Outcome> {
        switch_in(root, v, &hard_link, &plain_rename)
    }

    /// What switches left aside or half made under `root`.
    fn leftovers(root: &Path) -> Vec<String> {
        fs::read_dir(root)
            .unwrap()
            .flatten()
            .map(|e| e.file_name().to_string_lossy().into_owned())
            .filter(|n| n.starts_with(STAGING) || n.starts_with(RETIRED))
            .collect()
    }

    #[test]
    fn the_first_switch_mirrors_the_version_folder() {
        let root = scratch("first");
        let v = install(&root, "20.4.0", "2026-11-16T08:41:37Z", &[
            ("guarana.exe", "v1"),
            ("platforms/qwindows.dll", "q"),
            ("qt.conf", "c"),
        ]);
        match switch(&root, &v).unwrap() {
            Outcome::Switched(m) => assert_eq!(m.linked + m.copied, 3),
            other => panic!("{other:?}"),
        }
        let app = root.join(paths::APP_DIR);
        assert_eq!(read(&app.join("guarana.exe")), "v1");
        assert_eq!(read(&app.join("platforms").join("qwindows.dll")), "q");
        assert!(holds(&app, &v));
        assert_eq!(read_marker(&app).unwrap().source, v.path);
        assert!(leftovers(&root).is_empty());
        assert_eq!(switch(&root, &v).unwrap(), Outcome::Current);
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn the_programs_are_copied_and_the_rest_linked() {
        let root = scratch("programs");
        let v = install(&root, "20.4.0", "t", &[("guarana.exe", "e"), ("guarana-cli.exe", "c"), ("Qt6Core.dll", "q")]);
        let linked = std::cell::RefCell::new(Vec::new());
        let link = |from: &Path, to: &Path| {
            linked.borrow_mut().push(from.file_name().unwrap().to_string_lossy().into_owned());
            fs::hard_link(from, to)
        };
        switch_in(&root, &v, &link, &plain_rename).unwrap();
        assert_eq!(*linked.borrow(), ["Qt6Core.dll"]);
        // The program in `app` is its own file: a write to the version's copy does not reach it.
        fs::write(Path::new(&v.path).join("guarana.exe"), "patched").unwrap();
        assert_eq!(read(&root.join(paths::APP_DIR).join("guarana.exe")), "e");
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn an_update_and_a_rollback_keep_the_same_path() {
        let root = scratch("update");
        let old = install(&root, "20.3.0", "2026-11-10T00:00:00Z", &[("guarana.exe", "old"), ("gone.dll", "x")]);
        let new = install(&root, "20.4.0", "2026-11-16T00:00:00Z", &[("guarana.exe", "new"), ("guarana-cli.exe", "cli")]);
        let app = root.join(paths::APP_DIR);
        switch(&root, &old).unwrap();
        assert!(matches!(switch(&root, &new).unwrap(), Outcome::Switched(_)));
        assert_eq!(read(&app.join("guarana.exe")), "new");
        assert!(!app.join("gone.dll").exists(), "nothing of the previous version stays");
        assert!(holds(&app, &new) && !holds(&app, &old));
        // Retiring the previous folder left the installed copy whole.
        assert_eq!(read(&Path::new(&old.path).join("guarana.exe")), "old");
        assert_eq!(read(&Path::new(&old.path).join("gone.dll")), "x");
        assert!(matches!(switch(&root, &old).unwrap(), Outcome::Switched(_)));
        assert_eq!(read(&app.join("guarana.exe")), "old");
        assert_eq!(read(&app.join("gone.dll")), "x");
        assert!(leftovers(&root).is_empty());
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn a_reinstall_of_the_same_version_is_switched_again() {
        let root = scratch("reinstall");
        let first = install(&root, "nightly-20261116-7fa8e8c", "2026-11-16T04:38:50Z", &[("guarana.exe", "a")]);
        switch(&root, &first).unwrap();
        let again = install(&root, "nightly-20261116-7fa8e8c", "2026-11-16T09:00:00Z", &[("guarana.exe", "b")]);
        assert!(matches!(switch(&root, &again).unwrap(), Outcome::Switched(_)));
        assert_eq!(read(&root.join(paths::APP_DIR).join("guarana.exe")), "b");
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn the_emulator_folder_inside_a_version_folder_is_what_is_mirrored() {
        let root = scratch("nested");
        let mut v = install(&root, "20.0.2", "2026-11-13T00:00:00Z", &[("guarana-windows/guarana.exe", "e")]);
        v.path = Path::new(&v.path).join("guarana-windows").to_string_lossy().into_owned();
        switch(&root, &v).unwrap();
        assert_eq!(read(&root.join(paths::APP_DIR).join("guarana.exe")), "e");
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn a_failed_second_rename_puts_the_previous_folder_back() {
        let root = scratch("put-back");
        let old = install(&root, "20.3.0", "t1", &[("guarana.exe", "old")]);
        let new = install(&root, "20.4.0", "t2", &[("guarana.exe", "new")]);
        switch(&root, &old).unwrap();
        let scanner_holds_the_mirror = |from: &Path, to: &Path| {
            if from.file_name().unwrap().to_string_lossy().starts_with(STAGING) {
                Err(io::Error::new(io::ErrorKind::PermissionDenied, "held by a scanner"))
            } else {
                fs::rename(from, to)
            }
        };
        assert!(switch_in(&root, &new, &hard_link, &scanner_holds_the_mirror).is_err());
        let app = root.join(paths::APP_DIR);
        assert_eq!(read(&app.join("guarana.exe")), "old");
        assert!(holds(&app, &old));
        assert!(leftovers(&root).is_empty());
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn a_failed_first_rename_changes_nothing() {
        let root = scratch("held");
        let old = install(&root, "20.3.0", "t1", &[("guarana.exe", "old")]);
        let new = install(&root, "20.4.0", "t2", &[("guarana.exe", "new")]);
        switch(&root, &old).unwrap();
        let app_in_use = |from: &Path, to: &Path| {
            if from.ends_with(paths::APP_DIR) {
                Err(io::Error::new(io::ErrorKind::PermissionDenied, "in use"))
            } else {
                fs::rename(from, to)
            }
        };
        assert!(switch_in(&root, &new, &hard_link, &app_in_use).is_err());
        assert!(holds(&root.join(paths::APP_DIR), &old));
        assert!(leftovers(&root).is_empty());
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn a_user_folder_in_app_moves_to_each_new_folder() {
        let root = scratch("user-moves");
        let old = install(&root, "20.3.0", "t1", &[("guarana.exe", "old")]);
        let new = install(&root, "20.4.0", "t2", &[("guarana.exe", "new")]);
        let app = root.join(paths::APP_DIR);
        switch(&root, &old).unwrap();
        write(&app.join("user").join("nand").join("save.bin"), "save");
        switch(&root, &new).unwrap();
        assert_eq!(read(&app.join("guarana.exe")), "new");
        assert_eq!(read(&app.join("user").join("nand").join("save.bin")), "save");
        assert!(leftovers(&root).is_empty());
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn a_portable_version_runs_from_its_own_folder() {
        let root = scratch("portable");
        let v = install(&root, "20.1.0", "t", &[("lichii.exe", "e"), ("user/config/qt-config.ini", "u")]);
        assert_eq!(switch(&root, &v).unwrap(), Outcome::Portable);
        assert!(!root.join(paths::APP_DIR).exists());
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn the_mirror_leaves_out_user_and_copies_where_links_fail() {
        let root = scratch("mirror");
        let src = root.join("src");
        write(&src.join("guarana.exe"), "e");
        write(&src.join("plugins").join("a.dll"), "a");
        write(&src.join("user").join("config").join("qt-config.ini"), "u");
        write(&src.join(MARKER), "{}");
        let no_links = |_: &Path, _: &Path| -> io::Result<()> { Err(io::Error::new(io::ErrorKind::Unsupported, "no hard links")) };
        let dst = root.join("dst");
        let mut m = Mirror::default();
        mirror_dir(&src, &dst, true, &no_links, &mut m).unwrap();
        assert_eq!(m, Mirror { linked: 0, copied: 2 });
        assert_eq!(read(&dst.join("guarana.exe")), "e");
        assert_eq!(read(&dst.join("plugins").join("a.dll")), "a");
        assert!(!dst.join("user").exists() && !dst.join(MARKER).exists());
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn a_switch_cut_short_between_its_renames_is_put_back_at_the_next_start() {
        let root = scratch("recover");
        let v = install(&root, "20.4.0", "t", &[("guarana.exe", "v")]);
        switch(&root, &v).unwrap();
        let app = root.join(paths::APP_DIR);
        // The crash: `app` went aside and the next mirror was half made.
        fs::rename(&app, root.join(format!("{RETIRED}20261116-120000-1"))).unwrap();
        write(&root.join(format!("{STAGING}20261116-120001-1")).join("guarana.exe"), "half");
        recover_in(&root, &plain_rename);
        assert!(holds(&app, &v));
        assert!(leftovers(&root).is_empty());
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn recovery_never_deletes_a_user_folder() {
        let root = scratch("retire");
        let v = install(&root, "20.4.0", "t", &[("guarana.exe", "v")]);
        switch(&root, &v).unwrap();
        let app = root.join(paths::APP_DIR);
        let first = root.join(format!("{RETIRED}20261115-000000-1"));
        write(&first.join("guarana.exe"), "old");
        write(&first.join("user").join("nand").join("a.bin"), "a");
        recover_in(&root, &plain_rename);
        assert_eq!(read(&app.join("user").join("nand").join("a.bin")), "a", "moved into app");
        assert!(!first.exists());
        // `app` has a user folder now, so another one stays where it is.
        let second = root.join(format!("{RETIRED}20261115-000000-2"));
        write(&second.join("user").join("nand").join("b.bin"), "b");
        let odd_mirror = root.join(format!("{STAGING}x"));
        write(&odd_mirror.join("user").join("c.bin"), "c");
        recover_in(&root, &plain_rename);
        assert_eq!(read(&second.join("user").join("nand").join("b.bin")), "b");
        assert_eq!(read(&odd_mirror.join("user").join("c.bin")), "c");
        assert_eq!(read(&app.join("user").join("nand").join("a.bin")), "a");
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn renames_are_tried_again_while_a_scanner_holds_a_file() {
        let denied = || io::Error::from(io::ErrorKind::PermissionDenied);
        let calls = Cell::new(0);
        let r = retry(10, Duration::ZERO, || {
            calls.set(calls.get() + 1);
            if calls.get() < 3 { Err(denied()) } else { Ok(()) }
        });
        assert!(r.is_ok());
        assert_eq!(calls.get(), 3);
        calls.set(0);
        assert!(retry(10, Duration::ZERO, || {
            calls.set(calls.get() + 1);
            Err(denied())
        })
        .is_err());
        assert_eq!(calls.get(), 10);
        calls.set(0);
        assert!(retry(10, Duration::ZERO, || {
            calls.set(calls.get() + 1);
            Err(io::Error::from(io::ErrorKind::NotFound))
        })
        .is_err());
        assert_eq!(calls.get(), 1, "a missing folder is not waited for");
    }
}
