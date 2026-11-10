//! Pure scheduling decisions for the real-frame limiter (QPC ticks) and the frame-rate target.

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct LimiterStep {
    /// QPC ticks to aim at.
    pub deadline: u64,
    /// False when the frame is already late.
    pub wait: bool,
    /// Arrived at or after the deadline.
    pub late: bool,
    /// The schedule was reseeded because the frame was more than one interval late.
    pub reset: bool,
}

/// First frame seeds and never waits; later frames target `previous + interval` so the average
/// rate is exact; a frame over one interval late reseeds at `now` rather than bursting.
pub fn limiter_step(
    now: u64,
    previous_deadline: u64,
    have_previous: bool,
    interval: u64,
) -> LimiterStep {
    if !have_previous || interval == 0 {
        return LimiterStep {
            deadline: now,
            ..Default::default()
        };
    }
    let mut deadline = previous_deadline.saturating_add(interval);
    let mut reset = false;
    if now > deadline.saturating_add(interval) {
        deadline = now;
        reset = true;
    }
    let late = now >= deadline;
    LimiterStep {
        deadline,
        wait: !late,
        late,
        reset,
    }
}

/// Monitor-matching cap: `floor(Hz - Hz^2 / 3600)`; 0 when the refresh rate is unknown.
pub fn auto_target_fps(refresh_hz: f64) -> f64 {
    if !refresh_hz.is_finite() || !(2.0..=1000.0).contains(&refresh_hz) {
        return 0.0;
    }
    (refresh_hz - refresh_hz * refresh_hz / 3600.0).floor()
}

/// The output rate asked for, or 0 for uncapped.
pub fn resolve_target_fps(target_fps: f64, automatic: bool, refresh_hz: f64) -> f64 {
    let value = if automatic {
        auto_target_fps(refresh_hz)
    } else {
        target_fps
    };
    if value.is_finite() && value > 0.0 {
        value
    } else {
        0.0
    }
}

fn sane_ratio(ratio: f64) -> f64 {
    if ratio.is_finite() && ratio >= 1.0 {
        ratio
    } else {
        1.0
    }
}

/// The real-frame cap the limiter enforces for an output target; 0 is uncapped.
pub fn resolve_real_cap(target_fps: f64, automatic: bool, refresh_hz: f64, ratio: f64) -> f64 {
    let output = resolve_target_fps(target_fps, automatic, refresh_hz);
    if output <= 0.0 {
        0.0
    } else {
        output / sane_ratio(ratio)
    }
}

/// A legacy `limiter.fps` capped real frames; as an output target it is that cap times the ratio.
pub fn legacy_limiter_to_target(limiter_fps: f64, ratio: f64) -> f64 {
    limiter_fps * sane_ratio(ratio)
}

#[cfg(test)]
mod tests {
    use super::*;
    use rstest::rstest;

    #[test]
    fn first_frame_seeds_without_waiting() {
        let s = limiter_step(100, 0, false, 10);
        assert_eq!((s.deadline, s.wait, s.late), (100, false, false));
    }

    #[rstest]
    #[case(105, 110, true, false, false)] // early: wait for previous + interval
    #[case(110, 110, false, true, false)] // exactly on time counts as late
    #[case(125, 125, false, true, true)] // more than an interval late: reseed
    fn later_frames(
        #[case] now: u64,
        #[case] deadline: u64,
        #[case] wait: bool,
        #[case] late: bool,
        #[case] reset: bool,
    ) {
        let s = limiter_step(now, 100, true, 10);
        assert_eq!(
            (s.deadline, s.wait, s.late, s.reset),
            (deadline, wait, late, reset)
        );
    }

    #[rstest]
    #[case(60.0, 59.0)]
    #[case(75.0, 73.0)]
    #[case(144.0, 138.0)]
    #[case(240.0, 224.0)]
    #[case(0.0, 0.0)]
    #[case(f64::NAN, 0.0)]
    fn auto_target(#[case] hz: f64, #[case] want: f64) {
        assert_eq!(auto_target_fps(hz), want);
    }

    #[test]
    fn real_cap_divides_by_ratio() {
        assert_eq!(resolve_real_cap(60.0, false, 0.0, 2.0), 30.0);
        assert_eq!(resolve_real_cap(0.0, false, 0.0, 2.0), 0.0);
        assert_eq!(resolve_real_cap(60.0, false, 0.0, f64::NAN), 60.0);
        assert_eq!(legacy_limiter_to_target(30.0, 2.0), 60.0);
    }
}
