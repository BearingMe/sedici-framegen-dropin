//! Presentation pacing use case over the ports.
//!
//! The decision itself lives in [`crate::domain::pacing`]; this module owns the process-wide
//! state the swapchain wrapper calls into and the hook the presenter uses to ask whether a
//! later output supersedes this one. It stays free of clocks and D3D12: the wrapper passes
//! entry times in seconds and the presenter passes content times, both on the same monotonic
//! clock, exactly as the C++ `presentation/pacing.*` split does.

use std::sync::Mutex;

use crate::domain::pacing::{later_output_is_due, PacingOutput, PacingState};

/// One wrapper's pacing state. Cheap to create and to reset; no allocation.
#[derive(Debug, Default)]
pub struct Pacer {
    state: PacingState,
}

impl Pacer {
    pub fn new() -> Self {
        Self::default()
    }

    /// Forgets the last entry and the smoothed interval, as a swapchain reset does.
    pub fn reset(&mut self) {
        *self = Self::default();
    }

    /// Plans this cycle from the wrapper entry time and whether a midpoint is ready.
    pub fn plan(&mut self, entry: f64, generated_ready: bool) -> PacingOutput {
        self.state.plan(entry, generated_ready)
    }

    /// The smoothed game interval the plan used; 1/60 before the first cycle.
    pub fn interval(&self) -> f64 {
        self.state.interval()
    }
}

/// Process-wide pacer shared by the swapchain wrapper. A game normally has one, but a game
/// that creates two contexts must not see one's history in the other, so the caller passes a
/// key (the wrapper identity) and each key keeps its own state.
#[derive(Debug, Default)]
pub struct Pacers {
    inner: Mutex<Vec<(usize, Pacer)>>,
}

impl Pacers {
    pub fn new() -> Self {
        Self::default()
    }

    /// Plans one cycle for `key`, creating its pacer on first use.
    pub fn plan(&self, key: usize, entry: f64, generated_ready: bool) -> PacingOutput {
        let mut pacers = self.inner.lock().unwrap();
        let pacer = match pacers.iter_mut().find(|(k, _)| *k == key) {
            Some((_, pacer)) => pacer,
            None => {
                pacers.push((key, Pacer::new()));
                &mut pacers.last_mut().expect("just pushed").1
            }
        };
        pacer.plan(entry, generated_ready)
    }

    /// Drops `key`'s history, e.g. when its swapchain is destroyed.
    pub fn remove(&self, key: usize) {
        self.inner.lock().unwrap().retain(|(k, _)| *k != key);
    }
}

/// The presenter's stale-output predicate, exposed here so the presenter depends on the
/// application layer rather than on the domain module directly. See
/// [`later_output_is_due`] for the policy.
pub fn superseded(has_later: bool, later_content: f64, latency: f64, now: f64) -> bool {
    later_output_is_due(has_later, later_content, latency, now)
}

#[cfg(test)]
mod tests {
    use super::*;
    use rstest::rstest;

    #[test]
    fn a_fresh_pacer_seeds_the_first_cycle() {
        let mut pacer = Pacer::new();
        let plan = pacer.plan(10.0, true);
        assert!(!plan.present_generated);
        assert_eq!(plan.real_deadline, 10.0);
        assert_eq!(pacer.interval(), 1.0 / 60.0);
    }

    #[test]
    fn the_second_cycle_generates_and_smooths() {
        let mut pacer = Pacer::new();
        let _ = pacer.plan(10.0, false);
        let plan = pacer.plan(10.0 + 1.0 / 36.0, true);
        assert!(plan.present_generated);
        assert!((pacer.interval() - 1.0 / 36.0).abs() < 1e-9);
    }

    #[test]
    fn pacers_are_keyed_and_removable() {
        let pacers = Pacers::new();
        // Key 1 starts; key 2 starts independently and does not inherit key 1's history.
        let _ = pacers.plan(1, 10.0, false);
        let second = pacers.plan(2, 10.0, true);
        assert!(!second.present_generated, "key 2 has no pair yet");
        let _ = pacers.plan(1, 10.0 + 1.0 / 60.0, true);
        pacers.remove(1);
        let fresh = pacers.plan(1, 20.0, true);
        assert!(!fresh.present_generated, "the removed key starts over");
    }

    #[rstest]
    #[case(false, 1.0, 0.1, 9.0, false)]
    #[case(true, 1.0, 0.125, 1.124, false)]
    #[case(true, 1.0, 0.125, 1.125, true)]
    fn superseded_policy(
        #[case] has_later: bool,
        #[case] content: f64,
        #[case] latency: f64,
        #[case] now: f64,
        #[case] due: bool,
    ) {
        assert_eq!(superseded(has_later, content, latency, now), due);
    }
}
