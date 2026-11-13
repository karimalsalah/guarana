//! Community best settings: `best-settings.json` from the project's site (made by
//! `tools/ingest/best_settings.py` from the validated device reports). Cached in
//! `%LOCALAPPDATA%\Guarana\cache` with its ETag for 24 hours, schema-checked on every read, and
//! replaced by the built-in table (no community data) when offline and nothing is cached.
//! Only the three settings in `APPLIED` ever reach a profile, and only from a class with at least
//! `MIN_VALIDATED` validated reports.

use std::collections::BTreeMap;
use std::fs;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use serde::{Deserialize, Serialize};

use crate::types::{Form, SystemInfo, ToggleValue, Vendor};
use crate::util::{self, CmdResult, Context};
use crate::{paths, profiles};

pub const URL: &str = "https://karimalsalah.github.io/guarana/data/best-settings.json";
pub const TTL_SECS: i64 = 24 * 3600;
pub const MIN_VALIDATED: u32 = 3;
const MAX_BYTES: usize = 256 << 10;
const BUILTIN: &str = include_str!("best_settings_builtin.json");
/// Settings a community class may switch on or off in Auto. Frame generation and the power policy
/// are reported but never applied from data.
pub const APPLIED: [&str; 3] = ["fluid_presenter", "ocarina_extrapolation", "gm_routing"];

pub const CLASS_IDS: [&str; 6] =
    ["intel-arc-igpu", "amd-rdna3-igpu", "nvidia-rtx-desktop", "nvidia-rtx-laptop", "amd-desktop", "other"];

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct ClassEntry {
    pub label: String,
    pub samples: u32,
    pub validated: u32,
    pub settings: BTreeMap<String, ToggleValue>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum Source {
    Builtin,
    Cache,
    Network,
}

#[derive(Debug, Clone, Serialize)]
pub struct BestSettings {
    pub updated: Option<String>,
    pub classes: BTreeMap<String, ClassEntry>,
    pub source: Source,
}

#[derive(Deserialize)]
struct RawDoc {
    schema_version: Option<i64>,
    updated: Option<String>,
    classes: Option<BTreeMap<String, serde_json::Value>>,
}

/// Checks a document and keeps what is valid. Unknown classes and settings are dropped (a newer
/// producer may add some); a wrong schema version or no valid class rejects the file.
pub fn parse(bytes: &[u8], source: Source) -> Result<BestSettings, String> {
    if bytes.len() > MAX_BYTES {
        return Err("best-settings.json is too large".into());
    }
    let raw: RawDoc = serde_json::from_slice(bytes).map_err(|e| format!("not JSON: {e}"))?;
    if raw.schema_version != Some(1) {
        return Err("unsupported schema_version".into());
    }
    let mut classes = BTreeMap::new();
    for (id, value) in raw.classes.ok_or("no classes")? {
        if !CLASS_IDS.contains(&id.as_str()) {
            continue;
        }
        let count = |k: &str| value.get(k).and_then(|v| v.as_u64()).filter(|n| *n <= 1_000_000).map(|n| n as u32);
        let (Some(samples), Some(validated)) = (count("samples"), count("validated")) else { continue };
        if validated > samples {
            continue;
        }
        let label = value.get("label").and_then(|v| v.as_str()).unwrap_or(&id).chars().take(60).collect();
        let mut settings = BTreeMap::new();
        if let Some(obj) = value.get("settings").and_then(|v| v.as_object()) {
            for (k, v) in obj {
                let tv = match v {
                    serde_json::Value::Bool(b) => ToggleValue::Bool(*b),
                    serde_json::Value::String(s) => ToggleValue::Str(s.clone()),
                    serde_json::Value::Number(n) if n.is_i64() => ToggleValue::Int(n.as_i64().unwrap_or(0)),
                    _ => continue,
                };
                if let Ok(ok) = profiles::validate_setting(k, &tv) {
                    settings.insert(k.clone(), ok);
                }
            }
        }
        classes.insert(id, ClassEntry { label, samples, validated, settings });
    }
    if classes.is_empty() {
        return Err("no valid class".into());
    }
    let updated = raw.updated.map(|u| u.chars().take(32).collect());
    Ok(BestSettings { updated, classes, source })
}

pub fn builtin() -> BestSettings {
    parse(BUILTIN.as_bytes(), Source::Builtin).expect("the built-in best settings are valid")
}

/// The class of the primary GPU.
pub fn gpu_class(sys: &SystemInfo) -> &'static str {
    let Some(g) = sys.primary() else { return "other" };
    let n = g.name.to_ascii_lowercase();
    let laptop = matches!(sys.device.form, Form::Laptop | Form::Handheld);
    match (g.vendor, g.is_integrated) {
        (Vendor::Intel, true) if n.contains("arc") => "intel-arc-igpu",
        (Vendor::Amd, true) if ["780m", "760m", "880m", "890m", "z1", "z2"].iter().any(|t| n.contains(t)) => {
            "amd-rdna3-igpu"
        }
        (Vendor::Nvidia, false) if laptop => "nvidia-rtx-laptop",
        (Vendor::Nvidia, false) => "nvidia-rtx-desktop",
        (Vendor::Amd, false) => "amd-desktop",
        _ => "other",
    }
}

