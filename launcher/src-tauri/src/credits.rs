//! Credits: the website's hall of fame (`legacy.json`, made by `tools/ci/legacy.py`) for the About
//! screen. Cached in `%LOCALAPPDATA%\Guarana\cache` with its ETag for 24 hours, schema-checked on
//! every read, and replaced by the copy embedded at build time (`build.rs`, from
//! `site/data/legacy.json`) when offline and nothing is cached. People and tools are kept apart.

use std::fs;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use serde::{Deserialize, Serialize};

use crate::community::{self, Source};
use crate::paths;
use crate::util::{self, CmdResult, Context};

pub const URL: &str = "https://karimalsalah.github.io/guarana/data/legacy.json";
const MAX_BYTES: usize = 512 << 10;
const MAX_ENTRIES: usize = 5000;
const BUILTIN: &str = include_str!(concat!(env!("OUT_DIR"), "/legacy_builtin.json"));

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct Person {
    /// GitHub login; None for someone only known by the name in a commit trailer.
    pub login: Option<String>,
    pub name: Option<String>,
    pub commits: u32,
    pub coauthored: u32,
}

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct Tool {
    pub name: String,
    pub commits: u32,
}

#[derive(Debug, Clone, Serialize)]
pub struct Credits {
    pub as_of: Option<String>,
    pub commits: u32,
    pub people: Vec<Person>,
    pub tools: Vec<Tool>,
    pub source: Source,
}

#[derive(Deserialize)]
struct RawDoc {
    schema_version: Option<i64>,
    as_of: Option<String>,
    commits: Option<u64>,
    people: Option<Vec<serde_json::Value>>,
    tools: Option<Vec<serde_json::Value>>,
}

fn valid_login(s: &str) -> bool {
    let b = s.as_bytes();
    !b.is_empty() && b.len() <= 39 && b[0].is_ascii_alphanumeric() && b.iter().all(|c| c.is_ascii_alphanumeric() || *c == b'-')
}

/// Plain text only: control characters and angle brackets are dropped, length is capped.
fn clean(s: &str, max: usize) -> String {
    s.chars().filter(|c| !c.is_control() && *c != '<' && *c != '>').take(max).collect::<String>().trim().to_string()
}

fn count(v: &serde_json::Value, key: &str) -> u32 {
    v.get(key).and_then(|n| n.as_u64()).filter(|n| *n <= 10_000_000).unwrap_or(0) as u32
}

/// Checks a document and keeps what is valid: a wrong schema version rejects the file; an entry
/// with neither a valid login nor a name is dropped.
pub fn parse(bytes: &[u8], source: Source) -> Result<Credits, String> {
    if bytes.len() > MAX_BYTES {
        return Err("legacy.json is too large".into());
    }
    let raw: RawDoc = serde_json::from_slice(bytes).map_err(|e| format!("not JSON: {e}"))?;
    if raw.schema_version != Some(1) {
        return Err("unsupported schema_version".into());
    }
    let mut people = Vec::new();
    for v in raw.people.ok_or("no people")?.iter().take(MAX_ENTRIES) {
        let login = v.get("login").and_then(|x| x.as_str()).filter(|s| valid_login(s)).map(str::to_string);
        let name = v.get("name").and_then(|x| x.as_str()).map(|s| clean(s, 80)).filter(|s| !s.is_empty());
        if login.is_none() && name.is_none() {
            continue;
        }
        people.push(Person { login, name, commits: count(v, "commits"), coauthored: count(v, "coauthored") });
    }
    let mut tools = Vec::new();
    for v in raw.tools.unwrap_or_default().iter().take(MAX_ENTRIES) {
        let Some(name) = v.get("name").and_then(|x| x.as_str()).map(|s| clean(s, 60)).filter(|s| !s.is_empty()) else {
            continue;
        };
        tools.push(Tool { name, commits: count(v, "commits") });
    }
    let as_of = raw.as_of.map(|a| clean(&a, 32)).filter(|a| !a.is_empty());
    Ok(Credits { as_of, commits: raw.commits.unwrap_or(0).min(10_000_000) as u32, people, tools, source })
}

/// The copy embedded at build time (empty when built outside the repository).
pub fn builtin() -> Credits {
    parse(BUILTIN.as_bytes(), Source::Builtin)
        .unwrap_or(Credits { as_of: None, commits: 0, people: Vec::new(), tools: Vec::new(), source: Source::Builtin })
}

fn now_secs() -> i64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs() as i64).unwrap_or(0)
}

#[derive(Serialize, Deserialize, Default)]
struct Meta {
    etag: Option<String>,
    fetched_at: i64,
}

fn cache_file() -> std::path::PathBuf {
    paths::cache_dir().join("legacy.json")
}

