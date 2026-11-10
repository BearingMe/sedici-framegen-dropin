//! `sedici.toml` parsing, comment-preserving patching and the file-backed [`ConfigStore`]
//! (ports `settings/config.cpp` and `settings/config_patch.cpp`).
//!
//! Nothing here can fail a game call. A missing file, a syntax error, a key with the wrong type or
//! an unknown value is reported through `tracing` with its line and column, and the affected key
//! falls back to its built-in default; the rest of the file still applies.
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};

use toml_edit::{DocumentMut, Item, Table, Value};

use crate::application::ports::config_store::{
    same_stamp, ConfigStore, ConfigValues, FileStamp, SaveOutcome,
};
use crate::domain::config::{
    apply_env_overrides, finalize_target, parse_target, FrameGenerationSettings, LoadedSettings,
    LogSettings, Multiplier, Present, Settings, TearGuard, UiMode, UiScaleMode, UiSettings,
};

// ---------------------------------------------------------------------------
// Parsing: typed key access
// ---------------------------------------------------------------------------

enum Read<T> {
    Missing,
    Ok(T),
    WrongType,
}

fn section_key<'a>(root: &'a Table, section: &str, key: &str) -> Option<&'a Item> {
    root.get(section)
        .and_then(Item::as_table)
        .and_then(|table| table.get(key))
}

fn type_name(item: &Item) -> &'static str {
    match item {
        Item::None => "nothing",
        Item::Table(_) | Item::Value(Value::InlineTable(_)) => "a table",
        Item::ArrayOfTables(_) | Item::Value(Value::Array(_)) => "an array",
        Item::Value(Value::String(_)) => "a string",
        Item::Value(Value::Integer(_)) => "an integer",
        Item::Value(Value::Float(_)) => "a float",
        Item::Value(Value::Boolean(_)) => "a boolean",
        Item::Value(Value::Datetime(date_time)) => {
            match (
                date_time.value().date.is_some(),
                date_time.value().time.is_some(),
            ) {
                (true, true) => "a date-time",
                (true, false) => "a date",
                _ => "a time",
            }
        }
    }
}

fn read_string(item: Option<&Item>) -> Read<String> {
    match item {
        None => Read::Missing,
        Some(item) => match item.as_value().and_then(Value::as_str) {
            Some(text) => Read::Ok(text.to_owned()),
            None => Read::WrongType,
        },
    }
}

/// Accepts both TOML floats and integers, so `target_fps = 1` is valid as well as `1.0`.
fn read_number(item: Option<&Item>) -> Read<f64> {
    match item {
        None => Read::Missing,
        Some(item) => {
            let value = item.as_value();
            match (
                value.and_then(Value::as_float),
                value.and_then(Value::as_integer),
            ) {
                (Some(number), _) => Read::Ok(number),
                (None, Some(number)) => Read::Ok(number as f64),
                (None, None) => Read::WrongType,
            }
        }
    }
}

fn read_integer(item: Option<&Item>) -> Read<i64> {
    match item {
        None => Read::Missing,
        Some(item) => match item.as_value().and_then(Value::as_integer) {
            Some(number) => Read::Ok(number),
            None => Read::WrongType,
        },
    }
}

/// 1-based line and column of a byte offset.
fn line_col(text: &str, offset: usize) -> (usize, usize) {
    let offset = offset.min(text.len());
    let before = &text[..offset];
    let line = before.bytes().filter(|&b| b == b'\n').count() + 1;
    let column = before
        .rsplit('\n')
        .next()
        .map_or(0, |rest| rest.chars().count())
        + 1;
    (line, column)
}

fn position_of(text: &str, item: &Item) -> Option<(usize, usize)> {
    item.span().map(|span| line_col(text, span.start))
}

fn problem_at(
    text: &str,
    problems: &mut Vec<String>,
    item: &Item,
    key: &str,
    message: impl std::fmt::Display,
) {
    let mut line = String::from("sedici.toml");
    if let Some((row, column)) = position_of(text, item) {
        line.push_str(&format!(":{row}:{column}"));
    }
    line.push_str(&format!(": {key}: {message}"));
    problems.push(line);
}

fn check_section(text: &str, root: &Table, name: &str, problems: &mut Vec<String>) {
    let Some(section) = root.get(name) else {
        return;
    };
    if section.is_table() {
        return;
    }
    problem_at(
        text,
        problems,
        section,
        name,
        format!(
            "expected a table, got {} - ignoring the whole [{name}] section",
            type_name(section)
        ),
    );
}

// ---------------------------------------------------------------------------
// Loading sedici.toml
// ---------------------------------------------------------------------------

/// Sections and keys from the era when Sedici forwarded to AMD's frame generation. They no longer
/// do anything; an old `sedici.toml` still loads, and one note says why.
fn note_removed_keys(root: &Table, notes: &mut Vec<String>) {
    let mut found: Vec<&str> = Vec::new();
    if root.contains_key("pacing") {
        found.push("[pacing]");
    }
    if root.contains_key("async") {
        found.push("[async]");
    }
    if section_key(root, "fg", "mode").is_some() {
        found.push("fg.mode");
    }
    if !found.is_empty() {
        notes.push(format!(
            "sedici.toml: {} no longer exist (Sedici is the only frame generation backend) - ignored",
            found.join(", ")
        ));
    }
}

/// A key or section Sedici does not read is reported with its line, so a typo is not silently
/// ignored.
fn note_unknown_keys(text: &str, root: &Table, problems: &mut Vec<String>) {
    const KNOWN: [(&str, &[&str]); 4] = [
        (
            "fg",
            &["present", "multiplier", "target_fps", "tear_guard", "mode"],
        ),
        ("limiter", &["fps", "spin_us"]),
        ("log", &["level"]),
        ("ui", &["hotkey", "mode", "scale"]),
    ];

    for (name, item) in root.iter() {
        if name == "pacing" || name == "async" {
            continue; // noted as removed
        }
        let Some((_, keys)) = KNOWN.iter().find(|(section, _)| *section == name) else {
            let position = item
                .span()
                .map(|span| format!("sedici.toml:{}: ", line_col(text, span.start).0))
                .unwrap_or_else(|| "sedici.toml: ".to_owned());
            let is_table = item.is_table();
            problems.push(format!(
                "{position}unknown {}{name}{} - ignored (expected [fg], [limiter], [log] or [ui])",
                if is_table { "section [" } else { "key " },
                if is_table { "]" } else { "" }
            ));
            continue;
        };
        let Some(table) = item.as_table() else {
            continue;
        };
        for (key, value) in table.iter() {
            if !keys.contains(&key) {
                let position = value
                    .span()
                    .map(|span| format!("sedici.toml:{}: ", line_col(text, span.start).0))
                    .unwrap_or_else(|| "sedici.toml: ".to_owned());
                problems.push(format!("{position}unknown key {name}.{key} - ignored"));
            }
        }
    }
}

