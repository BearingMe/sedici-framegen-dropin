//! Replaces the C++ `sedici_layers` check: the hexagon's rules, read straight from the sources.
//! - `domain` imports nothing but std and thiserror; no OS types, no other layer.
//! - `application` imports `domain` only (plus tracing/thiserror); never `adapters` or `windows`.
//! - `unsafe` appears only under `src/adapters`.
use rstest::rstest;
use std::fs;
use std::path::{Path, PathBuf};

fn rust_files(dir: &Path, out: &mut Vec<PathBuf>) {
    for entry in fs::read_dir(dir).expect("read dir").flatten() {
        let path = entry.path();
        if path.is_dir() {
            rust_files(&path, out);
        } else if path.extension().is_some_and(|e| e == "rs") {
            out.push(path);
        }
    }
}

fn sources(layer: &str) -> Vec<(PathBuf, String)> {
    let root = Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("src")
        .join(layer);
    let mut files = Vec::new();
    rust_files(&root, &mut files);
    files
        .into_iter()
        .map(|p| (p.clone(), fs::read_to_string(&p).expect("read")))
        .collect()
}

/// Code lines only: comments and the `#[cfg(test)]` tail are not part of the production layer.
fn production_lines(text: &str) -> Vec<&str> {
    let body = text.split("#[cfg(test)]").next().unwrap_or(text);
    body.lines()
        .map(str::trim)
        .filter(|l| !l.starts_with("//"))
        .collect()
}

fn violations(layer: &str, forbidden: &[&str]) -> Vec<String> {
    let mut found = Vec::new();
    for (path, text) in sources(layer) {
        for line in production_lines(&text) {
            if forbidden.iter().any(|f| line.contains(f)) {
                found.push(format!("{}: {line}", path.display()));
            }
        }
    }
    found
}

#[rstest]
#[case("domain", &["windows::", "crate::application", "crate::adapters", "crate::entry", "std::fs", "std::time::Instant", "tracing::"])]
#[case("application", &["windows::", "crate::adapters", "crate::entry"])]
fn layer_imports(#[case] layer: &str, #[case] forbidden: &[&str]) {
    let found = violations(layer, forbidden);
    assert!(
        found.is_empty(),
        "{layer} breaks layering:\n{}",
        found.join("\n")
    );
}

#[rstest]
#[case("domain")]
#[case("application")]
fn no_unsafe_outside_adapters(#[case] layer: &str) {
    let found = violations(
        layer,
        &["unsafe {", "unsafe fn", "unsafe impl", "extern \"system\""],
    );
    assert!(found.is_empty(), "unsafe in {layer}:\n{}", found.join("\n"));
}

#[test]
fn crate_root_denies_unsafe() {
    let lib = fs::read_to_string(Path::new(env!("CARGO_MANIFEST_DIR")).join("src/lib.rs"))
        .expect("lib.rs");
    assert!(lib.contains("#![deny(unsafe_code)]"));
}
