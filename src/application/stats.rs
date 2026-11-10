//! Periodic prepare/present stats, the prepare stall detector and the `diag:` line
//! (ports diagnostics/stats and status/diag).
//!
//! Everything here is a value over injected inputs: QPC timestamps and the QPC frequency are
//! passed in, nothing reads a clock, a file or a global. The adapter that owns the QPC counter
//! feeds [`Stats`] and emits the returned lines with `tracing`.

use std::fmt::Write as _;

/// A report is emitted every this many prepares.
pub const REPORT_EVERY: usize = 300;

/// What one prepare desc says, under either of its two type ids. The deprecated desc predates
/// `reset` (its field is `unused_reset`), so `reset` is `None` there and the line says `n/a`.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct PrepareInfo {
    pub frame_time_delta: f32,
    pub frame_id: u64,
    /// `Some` only for the V2 desc.
    pub reset: Option<bool>,
    pub render_width: u32,
    pub render_height: u32,
}

/// Lines the caller should log at level 1 after one dispatch.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct StatsOutput {
    /// `stall prepare interval=..` when a stall was detected and not rate limited.
    pub stall: Option<String>,
    /// `stats prepares=..` when this prepare completed a report window.
    pub report: Option<String>,
}

// ---------------------------------------------------------------------------
// Stall detector
//
// Run I of docs/findings/002 showed five episodic stalls in a 30 s capture: a prepare
// interval of 24-72 ms followed immediately by a sub-2 ms one, i.e. a hitch and a catch-up
// present. This detector reports each one as it happens, with the context that might explain
// it (frame generation reset, presents queued behind the prepare, or a swapchain/configure
// event close in time).
//
//  - The baseline is a rolling median of the last 61 prepare intervals, taken before the new
//    sample is added, so the stall cannot hide itself in its own median.
//  - A stall must clear both a relative threshold (3x the median) and an absolute floor
//    (25 ms), so a slow but steady run never triggers.
//  - At most one line per second, so a genuinely broken minute cannot flood a 64 KB log.
// ---------------------------------------------------------------------------
const STALL_MEDIAN_WINDOW: usize = 61;
const STALL_MIN_SAMPLES: usize = 10;
const STALL_RATIO: f64 = 3.0;
const STALL_FLOOR_MS: f64 = 25.0;
const STALL_LOG_INTERVAL_MS: f64 = 1000.0;

const EVENT_RING: usize = 32;
const EVENT_WINDOW_MS: f64 = 1000.0;

/// Counters of what the log itself did. Part of the `diag:` contract; the logging adapter
/// fills the same shape.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct LogStats {
    /// Lines accepted into the ring.
    pub queued: u64,
    /// Lines that reached sedici.log.
    pub written: u64,
    /// Refused because the ring was full.
    pub dropped: u64,
    /// Refused by the per-message rate limit.
    pub suppressed: u64,
    /// Accepted but discarded because the file could not be written.
    pub lost: u64,
    /// Failed file writes (one per batch).
    pub sink_failures: u64,
    pub depth: u32,
    pub max_depth: u32,
    /// False from a failed write until the next success.
    pub sink_ok: bool,
}

impl Default for LogStats {
    fn default() -> Self {
        Self { queued: 0, written: 0, dropped: 0, suppressed: 0, lost: 0, sink_failures: 0, depth: 0, max_depth: 0, sink_ok: true }
    }
}

/// What Sedici's own safety machinery has done. Every counter is a count of something that
/// really happened; anything Sedici cannot measure is named in [`DIAG_UNAVAILABLE`] instead of
/// being reported as zero.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct DiagSnapshot {
    pub log: LogStats,
    /// Timed-out swapchains kept alive until their GPU work retires.
    pub swapchains_parked: u32,
    pub swapchains_parked_total: u64,
    pub swapchains_released_total: u64,
    /// Refused destroys waiting for their work to retire.
    pub router_owners_parked: u32,
    /// Settings jobs reported as not responding.
    pub settings_io_stalls: u64,
    /// A reported-stalled worker is still running.
    pub settings_io_stalled_now: bool,
}

/// Present timestamps come from the CPU. They are not physical flips and not input-to-photon
/// latency, and Sedici has no source for either.
pub const DIAG_UNAVAILABLE: &str = "physical_flip_time,input_to_photon_latency";

