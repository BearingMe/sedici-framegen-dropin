//! Real-frame limiter (ports `presentation/limiter.cpp`).
//!
//! Called from the swapchain Present/Present1 hook immediately before the original Present, with
//! the tick timestamp taken on entry. The hook sees exactly one Present per real frame, so capping
//! here caps the game's real frames; with 2x frame generation the output lands at about twice the
//! configured rate.
//!
//! Inert unless the target fps is greater than zero: when inert it does no work at all — no
//! timer, no wait, no allocation, no log.

use std::sync::Mutex;

use tracing::{error, info};

use crate::application::ports::clock::Clock;
use crate::domain::schedule::{limiter_step, LimiterStep};

/// Report cadence, matching the C++ `kReportEvery`.
pub const REPORT_EVERY: usize = 300;

/// Why the limiter stopped. Kept for the log line and tests; the game is never told.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LimiterError {
    /// No tick frequency from the platform.
    NoFrequency,
    /// The platform could not create or arm a waitable timer; the game continues uncapped.
    TimerUnavailable,
    /// A wait operation failed at runtime.
    WaitFailed,
}

/// What one real frame did, returned for tests and the presentation stats.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct FrameOutcome {
    /// The schedule reseeded because the frame was more than one interval late.
    pub reset: bool,
    /// The frame arrived at or after its deadline.
    pub late: bool,
    /// The limiter waited on this frame.
    pub waited: bool,
}

/// Rolling 300-frame window of measured values, in milliseconds.
#[derive(Debug, Default)]
struct Window {
    intervals: Vec<f64>,
    waits: Vec<f64>,
    overshoots: Vec<f64>,
    late: u64,
    resets: u64,
}

impl Window {
    fn clear(&mut self) {
        self.intervals.clear();
        self.waits.clear();
        self.overshoots.clear();
        self.late = 0;
        self.resets = 0;
    }
}

struct LimiterState {
    initialized: bool,
    disabled: bool,
    interval: u64,
    spin: u64,
    have_deadline: bool,
    deadline: u64,
    have_last_actual: bool,
    last_actual: u64,
    logged_mode: bool,
    logged_fg: bool,
    window: Window,
}

impl Default for LimiterState {
    fn default() -> Self {
        Self {
            initialized: false,
            disabled: false,
            interval: 0,
            spin: 0,
            have_deadline: false,
            deadline: 0,
            have_last_actual: false,
            last_actual: 0,
            logged_mode: false,
            logged_fg: false,
            window: Window::default(),
        }
    }
}

/// The limiter over a [`Clock`] and an fps source. Construct one per DLL lifetime.
pub struct Limiter<C: Clock> {
    clock: C,
    state: Mutex<LimiterState>,
}

/// The settings the limiter needs at the moment of use; both are live values from the settings
/// port, so the limiter never caches them across a configuration change.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct LimiterConfig {
    /// Real-frame cap in fps; `<= 0` disables the limiter.
    pub fps: f64,
    /// Busy-wait window before the deadline, in microseconds.
    pub spin_us: i64,
}

fn average(values: &[f64]) -> f64 {
    if values.is_empty() {
        0.0
    } else {
        values.iter().sum::<f64>() / values.len() as f64
    }
}

fn maximum(values: &[f64]) -> f64 {
    values.iter().copied().fold(0.0_f64, f64::max)
}

/// Nearest-rank percentile (same index arithmetic as the C++).
fn percentile(values: &[f64], fraction: f64) -> f64 {
    if values.is_empty() {
        return 0.0;
    }
    let mut sorted = values.to_vec();
    sorted.sort_by(|a, b| a.partial_cmp(b).unwrap_or(std::cmp::Ordering::Equal));
    let mut index = (fraction * sorted.len() as f64).ceil() as usize;
    index = index.clamp(1, sorted.len());
    sorted[index - 1]
}

/// Formats fps with three decimals and trailing zeros removed, matching the C++ log.
pub fn format_fps(fps: f64) -> String {
    let text = format!("{fps:.3}");
    let text = text.trim_end_matches('0');
    let text = text.strip_suffix('.').unwrap_or(text);
    text.to_string()
}