fn read_fg(text: &str, root: &Table, fg: &mut FrameGenerationSettings, problems: &mut Vec<String>) {
    check_section(text, root, "fg", problems);

    let entry = section_key(root, "fg", "present");
    match read_string(entry) {
        Read::Missing => {}
        Read::WrongType => problem_at(
            text,
            problems,
            entry.expect("wrong type has a node"),
            "fg.present",
            format!(
                "expected a string, got {} - using default \"vrr\"",
                type_name(entry.expect("wrong type has a node"))
            ),
        ),
        Read::Ok(value) => match Present::parse(&value) {
            Some(parsed) => {
                fg.present = parsed;
                fg.present_source = crate::domain::config::Source::File;
            }
            None => problem_at(
                text,
                problems,
                entry.expect("ok has a node"),
                "fg.present",
                format!(
                    "unknown value \"{value}\" (expected \"vsync\", \"vrr\" or \"game\") - using \
                     default \"vrr\""
                ),
            ),
        },
    }

    let entry = section_key(root, "fg", "multiplier");
    match read_string(entry) {
        Read::Missing => {}
        Read::WrongType => problem_at(
            text,
            problems,
            entry.expect("wrong type has a node"),
            "fg.multiplier",
            format!(
                "expected a string, got {} - using default \"2x\"",
                type_name(entry.expect("wrong type has a node"))
            ),
        ),
        Read::Ok(value) => match Multiplier::parse(&value) {
            Some(parsed) => {
                fg.multiplier = parsed;
                fg.multiplier_source = crate::domain::config::Source::File;
            }
            None => problem_at(
                text,
                problems,
                entry.expect("ok has a node"),
                "fg.multiplier",
                format!(
                    "unknown value \"{value}\" (expected \"2x\", \"1.75x\", \"1.5x\", \"1.25x\" or \
                     \"adaptive\") - using default \"2x\""
                ),
            ),
        },
    }

    if let Some(entry) = section_key(root, "fg", "target_fps") {
        if let Read::Ok(value) = read_string(Some(entry)) {
            match parse_target(&value) {
                Some((number, automatic)) => {
                    fg.target_fps = number;
                    fg.target_auto = automatic;
                    fg.target_fps_source = crate::domain::config::Source::File;
                }
                None => problem_at(
                    text,
                    problems,
                    entry,
                    "fg.target_fps",
                    format!(
                        "unknown value \"{value}\" (expected a number or \"auto\") - using default \
                         0 (uncapped)"
                    ),
                ),
            }
        } else if let Read::Ok(number) = read_number(Some(entry)) {
            if !number.is_finite() || !(0.0..=1000.0).contains(&number) {
                problem_at(
                    text,
                    problems,
                    entry,
                    "fg.target_fps",
                    "must be within [0, 1000] (0 = uncapped) - using default 0",
                );
            } else {
                fg.target_fps = number;
                fg.target_auto = false;
                fg.target_fps_source = crate::domain::config::Source::File;
            }
        } else {
            problem_at(
                text,
                problems,
                entry,
                "fg.target_fps",
                format!(
                    "expected a number or \"auto\", got {} - using default 0 (uncapped)",
                    type_name(entry)
                ),
            );
        }
    }

    let entry = section_key(root, "fg", "tear_guard");
    match read_string(entry) {
        Read::Missing => {}
        Read::WrongType => problem_at(
            text,
            problems,
            entry.expect("wrong type has a node"),
            "fg.tear_guard",
            format!(
                "expected a string, got {} - using default \"scanline\"",
                type_name(entry.expect("wrong type has a node"))
            ),
        ),
        Read::Ok(value) => match TearGuard::parse(&value) {
            Some(parsed) => {
                fg.tear_guard = parsed;
                fg.tear_guard_source = crate::domain::config::Source::File;
            }
            None => problem_at(
                text,
                problems,
                entry.expect("ok has a node"),
                "fg.tear_guard",
                format!(
                    "unknown value \"{value}\" (expected \"scanline\", \"timer\" or \"off\") - \
                     using default \"scanline\""
                ),
            ),
        },
    }
}

fn read_limiter(text: &str, root: &Table, config: &mut Settings, problems: &mut Vec<String>) {
    check_section(text, root, "limiter", problems);

    let entry = section_key(root, "limiter", "fps");
    match read_number(entry) {
        Read::Missing => {}
        Read::WrongType => problem_at(
            text,
            problems,
            entry.expect("wrong type has a node"),
            "limiter.fps",
            format!(
                "expected a number, got {} - ignored (limiter.fps is replaced by fg.target_fps)",
                type_name(entry.expect("wrong type has a node"))
            ),
        ),
        Read::Ok(value) => {
            if !value.is_finite() || !(0.0..=1000.0).contains(&value) {
                problem_at(
                    text,
                    problems,
                    entry.expect("ok has a node"),
                    "limiter.fps",
                    "must be within [0, 1000] (0 = off) - ignored",
                );
            } else {
                config.legacy_limiter_fps = value;
                config.legacy_limiter_source = crate::domain::config::Source::File;
            }
        }
    }

    let entry = section_key(root, "limiter", "spin_us");
    match read_integer(entry) {
        Read::Missing => {}
        Read::WrongType => problem_at(
            text,
            problems,
            entry.expect("wrong type has a node"),
            "limiter.spin_us",
            format!(
                "expected an integer, got {} - using default 1000",
                type_name(entry.expect("wrong type has a node"))
            ),
        ),
        Read::Ok(value) => {
            if !(0..=100_000).contains(&value) {
                problem_at(
                    text,
                    problems,
                    entry.expect("ok has a node"),
                    "limiter.spin_us",
                    "must be within [0, 100000] microseconds - using default 1000",
                );
            } else {
                config.limiter.spin_us = value as i32;
                config.limiter.spin_us_source = crate::domain::config::Source::File;
            }
        }
    }
}

fn read_log(text: &str, root: &Table, log: &mut LogSettings, problems: &mut Vec<String>) {
    check_section(text, root, "log", problems);

    let entry = section_key(root, "log", "level");
    match read_integer(entry) {
        Read::Missing => {}
        Read::WrongType => problem_at(
            text,
            problems,
            entry.expect("wrong type has a node"),
            "log.level",
            format!(
                "expected an integer, got {} - using default 1",
                type_name(entry.expect("wrong type has a node"))
            ),
        ),
        Read::Ok(value) => {
            if !(0..=2).contains(&value) {
                problem_at(
                    text,
                    problems,
                    entry.expect("ok has a node"),
                    "log.level",
                    "must be 0, 1 or 2 - using default 1",
                );
            } else {
                log.level = value as i32;
                log.level_source = crate::domain::config::Source::File;
            }
        }
    }
}

fn read_ui(text: &str, root: &Table, ui: &mut UiSettings, problems: &mut Vec<String>) {
    check_section(text, root, "ui", problems);

    let entry = section_key(root, "ui", "mode");
    match read_string(entry) {
        Read::Missing => {}
        Read::WrongType => problem_at(
            text,
            problems,
            entry.expect("wrong type has a node"),
            "ui.mode",
            format!(
                "expected a string, got {} - using default \"off\"",
                type_name(entry.expect("wrong type has a node"))
            ),
        ),
        Read::Ok(value) => {
            let mode = if value.eq_ignore_ascii_case("off") {
                Some(UiMode::Off)
            } else if value.eq_ignore_ascii_case("mini") {
                Some(UiMode::Mini)
            } else if value.eq_ignore_ascii_case("full") {
                Some(UiMode::Full)
            } else {
                None
            };
            match mode {
                Some(mode) => {
                    ui.mode = mode;
                    ui.mode_source = crate::domain::config::Source::File;
                }
                None => problem_at(
                    text,
                    problems,
                    entry.expect("ok has a node"),
                    "ui.mode",
                    format!(
                        "unknown value \"{value}\" (expected \"off\", \"mini\" or \"full\") - \
                         using default \"off\""
                    ),
                ),
            }
        }
    }

    if let Some(entry) = section_key(root, "ui", "scale") {
        if let Read::Ok(value) = read_string(Some(entry)) {
            if value.eq_ignore_ascii_case("auto") {
                ui.scale_mode = UiScaleMode::Auto;
                ui.scale_source = crate::domain::config::Source::File;
            } else {
                problem_at(
                    text,
                    problems,
                    entry,
                    "ui.scale",
                    format!(
                        "unknown value \"{value}\" (expected \"auto\" or a number in [0.5, 2.0]) - \
                         using default \"auto\""
                    ),
                );
            }
        } else {
            match read_number(Some(entry)) {
                Read::Ok(value) => {
                    if !value.is_finite() || !(0.5..=2.0).contains(&value) {
                        problem_at(
                            text,
                            problems,
                            entry,
                            "ui.scale",
                            "must be within [0.5, 2.0] - using default \"auto\"",
                        );
                    } else {
                        ui.scale = value as f32;
                        ui.scale_mode = UiScaleMode::Fixed;
                        ui.scale_source = crate::domain::config::Source::File;
                    }
                }
                _ => problem_at(
                    text,
                    problems,
                    entry,
                    "ui.scale",
                    format!(
                        "expected \"auto\" or a number, got {} - using default \"auto\"",
                        type_name(entry)
                    ),
                ),
            }
        }
    }

    let entry = section_key(root, "ui", "hotkey");
    match read_string(entry) {
        Read::Missing => {}
        Read::WrongType => problem_at(
            text,
            problems,
            entry.expect("wrong type has a node"),
            "ui.hotkey",
            format!(
                "expected a string, got {} - using default \"alt+f10\"",
                type_name(entry.expect("wrong type has a node"))
            ),
        ),
        Read::Ok(value) => {
            if crate::domain::config::valid_hotkey(&value) {
                ui.hotkey = value.to_ascii_lowercase();
                ui.hotkey_source = crate::domain::config::Source::File;
            } else {
                problem_at(
                    text,
                    problems,
                    entry.expect("ok has a node"),
                    "ui.hotkey",
                    "expected modifier+key such as \"alt+f10\" - using default \"alt+f10\"",
                );
            }
        }
    }
}

