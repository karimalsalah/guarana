use std::{env, fs, path::PathBuf};

/// Embeds a copy of the website's hall of fame (`site/data/legacy.json`) as the credits screen's
/// fallback. Built outside the repository (no `site/` next to the sources), it embeds an empty
/// document, and the screen then waits for the network copy.
fn main() {
    let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR"));
    let source = manifest.join("..").join("..").join("site").join("data").join("legacy.json");
    println!("cargo:rerun-if-changed={}", source.display());
    println!("cargo:rerun-if-changed=build.rs");
    let text = fs::read_to_string(&source)
        .ok()
        .filter(|t| looks_like_object(t))
        .unwrap_or_else(|| r#"{"schema_version":1,"people":[],"tools":[]}"#.to_string());
    let out = PathBuf::from(env::var("OUT_DIR").expect("OUT_DIR")).join("legacy_builtin.json");
    fs::write(out, text).expect("write legacy_builtin.json");
    tauri_build::build()
}

/// A cheap sanity check; `credits::parse` does the real one in the tests and at run time.
fn looks_like_object(text: &str) -> bool {
    let t = text.trim();
    t.starts_with('{') && t.ends_with('}')
}
