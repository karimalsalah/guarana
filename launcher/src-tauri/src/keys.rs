//! `import_keys`: copies the keys file the user picked (or the `prod.keys` / `title.keys` in the
//! folder they picked) into `%APPDATA%\guarana\keys\`, after a format check. A file already there
//! is kept in `keys\guarana-backups\` first.

use std::fs;
use std::path::{Path, PathBuf};

use crate::locate::key_kind;
use crate::types::{ImportResult, KeyKind};
use crate::{paths, util};

fn fail(message: impl Into<String>) -> ImportResult {
    ImportResult { ok: false, dest: String::new(), message: message.into() }
}

fn same_file(a: &Path, b: &Path) -> bool {
    match (fs::canonicalize(a), fs::canonicalize(b)) {
        (Ok(x), Ok(y)) => x == y,
        _ => false,
    }
}

pub fn import(path: &str) -> ImportResult {
    let src = PathBuf::from(path.trim());
    let Ok(meta) = fs::metadata(&src) else {
        return fail(format!("{} is not there", src.display()));
    };
    let candidates: Vec<(PathBuf, KeyKind)> = if meta.is_dir() {
        ["prod.keys", "title.keys"]
            .iter()
            .map(|n| src.join(n))
            .filter_map(|p| {
                let m = fs::metadata(&p).ok()?;
                key_kind(&p, &m).map(|k| (p, k))
            })
            .collect()
    } else {
        match key_kind(&src, &meta) {
            Some(k) => vec![(src.clone(), k)],
            None => return fail("This is not a keys file: it should hold lines of name = hex"),
        }
    };
    if candidates.is_empty() {
        return fail("No prod.keys or title.keys in this folder");
    }
    let dest_dir = paths::keys_dir();
    if let Err(e) = fs::create_dir_all(&dest_dir) {
        return fail(format!("Could not create {}: {e}", dest_dir.display()));
    }
    let mut done = Vec::new();
    let mut last_dest = dest_dir.clone();
    for (file, kind) in candidates {
        let name = match kind {
            KeyKind::Prod => "prod.keys".to_string(),
            KeyKind::Title => "title.keys".to_string(),
            KeyKind::Unknown => file.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_else(|| "imported.keys".into()),
        };
        let dest = dest_dir.join(&name);
        last_dest = dest.clone();
        if same_file(&file, &dest) {
            done.push(format!("{name} is already in place"));
            continue;
        }
        let bytes = match fs::read(&file) {
            Ok(b) => b,
            Err(e) => return fail(format!("Could not read {}: {e}", file.display())),
        };
        let backup = match util::backup_file(&dest) {
            Ok(b) => b,
            Err(e) => return fail(format!("Could not back up the existing {name}: {e}")),
        };
        if let Err(e) = util::write_atomic(&dest, &bytes) {
            return fail(format!("Could not write {}: {e}", dest.display()));
        }
        log::info!("Imported {name} into {}", dest_dir.display());
        done.push(match backup {
            Some(_) => format!("Imported {name} (the previous one is in keys\\guarana-backups)"),
            None => format!("Imported {name}"),
        });
    }
    let dest = if done.len() > 1 { dest_dir } else { last_dest };
    ImportResult { ok: true, dest: dest.to_string_lossy().into_owned(), message: done.join("; ") }
}