/// The class entry to use for a machine, only when it has enough validated reports.
pub fn usable<'a>(best: &'a BestSettings, class: &str) -> Option<&'a ClassEntry> {
    best.classes.get(class).filter(|c| c.validated >= MIN_VALIDATED && !c.settings.is_empty())
}

fn now_secs() -> i64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs() as i64).unwrap_or(0)
}

#[derive(Serialize, Deserialize, Default)]
struct Meta {
    etag: Option<String>,
    fetched_at: i64,
}

pub fn is_fresh(fetched_at: i64, now: i64) -> bool {
    fetched_at > 0 && now >= fetched_at && now - fetched_at < TTL_SECS
}

fn cache_file() -> std::path::PathBuf {
    paths::cache_dir().join("best-settings.json")
}

fn meta_file() -> std::path::PathBuf {
    paths::cache_dir().join("best-settings.meta.json")
}

fn read_meta() -> Meta {
    fs::read(meta_file()).ok().and_then(|b| serde_json::from_slice(&b).ok()).unwrap_or_default()
}

fn read_cache() -> Option<BestSettings> {
    parse(&fs::read(cache_file()).ok()?, Source::Cache).ok()
}

/// Cached data or the built-in table. Never touches the network.
pub fn cached_or_builtin() -> BestSettings {
    read_cache().unwrap_or_else(builtin)
}

/// Fetches when the cache is older than 24 h (conditional on the ETag), then returns what is best
/// available. `force` ignores the age. Every failure falls back to the cache or the built-in table.
pub async fn load(force: bool) -> BestSettings {
    let meta = read_meta();
    let cached = read_cache();
    if let Some(c) = &cached {
        if !force && is_fresh(meta.fetched_at, now_secs()) {
            return c.clone();
        }
    }
    match fetch(&meta, cached.is_some()).await {
        Ok(Some((best, etag, bytes))) => {
            let _ = fs::create_dir_all(paths::cache_dir());
            let _ = util::write_atomic(&cache_file(), &bytes);
            write_meta(etag, now_secs());
            best
        }
        Ok(None) => {
            write_meta(meta.etag.clone(), now_secs());
            cached.unwrap_or_else(builtin)
        }
        Err(e) => {
            log::info!("Community best settings not refreshed: {e}");
            cached.unwrap_or_else(builtin)
        }
    }
}

fn write_meta(etag: Option<String>, fetched_at: i64) {
    if let Ok(bytes) = serde_json::to_vec(&Meta { etag, fetched_at }) {
        let _ = fs::create_dir_all(paths::cache_dir());
        let _ = util::write_atomic(&meta_file(), &bytes);
    }
}