/// Reads `sedici.toml` text and applies every key it contains to a fresh default configuration.
/// The returned settings have `file_parsed = true`; the caller keeps its own existence/path state.
pub fn parse_settings(text: &str) -> (Settings, Vec<String>, Vec<String>) {
    let mut config = Settings::default();
    let mut problems = Vec::new();
    let mut notes = Vec::new();

    let document = match skim_parse(text) {
        Ok(document) => document,
        Err((line, column, message)) => {
            problems.push(format!(
                "sedici.toml:{line}:{column}: parse error: {message} - using built-in defaults for \
                 every key"
            ));
            return (config, problems, notes);
        }
    };

    config.file_parsed = true;
    let root = document.as_table();
    note_removed_keys(root, &mut notes);
    note_unknown_keys(text, root, &mut problems);
    read_fg(text, root, &mut config.fg, &mut problems);
    read_limiter(text, root, &mut config, &mut problems);
    read_log(text, root, &mut config.log, &mut problems);
    read_ui(text, root, &mut config.ui, &mut problems);

    (config, problems, notes)
}

/// A parse that keeps the position for the diagnostic. `toml_edit`'s span is a byte offset, and it
/// survives only while the document is the parsed `ImDocument` (editing despans it).
fn skim_parse(text: &str) -> Result<toml_edit::ImDocument<&str>, (usize, usize, String)> {
    match toml_edit::ImDocument::parse(text) {
        Ok(document) => Ok(document),
        Err(error) => {
            let (line, column) = error
                .span()
                .map_or((1, 1), |span| line_col(text, span.start));
            Err((line, column, error.message().to_owned()))
        }
    }
}

/// The full load: path resolution, existence, text, parse, environment overrides and the legacy
/// target fold. Nothing here can fail the game's call.
pub fn load_settings(
    store: &dyn ConfigStore,
    lookup: impl Fn(&str) -> Option<String>,
) -> LoadedSettings {
    let mut loaded = LoadedSettings::default();

    if let Some(path) = store.default_path() {
        if store.is_file(&path) {
            loaded.settings.file_exists = true;
            loaded.settings.path = Some(path.clone());
            match store.load_text(&path) {
                Err(reason) => loaded.problems.push(reason),
                Ok(text) => {
                    let (settings, problems, notes) = parse_settings(&text);
                    // `parse_settings` builds a fresh default; keep the file state found here.
                    let path = loaded.settings.path.clone();
                    let file_exists = loaded.settings.file_exists;
                    loaded.settings = settings;
                    loaded.settings.path = path;
                    loaded.settings.file_exists = file_exists;
                    loaded.problems.extend(problems);
                    loaded.notes.extend(notes);
                }
            }
        } else {
            loaded.settings.path = Some(path);
        }
    }

    apply_env_overrides(&mut loaded.settings, lookup, &mut loaded.problems);
    finalize_target(&mut loaded.settings, &mut loaded.notes);
    loaded
}

/// [`load_settings`] plus the once-at-load report. Problems are logged at error level and the rest
/// at info level; every line is also returned so a caller can inspect it.
pub fn load_and_log(
    store: &dyn ConfigStore,
    lookup: impl Fn(&str) -> Option<String>,
) -> LoadedSettings {
    let loaded = load_settings(store, lookup);
    for line in &loaded.problems {
        tracing::error!("{line}");
    }
    let start = loaded.problems.len();
    for line in loaded.report_lines().into_iter().skip(start) {
        tracing::info!("{line}");
    }
    loaded
}

// ---------------------------------------------------------------------------
// Comment-preserving patch (config_patch.cpp)
// ---------------------------------------------------------------------------

#[derive(Clone, Debug, PartialEq)]
struct Line {
    text: String,
    eol: String,
}

fn trim(text: &str) -> String {
    text.trim_matches([' ', '\t']).to_owned()
}

fn is_trimmed_empty(text: &str) -> bool {
    text.bytes().all(|b| b == b' ' || b == b'\t')
}

fn split_lines(text: &str) -> Vec<Line> {
    let mut lines = Vec::new();
    let bytes = text.as_bytes();
    let mut start = 0;
    while start < bytes.len() {
        let newline = text[start..].find('\n').map(|offset| start + offset);
        let end = newline.unwrap_or(bytes.len());
        let mut line = Line {
            text: text[start..end].to_owned(),
            eol: String::new(),
        };
        if line.text.ends_with('\r') {
            line.text.pop();
            line.eol = "\r\n".to_owned();
        } else if newline.is_some() {
            line.eol = "\n".to_owned();
        }
        lines.push(line);
        match newline {
            Some(_) => start = end + 1,
            None => break,
        }
    }
    lines
}

fn join_lines(lines: &[Line]) -> String {
    let mut text = String::new();
    for line in lines {
        text.push_str(&line.text);
        text.push_str(&line.eol);
    }
    text
}

fn dominant_eol(text: &str) -> String {
    if text.contains("\r\n") {
        "\r\n".to_owned()
    } else {
        "\n".to_owned()
    }
}

fn find_comment(line: &str) -> Option<usize> {
    let mut in_single = false;
    let mut in_double = false;
    let mut escaped = false;
    for (i, c) in line.char_indices() {
        if in_double && c == '\\' && !escaped {
            escaped = true;
            continue;
        }
        if c == '"' && !in_single && !escaped {
            in_double = !in_double;
        }
        if c == '\'' && !in_double {
            in_single = !in_single;
        }
        if c == '#' && !in_single && !in_double {
            return Some(i);
        }
        escaped = false;
    }
    None
}

fn section_name(line: &str) -> Option<String> {
    let comment = find_comment(line);
    let code = trim(&line[..comment.unwrap_or(line.len())]);
    if code.len() < 3 || !code.starts_with('[') || !code.ends_with(']') {
        return None;
    }
    if code.len() >= 4 && code.as_bytes()[1] == b'[' {
        return None; // array of tables
    }
    Some(trim(&code[1..code.len() - 1]))
}

fn replace_key(line: &mut Line, key: &str, value: &str) -> bool {
    let comment = find_comment(&line.text);
    let code = &line.text[..comment.unwrap_or(line.text.len())];
    let Some(equals) = code.find('=') else {
        return false;
    };
    if trim(&code[..equals]) != key {
        return false;
    }

    let mut value_start = equals + 1;
    let bytes = code.as_bytes();
    while value_start < code.len() && (bytes[value_start] == b' ' || bytes[value_start] == b'\t') {
        value_start += 1;
    }
    let mut out = String::new();
    if value_start >= code.len() {
        out.push_str(&code[..=equals]);
        out.push(' ');
        out.push_str(value);
        if let Some(comment) = comment {
            out.push_str(&line.text[comment..]);
        }
        line.text = out;
        return true;
    }

    let mut value_end = code.len();
    while value_end > value_start && (bytes[value_end - 1] == b' ' || bytes[value_end - 1] == b'\t')
    {
        value_end -= 1;
    }

    out.push_str(&code[..value_start]);
    out.push_str(value);
    out.push_str(&code[value_end..]);
    if let Some(comment) = comment {
        out.push_str(&line.text[comment..]);
    }
    line.text = out;
    true
}

