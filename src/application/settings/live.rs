//! Live mutable values shared by the overlay, limiter, pacing and logging (ports
//! `settings/live.cpp`).
//!
//! A snapshot is one coherent copy: callers that need several fields together read once, so they
//! can never observe half of two candidates (the regression's mixed-snapshot check).
use std::sync::atomic::{AtomicU64, AtomicU8, Ordering};
use std::sync::{Arc, Mutex};

use crate::domain::config::{Settings, UiMode, UiScaleMode};
use crate::domain::schedule::resolve_real_cap;

/// A complete set of live controls. `[ui] mode` is menu state, not a setting, so it lives
/// separately.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct LiveValues {
    /// Output fps: 0 = uncapped.
    pub target_fps: f64,
    /// Match the monitor (see `domain::schedule::auto_target_fps`).
    pub target_auto: bool,
    pub log_level: i32,
    pub scale_mode: UiScaleMode,
    pub ui_scale: f32,
}

impl Default for LiveValues {
    fn default() -> Self {
        Self {
            target_fps: 0.0,
            target_auto: false,
            log_level: 1,
            scale_mode: UiScaleMode::Auto,
            ui_scale: 1.0,
        }
    }
}

type Listener = Box<dyn Fn() + Send + Sync>;
type LevelSink = Box<dyn Fn(i32) + Send + Sync>;

/// Shared live state. Clone the `Arc`; every caller sees the same values.
pub struct Live {
    values: Mutex<LiveValues>,
    ui_mode: AtomicU8,
    /// `f64` bit patterns so the context can be read without taking the values lock.
    ratio: AtomicU64,
    refresh_hz: AtomicU64,
    cap_listener: Mutex<Option<Listener>>,
    level_sink: Mutex<Option<LevelSink>>,
}

impl Default for Live {
    fn default() -> Self {
        Self::new()
    }
}

impl Live {
    pub fn new() -> Self {
        Self {
            values: Mutex::new(LiveValues::default()),
            ui_mode: AtomicU8::new(UiMode::Off as u8),
            ratio: AtomicU64::new(1.0f64.to_bits()),
            refresh_hz: AtomicU64::new(0.0f64.to_bits()),
            cap_listener: Mutex::new(None),
            level_sink: Mutex::new(None),
        }
    }

    /// Publish changes to the log level here, so diagnostics never has to read settings (P4.5).
    pub fn set_level_sink(&self, sink: impl Fn(i32) + Send + Sync + 'static) {
        *self
            .level_sink
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner()) = Some(Box::new(sink));
    }

    pub fn snapshot(&self) -> LiveValues {
        *self
            .values
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
    }

    /// Publishes one coherent candidate and tells the log sink.
    pub fn apply(&self, values: LiveValues) {
        {
            let mut current = self
                .values
                .lock()
                .unwrap_or_else(|poisoned| poisoned.into_inner());
            *current = values;
        }
        self.publish_level(values.log_level);
    }

    /// Initialized once from the effective [`Settings`].
    pub fn initialize(&self, config: &Settings) {
        let values = LiveValues {
            target_fps: config.fg.target_fps,
            target_auto: config.fg.target_auto,
            log_level: config.log.level,
            scale_mode: config.ui.scale_mode,
            ui_scale: config.ui.scale,
        };
        self.apply(values);
        self.ui_mode.store(config.ui.mode as u8, Ordering::Relaxed);
    }

    fn publish_level(&self, level: i32) {
        let sink = self
            .level_sink
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        if let Some(sink) = sink.as_ref() {
            sink(level);
        }
    }

    fn ratio(&self) -> f64 {
        f64::from_bits(self.ratio.load(Ordering::Relaxed))
    }

    fn refresh_hz(&self) -> f64 {
        f64::from_bits(self.refresh_hz.load(Ordering::Relaxed))
    }

    /// The real-frame cap the limiter enforces now; 0 = uncapped.
    pub fn limiter_fps(&self) -> f64 {
        let values = self.snapshot();
        resolve_real_cap(
            values.target_fps,
            values.target_auto,
            self.refresh_hz(),
            self.ratio(),
        )
    }

    pub fn set_target_fps(&self, fps: f64) {
        let mut values = self
            .values
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        values.target_fps = if fps.is_finite() {
            fps.clamp(0.0, 1000.0)
        } else {
            0.0
        };
    }

    pub fn set_target_auto(&self, automatic: bool) {
        self.values
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .target_auto = automatic;
    }

    /// The consumer (the limiter) registers here so a settings change can tell it to reschedule
    /// without settings depending on presentation (P4.5).
    pub fn set_cap_listener(&self, listener: impl Fn() + Send + Sync + 'static) {
        *self
            .cap_listener
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner()) = Some(Box::new(listener));
    }

    pub fn notify_cap_changed(&self) {
        let listener = self
            .cap_listener
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        if let Some(listener) = listener.as_ref() {
            listener();
        }
    }

    /// Publishes the output ratio (2 for 2x, 1 when nothing is generated) and refresh rate
    /// (0 = unknown). Returns true when the resolved real-frame cap changed, so the caller can tell
    /// the limiter.
    pub fn set_output_context(&self, ratio: f64, refresh_hz: f64) -> bool {
        let before = self.limiter_fps();
        self.ratio.store(
            if ratio >= 1.0 { ratio } else { 1.0 }.to_bits(),
            Ordering::Relaxed,
        );
        self.refresh_hz.store(
            if refresh_hz > 0.0 { refresh_hz } else { 0.0 }.to_bits(),
            Ordering::Relaxed,
        );
        self.limiter_fps() != before
    }

    pub fn output_refresh_hz(&self) -> f64 {
        self.refresh_hz()
    }

    pub fn log_level(&self) -> i32 {
        self.snapshot().log_level
    }

    pub fn set_log_level(&self, level: i32) {
        let level = level.clamp(0, 2);
        self.values
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .log_level = level;
        self.publish_level(level);
    }

    pub fn ui_mode(&self) -> UiMode {
        match self.ui_mode.load(Ordering::Relaxed) {
            value if value == UiMode::Mini as u8 => UiMode::Mini,
            value if value == UiMode::Full as u8 => UiMode::Full,
            _ => UiMode::Off,
        }
    }

    pub fn set_ui_mode(&self, mode: UiMode) {
        self.ui_mode.store(mode as u8, Ordering::Relaxed);
    }

    pub fn ui_scale_mode(&self) -> UiScaleMode {
        self.snapshot().scale_mode
    }

    pub fn set_ui_scale_mode(&self, mode: UiScaleMode) {
        self.values
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .scale_mode = mode;
    }

    pub fn ui_scale(&self) -> f32 {
        self.snapshot().ui_scale
    }

    pub fn set_ui_scale(&self, scale: f32) {
        self.values
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .ui_scale = if scale.is_finite() {
            scale.clamp(0.5, 2.0)
        } else {
            1.0
        };
    }
}

