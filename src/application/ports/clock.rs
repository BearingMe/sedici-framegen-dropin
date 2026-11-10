//! Monotonic ticks and bounded waits (ports the QPC half of `presentation/limiter.cpp` and the
//! timer the limiter drives). The application never calls `QueryPerformanceCounter` itself: it
//! asks the [`Clock`] for ticks, converts with [`Clock::ticks_to_ms`], and waits until a tick
//! deadline with [`Clock::wait_until`].

use std::sync::Mutex;

/// A monotonic tick source with a fixed frequency, plus a waitable deadline.
///
/// Ticks are opaque: only differences and [`Clock::frequency`] give them meaning. The wait is
/// allowed to return early (the caller spins the last window) but must not block past `deadline`
/// once it is reached.
pub trait Clock: Send + Sync {
    /// Ticks per second. 0 when the platform cannot report it; callers then disable timing.
    fn frequency(&self) -> u64;

    /// The current tick value. Never goes backwards between calls on one thread.
    fn now(&self) -> u64;

    /// Blocks until the tick source reaches `deadline`. Returns `false` when the platform cannot
    /// wait (no timer, an arming failure); the caller then disables the limiter instead of
    /// spinning for a whole frame. `Ok` when the wait finished, early or not.
    fn wait_until(&self, deadline: u64) -> bool;

    /// Milliseconds for `ticks`; 0 when the frequency is unknown.
    fn ticks_to_ms(&self, ticks: u64) -> f64 {
        if self.frequency() == 0 {
            0.0
        } else {
            ticks as f64 * 1000.0 / self.frequency() as f64
        }
    }

    /// Milliseconds for `ms`, rounded to ticks (at least 0).
    fn ms_to_ticks(&self, ms: f64) -> u64 {
        if !ms.is_finite() || ms <= 0.0 {
            return 0;
        }
        (ms * self.frequency() as f64 / 1000.0).round() as u64
    }
}

impl<T: Clock + ?Sized> Clock for std::sync::Arc<T> {
    fn frequency(&self) -> u64 {
        (**self).frequency()
    }
    fn now(&self) -> u64 {
        (**self).now()
    }
    fn wait_until(&self, deadline: u64) -> bool {
        (**self).wait_until(deadline)
    }
}

/// A manual clock for tests: no real time, no sleeps. `wait_until` advances the tick source to the
/// deadline and records the wait, so a limiter test observes the same arithmetic the game would
/// without spending wall-clock time.
#[derive(Debug)]
pub struct FakeClock {
    frequency: u64,
    state: Mutex<FakeState>,
    /// Whether `wait_until` succeeds or reports a platform failure.
    waits_succeed: Mutex<bool>,
}

#[derive(Debug, Default)]
struct FakeState {
    now: u64,
    waits: Vec<u64>,
}

impl FakeClock {
    pub fn new(frequency: u64, start: u64) -> Self {
        Self { frequency, state: Mutex::new(FakeState { now: start, waits: Vec::new() }), waits_succeed: Mutex::new(true) }
    }

    /// Advances the clock by `ticks` without a wait (the game's own frame cost).
    pub fn advance(&self, ticks: u64) {
        let mut state = self.state.lock().unwrap();
        state.now = state.now.saturating_add(ticks);
    }

    /// Sets the tick value directly.
    pub fn set(&self, ticks: u64) {
        self.state.lock().unwrap().now = ticks;
    }

    /// Every `wait_until` deadline, in order.
    pub fn waits(&self) -> Vec<u64> {
        self.state.lock().unwrap().waits.clone()
    }

    pub fn clear_waits(&self) {
        self.state.lock().unwrap().waits.clear();
    }

    /// Makes `wait_until` report a platform failure from now on.
    pub fn fail_waits(&self) {
        *self.waits_succeed.lock().unwrap() = false;
    }
}

impl Clock for FakeClock {
    fn frequency(&self) -> u64 {
        self.frequency
    }

    fn now(&self) -> u64 {
        self.state.lock().unwrap().now
    }

    fn wait_until(&self, deadline: u64) -> bool {
        let mut state = self.state.lock().unwrap();
        state.waits.push(deadline);
        if !*self.waits_succeed.lock().unwrap() {
            return false;
        }
        state.now = state.now.max(deadline);
        true
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn conversion_uses_the_frequency() {
        let clock = FakeClock::new(10_000, 0);
        assert_eq!(clock.ticks_to_ms(10_000), 1000.0);
        assert_eq!(clock.ms_to_ticks(27.0), 270);
        assert_eq!(clock.ms_to_ticks(f64::NAN), 0);
        assert_eq!(FakeClock::new(0, 0).ticks_to_ms(5), 0.0);
    }

    #[test]
    fn fake_wait_advances_to_the_deadline() {
        let clock = FakeClock::new(1000, 10);
        assert!(clock.wait_until(25));
        assert_eq!(clock.now(), 25);
        assert_eq!(clock.waits(), vec![25]);
        clock.fail_waits();
        assert!(!clock.wait_until(30));
        assert_eq!(clock.now(), 25);
    }
}
