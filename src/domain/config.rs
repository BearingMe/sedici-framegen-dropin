//! Settings model, defaults, provenance and env-override precedence (ports settings/config).
//!
//! Everything here is pure: the file reader lives in `adapters::driven::config_toml` and hands the
//! parsed values to these types; environment overrides are applied through a lookup closure so the
//! precedence rules are testable without touching the process environment. Nothing in this module
//! fails a game call: a bad value is reported through `problems` and the key keeps its default.
use std::path::PathBuf;

use crate::domain::schedule::legacy_limiter_to_target;

/// Where an effective value came from. Used only by the once-at-load log.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum Source {
    #[default]
    Default,
    File,
    Env,
}

impl Source {
    /// "default", "file" or "env", for the provenance column of the load log.
    pub const fn name(self) -> &'static str {
        match self {
            Self::File => "file",
            Self::Env => "env",
            Self::Default => "default",
        }
    }
}

/// `[ui]` menu mode.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum UiMode {
    #[default]
    Off,
    Mini,
    Full,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum UiScaleMode {
    #[default]
    Auto,
    Fixed,
}

/// `[fg] present`: how the wrapper presents its outputs per cycle.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum Present {
    /// "game": the game's own SyncInterval and flags.
    Game,
    /// "vsync" (alias "vblank"): one output per vertical blank.
    Vsync,
    /// "vrr": present from a dedicated thread and queue. The default.
    #[default]
    Vrr,
}

impl Present {
    pub const fn name(self) -> &'static str {
        match self {
            Self::Game => "game",
            Self::Vrr => "vrr",
            Self::Vsync => "vsync",
        }
    }

    /// "vblank" is the name finding 008 shipped with; it still selects vsync.
    pub fn parse(text: &str) -> Option<Self> {
        if text.eq_ignore_ascii_case("vsync") || text.eq_ignore_ascii_case("vblank") {
            Some(Self::Vsync)
        } else if text.eq_ignore_ascii_case("vrr") {
            Some(Self::Vrr)
        } else if text.eq_ignore_ascii_case("game") {
            Some(Self::Game)
        } else {
            None
        }
    }
}

/// What "vrr" does to keep a flip out of the panel's scan.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum TearGuard {
    #[default]
    Scanline,
    Timer,
    Off,
}

impl TearGuard {
    pub const fn name(self) -> &'static str {
        match self {
            Self::Timer => "timer",
            Self::Off => "off",
            Self::Scanline => "scanline",
        }
    }

    pub fn parse(text: &str) -> Option<Self> {
        if text.eq_ignore_ascii_case("scanline") {
            Some(Self::Scanline)
        } else if text.eq_ignore_ascii_case("timer") {
            Some(Self::Timer)
        } else if text.eq_ignore_ascii_case("off") {
            Some(Self::Off)
        } else {
            None
        }
    }
}

/// Output frames per real frame.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum Multiplier {
    #[default]
    X2,
    X1_75,
    X1_5,
    X1_25,
    Adaptive,
}

impl Multiplier {
    pub const fn name(self) -> &'static str {
        match self {
            Self::X1_75 => "1.75x",
            Self::X1_5 => "1.5x",
            Self::X1_25 => "1.25x",
            Self::Adaptive => "adaptive",
            Self::X2 => "2x",
        }
    }

    pub fn parse(text: &str) -> Option<Self> {
        if text.eq_ignore_ascii_case("2x") || text.eq_ignore_ascii_case("2") {
            Some(Self::X2)
        } else if text.eq_ignore_ascii_case("1.75x") || text.eq_ignore_ascii_case("1.75") {
            Some(Self::X1_75)
        } else if text.eq_ignore_ascii_case("1.5x") || text.eq_ignore_ascii_case("1.5") {
            Some(Self::X1_5)
        } else if text.eq_ignore_ascii_case("1.25x") || text.eq_ignore_ascii_case("1.25") {
            Some(Self::X1_25)
        } else if text.eq_ignore_ascii_case("adaptive") {
            Some(Self::Adaptive)
        } else {
            None
        }
    }
}