fn find_section(lines: &[Line], name: &str) -> Option<(usize, usize)> {
    for (i, line) in lines.iter().enumerate() {
        if section_name(&line.text).as_deref() == Some(name) {
            let mut end = lines.len();
            for (j, later) in lines.iter().enumerate().skip(i + 1) {
                let code =
                    trim(&later.text[..find_comment(&later.text).unwrap_or(later.text.len())]);
                if !code.is_empty() && code.starts_with('[') {
                    end = j;
                    break;
                }
            }
            return Some((i, end));
        }
    }
    None
}

fn apply_value(lines: &mut Vec<Line>, section: &str, key: &str, value: &str, eol: &str) {
    let Some((begin, end)) = find_section(lines, section) else {
        if let Some(last) = lines.last_mut() {
            if last.eol.is_empty() {
                last.eol = eol.to_owned();
            }
        }
        if lines.last().is_some_and(|last| !last.text.is_empty()) {
            lines.push(Line {
                text: String::new(),
                eol: eol.to_owned(),
            });
        }
        lines.push(Line {
            text: format!("[{section}]"),
            eol: eol.to_owned(),
        });
        lines.push(Line {
            text: format!("{key} = {value}"),
            eol: eol.to_owned(),
        });
        return;
    };

    for line in lines.iter_mut().take(end).skip(begin + 1) {
        if replace_key(line, key, value) {
            return;
        }
    }

    if end > 0 && lines[end - 1].eol.is_empty() {
        lines[end - 1].eol = eol.to_owned();
    }

    let mut insert = end;
    while insert > begin + 1 && is_trimmed_empty(&lines[insert - 1].text) {
        insert -= 1;
    }
    if insert > 0 && lines[insert - 1].eol.is_empty() {
        lines[insert - 1].eol = eol.to_owned();
    }
    lines.insert(
        insert,
        Line {
            text: format!("{key} = {value}"),
            eol: eol.to_owned(),
        },
    );
}

fn quoted(value: &str) -> String {
    format!("\"{value}\"")
}

/// Semantic equality, because `toml_edit` values carry formatting that must not matter here.
fn item_eq(a: &Item, b: &Item) -> bool {
    match (a, b) {
        (Item::None, Item::None) => true,
        (Item::Table(a), Item::Table(b)) => table_eq(a, b),
        (Item::ArrayOfTables(a), Item::ArrayOfTables(b)) => {
            a.len() == b.len() && a.iter().zip(b.iter()).all(|(a, b)| table_eq(a, b))
        }
        (Item::Value(a), Item::Value(b)) => value_eq(a, b),
        _ => false,
    }
}

fn table_eq(a: &Table, b: &Table) -> bool {
    a.len() == b.len()
        && a.iter()
            .all(|(key, item)| b.get(key).is_some_and(|other| item_eq(item, other)))
}

fn value_eq(a: &Value, b: &Value) -> bool {
    match (a, b) {
        (Value::String(a), Value::String(b)) => a.value() == b.value(),
        (Value::Integer(a), Value::Integer(b)) => a.value() == b.value(),
        (Value::Float(a), Value::Float(b)) => a.value() == b.value(),
        (Value::Boolean(a), Value::Boolean(b)) => a.value() == b.value(),
        (Value::Datetime(a), Value::Datetime(b)) => a.value() == b.value(),
        (Value::Array(a), Value::Array(b)) => {
            a.len() == b.len() && a.iter().zip(b.iter()).all(|(a, b)| value_eq(a, b))
        }
        (Value::InlineTable(a), Value::InlineTable(b)) => {
            a.len() == b.len()
                && a.iter()
                    .all(|(key, value)| b.get(key).is_some_and(|other| value_eq(value, other)))
        }
        _ => false,
    }
}

/// Patches `values` into `text`, preserving comments and formatting. Returns the new text or a
/// user-facing reason the layout was refused.
pub fn patch_text(text: &str, values: &ConfigValues) -> Result<String, String> {
    let eol = dominant_eol(text);
    let mut lines = split_lines(text);
    // Validate with the real parser, not the line patcher's limited syntax.
    let mut expected = toml_edit::ImDocument::parse(text)
        .map_err(|_| {
            "invalid or unsupported TOML layout; original file was not changed".to_owned()
        })?
        .into_mut();

    let expect = |document: &mut DocumentMut, section: &str, key: &str, value: Value| {
        if document.get(section).is_none() {
            document.insert(section, Item::Table(Table::new()));
        }
        let table = document
            .get_mut(section)
            .and_then(Item::as_table_mut)
            .ok_or_else(|| "unexpected failure while saving sedici.toml".to_owned())?;
        table.insert(key, Item::Value(value));
        Ok::<_, String>(())
    };

    if values.write_target_fps {
        if values.target_auto {
            apply_value(&mut lines, "fg", "target_fps", &quoted("auto"), &eol);
            expect(&mut expected, "fg", "target_fps", Value::from("auto"))?;
        } else {
            let number = crate::domain::config::format_number(values.target_fps);
            apply_value(&mut lines, "fg", "target_fps", &number, &eol);
            // The patcher emits whole numbers as TOML integers.
            let value = if number.contains('.') {
                Value::from(
                    number
                        .parse::<f64>()
                        .map_err(|_| "invalid numeric setting")?,
                )
            } else {
                Value::from(
                    number
                        .parse::<i64>()
                        .map_err(|_| "invalid numeric setting")?,
                )
            };
            expect(&mut expected, "fg", "target_fps", value)?;
        }
    }
    if values.write_log {
        apply_value(
            &mut lines,
            "log",
            "level",
            &values.log_level.to_string(),
            &eol,
        );
        expect(
            &mut expected,
            "log",
            "level",
            Value::from(i64::from(values.log_level)),
        )?;
    }
    if values.write_ui_mode {
        apply_value(&mut lines, "ui", "mode", &quoted(&values.ui_mode), &eol);
        expect(
            &mut expected,
            "ui",
            "mode",
            Value::from(values.ui_mode.as_str()),
        )?;
    }
    if values.write_ui_scale {
        if values.ui_scale_auto {
            apply_value(&mut lines, "ui", "scale", &quoted("auto"), &eol);
            expect(&mut expected, "ui", "scale", Value::from("auto"))?;
        } else {
            let number = crate::domain::config::format_number(f64::from(values.ui_scale));
            apply_value(&mut lines, "ui", "scale", &number, &eol);
            let value = if number.contains('.') {
                Value::from(
                    number
                        .parse::<f64>()
                        .map_err(|_| "invalid numeric setting")?,
                )
            } else {
                Value::from(
                    number
                        .parse::<i64>()
                        .map_err(|_| "invalid numeric setting")?,
                )
            };
            expect(&mut expected, "ui", "scale", value)?;
        }
    }

    let mut output = join_lines(&lines);
    if !output.is_empty() && !output.ends_with('\n') {
        output.push_str(&eol);
    }
    let parsed = toml_edit::ImDocument::parse(&output)
        .map_err(|_| "unsupported TOML layout; original file was not changed".to_owned())?;
    if !table_eq(parsed.as_table(), expected.as_table()) {
        return Err("unsupported TOML layout; original file was not changed".to_owned());
    }
    Ok(output)
}

// ---------------------------------------------------------------------------
// Module directory
// ---------------------------------------------------------------------------

/// `sedici.toml` beside the module directory, or `None` when the directory is unknown.
pub fn config_path_in(module_dir: Option<&Path>) -> Option<PathBuf> {
    module_dir.map(|dir| dir.join("sedici.toml"))
}

#[cfg(windows)]
fn module_directory() -> Option<PathBuf> {
    use windows::core::PCWSTR;
    use windows::Win32::Foundation::HMODULE;
    use windows::Win32::System::LibraryLoader::{
        GetModuleFileNameW, GetModuleHandleExW, GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
    };

    // SAFETY: the address is a function in this module, so the UNCHANGED_REFCOUNT query only reads
    // the loader's module table and never owns a reference we would have to release.
    unsafe {
        let mut module = HMODULE::default();
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            PCWSTR(module_directory as *const u16),
            &mut module,
        )
        .ok()?;

        let mut buffer = vec![0u16; 512];
        loop {
            let length = GetModuleFileNameW(Some(module), &mut buffer) as usize;
            if length == 0 {
                return None;
            }
            if length < buffer.len() {
                let path = String::from_utf16_lossy(&buffer[..length]);
                return Path::new(&path).parent().map(Path::to_path_buf);
            }
            if buffer.len() >= 32_768 {
                return None;
            }
            buffer.resize(buffer.len() * 2, 0);
        }
    }
}