/// One line for the log, or `None` when nothing is out of the ordinary (no drops, suppression,
/// sink failure or parked objects). Reported with the periodic stats.
pub fn diag_notable_line(s: &DiagSnapshot) -> Option<String> {
    let notable = s.log.dropped != 0
        || s.log.suppressed != 0
        || s.log.lost != 0
        || s.log.sink_failures != 0
        || !s.log.sink_ok
        || s.swapchains_parked != 0
        || s.router_owners_parked != 0
        || s.settings_io_stalls != 0
        || s.settings_io_stalled_now;
    if !notable {
        return None;
    }
    Some(format!(
        "diag: log{{queued={} written={} dropped={} suppressed={} lost={} sink_failures={} max_depth={} sink={}}} \
         retention{{swapchains_parked={} parked_total={} released_total={} router_parked={}}} \
         settings_io{{stalls={} stalled_now={}}} unavailable{{{DIAG_UNAVAILABLE}}}",
        s.log.queued,
        s.log.written,
        s.log.dropped,
        s.log.suppressed,
        s.log.lost,
        s.log.sink_failures,
        s.log.max_depth,
        if s.log.sink_ok { "ok" } else { "failing" },
        s.swapchains_parked,
        s.swapchains_parked_total,
        s.swapchains_released_total,
        s.router_owners_parked,
        s.settings_io_stalls,
        if s.settings_io_stalled_now { "yes" } else { "no" },
    ))
}

/// Fixed-size ring; when `count` is below capacity the live entries are `[0, count)`.
#[derive(Clone, Debug)]
struct Ring {
    values: [f64; STALL_MEDIAN_WINDOW],
    next: usize,
    count: usize,
}

impl Default for Ring {
    fn default() -> Self {
        Self { values: [0.0; STALL_MEDIAN_WINDOW], next: 0, count: 0 }
    }
}

impl Ring {
    fn push(&mut self, value: f64) {
        self.values[self.next] = value;
        self.next = (self.next + 1) % STALL_MEDIAN_WINDOW;
        if self.count < STALL_MEDIAN_WINDOW {
            self.count += 1;
        }
    }

    /// The upper median of the live entries; 0 when empty.
    fn median(&self) -> f64 {
        if self.count == 0 {
            return 0.0;
        }
        let mut scratch = [0.0; STALL_MEDIAN_WINDOW];
        scratch[..self.count].copy_from_slice(&self.values[..self.count]);
        scratch[..self.count].sort_by(f64::total_cmp);
        scratch[self.count / 2]
    }
}

#[derive(Clone, Copy, Debug, Default)]
struct Event {
    qpc: u64,
    kind: &'static str,
}

#[derive(Clone, Debug, Default)]
struct EventRing {
    values: [Option<Event>; EVENT_RING],
    next: usize,
    count: usize,
}

impl EventRing {
    fn push(&mut self, event: Event) {
        self.values[self.next] = Some(event);
        self.next = (self.next + 1) % EVENT_RING;
        if self.count < EVENT_RING {
            self.count += 1;
        }
    }

    fn iter(&self) -> impl Iterator<Item = &Event> {
        self.values.iter().take(self.count).flatten()
    }
}

#[derive(Clone, Debug, Default)]
struct Window {
    frame_time_delta: Vec<f64>,
    prepare_interval: Vec<f64>,
    present_interval: Vec<f64>,
    presents_per_prepare: Vec<u32>,

    last_prepare_qpc: u64,
    last_present_qpc: u64,
    have_prepare: bool,
    have_present: bool,
    presents_since_prepare: u32,

    stall_baseline: Ring,
    events: EventRing,
    have_stall_log: bool,
    last_stall_log_qpc: u64,
    stall_count: u64,
    last_stall_interval_ms: f64,
    render_width: u32,
    render_height: u32,
}

/// The stats window. One instance per DLL, behind the adapter's lock.
#[derive(Clone, Debug)]
pub struct Stats {
    qpc_frequency: u64,
    /// The C++ skips present/event recording below log level 1.
    verbose: bool,
    window: Window,
    swapchain_events: u64,
    real_dropped: u64,
}

impl Stats {
    pub fn new(qpc_frequency: u64) -> Self {
        Self {
            qpc_frequency,
            verbose: true,
            window: Window::default(),
            swapchain_events: 0,
            real_dropped: 0,
        }
    }

    /// Mirrors the live log level's effect on the C++: below level 1, presents and swapchain
    /// events are counted but not recorded for the report.
    pub fn set_verbose(&mut self, verbose: bool) {
        self.verbose = verbose;
    }

    pub fn qpc_frequency(&self) -> u64 {
        self.qpc_frequency
    }