/// `[limiter]`
#[derive(Clone, Debug, PartialEq)]
pub struct LimiterSettings {
    /// Final busy-wait window before the deadline, in microseconds.
    pub spin_us: i32,
    pub spin_us_source: Source,
}

impl Default for LimiterSettings {
    fn default() -> Self {
        Self {
            spin_us: 1000,
            spin_us_source: Source::Default,
        }
    }
}

/// `[ui]`
#[derive(Clone, Debug, PartialEq)]
pub struct UiSettings {
    pub mode: UiMode,
    pub scale_mode: UiScaleMode,
    pub scale: f32,
    /// Modifier+key, lowercase, for example "alt+f10".
    pub hotkey: String,
    pub mode_source: Source,
    pub scale_source: Source,
    pub hotkey_source: Source,
}

impl Default for UiSettings {
    fn default() -> Self {
        Self {
            mode: UiMode::Off,
            scale_mode: UiScaleMode::Auto,
            scale: 1.0,
            hotkey: "alt+f10".to_owned(),
            mode_source: Source::Default,
            scale_source: Source::Default,
            hotkey_source: Source::Default,
        }
    }
}

/// `[log]`
#[derive(Clone, Debug, PartialEq)]
pub struct LogSettings {
    /// 0 = errors only, 1 = normal, 2 = verbose.
    pub level: i32,
    pub level_source: Source,
}

impl Default for LogSettings {
    fn default() -> Self {
        Self {
            level: 1,
            level_source: Source::Default,
        }
    }
}

/// `[fg]`
#[derive(Clone, Debug, PartialEq)]
pub struct FrameGenerationSettings {
    pub multiplier: Multiplier,
    pub multiplier_source: Source,
    /// The one frame-rate target: 0 = uncapped, N = capped, `target_auto` = match the monitor.
    pub target_fps: f64,
    pub target_auto: bool,
    pub target_fps_source: Source,
    pub present: Present,
    pub present_source: Source,
    pub tear_guard: TearGuard,
    pub tear_guard_source: Source,
}

impl Default for FrameGenerationSettings {
    fn default() -> Self {
        Self {
            multiplier: Multiplier::X2,
            multiplier_source: Source::Default,
            target_fps: 0.0,
            target_auto: false,
            target_fps_source: Source::Default,
            present: Present::Vrr,
            present_source: Source::Default,
            tear_guard: TearGuard::Scanline,
            tear_guard_source: Source::Default,
        }
    }
}

/// The effective configuration: what `sedici.toml` and the environment resolved to.
#[derive(Clone, Debug, PartialEq)]
pub struct Settings {
    pub limiter: LimiterSettings,
    pub log: LogSettings,
    pub ui: UiSettings,
    pub fg: FrameGenerationSettings,
    /// `[limiter] fps` / `SEDICI_LIMIT_FPS` as found, or -1 when absent. Folded into
    /// `fg.target_fps` at load.
    pub legacy_limiter_fps: f64,
    pub legacy_limiter_source: Source,
    /// Absolute path of the config file consulted, or `None` when the DLL directory is unknown.
    pub path: Option<PathBuf>,
    /// True when `sedici.toml` exists beside the DLL.
    pub file_exists: bool,
    /// True when the file existed and parsed.
    pub file_parsed: bool,
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            limiter: LimiterSettings::default(),
            log: LogSettings::default(),
            ui: UiSettings::default(),
            fg: FrameGenerationSettings::default(),
            legacy_limiter_fps: -1.0,
            legacy_limiter_source: Source::Default,
            path: None,
            file_exists: false,
            file_parsed: false,
        }
    }
}

#[derive(Clone, Debug, Default)]
pub struct LoadedSettings {
    pub settings: Settings,
    pub problems: Vec<String>,
    pub notes: Vec<String>,
}

impl LoadedSettings {
    /// The once-at-load report: problems first, then notes, then the resolved values with their
    /// provenance. One line per entry, in the order the C++ wrote them.
    pub fn report_lines(&self) -> Vec<String> {
        self.report_lines_for(concat!("config.rs ", env!("CARGO_PKG_VERSION")))
    }