type Fetched = Option<(BestSettings, Option<String>, Vec<u8>)>;

async fn fetch(meta: &Meta, have_cache: bool) -> CmdResult<Fetched> {
    let client = crate::install::client()?;
    let mut req = client.get(URL).timeout(Duration::from_secs(10));
    if have_cache {
        if let Some(etag) = &meta.etag {
            req = req.header("If-None-Match", etag);
        }
    }
    let mut resp = req.send().await.ctx("The community data is not reachable")?;
    if resp.status().as_u16() == 304 {
        return Ok(None);
    }
    if !resp.status().is_success() {
        return Err(format!("HTTP {}", resp.status()));
    }
    let etag = resp.headers().get("etag").and_then(|v| v.to_str().ok()).map(|s| s.to_string());
    let mut bytes = Vec::new();
    while let Some(chunk) = resp.chunk().await.ctx("The community data connection dropped")? {
        bytes.extend_from_slice(&chunk);
        if bytes.len() > MAX_BYTES {
            return Err("too large".into());
        }
    }
    let best = parse(&bytes, Source::Network)?;
    Ok(Some((best, etag, bytes)))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn doc() -> String {
        r#"{"schema_version":1,"updated":"2026-11-20","classes":{"intel-arc-igpu":{"label":"Intel Arc iGPU","samples":9,"validated":5,"settings":{"fluid_presenter":true,"gm_routing":"on","resolution":"1.5x","bogus":true,"fps_cap":"99"}}}}"#.to_string()
    }

    #[test]
    fn builtin_is_valid() {
        let b = builtin();
        assert_eq!(b.source, Source::Builtin);
        assert_eq!(b.classes.len(), CLASS_IDS.len());
        assert!(usable(&b, "intel-arc-igpu").is_none());
    }

    #[test]
    fn keeps_valid_settings_and_drops_the_rest() {
        let b = parse(doc().as_bytes(), Source::Network).unwrap();
        let c = &b.classes["intel-arc-igpu"];
        assert_eq!((c.samples, c.validated), (9, 5));
        assert_eq!(c.settings.get("fluid_presenter"), Some(&ToggleValue::Bool(true)));
        assert_eq!(c.settings.get("gm_routing"), Some(&ToggleValue::Bool(true)));
        assert_eq!(c.settings.get("resolution"), Some(&ToggleValue::Str("1.5x".into())));
        assert!(!c.settings.contains_key("bogus"));
        assert!(!c.settings.contains_key("fps_cap"));
        assert!(usable(&b, "intel-arc-igpu").is_some());
        assert!(usable(&b, "other").is_none());
    }

    #[test]
    fn rejects_bad_documents() {
        assert!(parse(b"{}", Source::Network).is_err());
        assert!(parse(br#"{"schema_version":2,"classes":{}}"#, Source::Network).is_err());
        assert!(parse(b"nope", Source::Network).is_err());
        assert!(parse(br#"{"schema_version":1,"classes":{"mars":{"samples":1,"validated":1}}}"#, Source::Network).is_err());
        let bad = br#"{"schema_version":1,"classes":{"other":{"samples":1,"validated":4,"settings":{}}}}"#;
        assert!(parse(bad, Source::Network).is_err());
        assert!(parse(&vec![b' '; MAX_BYTES + 1], Source::Network).is_err());
    }

    #[test]
    fn a_class_with_too_few_validated_reports_is_not_used() {
        let few = doc().replace("\"validated\":5", "\"validated\":2");
        let b = parse(few.as_bytes(), Source::Cache).unwrap();
        assert!(usable(&b, "intel-arc-igpu").is_none());
    }

    #[test]
    fn freshness_is_24_hours() {
        assert!(is_fresh(1000, 1000 + TTL_SECS - 1));
        assert!(!is_fresh(1000, 1000 + TTL_SECS));
        assert!(!is_fresh(0, 5));
        assert!(!is_fresh(2000, 1000));
    }
}