    /// Called after a successful `ffxDispatch` with the prepare desc's contents.
    pub fn on_dispatch(&mut self, info: &PrepareInfo, now_qpc: u64) -> StatsOutput {
        if self.qpc_frequency == 0 {
            return StatsOutput::default();
        }
        let frequency = self.qpc_frequency;
        let mut out = StatsOutput::default();
        let w = &mut self.window;

        if w.have_prepare {
            let interval_ms = ticks_to_ms(now_qpc.saturating_sub(w.last_prepare_qpc), frequency);

            // Compare against the baseline that predates this sample.
            let median_ms = w.stall_baseline.median();
            let enough_samples = w.stall_baseline.count >= STALL_MIN_SAMPLES;
            let rate_limited = w.have_stall_log
                && ticks_to_ms(now_qpc.saturating_sub(w.last_stall_log_qpc), frequency) < STALL_LOG_INTERVAL_MS;

            if enough_samples && interval_ms > STALL_RATIO * median_ms && interval_ms >= STALL_FLOOR_MS {
                w.stall_count += 1;
                w.last_stall_interval_ms = interval_ms;
                if !rate_limited {
                    out.stall = Some(stall_line(w, info, interval_ms, median_ms, now_qpc, self.qpc_frequency));
                    w.have_stall_log = true;
                    w.last_stall_log_qpc = now_qpc;
                }
            }

            w.stall_baseline.push(interval_ms);
            w.prepare_interval.push(interval_ms);
            w.presents_per_prepare.push(w.presents_since_prepare);
        }

        w.frame_time_delta.push(f64::from(info.frame_time_delta));
        w.render_width = info.render_width;
        w.render_height = info.render_height;
        w.last_prepare_qpc = now_qpc;
        w.have_prepare = true;
        w.presents_since_prepare = 0;

        if w.frame_time_delta.len() >= REPORT_EVERY {
            out.report = Some(stats_line(w));
            reset(w);
        }
        out
    }

    /// Called from the wrapper Present hook with a QPC timestamp.
    pub fn on_present(&mut self, qpc: u64) {
        if !self.verbose || self.qpc_frequency == 0 {
            return;
        }
        let frequency = self.qpc_frequency;
        let w = &mut self.window;
        if w.have_present {
            w.present_interval.push(ticks_to_ms(qpc.saturating_sub(w.last_present_qpc), frequency));
        }
        w.last_present_qpc = qpc;
        w.have_present = true;
        w.presents_since_prepare += 1;
    }

    /// A frame generation swapchain context was created, or a successful configure reached the
    /// backend. Counted at any log level; recorded in the event ring only when verbose.
    pub fn on_swapchain_event(&mut self, kind: &'static str, now_qpc: u64) {
        self.swapchain_events += 1;
        if !self.verbose || self.qpc_frequency == 0 {
            return;
        }
        self.window.events.push(Event { qpc: now_qpc, kind });
    }

    pub fn swapchain_event_count(&self) -> u64 {
        self.swapchain_events
    }

    /// A real frame the presenter dropped because a later output was already due (K125).
    pub fn on_real_dropped(&mut self) {
        self.real_dropped += 1;
    }

    pub fn real_dropped(&self) -> u64 {
        self.real_dropped
    }

    pub fn stall_count(&self) -> u64 {
        self.window.stall_count
    }

    pub fn last_stall_ms(&self) -> f64 {
        self.window.last_stall_interval_ms
    }

    pub fn render_size(&self) -> (u32, u32) {
        (self.window.render_width, self.window.render_height)
    }
}

fn reset(w: &mut Window) {
    w.frame_time_delta.clear();
    w.prepare_interval.clear();
    w.present_interval.clear();
    w.presents_per_prepare.clear();
    w.have_present = false;
    w.presents_since_prepare = 0;
    // last_prepare_qpc is intentionally kept: it is the interval baseline for the stall
    // detector, which spans stats windows.
}

fn average(values: &[f64]) -> f64 {
    if values.is_empty() {
        return 0.0;
    }
    values.iter().sum::<f64>() / values.len() as f64
}

fn minimum(values: &[f64]) -> f64 {
    values.iter().copied().reduce(f64::min).unwrap_or(0.0)
}

fn maximum(values: &[f64]) -> f64 {
    values.iter().copied().reduce(f64::max).unwrap_or(0.0)
}