    /// The report with a caller-supplied build identity (K101: a game-side sedici.log can mix
    /// builds, so each session must say which one wrote it).
    pub fn report_lines_for(&self, build: &str) -> Vec<String> {
        let mut lines = Vec::new();
        lines.extend(self.problems.iter().cloned());
        lines.extend(self.notes.iter().cloned());

        let config = &self.settings;
        if !config.file_exists {
            if config.path.is_none() {
                lines.push(
                    "sedici: could not resolve the DLL directory; using built-in defaults".into(),
                );
            } else {
                lines.push("sedici: no sedici.toml beside the DLL; using built-in defaults".into());
            }
        } else if !config.file_parsed {
            lines.push(
                "sedici: sedici.toml was found but could not be used; using built-in defaults"
                    .into(),
            );
        } else {
            let path = config
                .path
                .as_ref()
                .map_or_else(String::new, |p| p.display().to_string());
            lines.push(format!("sedici: read {path} (once, on the first ffx call)"));
        }

        let fg = &config.fg;
        let rows: [(&str, String, Source); 9] = [
            (
                "fg.present",
                format!("\"{}\"", fg.present.name()),
                fg.present_source,
            ),
            (
                "fg.tear_guard",
                format!("\"{}\"", fg.tear_guard.name()),
                fg.tear_guard_source,
            ),
            (
                "fg.multiplier",
                format!("\"{}\"", fg.multiplier.name()),
                fg.multiplier_source,
            ),
            ("fg.target_fps", target_text(fg), fg.target_fps_source),
            (
                "limiter.spin_us",
                config.limiter.spin_us.to_string(),
                config.limiter.spin_us_source,
            ),
            (
                "ui.mode",
                format!(
                    "\"{}\"",
                    match config.ui.mode {
                        UiMode::Mini => "mini",
                        UiMode::Full => "full",
                        UiMode::Off => "off",
                    }
                ),
                config.ui.mode_source,
            ),
            ("ui.scale", ui_scale_text(config), config.ui.scale_source),
            (
                "ui.hotkey",
                format!("\"{}\"", config.ui.hotkey),
                config.ui.hotkey_source,
            ),
            (
                "log.level",
                config.log.level.to_string(),
                config.log.level_source,
            ),
        ];

        let width = rows.iter().map(|(key, _, _)| key.len()).max().unwrap_or(0) + 2;
        for (key, value, source) in rows {
            lines.push(format!(
                "sedici:   {key:<width$}= {value} [{}]",
                source.name()
            ));
        }

        lines.push(format!("sedici: build {build}"));

        let mut effective = String::from("sedici: effective: target fps ");
        if fg.target_auto {
            effective.push_str(&format!(
                "auto (floor(Hz - Hz^2/3600) of the monitor, uncapped if unknown; spin {} us)",
                config.limiter.spin_us
            ));
        } else if fg.target_fps > 0.0 {
            effective.push_str(&format!(
                "{} output fps (spin {} us)",
                format_number(fg.target_fps),
                config.limiter.spin_us
            ));
        } else {
            effective.push_str("uncapped");
        }
        lines.push(effective);
        lines
    }
}

fn target_text(fg: &FrameGenerationSettings) -> String {
    if fg.target_auto {
        "\"auto\"".to_owned()
    } else {
        format_number(fg.target_fps)
    }
}

fn ui_scale_text(config: &Settings) -> String {
    if config.ui.scale_mode == UiScaleMode::Auto {
        "\"auto\"".to_owned()
    } else {
        format_number(f64::from(config.ui.scale))
    }
}

/// 37 -> "37", 36.5 -> "36.5", 36.500 -> "36.5". A whole number does not acquire a decimal point.
pub fn format_number(value: f64) -> String {
    let mut text = format!("{value:.3}");
    while text.ends_with('0') {
        text.pop();
    }
    if text.ends_with('.') {
        text.pop();
    }
    text
}