#[cfg(not(windows))]
fn module_directory() -> Option<PathBuf> {
    None
}

// ---------------------------------------------------------------------------
// Stamps and the file-backed store
// ---------------------------------------------------------------------------

#[cfg(windows)]
fn windows_path(path: &Path) -> Vec<u16> {
    use std::os::windows::ffi::OsStrExt;
    path.as_os_str()
        .encode_wide()
        .chain(std::iter::once(0))
        .collect()
}

#[cfg(windows)]
fn file_stamp(path: &Path) -> FileStamp {
    use sha2::{Digest, Sha256};
    use windows::Win32::Foundation::GENERIC_READ;
    use windows::Win32::Foundation::{
        CloseHandle, ERROR_FILE_NOT_FOUND, ERROR_PATH_NOT_FOUND, HANDLE,
    };
    use windows::Win32::Storage::FileSystem::FILE_SHARE_DELETE;
    use windows::Win32::Storage::FileSystem::{
        CreateFileW, GetFileInformationByHandle, ReadFile, BY_HANDLE_FILE_INFORMATION,
        FILE_ATTRIBUTE_DIRECTORY, FILE_FLAG_SEQUENTIAL_SCAN, FILE_SHARE_READ, FILE_SHARE_WRITE,
        OPEN_EXISTING,
    };

    let wide = windows_path(path);
    // SAFETY: `wide` is a NUL-terminated UTF-16 path and the handle is closed before every exit.
    unsafe {
        let handle: HANDLE = match CreateFileW(
            windows::core::PCWSTR(wide.as_ptr()),
            GENERIC_READ.0,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            None,
            OPEN_EXISTING,
            FILE_FLAG_SEQUENTIAL_SCAN,
            None,
        ) {
            Ok(handle) => handle,
            Err(error) => {
                let code = (error.code().0 as u32) & 0xFFFF;
                let mut stamp = FileStamp::default();
                if code == ERROR_FILE_NOT_FOUND.0 || code == ERROR_PATH_NOT_FOUND.0 {
                    stamp.valid = true;
                    stamp.exists = false;
                }
                return stamp;
            }
        };

        let mut before = BY_HANDLE_FILE_INFORMATION::default();
        if GetFileInformationByHandle(handle, &mut before).is_err()
            || before.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY.0 != 0
        {
            let _ = CloseHandle(handle);
            return FileStamp::default();
        }

        let mut hasher = Sha256::new();
        let mut buffer = [0u8; 16_384];
        let mut total: u64 = 0;
        loop {
            let mut read = 0u32;
            if ReadFile(handle, Some(&mut buffer), Some(&mut read), None).is_err() {
                let _ = CloseHandle(handle);
                return FileStamp::default();
            }
            if read == 0 {
                break;
            }
            hasher.update(&buffer[..read as usize]);
            total += u64::from(read);
        }

        let mut after = BY_HANDLE_FILE_INFORMATION::default();
        if GetFileInformationByHandle(handle, &mut after).is_err() {
            let _ = CloseHandle(handle);
            return FileStamp::default();
        }
        let _ = CloseHandle(handle);

        let size = (u64::from(before.nFileSizeHigh) << 32) | u64::from(before.nFileSizeLow);
        let unchanged = total == size
            && before.nFileSizeHigh == after.nFileSizeHigh
            && before.nFileSizeLow == after.nFileSizeLow
            && before.ftLastWriteTime.dwHighDateTime == after.ftLastWriteTime.dwHighDateTime
            && before.ftLastWriteTime.dwLowDateTime == after.ftLastWriteTime.dwLowDateTime
            && before.dwVolumeSerialNumber == after.dwVolumeSerialNumber
            && before.nFileIndexHigh == after.nFileIndexHigh
            && before.nFileIndexLow == after.nFileIndexLow;
        if !unchanged {
            return FileStamp::default();
        }

        let mut digest = [0u8; 32];
        digest.copy_from_slice(&hasher.finalize());
        FileStamp {
            valid: true,
            exists: true,
            size,
            last_write: (u64::from(before.ftLastWriteTime.dwHighDateTime) << 32)
                | u64::from(before.ftLastWriteTime.dwLowDateTime),
            file_id: (u64::from(before.nFileIndexHigh) << 32) | u64::from(before.nFileIndexLow),
            volume_serial: before.dwVolumeSerialNumber,
            sha256: digest,
        }
    }
}

#[cfg(not(windows))]
fn file_stamp(path: &Path) -> FileStamp {
    use sha2::{Digest, Sha256};
    match std::fs::read(path) {
        Ok(bytes) => {
            let mut digest = [0u8; 32];
            digest.copy_from_slice(&Sha256::digest(&bytes));
            FileStamp {
                valid: true,
                exists: true,
                size: bytes.len() as u64,
                sha256: digest,
                ..FileStamp::default()
            }
        }
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => FileStamp {
            valid: true,
            exists: false,
            ..FileStamp::default()
        },
        Err(_) => FileStamp::default(),
    }
}

/// Writes a new file that must not exist yet. `Err(true)` means the name is taken (try another);
/// `Err(false)` means a real failure.
fn write_bytes(path: &Path, bytes: &[u8]) -> Result<(), bool> {
    #[cfg(windows)]
    {
        use windows::Win32::Foundation::GENERIC_WRITE;
        use windows::Win32::Foundation::{
            CloseHandle, ERROR_ALREADY_EXISTS, ERROR_FILE_EXISTS, HANDLE,
        };
        use windows::Win32::Storage::FileSystem::{
            CreateFileW, FlushFileBuffers, WriteFile, CREATE_NEW, FILE_ATTRIBUTE_NORMAL,
        };

        let wide = windows_path(path);
        // SAFETY: the path is NUL-terminated, CREATE_NEW never truncates an existing file, and the
        // handle is closed on every exit.
        unsafe {
            let handle: HANDLE = match CreateFileW(
                windows::core::PCWSTR(wide.as_ptr()),
                GENERIC_WRITE.0,
                windows::Win32::Storage::FileSystem::FILE_SHARE_MODE(0),
                None,
                CREATE_NEW,
                FILE_ATTRIBUTE_NORMAL,
                None,
            ) {
                Ok(handle) => handle,
                Err(error) => {
                    let code = (error.code().0 as u32) & 0xFFFF;
                    return Err(code == ERROR_FILE_EXISTS.0 || code == ERROR_ALREADY_EXISTS.0);
                }
            };
            let mut written = true;
            for chunk in bytes.chunks(1 << 20) {
                let mut count = 0u32;
                if WriteFile(handle, Some(chunk), Some(&mut count), None).is_err()
                    || count as usize != chunk.len()
                {
                    written = false;
                    break;
                }
            }
            if written {
                written = FlushFileBuffers(handle).is_ok();
            }
            if CloseHandle(handle).is_err() {
                written = false;
            }
            if written {
                Ok(())
            } else {
                Err(false)
            }
        }
    }
    #[cfg(not(windows))]
    {
        use std::io::Write;
        match std::fs::OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(path)
        {
            Ok(mut file) => {
                if file.write_all(bytes).and_then(|()| file.sync_all()).is_ok() {
                    Ok(())
                } else {
                    Err(false)
                }
            }
            Err(error) => Err(error.kind() == std::io::ErrorKind::AlreadyExists),
        }
    }
}

fn delete_file(path: &Path) {
    #[cfg(windows)]
    {
        use windows::core::PCWSTR;
        use windows::Win32::Storage::FileSystem::DeleteFileW;
        let wide = windows_path(path);
        // SAFETY: the path is NUL-terminated.
        unsafe {
            let _ = DeleteFileW(PCWSTR(wide.as_ptr()));
        }
    }
    #[cfg(not(windows))]
    {
        let _ = std::fs::remove_file(path);
    }
}

