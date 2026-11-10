//! Output pacing math (ports presentation/pacing).
//!
//! The wrapper generates from the current pair N-1/N before either output, then presents
//! midpoint(N-1,N) followed by real N. It never carries an older midpoint past a real frame
//! that is newer than it; the first cycle only seeds history. No clocks, no D3D12: entry times
//! arrive as seconds on one monotonic clock, so every decision here is checkable offline.

/// Smallest interval the pacing math will believe (480 fps); anything else falls back to 60.
const MIN_INTERVAL: f64 = 1.0 / 480.0;
const MAX_INTERVAL: f64 = 1.0;
/// Two-sample mean real-frame interval at which generation stops (just over 50 ms, 20 fps)
/// and resumes (about 21.7 fps), and the largest single interval that may be interpolated
/// across (a longer gap is a discontinuity, not a frame).
const SLOW_ENTER: f64 = 0.0505;
const SLOW_EXIT: f64 = 0.046;
const SAMPLE_CAP: f64 = 0.1;

/// At most two generated frames plus the real one.
pub const MAX_PLANNED_OUTPUTS: usize = 3;

fn clamp_interval(interval: f64) -> f64 {
    if !(interval > 0.0) {
        return 1.0 / 60.0; // also rejects NaN
    }
    interval.clamp(MIN_INTERVAL, MAX_INTERVAL)
}

/// QPC ticks to seconds; 0 when the frequency is unknown.
pub fn ticks_to_seconds(ticks: u64, frequency: u64) -> f64 {
    if frequency == 0 {
        0.0
    } else {
        ticks as f64 / frequency as f64
    }
}

/// A later output whose content-time deadline is due supersedes this output, regardless of
/// whether either output is real or generated. No later output means keep it.
pub fn later_output_is_due(has_later: bool, later_content: f64, latency: f64, now: f64) -> bool {
    has_later && later_content + latency <= now
}

/// One cycle's decision for the midpoint/real pair.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct PacingInput {
    /// Seconds, monotonic.
    pub now: f64,
    /// Previous wrapper entry, never a delayed submission.
    pub last_cycle: f64,
    /// Smoothed game frame interval, seconds.
    pub interval: f64,
    /// False before the first real Present.
    pub cycle_started: bool,
    /// A midpoint exists and is still current.
    pub generated_ready: bool,
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct PacingOutput {
    pub present_generated: bool,
    /// Seconds before the first present of this cycle.
    pub first_wait: f64,
    /// Seconds between the generated and real present.
    pub second_wait: f64,
    /// Absolute deadline anchored at wrapper entry.
    pub real_deadline: f64,
}

/// No wait before the midpoint. The real-frame deadline is at most half an interval after
/// entry; work/API time already spent consumes that budget.
pub fn plan_present(input: &PacingInput) -> PacingOutput {
    let mut output = PacingOutput { real_deadline: input.now, ..Default::default() };
    output.present_generated = input.generated_ready && input.cycle_started;
    if !input.cycle_started {
        // No temporal pair yet: never show the seeded real image as a midpoint.
        return output;
    }
    // Menus/FG-off and failed generation must remain real-frame-only, without inheriting a
    // timing delay from an earlier generated cycle.
    if !input.generated_ready {
        return output;
    }

    let interval = clamp_interval(input.interval);
    let half = 0.5 * interval;

    // Use entries for both sides of the comparison: do not wait for
    // last submission + interval, which accumulates the previous first wait and submission
    // overhead into the next first wait.
    let target = input.last_cycle + interval;
    // A midpoint whose slot is more than half a cycle late must not become a backlog. Keep the
    // real frame moving instead of showing stale output.
    if input.now > target + half {
        output.present_generated = false;
    }

    // Without a midpoint the real frame should not be held at all.
    output.second_wait = if output.present_generated { half } else { 0.0 };
    output.real_deadline = input.now + output.second_wait;
    output
}

/// One anchor drives both production and the sustained-loop check, so a delayed submission
/// never feeds into the next cycle's entry-based interval estimate.
#[derive(Clone, Copy, Debug)]
pub struct PacingState {
    last_entry: f64,
    interval: f64,
    cycle_started: bool,
    interval_valid: bool,
}

