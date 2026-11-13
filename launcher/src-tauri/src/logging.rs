//! `%LOCALAPPDATA%\Guarana\launcher.log`: the launcher's own lines at Info and above, and
//! warnings from its libraries. Rotated to `launcher.log.old` past 4 MiB.

use std::fs::{self, File, OpenOptions};
use std::io::Write;
use std::sync::Mutex;

use log::{Level, LevelFilter, Metadata, Record};

use crate::{paths, util};

const MAX_BYTES: u64 = 4 << 20;

struct FileLogger {
    file: Mutex<Option<File>>,
}

impl log::Log for FileLogger {
    fn enabled(&self, metadata: &Metadata) -> bool {
        let ours = metadata.target().starts_with("guarana_launcher");
        (ours && metadata.level() <= Level::Info) || metadata.level() <= Level::Warn
    }

    fn log(&self, record: &Record) {
        if !self.enabled(record.metadata()) {
            return;
        }
        let line = format!(
            "{} {:<5} {}: {}\r\n",
            util::now_rfc3339(),
            record.level(),
            record.target(),
            record.args()
        );
        if let Ok(mut guard) = self.file.lock() {
            if let Some(file) = guard.as_mut() {
                let _ = file.write_all(line.as_bytes());
            }
        }
        #[cfg(debug_assertions)]
        eprint!("{line}");
    }

    fn flush(&self) {
        if let Ok(mut guard) = self.file.lock() {
            if let Some(file) = guard.as_mut() {
                let _ = file.flush();
            }
        }
    }
}

pub fn init() {
    let path = paths::launcher_log();
    if let Some(dir) = path.parent() {
        let _ = fs::create_dir_all(dir);
    }
    if fs::metadata(&path).map(|m| m.len() > MAX_BYTES).unwrap_or(false) {
        let mut old = path.clone().into_os_string();
        old.push(".old");
        let _ = fs::rename(&path, old);
    }
    let file = OpenOptions::new().create(true).append(true).open(&path).ok();
    let logger = FileLogger { file: Mutex::new(file) };
    if log::set_boxed_logger(Box::new(logger)).is_ok() {
        log::set_max_level(LevelFilter::Info);
    }
}