fn meta_file() -> std::path::PathBuf {
    paths::cache_dir().join("legacy.meta.json")
}

fn read_meta() -> Meta {
    fs::read(meta_file()).ok().and_then(|b| serde_json::from_slice(&b).ok()).unwrap_or_default()
}

fn read_cache() -> Option<Credits> {
    parse(&fs::read(cache_file()).ok()?, Source::Cache).ok()
}

fn write_meta(etag: Option<String>, fetched_at: i64) {
    if let Ok(bytes) = serde_json::to_vec(&Meta { etag, fetched_at }) {
        let _ = fs::create_dir_all(paths::cache_dir());
        let _ = util::write_atomic(&meta_file(), &bytes);
    }
}

/// Fetches when the cache is older than 24 h (conditional on the ETag), then returns the best
/// available: network, cache, embedded copy. Every failure falls back quietly.
pub async fn load(force: bool) -> Credits {
    let meta = read_meta();
    let cached = read_cache();
    if let Some(c) = &cached {
        if !force && community::is_fresh(meta.fetched_at, now_secs()) {
            return c.clone();
        }
    }
    match fetch(&meta, cached.is_some()).await {
        Ok(Some((credits, etag, bytes))) => {
            let _ = fs::create_dir_all(paths::cache_dir());
            let _ = util::write_atomic(&cache_file(), &bytes);
            write_meta(etag, now_secs());
            credits
        }
        Ok(None) => {
            write_meta(meta.etag.clone(), now_secs());
            cached.unwrap_or_else(builtin)
        }
        Err(e) => {
            log::info!("Credits not refreshed: {e}");
            cached.unwrap_or_else(builtin)
        }
    }
}

type Fetched = Option<(Credits, Option<String>, Vec<u8>)>;

async fn fetch(meta: &Meta, have_cache: bool) -> CmdResult<Fetched> {
    let client = crate::install::client()?;
    let mut req = client.get(URL).timeout(Duration::from_secs(10));
    if have_cache {
        if let Some(etag) = &meta.etag {
            req = req.header("If-None-Match", etag);
        }
    }
    let mut resp = req.send().await.ctx("The credits are not reachable")?;
    if resp.status().as_u16() == 304 {
        return Ok(None);
    }
    if !resp.status().is_success() {
        return Err(format!("HTTP {}", resp.status()));
    }
    let etag = resp.headers().get("etag").and_then(|v| v.to_str().ok()).map(|s| s.to_string());
    let mut bytes = Vec::new();
    while let Some(chunk) = resp.chunk().await.ctx("The credits connection dropped")? {
        bytes.extend_from_slice(&chunk);
        if bytes.len() > MAX_BYTES {
            return Err("too large".into());
        }
    }
    let credits = parse(&bytes, Source::Network)?;
    Ok(Some((credits, etag, bytes)))
}

#[cfg(test)]
mod tests {
    use super::*;

    const DOC: &str = r#"{"schema_version":1,"as_of":"2026-11-20","commits":9,
      "people":[{"login":"alice","name":null,"commits":3,"coauthored":1},
                {"login":null,"name":"Bob <b@x> Builder","commits":0,"coauthored":2},
                {"login":"bad login!","name":null,"commits":1,"coauthored":0},
                {"login":null,"name":"","commits":1,"coauthored":0}],
      "tools":[{"name":"Claude","commits":5},{"name":"","commits":1}]}"#;

    #[test]
    fn embedded_copy_is_valid() {
        let c = builtin();
        assert_eq!(c.source, Source::Builtin);
        assert!(c.people.iter().all(|p| p.login.is_some() || p.name.is_some()));
    }

    #[test]
    fn keeps_valid_entries_and_cleans_names() {
        let c = parse(DOC.as_bytes(), Source::Network).unwrap();
        assert_eq!(c.people.len(), 2);
        assert_eq!(c.people[0].login.as_deref(), Some("alice"));
        assert_eq!(c.people[1].name.as_deref(), Some("Bob b@x Builder"));
        assert_eq!(c.tools, vec![Tool { name: "Claude".into(), commits: 5 }]);
        assert_eq!(c.as_of.as_deref(), Some("2026-11-20"));
    }

    #[test]
    fn rejects_wrong_schema_and_junk() {
        assert!(parse(br#"{"schema_version":2,"people":[]}"#, Source::Network).is_err());
        assert!(parse(b"nope", Source::Network).is_err());
        assert!(parse(br#"{"schema_version":1}"#, Source::Network).is_err());
    }

    #[test]
    fn login_rules() {
        assert!(valid_login("a-b1"));
        assert!(!valid_login("-ab"));
        assert!(!valid_login("a b"));
        assert!(!valid_login(&"a".repeat(40)));
    }
}