impl Default for PacingState {
    fn default() -> Self {
        Self { last_entry: 0.0, interval: 1.0 / 60.0, cycle_started: false, interval_valid: false }
    }
}

impl PacingState {
    pub fn new() -> Self {
        Self::default()
    }

    /// The smoothed interval; the closed-loop check reads it.
    pub fn interval(&self) -> f64 {
        self.interval
    }

    pub fn plan(&mut self, entry: f64, generated_ready: bool) -> PacingOutput {
        if self.cycle_started {
            let delta = entry - self.last_entry;
            if delta > 1e-4 && delta < 1.0 {
                self.interval = if self.interval_valid {
                    self.interval * 0.9 + delta * 0.1
                } else {
                    delta
                };
                self.interval_valid = true;
            }
        }
        let input = PacingInput {
            now: entry,
            last_cycle: self.last_entry,
            interval: self.interval,
            cycle_started: self.cycle_started,
            generated_ready,
        };
        let output = plan_present(&input);
        self.last_entry = entry;
        self.cycle_started = true;
        output
    }
}

/// One frame the schedule shows, with its content time on the entry clock.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct PlannedOutput {
    /// In (0, 1); 1 means the real frame itself.
    pub alpha: f32,
    pub real: bool,
    /// Seconds on the entry clock.
    pub content: f64,
}

impl Default for PlannedOutput {
    fn default() -> Self {
        Self { alpha: 1.0, real: true, content: 0.0 }
    }
}

/// What one real frame's Present should show, in order.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct OutputPlan {
    outputs: [PlannedOutput; MAX_PLANNED_OUTPUTS],
    count: usize,
}

impl OutputPlan {
    pub fn outputs(&self) -> &[PlannedOutput] {
        &self.outputs[..self.count]
    }

    pub fn count(&self) -> usize {
        self.count
    }

    pub fn generated(&self) -> usize {
        self.outputs().iter().filter(|o| !o.real).count()
    }

    fn push(&mut self, output: PlannedOutput) {
        if self.count < MAX_PLANNED_OUTPUTS {
            self.outputs[self.count] = output;
            self.count += 1;
        }
    }

    fn real_only(entry: f64) -> Self {
        let mut plan = Self::default();
        plan.push(PlannedOutput { alpha: 1.0, real: true, content: entry });
        plan
    }
}

/// Output schedule for `[fg] present = "vrr"`: which frames to show for each real frame, and
/// at what content time, for a fixed output/real ratio or an adaptive output-rate target.
///
/// Outputs sit on an even grid in content time (the game's Present clock). A grid point
/// between real frames N-1 and N becomes a generated frame at
/// `alpha = (t - t[N-1]) / (t[N] - t[N-1])`; a point on N is the real frame. Below 2x some
/// real frames are never shown on their own: 1.5x shows every second real frame, 1.25x every
/// fourth. Fixed ratio p/q: a pattern repeating every q real frames with p outputs. Adaptive:
/// grid points every 1/target seconds, a point within a tenth of an output period of the real
/// frame showing the real frame instead.
#[derive(Clone, Debug)]
pub struct OutputSchedule {
    adaptive: bool,
    numerator: u32,
    denominator: u32,
    target: f64,
    started: bool,
    previous: f64,
    /// Fixed: real frames into the current pattern, 1..denominator.
    index: u32,
    /// Adaptive: content time of the next grid point.
    next_slot: f64,
    /// Recent real-frame interval, for hitch detection.
    interval: f64,
    /// Previous real-frame interval, for the two-sample mean.
    last_sample: f64,
    /// Smoothed rate below ~20 fps: real frames only (hysteresis).
    slow: bool,
}

impl Default for OutputSchedule {
    fn default() -> Self {
        Self {
            adaptive: false,
            numerator: 2,
            denominator: 1,
            target: 0.0,
            started: false,
            previous: 0.0,
            index: 0,
            next_slot: 0.0,
            interval: 0.0,
            last_sample: 0.0,
            slow: false,
        }
    }
}

impl OutputSchedule {
    pub fn new() -> Self {
        Self::default()
    }