fn stddev(values: &[f64]) -> f64 {
    if values.len() < 2 {
        return 0.0;
    }
    let mean = average(values);
    let sum: f64 = values.iter().map(|v| (v - mean) * (v - mean)).sum();
    (sum / values.len() as f64).sqrt()
}

fn percentile(values: &[f64], fraction: f64) -> f64 {
    if values.is_empty() {
        return 0.0;
    }
    let mut sorted = values.to_vec();
    sorted.sort_by(f64::total_cmp);
    let mut index = (fraction * sorted.len() as f64).ceil() as usize;
    if index == 0 {
        index = 1;
    }
    if index > sorted.len() {
        index = sorted.len();
    }
    sorted[index - 1]
}

/// `stats prepares=..` with every float at three decimals, as the C++ `fixed` stream writes it.
fn stats_line(w: &Window) -> String {
    let mut out = String::new();
    let _ = write!(out, "stats prepares={}", w.frame_time_delta.len());

    let _ = write!(
        out,
        " frameTimeDelta_ms{{avg={:.3} min={:.3} max={:.3} sd={:.3}}}",
        average(&w.frame_time_delta),
        minimum(&w.frame_time_delta),
        maximum(&w.frame_time_delta),
        stddev(&w.frame_time_delta),
    );
    let _ = write!(
        out,
        " prepareQpc_ms{{avg={:.3} min={:.3} max={:.3} sd={:.3}}}",
        average(&w.prepare_interval),
        minimum(&w.prepare_interval),
        maximum(&w.prepare_interval),
        stddev(&w.prepare_interval),
    );

    if w.present_interval.is_empty() {
        out.push_str(" presentInterval_ms=n/a");
    } else {
        let _ = write!(
            out,
            " presentInterval_ms{{avg={:.3} p50={:.3} p95={:.3} p99={:.3} max={:.3}}}",
            average(&w.present_interval),
            percentile(&w.present_interval, 0.50),
            percentile(&w.present_interval, 0.95),
            percentile(&w.present_interval, 0.99),
            maximum(&w.present_interval),
        );
    }

    if w.presents_per_prepare.is_empty() {
        out.push_str(" presentsPerPrepare=n/a");
    } else {
        let sum: f64 = w.presents_per_prepare.iter().map(|&v| f64::from(v)).sum();
        let min = w.presents_per_prepare.iter().copied().min().unwrap_or(0);
        let max = w.presents_per_prepare.iter().copied().max().unwrap_or(0);
        let _ = write!(
            out,
            " presentsPerPrepare{{avg={:.3} min={} max={}}}",
            sum / w.presents_per_prepare.len() as f64,
            min,
            max,
        );
    }
    out
}

/// QPC ticks to milliseconds; 0 when the frequency is unknown.
fn ticks_to_ms(delta: u64, frequency: u64) -> f64 {
    if frequency == 0 {
        0.0
    } else {
        delta as f64 * 1000.0 / frequency as f64
    }
}