fn move_file(source: &Path, destination: &Path) -> bool {
    #[cfg(windows)]
    {
        use windows::core::PCWSTR;
        use windows::Win32::Storage::FileSystem::{
            MoveFileExW, MOVEFILE_REPLACE_EXISTING, MOVEFILE_WRITE_THROUGH,
        };
        let source_wide = windows_path(source);
        let destination_wide = windows_path(destination);
        // SAFETY: both paths are NUL-terminated; the flags replace the destination atomically.
        unsafe {
            MoveFileExW(
                PCWSTR(source_wide.as_ptr()),
                PCWSTR(destination_wide.as_ptr()),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH,
            )
            .is_ok()
        }
    }
    #[cfg(not(windows))]
    {
        std::fs::rename(source, destination).is_ok()
    }
}

/// `ERROR_FILE_NOT_FOUND` as `ReplaceFileW` reports it: the file to displace vanished meanwhile.
const ERROR_FILE_NOT_FOUND_RAW: u32 = 2;

/// `ReplaceFileW`, which keeps the file it displaces so an outside edit can be put back (K43).
fn replace_file(destination: &Path, replacement: &Path, displaced: &Path) -> Result<(), u32> {
    #[cfg(windows)]
    {
        use windows::core::PCWSTR;
        use windows::Win32::Storage::FileSystem::{
            ReplaceFileW, REPLACEFILE_IGNORE_ACL_ERRORS, REPLACEFILE_IGNORE_MERGE_ERRORS,
        };
        let destination_wide = windows_path(destination);
        let replacement_wide = windows_path(replacement);
        let displaced_wide = windows_path(displaced);
        // SAFETY: all three paths are NUL-terminated; the ignored merge/ACL errors only relax
        // security and attribute merging, never the file content.
        unsafe {
            ReplaceFileW(
                PCWSTR(destination_wide.as_ptr()),
                PCWSTR(replacement_wide.as_ptr()),
                PCWSTR(displaced_wide.as_ptr()),
                REPLACEFILE_IGNORE_MERGE_ERRORS | REPLACEFILE_IGNORE_ACL_ERRORS,
                None,
                None,
            )
        }
        .map_err(|error| (error.code().0 as u32) & 0xFFFF)
    }
    #[cfg(not(windows))]
    {
        let _ = (destination, replacement, displaced);
        Err(1)
    }
}

/// A [`ConfigStore`] over real files. Every Windows call is contained here.
#[derive(Clone, Debug)]
pub struct TomlConfigStore {
    path: Option<PathBuf>,
}

impl TomlConfigStore {
    /// `sedici.toml` beside this DLL.
    pub fn beside_module() -> Self {
        Self {
            path: config_path_in(module_directory().as_deref()),
        }
    }

    /// A specific file, used by tests and by `settings_use_file`.
    pub fn at(path: impl Into<PathBuf>) -> Self {
        Self {
            path: Some(path.into()),
        }
    }
}

impl ConfigStore for TomlConfigStore {
    fn default_path(&self) -> Option<PathBuf> {
        self.path.clone()
    }

    fn is_file(&self, path: &Path) -> bool {
        std::fs::metadata(path)
            .map(|metadata| metadata.is_file())
            .unwrap_or(false)
    }

    fn stamp(&self, path: &Path) -> FileStamp {
        file_stamp(path)
    }

    fn load_text(&self, path: &Path) -> Result<String, String> {
        match std::fs::read(path) {
            Ok(bytes) => String::from_utf8(bytes)
                .map_err(|_| "sedici.toml: parse error: the file is not valid UTF-8 - using built-in defaults for every key".to_owned()),
            Err(_) => Err(
                "sedici.toml: could not be opened for reading - using built-in defaults for every key"
                    .to_owned(),
            ),
        }
    }