    /// `numerator/denominator` within [1, 2], e.g. 3/2. Resets the pattern. An out-of-range
    /// ratio falls back to 2x.
    pub fn set_ratio(&mut self, numerator: u32, denominator: u32) {
        let (numerator, denominator) = if denominator == 0
            || numerator < denominator
            || u64::from(numerator) > 2 * u64::from(denominator)
        {
            (2, 1)
        } else {
            (numerator, denominator)
        };
        self.adaptive = false;
        self.numerator = numerator;
        self.denominator = denominator;
        self.reset();
    }

    /// Output frames per second to aim for. Resets the grid.
    pub fn set_target(&mut self, fps: f64) {
        self.adaptive = true;
        self.target = if fps.is_finite() { fps.clamp(1.0, 1000.0) } else { 60.0 };
        self.reset();
    }

    /// Forget history: the next plan shows only its real frame.
    pub fn reset(&mut self) {
        self.started = false;
        self.interval = 0.0;
        self.index = 0;
        self.slow = false;
        self.last_sample = 0.0;
    }

    pub fn adaptive(&self) -> bool {
        self.adaptive
    }

    pub fn target(&self) -> f64 {
        self.target
    }

    pub fn numerator(&self) -> u32 {
        self.numerator
    }

    pub fn denominator(&self) -> u32 {
        self.denominator
    }