/// The one-line stall report.
fn stall_line(w: &Window, info: &PrepareInfo, interval_ms: f64, median_ms: f64, now_qpc: u64, frequency: u64) -> String {
    let mut out = String::new();
    let ratio = if median_ms > 0.0 { interval_ms / median_ms } else { 0.0 };
    let _ = write!(
        out,
        "sedici: stall prepare interval={interval_ms:.3} ms median={median_ms:.3} ms ratio={ratio:.3}x frameID={}",
        info.frame_id
    );
    match info.reset {
        Some(reset) => {
            let _ = write!(out, " reset={reset}");
        }
        None => out.push_str(" reset=n/a"),
    }
    let _ = write!(out, " presentsSincePrepare={}", w.presents_since_prepare);

    // Anything that might have disturbed the swapchain in the last second.
    let mut recent = 0;
    let mut newest: Option<&Event> = None;
    let mut newest_age_ms = 0.0;
    for event in w.events.iter() {
        let age_ms = ticks_to_ms(now_qpc.saturating_sub(event.qpc), frequency);
        if age_ms > EVENT_WINDOW_MS {
            continue;
        }
        recent += 1;
        if newest.is_none_or(|n| event.qpc > n.qpc) {
            newest = Some(event);
            newest_age_ms = age_ms;
        }
    }
    let _ = write!(out, " swapchainEvents1s{{count={recent}");
    if let Some(newest) = newest {
        let _ = write!(out, " last={} {newest_age_ms:.3} ms ago", newest.kind);
    }
    out.push('}');
    let _ = write!(out, " qpc={now_qpc}");
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use rstest::rstest;

    const FREQ: u64 = 1_000_000; // one tick per microsecond

    fn ms_ticks(ms: f64) -> u64 {
        (ms * 1000.0) as u64
    }

    fn prepare(frame_time_delta: f32, frame_id: u64) -> PrepareInfo {
        PrepareInfo { frame_time_delta, frame_id, reset: None, render_width: 1920, render_height: 1080 }
    }

    #[test]
    fn report_comes_every_300_prepares() {
        let mut stats = Stats::new(FREQ);
        let mut reports = Vec::new();
        for i in 0..600u64 {
            let now = ms_ticks(16.0) * i;
            if let Some(report) = stats.on_dispatch(&prepare(16.0, i), now).report {
                reports.push((i, report));
            }
        }
        assert_eq!(reports.len(), 2);
        assert_eq!(reports[0].0, 299);
        assert_eq!(reports[1].0, 599);
        let line = &reports[0].1;
        assert!(line.starts_with("stats prepares=300"), "{line}");
        assert!(line.contains("frameTimeDelta_ms{avg=16.000 min=16.000 max=16.000 sd=0.000}"), "{line}");
        assert!(line.contains("prepareQpc_ms{avg=16.000 min=16.000 max=16.000 sd=0.000}"), "{line}");
        assert!(line.contains("presentInterval_ms=n/a"), "{line}");
        assert!(line.contains("presentsPerPrepare{avg=0.000 min=0 max=0}"), "{line}");
    }

    #[test]
    fn presents_are_counted_per_prepare_and_as_intervals() {
        let mut stats = Stats::new(FREQ);
        let mut now = 0u64;
        for i in 0..300u64 {
            stats.on_present(now);
            stats.on_present(now + ms_ticks(8.0));
            let out = stats.on_dispatch(&prepare(16.0, i), now + ms_ticks(16.0));
            if let Some(report) = out.report {
                assert!(report.contains("presentsPerPrepare{avg=2.000 min=2 max=2}"), "{report}");
                assert!(report.contains("presentInterval_ms{avg=8.000 p50=8.000"), "{report}");
                return;
            }
            now += ms_ticks(16.0);
        }
        panic!("no report");
    }

    #[rstest]
    // A 100 ms interval against a 16 ms median clears both thresholds.
    #[case(100.0, true)]
    // 30 ms is 1.875x the median: the ratio fails.
    #[case(30.0, false)]
    // 60 ms is 3.75x but only just above the floor; still a stall.
    #[case(60.0, true)]
    fn stall_detector(#[case] stall_ms: f64, #[case] expect: bool) {
        let mut stats = Stats::new(FREQ);
        let mut last = 0u64;
        for i in 0..20u64 {
            stats.on_dispatch(&prepare(16.0, i), last);
            last += ms_ticks(16.0);
        }
        // `last` is one interval past the final dispatch; the stall interval is measured from
        // that dispatch, so 100 ms means exactly 100 ms.
        let previous = last - ms_ticks(16.0);
        let out = stats.on_dispatch(&prepare(16.0, 99), previous + ms_ticks(stall_ms));
        assert_eq!(out.stall.is_some(), expect);
        if expect {
            assert_eq!(stats.stall_count(), 1);
            assert_eq!(stats.last_stall_ms(), stall_ms);
            let line = out.stall.unwrap();
            assert!(line.starts_with(&format!("sedici: stall prepare interval={stall_ms:.3} ms median=16.000 ms ratio=")), "{line}");
            assert!(line.contains("frameID=99"), "{line}");
            assert!(line.contains("reset=n/a"), "{line}");
            assert!(line.contains("swapchainEvents1s{count=0}"), "{line}");
        } else {
            assert_eq!(stats.stall_count(), 0);
        }
    }

    #[test]
    fn a_stall_needs_ten_baseline_samples() {
        let mut stats = Stats::new(FREQ);
        let mut now = 0u64;
        for i in 0..5u64 {
            stats.on_dispatch(&prepare(16.0, i), now);
            now += ms_ticks(16.0);
        }
        let out = stats.on_dispatch(&prepare(16.0, 9), now + ms_ticks(200.0));
        assert!(out.stall.is_none());
    }

    #[test]
    fn stalls_are_rate_limited_but_still_counted() {
        let mut stats = Stats::new(FREQ);
        let mut now = 0u64;
        for i in 0..20u64 {
            stats.on_dispatch(&prepare(16.0, i), now);
            now += ms_ticks(16.0);
        }
        let first = stats.on_dispatch(&prepare(16.0, 100), now + ms_ticks(100.0));
        assert!(first.stall.is_some());
        now += ms_ticks(100.0);
        let second = stats.on_dispatch(&prepare(16.0, 101), now + ms_ticks(100.0));
        assert!(second.stall.is_none(), "a second stall within a second is not logged");
        assert_eq!(stats.stall_count(), 2, "but it is still counted");
    }

    #[test]
    fn stall_line_names_the_newest_recent_swapchain_event() {
        let mut stats = Stats::new(FREQ);
        let mut now = 0u64;
        for i in 0..20u64 {
            stats.on_dispatch(&prepare(16.0, i), now);
            now += ms_ticks(16.0);
        }
        stats.on_swapchain_event("resize", now);
        let out = stats.on_dispatch(&prepare(16.0, 7), now + ms_ticks(100.0));
        let line = out.stall.expect("stall");
        assert!(line.contains("swapchainEvents1s{count=1 last=resize 100.000 ms ago}"), "{line}");
        assert_eq!(stats.swapchain_event_count(), 1);
    }

    #[test]
    fn reset_is_reported_only_for_the_v2_desc() {
        let mut stats = Stats::new(FREQ);
        let mut now = 0u64;
        for i in 0..20u64 {
            stats.on_dispatch(&prepare(16.0, i), now);
            now += ms_ticks(16.0);
        }
        let mut v2 = prepare(16.0, 42);
        v2.reset = Some(true);
        let line = stats.on_dispatch(&v2, now + ms_ticks(100.0)).stall.expect("stall");
        assert!(line.contains(" reset=true"), "{line}");
        assert_eq!(stats.render_size(), (1920, 1080));
    }

    #[test]
    fn non_verbose_records_neither_presents_nor_events() {
        let mut stats = Stats::new(FREQ);
        stats.set_verbose(false);
        stats.on_present(0);
        stats.on_present(ms_ticks(16.0));
        stats.on_swapchain_event("create", 0);
        assert_eq!(stats.swapchain_event_count(), 1, "the count is kept at any level");
        assert!(stats.window.events.iter().next().is_none());
        assert!(stats.window.present_interval.is_empty());
    }

    #[test]
    fn real_drops_are_counted() {
        let mut stats = Stats::new(FREQ);
        stats.on_real_dropped();
        stats.on_real_dropped();
        assert_eq!(stats.real_dropped(), 2);
    }

    #[test]
    fn zero_frequency_records_nothing() {
        let mut stats = Stats::new(0);
        assert_eq!(stats.on_dispatch(&prepare(16.0, 0), 0), StatsOutput::default());
    }

    #[rstest]
    #[case(DiagSnapshot::default(), false)]
    #[case(DiagSnapshot { log: LogStats { dropped: 1, ..LogStats::default() }, ..DiagSnapshot::default() }, true)]
    #[case(DiagSnapshot { log: LogStats { sink_ok: false, ..LogStats::default() }, ..DiagSnapshot::default() }, true)]
    #[case(DiagSnapshot { swapchains_parked: 2, ..DiagSnapshot::default() }, true)]
    #[case(DiagSnapshot { settings_io_stalled_now: true, ..DiagSnapshot::default() }, true)]
    fn diag_line_is_only_written_when_notable(#[case] snapshot: DiagSnapshot, #[case] notable: bool) {
        assert_eq!(diag_notable_line(&snapshot).is_some(), notable);
    }

    #[test]
    fn diag_line_shape() {
        let snapshot = DiagSnapshot {
            log: LogStats {
                queued: 10,
                written: 8,
                dropped: 1,
                suppressed: 2,
                lost: 3,
                sink_failures: 4,
                depth: 1,
                max_depth: 7,
                sink_ok: false,
            },
            swapchains_parked: 1,
            swapchains_parked_total: 5,
            swapchains_released_total: 4,
            router_owners_parked: 2,
            settings_io_stalls: 1,
            settings_io_stalled_now: true,
        };
        let line = diag_notable_line(&snapshot).expect("notable");
        assert_eq!(
            line,
            "diag: log{queued=10 written=8 dropped=1 suppressed=2 lost=3 sink_failures=4 max_depth=7 sink=failing} \
             retention{swapchains_parked=1 parked_total=5 released_total=4 router_parked=2} \
             settings_io{stalls=1 stalled_now=yes} unavailable{physical_flip_time,input_to_photon_latency}"
        );
    }
}
