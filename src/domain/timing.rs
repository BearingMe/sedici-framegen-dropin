//! Real/output interval rings and percentiles (ports diagnostics/timing).
//!
//! A 360-sample ring of frame intervals per stream, with the summary the overlay shows
//! (average, p50, p99, max, fps). Time is injected as QPC ticks plus the frequency, so the
//! ring stays clock-free and testable; the adapter that owns the QPC counter feeds it.

/// Samples kept per stream, oldest overwritten first.
pub const TIMING_SAMPLES: usize = 360;

/// An interval over ten seconds is a pause, not a frame: the C++ ignores it but still moves
/// the baseline, so the next interval is measured from the frame that resumed.
const MAX_INTERVAL_MS: f64 = 10_000.0;

/// Summary over one ring, in the units the overlay displays.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct TimingStats {
    pub average_ms: f32,
    pub p50_ms: f32,
    pub p99_ms: f32,
    pub max_ms: f32,
    pub fps: f32,
    pub count: usize,
}

/// Fixed-capacity interval ring. `head` is the next write slot; when `count == TIMING_SAMPLES`
/// the oldest sample lives at `head`.
#[derive(Debug)]
pub struct TimingRing {
    samples: [f32; TIMING_SAMPLES],
    head: usize,
    count: usize,
    last_qpc: u64,
    have_last: bool,
}

impl Default for TimingRing {
    fn default() -> Self {
        Self {
            samples: [0.0; TIMING_SAMPLES],
            head: 0,
            count: 0,
            last_qpc: 0,
            have_last: false,
        }
    }
}

impl TimingRing {
    pub fn new() -> Self {
        Self::default()
    }

    /// Records one present at `qpc`. A non-increasing timestamp or a zero frequency is not a
    /// frame boundary; the baseline still moves so the next interval starts from this present.
    pub fn push(&mut self, qpc: u64, frequency: u64) {
        if frequency == 0 {
            return;
        }
        if self.have_last && qpc > self.last_qpc {
            let ms = (qpc - self.last_qpc) as f64 * 1000.0 / frequency as f64;
            if ms > 0.0 && ms < MAX_INTERVAL_MS {
                self.samples[self.head] = ms as f32;
                self.head = (self.head + 1) % TIMING_SAMPLES;
                if self.count < TIMING_SAMPLES {
                    self.count += 1;
                }
            }
        }
        self.last_qpc = qpc;
        self.have_last = true;
    }

    pub fn count(&self) -> usize {
        self.count
    }

    /// Summary of the samples currently in the ring.
    pub fn stats(&self) -> TimingStats {
        if self.count == 0 {
            return TimingStats::default();
        }
        let mut values = self.chronological();
        values.sort_by(|a, b| a.total_cmp(b));

        let sum: f64 = values.iter().map(|&v| f64::from(v)).sum();
        let average = (sum / values.len() as f64) as f32;
        let percentile = |fraction: f64| -> f32 {
            let mut index = (fraction * values.len() as f64).ceil() as usize;
            if index == 0 {
                index = 1;
            }
            if index > values.len() {
                index = values.len();
            }
            values[index - 1]
        };
        let max = values[values.len() - 1];
        TimingStats {
            average_ms: average,
            p50_ms: percentile(0.50),
            p99_ms: percentile(0.99),
            max_ms: max,
            fps: if average > 0.0 { 1000.0 / average } else { 0.0 },
            count: self.count,
        }
    }

    /// Samples oldest first, for a plot.
    pub fn chronological(&self) -> Vec<f32> {
        let start = (self.head + TIMING_SAMPLES - self.count) % TIMING_SAMPLES;
        (0..self.count)
            .map(|i| self.samples[(start + i) % TIMING_SAMPLES])
            .collect()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use rstest::rstest;

    #[rstest]
    #[case(&[], TimingStats::default())]
    #[case(&[10.0], TimingStats { average_ms: 10.0, p50_ms: 10.0, p99_ms: 10.0, max_ms: 10.0, fps: 100.0, count: 1 })]
    #[case(&[10.0, 20.0], TimingStats { average_ms: 15.0, p50_ms: 10.0, p99_ms: 20.0, max_ms: 20.0, fps: 1000.0 / 15.0, count: 2 })]
    fn summaries(#[case] values: &[f32], #[case] want: TimingStats) {
        let mut ring = TimingRing::new();
        let mut t = 1_000_000u64; // 1 ms per 1000 ticks at 1 MHz
        for &v in values {
            ring.push(t, 1_000_000);
            t += (v * 1000.0) as u64;
        }
        if !values.is_empty() {
            ring.push(t, 1_000_000); // the final interval is measured at this present
        }
        let got = ring.stats();
        assert_eq!(got.count, want.count);
        assert!((got.average_ms - want.average_ms).abs() < 1e-4, "{got:?}");
        assert!((got.p50_ms - want.p50_ms).abs() < 1e-4, "{got:?}");
        assert!((got.p99_ms - want.p99_ms).abs() < 1e-4, "{got:?}");
        assert!((got.max_ms - want.max_ms).abs() < 1e-4, "{got:?}");
        assert!((got.fps - want.fps).abs() < 1e-2, "{got:?}");
    }

    #[test]
    fn ring_wraps_and_keeps_chronological_order() {
        let mut ring = TimingRing::new();
        let mut t = 0u64;
        // 400 intervals of 1 ms: only the newest 360 survive.
        for _ in 0..=400 {
            ring.push(t, 1_000_000);
            t += 1000;
        }
        assert_eq!(ring.count(), TIMING_SAMPLES);
        let samples = ring.chronological();
        assert_eq!(samples.len(), TIMING_SAMPLES);
        assert!(samples.iter().all(|&s| (s - 1.0).abs() < 1e-6));
    }

    #[test]
    fn pause_is_ignored_but_moves_the_baseline() {
        let mut ring = TimingRing::new();
        ring.push(0, 1_000_000);
        ring.push(1_000_000, 1_000_000); // 1000 ms
        ring.push(20_000_000, 1_000_000); // 19000 ms: dropped
        ring.push(20_002_000, 1_000_000); // 2 ms: measured from the resuming frame
        let samples = ring.chronological();
        assert_eq!(samples.len(), 2);
        assert!((samples[0] - 1000.0).abs() < 1e-3);
        assert!((samples[1] - 2.0).abs() < 1e-3);
    }

    #[test]
    fn zero_frequency_records_nothing() {
        let mut ring = TimingRing::new();
        ring.push(1, 0);
        ring.push(2, 0);
        assert_eq!(ring.count(), 0);
    }
}