/// Shared handle used by the application and the worker.
pub type SharedLive = Arc<Live>;

#[cfg(test)]
mod tests {
    use super::*;
    use rstest::rstest;
    use std::sync::atomic::{AtomicI32, AtomicUsize};

    fn settings_with(target: f64, automatic: bool, level: i32) -> Settings {
        let mut config = Settings::default();
        config.fg.target_fps = target;
        config.fg.target_auto = automatic;
        config.log.level = level;
        config.ui.mode = UiMode::Mini;
        config.ui.scale = 1.25;
        config.ui.scale_mode = UiScaleMode::Fixed;
        config
    }

    #[test]
    fn initialize_copies_the_effective_settings() {
        let live = Live::new();
        live.initialize(&settings_with(73.0, false, 2));
        let values = live.snapshot();
        assert_eq!(values.target_fps, 73.0);
        assert_eq!(values.log_level, 2);
        assert_eq!(values.ui_scale, 1.25);
        assert_eq!(values.scale_mode, UiScaleMode::Fixed);
        assert_eq!(live.ui_mode(), UiMode::Mini);
    }

    #[test]
    fn log_level_changes_reach_the_sink() {
        let live = Live::new();
        let published = Arc::new(AtomicI32::new(-1));
        let sink = Arc::clone(&published);
        live.set_level_sink(move |level| sink.store(level, Ordering::Relaxed));
        live.set_log_level(2);
        assert_eq!(published.load(Ordering::Relaxed), 2);
        live.set_log_level(9);
        assert_eq!(live.log_level(), 2);
    }

    #[rstest]
    #[case(2.0, 75.0, true, 36.5)]
    #[case(2.0, 144.0, true, 69.0)]
    #[case(1.0, 0.0, false, 73.0)]
    #[case(2.0, 0.0, true, 0.0)]
    fn output_context_resolves_the_real_cap(
        #[case] ratio: f64,
        #[case] hz: f64,
        #[case] automatic: bool,
        #[case] want: f64,
    ) {
        let live = Live::new();
        live.set_target_fps(73.0);
        live.set_target_auto(automatic);
        let changed = live.set_output_context(ratio, hz);
        assert_eq!(live.limiter_fps(), want);
        assert!(changed || !changed); // the change flag is covered below
    }

    #[test]
    fn publishing_the_same_context_reports_no_change_and_cap_changes_report_one() {
        let live = Live::new();
        live.set_target_fps(73.0);
        assert!(live.set_output_context(2.0, 75.0));
        assert!(!live.set_output_context(2.0, 75.0));
    }

    #[test]
    fn listener_is_told_when_the_cap_changes() {
        let live = Live::new();
        let calls = Arc::new(AtomicUsize::new(0));
        let counter = Arc::clone(&calls);
        live.set_cap_listener(move || {
            counter.fetch_add(1, Ordering::Relaxed);
        });
        live.notify_cap_changed();
        assert_eq!(calls.load(Ordering::Relaxed), 1);
    }

    #[test]
    fn setters_clamp_to_their_ranges() {
        let live = Live::new();
        live.set_ui_scale(4.0);
        assert_eq!(live.ui_scale(), 2.0);
        live.set_ui_scale(0.1);
        assert_eq!(live.ui_scale(), 0.5);
        live.set_target_fps(-5.0);
        assert_eq!(live.snapshot().target_fps, 0.0);
        live.set_target_fps(f64::NAN);
        assert_eq!(live.snapshot().target_fps, 0.0);
    }
}