impl<C: Clock> Limiter<C> {
    pub fn new(clock: C) -> Self {
        Self { clock, state: Mutex::new(LimiterState::default()) }
    }

    /// Drops the schedule and allowed timer state after a live target change. The next Present
    /// initializes with the new target and reseeds instead of waiting out the old interval.
    pub fn on_config_changed(&self) {
        let mut state = self.state.lock().unwrap_or_else(|e| e.into_inner());
        *state = LimiterState::default();
    }

    /// True once the limiter has decided it cannot run (no frequency or a timer failure).
    pub fn disabled(&self) -> bool {
        self.state.lock().unwrap_or_else(|e| e.into_inner()).disabled
    }

    /// The measured interval between real presents in the current window, in milliseconds. For
    /// tests and the presentation stats.
    pub fn intervals_ms(&self) -> Vec<f64> {
        self.state.lock().unwrap_or_else(|e| e.into_inner()).window.intervals.clone()
    }

    /// Makes the frame's deadline decision, waits if needed, and records the frame. `now` is the
    /// tick taken on entry to the Present hook. `fg_active` labels the log lines.
    ///
    /// Never panics and never returns a game-facing error: bookkeeping failures just leave the
    /// game uncapped.
    pub fn before_present(&self, now: u64, config: LimiterConfig, fg_active: bool) -> FrameOutcome {
        if !(config.fps > 0.0) {
            return FrameOutcome::default();
        }

        let mut state = self.state.lock().unwrap_or_else(|e| e.into_inner());
        if !self.initialize(&mut state, config) {
            return FrameOutcome::default();
        }
        if !state.logged_mode || state.logged_fg != fg_active {
            state.logged_mode = true;
            state.logged_fg = fg_active;
            info!("{}", activation_line(config.fps, state.interval, config.spin_us, self.clock.frequency(), fg_active));
        }

        let LimiterStep { deadline, wait, late, reset } = limiter_step(now, state.deadline, state.have_deadline, state.interval);
        state.deadline = deadline;
        state.have_deadline = true;

        let mut timer_failed = false;
        if wait {
            let wait_target = if deadline > state.spin { deadline - state.spin } else { now };
            if wait_target > now && !self.clock.wait_until(wait_target) {
                timer_failed = true;
            }
            if !timer_failed {
                self.spin_until(deadline);
            }
        }

        let actual = self.clock.now();
        if timer_failed {
            state.disabled = true;
        }

        if state.have_last_actual {
            let elapsed = self.clock.ticks_to_ms(actual.saturating_sub(state.last_actual));
            state.window.intervals.push(elapsed);
        }
        state.last_actual = actual;
        state.have_last_actual = true;

        state.window.waits.push(self.clock.ticks_to_ms(actual.saturating_sub(now)));
        if wait && !timer_failed {
            let overshoot = if actual > deadline { self.clock.ticks_to_ms(actual - deadline) } else { 0.0 };
            state.window.overshoots.push(overshoot);
        }

        if late {
            state.window.late += 1;
        }
        if reset {
            state.window.resets += 1;
        }

        let stats = if state.window.intervals.len() >= REPORT_EVERY {
            let line = stats_line(&state.window, config.fps, fg_active);
            state.window.clear();
            Some(line)
        } else {
            None
        };

        // Log after dropping the lock would need owning the line; the strings are built under the
        // lock because formatting is cheap relative to the wait, matching the C++.
        if timer_failed {
            error!("sedici: limiter disabled: a waitable timer operation failed; the game continues uncapped");
        }
        if let Some(line) = stats {
            info!("{line}");
        }

        FrameOutcome { reset, late, waited: wait && !timer_failed }
    }

    fn initialize(&self, state: &mut LimiterState, config: LimiterConfig) -> bool {
        if state.initialized {
            return !state.disabled;
        }
        state.initialized = true;

        let frequency = self.clock.frequency();
        if frequency == 0 {
            state.disabled = true;
            error!("sedici: limiter disabled: QueryPerformanceFrequency failed");
            return false;
        }

        state.interval = ((frequency as f64 / config.fps).round() as u64).max(1);
        state.spin = (config.spin_us.max(0) as f64 * 1e-6 * frequency as f64).round() as u64;
        true
    }