/// Strict non-negative decimal. Rejects signs, whitespace and trailing junk, so an override such as
/// "36.5 " or "-1" is reported instead of being half-parsed.
pub fn parse_decimal(text: &str) -> Option<f64> {
    if text.is_empty() || text.starts_with(['+', '-']) {
        return None;
    }
    if text.chars().any(char::is_whitespace) {
        return None;
    }
    let value: f64 = text.parse().ok()?;
    if !value.is_finite() || value < 0.0 {
        None
    } else {
        Some(value)
    }
}

/// "auto" (any case) or a number within [0, 1000].
pub fn parse_target(text: &str) -> Option<(f64, bool)> {
    if text.eq_ignore_ascii_case("auto") {
        return Some((0.0, true));
    }
    let value = parse_decimal(text)?;
    if value > 1000.0 {
        None
    } else {
        Some((value, false))
    }
}

/// A hotkey is one or more of alt/ctrl/shift plus exactly one key: `f1`..`f12`, a letter or a digit.
/// The file value is case-insensitive; it is stored lowercase.
pub fn valid_hotkey(text: &str) -> bool {
    if text.is_empty() || text.len() > 32 {
        return false;
    }

    let mut modifiers = 0;
    let mut has_key = false;
    for part in text.split('+') {
        if part.is_empty() {
            return false;
        }
        if part.eq_ignore_ascii_case("alt")
            || part.eq_ignore_ascii_case("ctrl")
            || part.eq_ignore_ascii_case("shift")
        {
            modifiers += 1;
        } else if !has_key {
            let bytes = part.as_bytes();
            if bytes.len() >= 2 && bytes[0].eq_ignore_ascii_case(&b'f') {
                let digits = &part[1..];
                if !digits.bytes().all(|b| b.is_ascii_digit()) {
                    return false;
                }
                let Ok(number) = digits.parse::<u32>() else {
                    return false;
                };
                if !(1..=12).contains(&number) {
                    return false;
                }
            } else if part.len() != 1
                || !(bytes[0].is_ascii_alphabetic() || bytes[0].is_ascii_digit())
            {
                return false;
            }
            has_key = true;
        } else {
            return false;
        }
    }
    modifiers > 0 && has_key
}

/// Output frames per real frame of the nominal schedule, used to turn a legacy real-frame cap into
/// an output target.
pub fn nominal_output_ratio(fg: &FrameGenerationSettings) -> f64 {
    if fg.present != Present::Vrr {
        return 2.0;
    }
    match fg.multiplier {
        Multiplier::X1_75 => 1.75,
        Multiplier::X1_5 => 1.5,
        Multiplier::X1_25 => 1.25,
        Multiplier::Adaptive => 1.0,
        Multiplier::X2 => 2.0,
    }
}