    fn save(
        &self,
        path: &Path,
        values: &ConfigValues,
        expected: Option<&FileStamp>,
    ) -> SaveOutcome {
        let mut outcome = SaveOutcome::default();

        let initial = file_stamp(path);
        if !initial.valid {
            outcome.error = "could not determine the state of sedici.toml".to_owned();
            return outcome;
        }
        if let Some(expected) = expected {
            if !same_stamp(&initial, expected) {
                outcome.conflict = true;
                outcome.error = "sedici.toml changed before save".to_owned();
                return outcome;
            }
        }
        let text = if initial.exists {
            match self.load_text(path) {
                Ok(text) => text,
                Err(_) => {
                    outcome.error = "could not open sedici.toml for reading".to_owned();
                    return outcome;
                }
            }
        } else {
            String::new()
        };

        let output = match patch_text(&text, values) {
            Ok(output) => output,
            Err(reason) => {
                outcome.error = reason;
                return outcome;
            }
        };

        // Create a private, unique sibling with CREATE_NEW. A fixed .tmp name could truncate
        // someone else's file before save failed (K49).
        static SEQUENCE: AtomicU64 = AtomicU64::new(0);
        let mut temporary = None;
        for _ in 0..64 {
            let candidate = PathBuf::from(format!(
                "{}.sedici-{}-{}.tmp",
                path.display(),
                std::process::id(),
                SEQUENCE.fetch_add(1, Ordering::Relaxed)
            ));
            match write_bytes(&candidate, output.as_bytes()) {
                Ok(()) => {
                    temporary = Some(candidate);
                    break;
                }
                // Only an existing sibling is worth another name; a directory that cannot be
                // written keeps failing under every name.
                Err(true) => continue,
                Err(false) => break,
            }
        }
        let Some(temporary) = temporary else {
            outcome.error = "could not create a unique temporary file".to_owned();
            return outcome;
        };

        let staged = file_stamp(&temporary);
        if !staged.valid || !staged.exists {
            outcome.error = "could not verify the temporary settings file".to_owned();
            delete_file(&temporary);
            return outcome;
        }

        // This narrows, but cannot eliminate, the external-writer race: another process can still
        // replace the path after this check and before the rename.
        if let Some(expected) = expected {
            if !same_stamp(&file_stamp(path), expected) {
                outcome.conflict = true;
                outcome.error = "sedici.toml changed before replacement".to_owned();
                delete_file(&temporary);
                return outcome;
            }
        }

        let mut displaced: Option<PathBuf> = None;
        let mut swapped = false;
        if initial.exists {
            let backup = PathBuf::from(format!("{}.prev", temporary.display()));
            match replace_file(path, &temporary, &backup) {
                Ok(()) => {
                    swapped = true;
                    displaced = Some(backup);
                }
                // No file to displace (created or removed meanwhile): fall through to a plain move.
                Err(code) if code == ERROR_FILE_NOT_FOUND_RAW => {}
                Err(code) => {
                    outcome.error = format!("could not replace sedici.toml (error {code})");
                    delete_file(&temporary);
                    return outcome;
                }
            }
        }
        if !swapped {
            // No file to displace: plain move. The caller's baseline check above already refused
            // an outside edit, so a file that appeared meanwhile is replaced deliberately.
            if !move_file(&temporary, path) {
                outcome.error = "could not replace sedici.toml".to_owned();
                delete_file(&temporary);
                return outcome;
            }
            displaced = None;
        }

        if let Some(backup) = displaced {
            if let Some(expected) = expected {
                if !same_stamp(&file_stamp(&backup), expected) {
                    // Restore the external version; our candidate is discarded.
                    if move_file(&backup, path) {
                        outcome.conflict = true;
                        outcome.error =
                            "sedici.toml changed during replacement; the outside edit was kept"
                                .to_owned();
                    } else {
                        let name = backup
                            .file_name()
                            .map_or_else(String::new, |n| n.to_string_lossy().into_owned());
                        outcome.conflict = true;
                        outcome.error = format!(
                            "sedici.toml changed during replacement; the outside edit is kept as {name}"
                        );
                    }
                    return outcome;
                }
            }
            delete_file(&backup);
        }

        outcome.written = true;
        outcome.written_stamp = staged;
        outcome
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::domain::config::Source;
    use rstest::rstest;

    fn values() -> ConfigValues {
        ConfigValues {
            write_log: true,
            log_level: 2,
            ..ConfigValues::default()
        }
    }

    /// A writable scratch directory per test, cleaned up afterwards.
    struct Scratch {
        dir: PathBuf,
    }

    impl Scratch {
        fn new(name: &str) -> Self {
            let dir =
                std::env::temp_dir().join(format!("sedici-rust-{name}-{}", std::process::id()));
            let _ = std::fs::remove_dir_all(&dir);
            std::fs::create_dir_all(&dir).expect("scratch directory");
            Self { dir }
        }
        fn file(&self, name: &str) -> PathBuf {
            self.dir.join(name)
        }
    }

    impl Drop for Scratch {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.dir);
        }
    }

    #[rstest]
    #[case("[fg]\npresent = \"game\"\nmultiplier = \"1.5x\"\ntarget_fps = \"auto\"\ntear_guard = \"off\"\n", Present::Game, Multiplier::X1_5, true, TearGuard::Off)]
    #[case(
        "[fg]\npresent = \"vblank\"\nmultiplier = \"2\"\ntarget_fps = 73\ntear_guard = \"timer\"\n",
        Present::Vsync,
        Multiplier::X2,
        false,
        TearGuard::Timer
    )]
    fn reads_fg_keys(
        #[case] text: &str,
        #[case] present: Present,
        #[case] multiplier: Multiplier,
        #[case] automatic: bool,
        #[case] guard: TearGuard,
    ) {
        let (settings, problems, _) = parse_settings(text);
        assert!(problems.is_empty(), "{problems:?}");
        assert!(settings.file_parsed);
        assert_eq!(settings.fg.present, present);
        assert_eq!(settings.fg.present_source, Source::File);
        assert_eq!(settings.fg.multiplier, multiplier);
        assert_eq!(settings.fg.target_auto, automatic);
        assert_eq!(settings.fg.tear_guard, guard);
    }

    #[rstest]
    #[case("[fg]\nmode = \"amd\"\n", "fg.mode")]
    #[case("[pacing]\nfps = 30\n[async]\nmode = \"x\"\n", "[pacing], [async]")]
    fn removed_keys_are_noted(#[case] text: &str, #[case] fragment: &str) {
        let (_, problems, notes) = parse_settings(text);
        assert!(problems.is_empty(), "{problems:?}");
        assert!(notes[0].contains(fragment));
        assert!(notes[0].contains("no longer exist"));
    }

    #[rstest]
    #[case("[log]\nlevel = 7\n", "log.level")]
    #[case("[fg]\npresent = \"scanline\"\n", "fg.present")]
    #[case("[fg]\nmultiplier = 3\n", "fg.multiplier")]
    #[case("[limiter]\nspin_us = -1\n", "limiter.spin_us")]
    #[case("[ui]\nhotkey = \"f10\"\n", "ui.hotkey")]
    #[case("[ui]\nhotkey = 7\n", "ui.hotkey")]
    #[case("[ui]\nscale = 2.5\n", "ui.scale")]
    #[case("[fg]\ntarget_fps = 5000\n", "fg.target_fps")]
    #[case("[unknown]\nx = 1\n", "unknown section [unknown]")]
    #[case("[ui]\nnope = 1\n", "unknown key ui.nope")]
    fn bad_keys_report_the_key_and_keep_the_default(#[case] text: &str, #[case] fragment: &str) {
        let (settings, problems, _) = parse_settings(text);
        assert!(
            problems.iter().any(|p| p.contains(fragment)),
            "{problems:?}"
        );
        // Every per-key problem carries its line and column; the walker's unknown-key problem
        // carries the line.
        assert!(
            problems
                .iter()
                .any(|p| p.starts_with("sedici.toml:2:") || p.starts_with("sedici.toml:1:")),
            "{problems:?}"
        );
        assert_eq!(settings.log.level, 1);
        assert_eq!(settings.fg.present, Present::Vrr);
        assert_eq!(settings.fg.multiplier, Multiplier::X2);
        assert_eq!(settings.limiter.spin_us, 1000);
        assert_eq!(settings.ui.hotkey, "alt+f10");
        assert_eq!(settings.ui.scale_mode, UiScaleMode::Auto);
        assert_eq!(settings.fg.target_fps, 0.0);
    }

    #[test]
    fn wrong_typed_section_is_one_problem_and_its_keys_are_ignored() {
        let (settings, problems, _) = parse_settings("fg = 3\nlog = \"x\"\n");
        assert!(problems
            .iter()
            .any(|p| p.contains("expected a table, got an integer")));
        assert!(problems
            .iter()
            .any(|p| p.contains("expected a table, got a string")));
        // Both sections are still reported as unknown keys by the walker as well.
        assert!(settings.fg.present_source == Source::Default);
    }

    #[test]
    fn syntax_error_reports_line_and_column_and_falls_back_everywhere() {
        let (settings, problems, _) = parse_settings("[fg]\npresent = \"vrr\"\n[broken\n");
        assert_eq!(problems.len(), 1);
        assert!(problems[0].contains("parse error:"), "{problems:?}");
        assert!(problems[0].contains("using built-in defaults for every key"));
        assert!(!settings.file_parsed);
        assert_eq!(settings.fg.present, Present::Vrr);
    }

    #[test]
    fn missing_file_uses_defaults_and_reports_absence() {
        let scratch = Scratch::new("missing");
        let store = TomlConfigStore::at(scratch.file("sedici.toml"));
        let loaded = load_settings(&store, |_| None);
        assert!(!loaded.settings.file_exists);
        assert!(loaded.problems.is_empty());
        let report = loaded.report_lines();
        assert!(report
            .iter()
            .any(|line| line.contains("no sedici.toml beside the DLL")));
    }

    #[test]
    fn load_combines_file_values_with_environment_precedence() {
        let scratch = Scratch::new("env");
        let path = scratch.file("sedici.toml");
        std::fs::write(
            &path,
            "[fg]\npresent = \"game\"\ntarget_fps = 50\n[log]\nlevel = 0\n",
        )
        .unwrap();
        let store = TomlConfigStore::at(&path);
        let loaded = load_settings(&store, |name| match name {
            "SEDICI_FG_TARGET_FPS" => Some("auto".to_owned()),
            "MFSRFG_LOG" => Some("2".to_owned()),
            _ => None,
        });
        assert!(loaded.settings.file_exists);
        assert!(loaded.settings.file_parsed);
        assert_eq!(loaded.settings.fg.present, Present::Game);
        assert_eq!(loaded.settings.fg.present_source, Source::File);
        assert!(loaded.settings.fg.target_auto);
        assert_eq!(loaded.settings.fg.target_fps_source, Source::Env);
        assert_eq!(loaded.settings.log.level, 2);
        assert_eq!(loaded.settings.log.level_source, Source::Env);
    }

    #[rstest]
    #[case("[log]\nlevel = 1 # keep\n[limiter]\nfps = 0\n", "fps = 36.5")]
    #[case("[fg]\ntarget_fps = 60 # keep\n", "target_fps = \"auto\" # keep")]
    fn patch_preserves_comments_and_rewrites_the_key(#[case] text: &str, #[case] fragment: &str) {
        let mut values = ConfigValues {
            write_target_fps: true,
            ..ConfigValues::default()
        };
        if fragment.contains("auto") {
            values.target_auto = true;
        } else {
            values.target_fps = 36.5;
        }
        let output = patch_text(text, &values).expect("patch");
        assert!(output.contains(fragment), "{output}");
        assert!(output.contains("# keep"));
    }

    #[test]
    fn patch_keeps_array_tables_and_differently_cased_tables_untouched() {
        let text =
            "[log] # keep\r\nnote = 0.2\r\n\r\n[[profiles]]\r\nmode = \"unrelated\" # keep too\r\n";
        let output = patch_text(text, &values()).expect("patch");
        assert!(output.contains("level = 2\r\n"));
        assert!(output.contains("[[profiles]]\r\nmode = \"unrelated\" # keep too\r\n"));
        assert!(
            output.find("level = 2\r\n").unwrap() < output.find("[[profiles]]").unwrap(),
            "{output}"
        );

        let output = patch_text("[Log]\nlevel = 7\n", &values()).expect("patch");
        assert!(output.contains("[Log]\nlevel = 7"));
        assert!(output.contains("[log]\nlevel = 2"));
    }

    #[rstest]
    #[case("[log]\ntext = '''\nlevel = \"inside string\"\n'''\n")]
    #[case("[log]\n\"level\" = 0\n")]
    fn unsafe_layout_is_refused_without_any_byte_changed(#[case] text: &str) {
        let error = patch_text(text, &values()).expect_err("refused");
        assert_eq!(
            error,
            "unsupported TOML layout; original file was not changed"
        );
    }

    #[test]
    fn numbers_that_round_to_integers_are_written_and_validated_as_integers() {
        let mut values = ConfigValues {
            write_target_fps: true,
            target_fps: 36.9999,
            write_ui_scale: true,
            ui_scale: 0.9999,
            ..ConfigValues::default()
        };
        let mut output = patch_text("[log]\nlevel = 1\n", &values).expect("patch");
        assert!(output.contains("target_fps = 37\n"), "{output}");
        assert!(output.contains("scale = 1\n"), "{output}");

        values.write_ui_scale = false;
        output = patch_text("[log]\nlevel = 1\n", &values).expect("patch");
        assert!(output.contains("target_fps = 37\n"));
    }

    #[test]
    fn save_patches_in_place_and_keeps_a_comment() {
        let scratch = Scratch::new("save");
        let path = scratch.file("save.toml");
        std::fs::write(&path, "[limiter]\n# keep this comment\nfps = 0\n").unwrap();
        let store = TomlConfigStore::at(&path);
        let values = ConfigValues {
            write_target_fps: true,
            target_fps: 45.0,
            ..ConfigValues::default()
        };
        let outcome = store.save(&path, &values, None);
        assert!(outcome.written, "{:?}", outcome.error);
        let saved = std::fs::read_to_string(&path).unwrap();
        assert!(saved.contains("target_fps = 45"));
        assert!(saved.contains("# keep this comment"));
        assert!(outcome.written_stamp.valid && outcome.written_stamp.exists);
    }

    #[test]
    fn save_refuses_an_outside_edit_between_begin_and_save() {
        let scratch = Scratch::new("conflict");
        let path = scratch.file("conflict.toml");
        std::fs::write(&path, "[fg]\ntarget_fps = 12\n").unwrap();
        let store = TomlConfigStore::at(&path);
        let baseline = store.stamp(&path);
        std::fs::write(&path, "[fg]\ntarget_fps = 13\n").unwrap();

        let values = ConfigValues {
            write_target_fps: true,
            target_fps: 33.0,
            ..ConfigValues::default()
        };
        let outcome = store.save(&path, &values, Some(&baseline));
        assert!(!outcome.written);
        assert!(outcome.conflict);
        assert_eq!(outcome.error, "sedici.toml changed before save");
        assert_eq!(
            std::fs::read_to_string(&path).unwrap(),
            "[fg]\ntarget_fps = 13\n"
        );
    }

    #[test]
    fn same_size_edit_with_a_restored_timestamp_still_conflicts() {
        let scratch = Scratch::new("digest");
        let path = scratch.file("digest.toml");
        std::fs::write(&path, "[fg]\ntarget_fps = 12\n").unwrap();
        let store = TomlConfigStore::at(&path);
        let baseline = store.stamp(&path);

        std::fs::write(&path, "[fg]\ntarget_fps = 13\n").unwrap();
        let changed = store.stamp(&path);
        assert_eq!(baseline.size, changed.size);
        assert_ne!(baseline.sha256, changed.sha256);

        let values = ConfigValues {
            write_target_fps: true,
            target_fps: 33.0,
            ..ConfigValues::default()
        };
        let outcome = store.save(&path, &values, Some(&baseline));
        assert!(outcome.conflict);
        assert_eq!(
            std::fs::read_to_string(&path).unwrap(),
            "[fg]\ntarget_fps = 13\n"
        );
    }

    #[test]
    fn attribute_query_failure_is_not_a_missing_file() {
        let scratch = Scratch::new("attrs");
        // A path with `|` cannot be created on NTFS, so the query fails without saying "absent".
        let invalid = scratch.file("invalid|config.toml");
        let store = TomlConfigStore::at(&invalid);
        let stamp = store.stamp(&invalid);
        assert!(!stamp.valid && !stamp.exists);
        let outcome = store.save(&invalid, &ConfigValues::default(), None);
        assert!(!outcome.written);
        assert_eq!(
            outcome.error,
            "could not determine the state of sedici.toml"
        );
    }

    #[test]
    fn save_failure_is_a_result_and_leaves_the_original_alone() {
        let scratch = Scratch::new("failed");
        let directory = scratch.file("settings-blocked.toml");
        std::fs::create_dir_all(&directory).unwrap();
        let store = TomlConfigStore::at(&directory);
        let outcome = store.save(&directory, &ConfigValues::default(), None);
        assert!(!outcome.written);
        assert!(!outcome.conflict);
        assert!(outcome.error.starts_with("could not"), "{}", outcome.error);
    }

    #[test]
    fn save_leaves_a_preexisting_sibling_untouched() {
        let scratch = Scratch::new("existing-sibling");
        let path = scratch.file("save.toml");
        std::fs::write(&path, "[log]\nlevel = 1\n").unwrap();
        let store = TomlConfigStore::at(&path);
        let outcome = store.save(
            &path,
            &ConfigValues {
                write_log: true,
                log_level: 2,
                ..ConfigValues::default()
            },
            None,
        );
        assert!(outcome.written, "{:?}", outcome.error);
        // A fixed `.tmp` sibling is never touched even when it exists (K49).
        let fixed = scratch.file("save.toml.tmp");
        std::fs::write(&fixed, "do not overwrite").unwrap();
        assert!(store.save(&path, &values(), None).written);
        assert_eq!(std::fs::read_to_string(&fixed).unwrap(), "do not overwrite");
    }

    #[test]
    fn an_unreadable_file_is_reported_and_defaults_apply() {
        let scratch = Scratch::new("unreadable");
        let path = scratch.file("sedici.toml");
        std::fs::create_dir_all(&path).unwrap(); // a directory, not a file
        let store = TomlConfigStore::at(&path);
        let loaded = load_settings(&store, |_| None);
        assert!(!loaded.settings.file_exists);
        assert!(loaded
            .report_lines()
            .iter()
            .any(|l| l.contains("no sedici.toml")));
    }

    #[test]
    fn saving_a_missing_file_appends_every_section() {
        let scratch = Scratch::new("new-file");
        let path = scratch.file("sedici.toml");
        let store = TomlConfigStore::at(&path);
        let outcome = store.save(
            &path,
            &ConfigValues {
                write_target_fps: true,
                target_fps: 60.0,
                write_log: true,
                log_level: 2,
                write_ui_mode: true,
                ui_mode: "mini".to_owned(),
                write_ui_scale: true,
                ui_scale_auto: true,
                ..ConfigValues::default()
            },
            None,
        );
        assert!(outcome.written, "{:?}", outcome.error);
        let saved = std::fs::read_to_string(&path).unwrap();
        assert!(saved.contains("[fg]\ntarget_fps = 60"), "{saved}");
        assert!(saved.contains("[log]\nlevel = 2"), "{saved}");
        assert!(saved.contains("[ui]\nmode = \"mini\""), "{saved}");
        assert!(saved.contains("scale = \"auto\""), "{saved}");
        // A missing file has no baseline, so a fresh stamp is the caller's.
        assert!(outcome.written_stamp.valid && outcome.written_stamp.exists);
    }

    #[test]
    fn a_crlf_file_keeps_its_line_endings() {
        let scratch = Scratch::new("crlf");
        let path = scratch.file("save.toml");
        std::fs::write(&path, "[log]\r\nlevel = 1\r\n").unwrap();
        let store = TomlConfigStore::at(&path);
        assert!(store.save(&path, &values(), None).written);
        assert_eq!(
            std::fs::read_to_string(&path).unwrap(),
            "[log]\r\nlevel = 2\r\n"
        );
    }
}