    /// Spins on the clock for the last window before the deadline.
    fn spin_until(&self, deadline: u64) {
        while self.clock.now() < deadline {
            std::hint::spin_loop();
        }
    }
}

fn activation_line(fps: f64, interval: u64, spin_us: i64, frequency: u64, fg_active: bool) -> String {
    let interval_ms = if frequency == 0 { 0.0 } else { interval as f64 * 1000.0 / frequency as f64 };
    format!(
        "sedici: limiter active target={} fps interval={interval_ms:.3} ms spin={spin_us} us fg={}",
        format_fps(fps),
        if fg_active { "on" } else { "off" }
    )
}

fn stats_line(window: &Window, fps: f64, fg_active: bool) -> String {
    let mut out = format!(
        "sedici: limiter stats frames={} target_ms={:.3}",
        window.intervals.len(),
        if fps > 0.0 { 1000.0 / fps } else { 0.0 }
    );
    out.push_str(&format!(
        " interval_ms{{avg={:.3} p50={:.3} p99={:.3} max={:.3}}}",
        average(&window.intervals),
        percentile(&window.intervals, 0.50),
        percentile(&window.intervals, 0.99),
        maximum(&window.intervals)
    ));
    out.push_str(&format!(" wait_ms{{avg={:.3}}}", average(&window.waits)));
    if window.overshoots.is_empty() {
        out.push_str(" overshoot_ms=n/a");
    } else {
        out.push_str(&format!(
            " overshoot_ms{{avg={:.3} p99={:.3} max={:.3}}}",
            average(&window.overshoots),
            percentile(&window.overshoots, 0.99),
            maximum(&window.overshoots)
        ));
    }
    out.push_str(&format!(
        " late={} resets={} fg={}",
        window.late,
        window.resets,
        if fg_active { "on" } else { "off" }
    ));
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::application::ports::clock::FakeClock;
    use rstest::rstest;
    use std::sync::Arc;

    fn limiter(frequency: u64, start: u64) -> (Arc<FakeClock>, Limiter<Arc<FakeClock>>) {
        let clock = Arc::new(FakeClock::new(frequency, start));
        let limiter = Limiter::new(Arc::clone(&clock));
        (clock, limiter)
    }

    #[rstest]
    #[case(37.0, "37")]
    #[case(36.5, "36.5")]
    #[case(73.0, "73")]
    #[case(0.001, "0.001")]
    fn fps_text_matches_the_log(#[case] fps: f64, #[case] want: &str) {
        assert_eq!(format_fps(fps), want);
    }

    #[test]
    fn inert_when_target_is_zero() {
        let (clock, limiter) = limiter(1000, 100);
        let out = limiter.before_present(100, LimiterConfig { fps: 0.0, spin_us: 1000 }, false);
        assert_eq!(out, FrameOutcome::default());
        assert!(clock.waits().is_empty(), "no timer, no wait when the limiter is off");
        assert!(limiter.intervals_ms().is_empty());
    }

    #[test]
    fn first_frame_seeds_and_never_waits() {
        let (clock, limiter) = limiter(1000, 500);
        let out = limiter.before_present(500, LimiterConfig { fps: 100.0, spin_us: 0 }, false);
        assert_eq!(out, FrameOutcome::default());
        assert!(clock.waits().is_empty());
    }

    #[test]
    fn a_fast_game_is_held_to_one_interval_per_frame() {
        // 1000 Hz, 50 fps => 20 tick interval. Game finishes each frame in 8 ticks.
        let (clock, limiter) = limiter(1000, 0);
        let config = LimiterConfig { fps: 50.0, spin_us: 0 };
        limiter.before_present(0, config, false); // seed at 0
        let mut now = 0;
        for _ in 0..10 {
            now += 8;
            clock.set(now);
            limiter.before_present(now, config, false);
            now = clock.now(); // the limiter waited until the deadline
        }
        // Deadlines 20,40,...,200; each waited about one interval.
        assert_eq!(clock.waits(), vec![20, 40, 60, 80, 100, 120, 140, 160, 180, 200]);
        assert_eq!(clock.now(), 200);
    }

    #[test]
    fn a_slow_game_is_never_delayed() {
        // interval 20 ticks, game takes 50 ticks.
        let (clock, limiter) = limiter(1000, 0);
        let config = LimiterConfig { fps: 50.0, spin_us: 0 };
        limiter.before_present(0, config, false);
        let mut now = 0;
        for _ in 0..20 {
            now += 50;
            clock.set(now);
            limiter.before_present(now, config, false);
        }
        assert!(clock.waits().is_empty(), "a game slower than the cap is never delayed");
    }

    #[test]
    fn a_live_target_change_reseeds() {
        let (clock, limiter) = limiter(1000, 0);
        // Rate 10 fps: 100 tick interval. Seed, then one frame waits about 100 ticks.
        let slow = LimiterConfig { fps: 10.0, spin_us: 0 };
        limiter.before_present(0, slow, false);
        clock.set(5);
        limiter.before_present(5, slow, false);
        assert!(clock.now() >= 100);
        // Change target: reset, then the next frame seeds instead of waiting out the old interval.
        limiter.on_config_changed();
        clock.clear_waits();
        let fast = LimiterConfig { fps: 1000.0, spin_us: 0 };
        let before = clock.now();
        limiter.before_present(before, fast, false);
        assert!(clock.waits().is_empty() && clock.now() == before, "a reset reseeds without waiting");
    }

    #[test]
    fn a_failed_wait_disables_the_limiter() {
        let (clock, limiter) = limiter(1000, 0);
        let config = LimiterConfig { fps: 50.0, spin_us: 0 };
        limiter.before_present(0, config, false);
        clock.fail_waits();
        clock.set(1);
        let out = limiter.before_present(1, config, false);
        assert!(!out.waited);
        assert!(limiter.disabled());
        // Further frames do no work.
        clock.clear_waits();
        let out = limiter.before_present(2, config, false);
        assert_eq!(out, FrameOutcome::default());
        assert!(clock.waits().is_empty());
    }

    #[test]
    fn missing_frequency_disables() {
        let (_, limiter) = limiter(0, 0);
        let out = limiter.before_present(0, LimiterConfig { fps: 60.0, spin_us: 1000 }, false);
        assert_eq!(out, FrameOutcome::default());
        assert!(limiter.disabled());
    }

    #[test]
    fn reports_every_300_frames_through_tracing() {
        use std::sync::atomic::{AtomicUsize, Ordering};
        let events = Arc::new(AtomicUsize::new(0));
        let _guard = tracing::subscriber::set_default(CountingSubscriber(Arc::clone(&events)));

        let (clock, limiter) = limiter(1000, 0);
        let config = LimiterConfig { fps: 50.0, spin_us: 0 };
        limiter.before_present(0, config, false);
        for frame in 1..=REPORT_EVERY {
            clock.set((frame * 20) as u64); // exactly on the deadline: late, no wait
            limiter.before_present((frame * 20) as u64, config, false);
        }
        // Activation line plus one stats line once the window fills.
        assert!(events.load(Ordering::SeqCst) >= 2, "activation and 300-frame stats are logged");
        assert!(limiter.intervals_ms().is_empty(), "the window clears after the report");
    }

    /// Counts tracing events without pulling `tracing-subscriber` into the application layer.
    struct CountingSubscriber(Arc<std::sync::atomic::AtomicUsize>);

    impl tracing::Subscriber for CountingSubscriber {
        fn enabled(&self, _metadata: &tracing::Metadata<'_>) -> bool {
            true
        }
        fn new_span(&self, _span: &tracing::span::Attributes<'_>) -> tracing::span::Id {
            tracing::span::Id::from_u64(1)
        }
        fn record(&self, _span: &tracing::span::Id, _values: &tracing::span::Record<'_>) {}
        fn record_follows_from(&self, _span: &tracing::span::Id, _follows: &tracing::span::Id) {}
        fn event(&self, _event: &tracing::Event<'_>) {
            self.0.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
        }
        fn enter(&self, _span: &tracing::span::Id) {}
        fn exit(&self, _span: &tracing::span::Id) {}
    }
}