/// Environment overrides win over the file. An unrecognised value is reported and then ignored, so
/// the file (or the default) stays in effect rather than silently falling back to a default the
/// user did not ask for. `lookup` returns an unset variable as `None`; an empty or implausibly long
/// value is treated as unset, which is what the C++ `env_string` did.
pub fn apply_env_overrides(
    config: &mut Settings,
    lookup: impl Fn(&str) -> Option<String>,
    problems: &mut Vec<String>,
) {
    let env = |name: &str| -> Option<String> {
        lookup(name).filter(|value| !value.is_empty() && value.len() < 256)
    };

    if let Some(value) = env("SEDICI_LIMIT_FPS") {
        match parse_decimal(&value) {
            Some(parsed) if parsed <= 1000.0 => {
                config.legacy_limiter_fps = parsed;
                config.legacy_limiter_source = Source::Env;
            }
            _ => problems.push(format!(
                "SEDICI_LIMIT_FPS=\"{value}\": expected a number within [0, 1000] (0 = off) - \
                 override ignored"
            )),
        }
    }

    if let Some(value) = env("SEDICI_FG_PRESENT") {
        match Present::parse(&value) {
            Some(parsed) => {
                config.fg.present = parsed;
                config.fg.present_source = Source::Env;
            }
            None => problems.push(format!(
                "SEDICI_FG_PRESENT=\"{value}\": unknown value (expected \"vsync\", \"vrr\" or \
                 \"game\") - override ignored"
            )),
        }
    }

    if let Some(value) = env("SEDICI_FG_MULTIPLIER") {
        match Multiplier::parse(&value) {
            Some(parsed) => {
                config.fg.multiplier = parsed;
                config.fg.multiplier_source = Source::Env;
            }
            None => problems.push(format!(
                "SEDICI_FG_MULTIPLIER=\"{value}\": unknown value (expected \"2x\", \"1.75x\", \
                 \"1.5x\", \"1.25x\" or \"adaptive\") - override ignored"
            )),
        }
    }

    if let Some(value) = env("SEDICI_FG_TARGET_FPS") {
        match parse_target(&value) {
            Some((parsed, automatic)) => {
                config.fg.target_fps = parsed;
                config.fg.target_auto = automatic;
                config.fg.target_fps_source = Source::Env;
            }
            None => problems.push(format!(
                "SEDICI_FG_TARGET_FPS=\"{value}\": expected a number within [0, 1000] or \"auto\" \
                 - override ignored"
            )),
        }
    }

    if let Some(value) = env("SEDICI_FG_TEAR_GUARD") {
        match TearGuard::parse(&value) {
            Some(parsed) => {
                config.fg.tear_guard = parsed;
                config.fg.tear_guard_source = Source::Env;
            }
            None => problems.push(format!(
                "SEDICI_FG_TEAR_GUARD=\"{value}\": unknown value (expected \"scanline\", \
                 \"timer\" or \"off\") - override ignored"
            )),
        }
    }

    // Legacy variable, kept working unchanged: one digit, 0 to 2.
    if let Some(value) = env("MFSRFG_LOG") {
        if value.len() == 1 && matches!(value.as_bytes()[0], b'0'..=b'2') {
            config.log.level = i32::from(value.as_bytes()[0] - b'0');
            config.log.level_source = Source::Env;
        } else {
            problems.push(format!(
                "MFSRFG_LOG=\"{value}\": expected 0, 1 or 2 - override ignored"
            ));
        }
    }
}