    /// `entry`: seconds at this real frame's Present. The first frame after a reset, after an
    /// interval over 50 ms (below 20 real fps) or over 2.5x the recent one, returns just the
    /// real frame.
    pub fn plan(&mut self, entry: f64) -> OutputPlan {
        let interval = entry - self.previous;
        // A hitch (more than 2.5x the recent interval) is a discontinuity too: spreading
        // generated frames across it would replay the stall in slow motion. The recent
        // interval still follows a genuine slowdown, by at most 2.5x per frame.
        let hitch = self.interval > 0.0 && interval > 2.5 * self.interval;
        if self.started && interval > 1e-4 && interval < 0.25 {
            self.interval = if self.interval > 0.0 {
                0.9 * self.interval + 0.1 * interval.min(2.5 * self.interval)
            } else {
                interval
            };
        }
        // Below 20 real fps interpolation would need over 50 ms of extra latency and replays
        // loading stutters (157 ms at 14 fps in finding 009): show real frames only until the
        // rate recovers. The cutoff applies to the mean of the last two intervals with
        // hysteresis (K95). A gap over 100 ms is a discontinuity and forgets the history.
        if !self.started || interval <= 1e-4 || interval > SAMPLE_CAP {
            self.last_sample = 0.0;
        } else {
            if self.last_sample > 0.0 {
                let recent = 0.5 * (interval + self.last_sample);
                if recent > SLOW_ENTER {
                    self.slow = true;
                } else if recent < SLOW_EXIT {
                    self.slow = false;
                }
            } else if interval > 1.1 * SLOW_ENTER {
                self.slow = true; // a lone sample needs a 10% margin to count as slow
            }
            self.last_sample = interval;
        }
        let continuous =
            self.started && interval > 1e-4 && interval <= SAMPLE_CAP && !hitch && !self.slow;
        let previous = self.previous;
        self.previous = entry;
        self.started = true;
        if !continuous {
            self.index = 0;
            self.next_slot = entry + if self.adaptive { 1.0 / self.target } else { 0.0 };
            return OutputPlan::real_only(entry);
        }

        let mut plan = OutputPlan::default();
        let adaptive = self.adaptive;
        let add_generated = |plan: &mut OutputPlan, alpha: f64| {
            if plan.count() >= MAX_PLANNED_OUTPUTS || plan.generated() >= 2 {
                return;
            }
            let alpha = alpha.clamp(0.02, 0.98);
            plan.push(PlannedOutput { alpha: alpha as f32, real: false, content: previous + alpha * interval });
        };

        if !adaptive {
            // Grid points m with m*q/p in (n-1, n]: alpha = (m*q - (n-1)*p) / p.
            let p = u64::from(self.numerator);
            let q = u64::from(self.denominator);
            self.index = self.index % self.denominator + 1;
            let n = u64::from(self.index);
            let first = ((n - 1) * p) / q + 1;
            let last = (n * p) / q;
            for m in first..=last {
                let numerator = m * q - (n - 1) * p;
                if numerator >= p {
                    plan.push(PlannedOutput { alpha: 1.0, real: true, content: entry });
                } else {
                    add_generated(&mut plan, numerator as f64 / p as f64);
                }
            }
            return plan;
        }

        let period = 1.0 / self.target;
        // A real frame stands in for a grid point only when it is within a tenth of an output
        // period; at a quarter real frames visibly broke the even spacing when the real rate
        // was close to the target.
        let snap = 0.1 * period;
        // After a hitch the grid may be behind the previous real frame; skip the points that
        // can no longer be shown rather than replaying them.
        if self.next_slot <= previous {
            self.next_slot += ((previous - self.next_slot) / period + 1e-9).ceil() * period;
        }
        let mut guard = 0;
        while guard < 64 && self.next_slot <= entry + snap {
            guard += 1;
            if (entry - self.next_slot).abs() <= snap {
                plan.push(PlannedOutput { alpha: 1.0, real: true, content: entry });
                self.next_slot += period;
                break; // the real frame closes this interval
            }
            add_generated(&mut plan, (self.next_slot - previous) / interval);
            self.next_slot += period;
        }
        plan
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use rstest::rstest;

    #[test]
    fn qpc_conversion() {
        assert_eq!(ticks_to_seconds(30_000_000, 10_000_000), 3.0);
        let uptime = ticks_to_seconds(987_654_321_000_000, 10_000_000);
        let later = ticks_to_seconds(987_654_321_277_778, 10_000_000);
        assert!(((later - uptime) - 1.0 / 36.0).abs() < 1e-7);
        assert_eq!(ticks_to_seconds(1, 0), 0.0);
    }

    #[rstest]
    // First cycle: seed only, no wait.
    #[case(PacingInput { now: 0.0, last_cycle: 0.0, interval: 0.0, cycle_started: false, generated_ready: true },
           PacingOutput { present_generated: false, first_wait: 0.0, second_wait: 0.0, real_deadline: 0.0 })]
    // On time: midpoint then real, half an interval apart.
    #[case(PacingInput { now: 10.0 + 1.0 / 60.0, last_cycle: 10.0, interval: 1.0 / 60.0, cycle_started: true, generated_ready: true },
           PacingOutput { present_generated: true, first_wait: 0.0, second_wait: 1.0 / 120.0, real_deadline: 10.0 + 1.0 / 60.0 + 1.0 / 120.0 })]
    // Three intervals late: the midpoint is stale, the real frame is not delayed.
    #[case(PacingInput { now: 10.0 + 3.0 / 60.0, last_cycle: 10.0, interval: 1.0 / 60.0, cycle_started: true, generated_ready: true },
           PacingOutput { present_generated: false, first_wait: 0.0, second_wait: 0.0, real_deadline: 10.0 + 3.0 / 60.0 })]
    // No midpoint: never hold the real frame.
    #[case(PacingInput { now: 10.0, last_cycle: 10.0, interval: 1.0 / 60.0, cycle_started: true, generated_ready: false },
           PacingOutput { present_generated: false, first_wait: 0.0, second_wait: 0.0, real_deadline: 10.0 })]
    fn plan_present_cases(#[case] input: PacingInput, #[case] want: PacingOutput) {
        let got = plan_present(&input);
        assert_eq!(got.present_generated, want.present_generated);
        assert!(got.first_wait.abs() < 1e-9);
        assert!((got.second_wait - want.second_wait).abs() < 1e-9, "{got:?}");
        assert!((got.real_deadline - want.real_deadline).abs() < 1e-9, "{got:?}");
    }

    #[test]
    fn invalid_interval_falls_back_to_a_bounded_wait() {
        let input = PacingInput { now: 10.0 + 1.0 / 60.0, last_cycle: 10.0, interval: 0.0, cycle_started: true, generated_ready: true };
        let plan = plan_present(&input);
        assert!(plan.second_wait > 0.0 && plan.second_wait <= 0.5);
    }

    /// Nine minutes of closed loop at 36 real fps, including FG off/on and a 200 ms source
    /// stall, with no real sleeps: the schedule must stay bounded and recover.
    #[test]
    fn closed_loop_stays_at_36_real_and_72_output() {
        let mut pacing = PacingState::new();
        let period = 1.0 / 36.0;
        let mut entry = 1_000_000.0;
        let mut bounded = true;
        let mut recovered = true;
        let mut outputs = 0u32;
        for cycle in 0..20_000u32 {
            let enabled = cycle < 5000 || cycle >= 5100;
            if cycle == 5000 || cycle == 5100 {
                pacing = PacingState::new();
            }
            if cycle == 10_000 {
                entry += 0.2;
            }
            let next = pacing.plan(entry, enabled);
            // Work already spent consumes the entry-based budget instead of extending it.
            let work = if cycle % 2 == 1 { 0.002 } else { 0.004 };
            let real = (entry + work).max(next.real_deadline);
            let following_entry = (entry + period).max(real + 0.004);
            bounded &= next.first_wait == 0.0 && next.real_deadline <= entry + pacing.interval() * 0.5 + 1e-8;
            if !enabled {
                bounded &= !next.present_generated && next.real_deadline == entry;
            }
            if cycle > 10_500 {
                recovered &= (pacing.interval() - period).abs() < 1e-7;
            }
            outputs += if next.present_generated { 2 } else { 1 };
            entry = following_entry;
        }
        assert!(bounded, "the schedule is bounded");
        assert!(recovered, "the schedule recovers from off/on and a stall");
        assert!((pacing.interval() - period).abs() < 1e-7);
        assert!(outputs > 39_800, "{outputs} outputs");
    }

    #[rstest]
    #[case(false, 1.0, 0.1, 9.0, false)]
    #[case(true, 1.0, 0.125, 1.124, false)]
    #[case(true, 1.0, 0.125, 1.125, true)] // equality counts as due
    #[case(true, 1.0, 0.125, 1.126, true)]
    fn stale_output_policy(#[case] has_later: bool, #[case] content: f64, #[case] latency: f64, #[case] now: f64, #[case] due: bool) {
        assert_eq!(later_output_is_due(has_later, content, latency, now), due);
    }

    #[test]
    fn stale_output_policy_drains_a_hitch_backlog_chronologically() {
        let content = [1.0, 1.01, 1.02, 1.03];
        let now = 1.035;
        let mut dropped = 0;
        let mut shown = 0;
        let mut last_shown = 0.0;
        for i in 0..content.len() {
            let has_later = i + 1 < content.len();
            let next_content = if has_later { content[i + 1] } else { 0.0 };
            if later_output_is_due(has_later, next_content, 0.0, now) {
                dropped += 1;
            } else {
                shown += 1;
                last_shown = content[i];
            }
        }
        assert_eq!((dropped, shown), (3, 1));
        assert_eq!(last_shown, 1.03);
    }

    #[rstest]
    #[case(u32::MAX, u32::MAX, u32::MAX, u32::MAX)] // large denominators do not overflow
    #[case(u32::MAX, 0, 2, 1)] // zero denominator rejected
    #[case(1, 3, 2, 1)] // ratio below 1 rejected
    #[case(3, 2, 3, 2)]
    fn ratio_validation(#[case] numerator: u32, #[case] denominator: u32, #[case] want_n: u32, #[case] want_d: u32) {
        let mut schedule = OutputSchedule::new();
        schedule.set_ratio(numerator, denominator);
        assert_eq!((schedule.numerator(), schedule.denominator()), (want_n, want_d));
    }

    /// Every output lands on an even grid in content time; no real frame ever needs more than
    /// two generated frames, and the spacing between consecutive outputs is constant.
    #[rstest]
    #[case(2, 1)]
    #[case(7, 4)]
    #[case(3, 2)]
    #[case(5, 4)]
    fn fixed_ratio_keeps_an_even_grid(#[case] p: u32, #[case] q: u32) {
        let mut schedule = OutputSchedule::new();
        schedule.set_ratio(p, q);
        let interval = 1.0 / 48.0;
        let mut previous_content = -1.0;
        let mut min_gap = 1.0_f64;
        let mut max_gap = 0.0_f64;
        let mut produced = 0u32;
        let mut reals = 0u32;
        let mut max_generated = 0usize;
        let frames = 1 + 400 * q;
        for frame in 0..frames {
            let step = schedule.plan(10.0 + f64::from(frame) * interval);
            max_generated = max_generated.max(step.generated());
            for out in step.outputs() {
                if frame > 0 {
                    produced += 1;
                    reals += u32::from(out.real);
                    let gap = out.content - previous_content;
                    min_gap = min_gap.min(gap);
                    max_gap = max_gap.max(gap);
                }
                previous_content = out.content;
            }
        }
        let expected_gap = interval * f64::from(q) / f64::from(p);
        assert_eq!(produced, 400 * p);
        assert_eq!(reals, 400);
        assert!(max_generated <= 2);
        assert!((min_gap - expected_gap).abs() < 1e-6, "min gap {min_gap}");
        assert!((max_gap - expected_gap).abs() < 1e-6, "max gap {max_gap}");
    }

    #[test]
    fn hitch_shows_only_the_real_frame_then_generation_resumes() {
        let mut schedule = OutputSchedule::new();
        schedule.set_ratio(3, 2);
        let mut t = 10.0;
        for _ in 0..20 {
            t += 1.0 / 48.0;
            let _ = schedule.plan(t);
        }
        t += 3.0 / 48.0;
        let after_hitch = schedule.plan(t);
        t += 1.0 / 48.0;
        let resumed = schedule.plan(t);
        assert_eq!(after_hitch.count(), 1);
        assert!(after_hitch.outputs()[0].real);
        assert!(resumed.generated() >= 1);
    }

    #[test]
    fn adaptive_schedule_aims_at_the_target_and_caps_at_two_generated() {
        let mut schedule = OutputSchedule::new();
        schedule.set_target(72.0);
        let interval = 1.0 / 48.0;
        let mut previous_content = -1.0;
        let mut max_gap_error = 0.0_f64;
        let mut produced = 0u32;
        let mut max_generated = 0usize;
        for frame in 0..4801u32 {
            let step = schedule.plan(10.0 + f64::from(frame) * interval);
            max_generated = max_generated.max(step.generated());
            for out in step.outputs() {
                if frame > 1 {
                    produced += 1;
                    max_gap_error = max_gap_error.max((out.content - previous_content - 1.0 / 72.0).abs());
                }
                previous_content = out.content;
            }
        }
        assert!((7190..=7210).contains(&produced), "{produced} outputs");
        assert!(max_generated <= 2);
        assert!(max_gap_error <= 0.1 / 72.0 + 1e-9);

        // A pause resets to the real frame alone.
        let paused = schedule.plan(20.0);
        assert_eq!(paused.count(), 1);
        assert!(paused.outputs()[0].real);

        // Below 20 real fps the schedule stops generating.
        let mut slow_generated = 0;
        for frame in 0..40 {
            slow_generated += schedule.plan(21.0 + f64::from(frame) * 0.055).generated();
        }
        assert_eq!(slow_generated, 0);

        // 40 real fps towards 144 still caps at two generated frames.
        schedule.set_target(144.0);
        let mut worst = 0;
        for frame in 0..200u32 {
            worst = worst.max(schedule.plan(30.0 + f64::from(frame) / 40.0).generated());
        }
        assert!(worst <= 2);
    }

    #[test]
    fn jitter_around_20_fps_still_generates_and_recovers() {
        // K95: alternating 46 ms and 54 ms averages 50 ms; the output must not halve.
        let mut jitter = OutputSchedule::new();
        jitter.set_ratio(2, 1);
        let mut t = 40.0;
        let mut frames = 0u32;
        let mut generated = 0u32;
        for frame in 0..400u32 {
            t += if frame % 2 == 0 { 0.046 } else { 0.054 };
            let step = jitter.plan(t);
            if frame >= 20 {
                frames += 1;
                generated += u32::from(step.generated() > 0);
            }
        }
        assert!(generated >= frames * 95 / 100, "{generated} of {frames}");

        // Steady 60 ms is real-only; recovery to 40 ms generates again.
        jitter.reset();
        let mut slow_only = 0;
        let mut recovered = 0;
        for frame in 0..100 {
            slow_only += jitter.plan(50.0 + f64::from(frame) * 0.06).generated();
        }
        for frame in 0..100 {
            recovered += jitter.plan(60.0 + f64::from(frame) * 0.04).generated();
        }
        assert_eq!(slow_only, 0);
        assert!(recovered > 90, "{recovered} generated after recovery");
    }
}