/// Folds the legacy real-frame cap into the one target and notes the spin window.
/// Priority: SEDICI_FG_TARGET_FPS, SEDICI_LIMIT_FPS, fg.target_fps, limiter.fps.
pub fn finalize_target(config: &mut Settings, notes: &mut Vec<String>) {
    if config.legacy_limiter_fps >= 0.0 {
        let legacy_env = config.legacy_limiter_source == Source::Env;
        let name = if legacy_env {
            "SEDICI_LIMIT_FPS"
        } else {
            "limiter.fps"
        };
        let wins = if legacy_env {
            config.fg.target_fps_source != Source::Env
        } else {
            config.fg.target_fps_source == Source::Default
        };
        if wins {
            let ratio = nominal_output_ratio(&config.fg);
            config.fg.target_fps = legacy_limiter_to_target(config.legacy_limiter_fps, ratio);
            config.fg.target_auto = false;
            config.fg.target_fps_source = config.legacy_limiter_source;
            notes.push(format!(
                "sedici: {name} is deprecated (it capped real frames); read as fg.target_fps = {} \
                 output fps ({} x {}). Use fg.target_fps, a number or \"auto\"",
                format_number(config.fg.target_fps),
                format_number(config.legacy_limiter_fps),
                format_number(ratio)
            ));
        } else {
            notes.push(format!(
                "sedici: {name} is ignored because fg.target_fps is set"
            ));
        }
    }

    if !config.fg.target_auto && config.fg.target_fps > 0.0 {
        let cap = config.fg.target_fps / nominal_output_ratio(&config.fg);
        let interval_us = (1_000_000.0 / cap).round() as i64;
        if i64::from(config.limiter.spin_us) >= interval_us {
            notes.push(format!(
                "sedici.toml: limiter.spin_us: {} us is not shorter than the {interval_us} us \
                 frame interval; the timer wait is skipped and the limiter spins for the whole wait",
                config.limiter.spin_us
            ));
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use rstest::rstest;
    use std::collections::HashMap;

    #[rstest]
    #[case("vsync", Some(Present::Vsync))]
    #[case("VBLANK", Some(Present::Vsync))]
    #[case("Vrr", Some(Present::Vrr))]
    #[case("game", Some(Present::Game))]
    #[case("v-sync", None)]
    fn present_aliases(#[case] text: &str, #[case] want: Option<Present>) {
        assert_eq!(Present::parse(text), want);
    }

    #[rstest]
    #[case("2", Some(Multiplier::X2))]
    #[case("1.75X", Some(Multiplier::X1_75))]
    #[case("1.5", Some(Multiplier::X1_5))]
    #[case("1.25x", Some(Multiplier::X1_25))]
    #[case("adaptive", Some(Multiplier::Adaptive))]
    #[case("3x", None)]
    fn multiplier_aliases(#[case] text: &str, #[case] want: Option<Multiplier>) {
        assert_eq!(Multiplier::parse(text), want);
    }

    #[rstest]
    #[case("auto", Some((0.0, true)))]
    #[case("AUTO", Some((0.0, true)))]
    #[case("36.5", Some((36.5, false)))]
    #[case("1000", Some((1000.0, false)))]
    #[case("1000.1", None)]
    #[case("-1", None)]
    #[case("+1", None)]
    #[case("36.5 ", None)]
    #[case("", None)]
    #[case("inf", None)]
    fn target_values(#[case] text: &str, #[case] want: Option<(f64, bool)>) {
        assert_eq!(parse_target(text), want);
    }

    #[rstest]
    #[case("alt+f10", true)]
    #[case("ctrl+shift+f12", true)]
    #[case("alt+a", true)]
    #[case("alt+1", true)]
    #[case("a", false)] // no modifier
    #[case("f10", false)] // no modifier
    #[case("alt+", false)]
    #[case("alt+f13", false)]
    #[case("alt+f10+x", false)]
    #[case("alt+shift", false)] // modifiers only
    #[case("Alt+F10", true)] // case-insensitive, lowercased on load
    fn hotkeys(#[case] text: &str, #[case] valid: bool) {
        assert_eq!(valid_hotkey(text), valid);
    }

    #[rstest]
    #[case(37.0, "37")]
    #[case(36.5, "36.5")]
    #[case(36.9999, "37")]
    #[case(36.500, "36.5")]
    #[case(0.9999, "1")]
    fn numbers(#[case] value: f64, #[case] want: &str) {
        assert_eq!(format_number(value), want);
    }

    fn lookup(pairs: &[(&str, &str)]) -> impl Fn(&str) -> Option<String> {
        let map: HashMap<String, String> = pairs
            .iter()
            .map(|(k, v)| ((*k).to_owned(), (*v).to_owned()))
            .collect();
        move |name| map.get(name).cloned()
    }

    #[test]
    fn environment_overrides_win_and_report_bad_values() {
        let mut config = Settings::default();
        let mut problems = Vec::new();
        apply_env_overrides(
            &mut config,
            lookup(&[
                ("SEDICI_FG_TARGET_FPS", "auto"),
                ("SEDICI_FG_PRESENT", "game"),
                ("SEDICI_FG_MULTIPLIER", "1.5x"),
                ("SEDICI_FG_TEAR_GUARD", "off"),
                ("MFSRFG_LOG", "2"),
                ("SEDICI_LIMIT_FPS", "45"),
            ]),
            &mut problems,
        );
        assert!(problems.is_empty());
        assert!(config.fg.target_auto);
        assert_eq!(config.fg.present, Present::Game);
        assert_eq!(config.fg.present_source, Source::Env);
        assert_eq!(config.fg.multiplier, Multiplier::X1_5);
        assert_eq!(config.fg.tear_guard, TearGuard::Off);
        assert_eq!(config.log.level, 2);
        assert_eq!(config.log.level_source, Source::Env);
        assert_eq!(config.legacy_limiter_fps, 45.0);
        assert_eq!(config.legacy_limiter_source, Source::Env);
    }

    #[test]
    fn bad_environment_values_leave_the_file_value_alone() {
        let mut config = Settings::default();
        config.fg.present = Present::Vsync;
        config.fg.present_source = Source::File;
        let mut problems = Vec::new();
        apply_env_overrides(
            &mut config,
            lookup(&[
                ("SEDICI_FG_PRESENT", "nope"),
                ("SEDICI_FG_TARGET_FPS", "36.5 "),
                ("MFSRFG_LOG", "3"),
                ("SEDICI_LIMIT_FPS", ""),
            ]),
            &mut problems,
        );
        assert_eq!(config.fg.present, Present::Vsync);
        assert_eq!(config.fg.present_source, Source::File);
        assert_eq!(config.fg.target_fps, 0.0);
        assert_eq!(config.log.level, 1);
        assert_eq!(config.legacy_limiter_fps, -1.0);
        assert_eq!(problems.len(), 3);
        assert!(problems[0].starts_with("SEDICI_FG_PRESENT"));
    }

    #[test]
    fn legacy_limiter_becomes_an_output_target_at_the_nominal_ratio() {
        let mut config = Settings::default();
        config.legacy_limiter_fps = 25.0;
        config.legacy_limiter_source = Source::File;
        let mut notes = Vec::new();
        finalize_target(&mut config, &mut notes);
        assert_eq!(config.fg.target_fps, 50.0);
        assert_eq!(config.fg.target_fps_source, Source::File);
        assert!(notes[0].contains("limiter.fps is deprecated"));
        assert!(notes[0].contains("fg.target_fps = 50 output fps (25 x 2)"));
    }

    #[test]
    fn explicit_target_beats_a_legacy_file_cap_but_not_a_legacy_env_cap() {
        let mut config = Settings::default();
        config.fg.target_fps = 73.0;
        config.fg.target_fps_source = Source::File;
        config.legacy_limiter_fps = 25.0;
        config.legacy_limiter_source = Source::File;
        let mut notes = Vec::new();
        finalize_target(&mut config, &mut notes);
        assert_eq!(config.fg.target_fps, 73.0);
        assert!(notes[0].contains("is ignored because fg.target_fps is set"));

        let mut config = Settings::default();
        config.fg.target_fps = 73.0;
        config.fg.target_fps_source = Source::File;
        config.legacy_limiter_fps = 25.0;
        config.legacy_limiter_source = Source::Env;
        let mut notes = Vec::new();
        finalize_target(&mut config, &mut notes);
        assert_eq!(config.fg.target_fps, 50.0);
        assert_eq!(config.fg.target_fps_source, Source::Env);
    }

    #[test]
    fn spin_window_longer_than_the_frame_interval_is_noted() {
        let mut config = Settings::default();
        config.fg.target_fps = 60.0;
        config.limiter.spin_us = 40_000; // 60 output fps at 2x is 30 real fps: 33 333 us
        let mut notes = Vec::new();
        finalize_target(&mut config, &mut notes);
        assert_eq!(notes.len(), 1);
        assert!(notes[0].contains("the timer wait is skipped"));
    }

    #[test]
    fn report_names_every_key_with_its_provenance() {
        let mut loaded = LoadedSettings::default();
        loaded.settings.path = Some(std::path::PathBuf::from("C:/game/sedici.toml"));
        loaded.settings.limiter.spin_us = 500;
        loaded.settings.limiter.spin_us_source = Source::File;
        let lines = loaded.report_lines();
        assert!(lines
            .iter()
            .any(|l| l.contains("no sedici.toml beside the DLL")));
        assert!(lines
            .iter()
            .any(|l| l.contains("fg.present") && l.contains("\"vrr\" [default]")));
        assert!(lines.iter().any(|l| l.contains("= 500 [file]")));
        assert!(lines.iter().any(|l| l.starts_with("sedici: build")));
        assert!(lines
            .iter()
            .any(|l| l == "sedici: effective: target fps uncapped"));

        loaded.settings.path = None;
        let lines = loaded.report_lines();
        assert!(lines
            .iter()
            .any(|l| l.contains("could not resolve the DLL directory")));
    }
}
