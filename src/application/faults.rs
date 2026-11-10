//! Fault injection for the offline production-object test host (ports diagnostics/bookkeeping_faults).
//!
//! These are not DLL exports and no environment switch arms them: the offline host calls
//! [`faults::arm`] to make one bookkeeping step fail, so a test can prove the game-facing
//! behaviour of each owner-approved exception (K29, K63, K105, ...) without touching the game.
//! Normal runtime never arms a fault.
//!
//! The C++ hooks are one-shot function pointers that force a lookup/destroy interleaving
//! (K41, K47, K60). They are represented here as one-shot boxed closures.

use std::sync::atomic::{AtomicBool, AtomicU8, AtomicU32, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};

/// A bookkeeping step a test can make fail. The discriminant is the value stored atomically;
/// only one fault can be armed at a time.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum BookkeepingFault {
    #[default]
    None = 0,
    OwnerAllocation = 1,
    RouteRegistryAllocation = 2,
    WrapperRegistryAllocation = 3,
    /// Simulate queue Signal failure after Execute.
    GenerationSignal = 4,
    /// Simulate a failed command-list Close.
    GenerationClose = 5,
    /// Simulate a failed allocator Reset.
    GenerationReset = 6,
    /// Reject a retirement token.
    SubmissionRegistration = 7,
    /// Simulate dirty-rectangle allocation failure.
    PresentParametersAllocation = 8,
    /// Simulate handle-namespace reservation failure (K29).
    TokenReservation = 9,
    /// Simulate backend allocation failure before creation (K63).
    BackendCreation = 10,
    /// Simulate a refused presenter job (K105).
    PresenterSubmit = 11,
    /// Simulate an overlay renderer failure while drawing.
    OverlayDraw = 12,
}

/// A one-shot pause hook. The host stores it and the production path runs it at the seam.
type Hook = Box<dyn FnOnce() + Send>;

/// Process-wide fault state. Tests share one process, so a single instance serialises through
/// atomics exactly as the C++ does.
#[derive(Default)]
pub struct Faults {
    armed: AtomicU8,
    lookup_hook: Mutex<Option<Hook>>,
    creation_hook: Mutex<Option<Hook>>,
    context_record_hook: Mutex<Option<Hook>>,
    queue_recovery_hook: Mutex<Option<Hook>>,
    swapchain_retentions: AtomicU32,
    swapchain_releases: AtomicU32,
    retirement_hold: AtomicBool,
    swapchain_parked: AtomicUsize,
}

impl Faults {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn arm(&self, fault: BookkeepingFault) {
        self.armed.store(fault as u8, Ordering::Release);
    }

    /// True exactly once, for the armed fault; the arm is consumed.
    pub fn consume(&self, fault: BookkeepingFault) -> bool {
        if fault == BookkeepingFault::None {
            return false;
        }
        self.armed
            .compare_exchange(fault as u8, BookkeepingFault::None as u8, Ordering::AcqRel, Ordering::Acquire)
            .is_ok()
    }

    pub fn arm_wrapper_lookup_hook(&self, hook: impl FnOnce() + Send + 'static) {
        *self.lookup_hook.lock().unwrap() = Some(Box::new(hook));
    }

    pub fn run_wrapper_lookup_hook(&self) {
        if let Some(hook) = self.lookup_hook.lock().unwrap().take() {
            hook();
        }
    }

    pub fn arm_creation_hook(&self, hook: impl FnOnce() + Send + 'static) {
        *self.creation_hook.lock().unwrap() = Some(Box::new(hook));
    }

    pub fn run_creation_hook(&self) {
        if let Some(hook) = self.creation_hook.lock().unwrap().take() {
            hook();
        }
    }

    pub fn arm_context_record_hook(&self, hook: impl FnOnce() + Send + 'static) {
        *self.context_record_hook.lock().unwrap() = Some(Box::new(hook));
    }

    pub fn run_context_record_hook(&self) {
        if let Some(hook) = self.context_record_hook.lock().unwrap().take() {
            hook();
        }
    }

    pub fn arm_queue_recovery_hook(&self, hook: impl FnOnce() + Send + 'static) {
        *self.queue_recovery_hook.lock().unwrap() = Some(Box::new(hook));
    }

    pub fn run_queue_recovery_hook(&self) {
        if let Some(hook) = self.queue_recovery_hook.lock().unwrap().take() {
            hook();
        }
    }

    pub fn note_swapchain_retention(&self) {
        self.swapchain_retentions.fetch_add(1, Ordering::Relaxed);
        self.swapchain_parked.fetch_add(1, Ordering::Relaxed);
    }

    pub fn swapchain_retention_count(&self) -> u32 {
        self.swapchain_retentions.load(Ordering::Relaxed)
    }

    /// A parked swapchain whose GPU work later retired was destroyed normally (K47).
    pub fn note_swapchain_release(&self) {
        self.swapchain_releases.fetch_add(1, Ordering::Relaxed);
        self.swapchain_parked.fetch_sub(1, Ordering::Relaxed);
    }

    pub fn swapchain_release_count(&self) -> u32 {
        self.swapchain_releases.load(Ordering::Relaxed)
    }

    /// Parked and not yet released; the `diag:` line reports this.
    pub fn swapchain_parked_now(&self) -> u32 {
        self.swapchain_parked.load(Ordering::Relaxed) as u32
    }

    /// While held, no Context reports its GPU work as retired, so a Session destroy is refused
    /// and the router must park the owner (K41). Sticky, not one-shot.
    pub fn set_retirement_hold(&self, held: bool) {
        self.retirement_hold.store(held, Ordering::Release);
    }

    pub fn retirement_held(&self) -> bool {
        self.retirement_hold.load(Ordering::Acquire)
    }
}

/// Shared handle so a test host and the production path see one state.
pub type SharedFaults = Arc<Faults>;

#[cfg(test)]
mod tests {
    use super::*;
    use rstest::rstest;

    #[rstest]
    #[case(BookkeepingFault::OwnerAllocation)]
    #[case(BookkeepingFault::TokenReservation)]
    #[case(BookkeepingFault::PresenterSubmit)]
    #[case(BookkeepingFault::OverlayDraw)]
    fn a_fault_is_consumed_once(#[case] fault: BookkeepingFault) {
        let faults = Faults::new();
        faults.arm(fault);
        assert!(faults.consume(fault));
        assert!(!faults.consume(fault), "the arm is one-shot");
    }

    #[test]
    fn a_different_fault_is_not_consumed() {
        let faults = Faults::new();
        faults.arm(BookkeepingFault::BackendCreation);
        assert!(!faults.consume(BookkeepingFault::OwnerAllocation));
        assert!(faults.consume(BookkeepingFault::BackendCreation));
    }

    #[test]
    fn hooks_run_once() {
        let faults = Faults::new();
        let hits = Arc::new(AtomicU32::new(0));
        faults.arm_wrapper_lookup_hook({
            let hits = Arc::clone(&hits);
            move || {
                hits.fetch_add(1, Ordering::Relaxed);
            }
        });
        faults.run_wrapper_lookup_hook();
        faults.run_wrapper_lookup_hook();
        assert_eq!(hits.load(Ordering::Relaxed), 1);
    }

    #[test]
    fn retention_and_release_balance() {
        let faults = Faults::new();
        faults.note_swapchain_retention();
        faults.note_swapchain_retention();
        assert_eq!(faults.swapchain_retention_count(), 2);
        assert_eq!(faults.swapchain_parked_now(), 2);
        faults.note_swapchain_release();
        assert_eq!(faults.swapchain_release_count(), 1);
        assert_eq!(faults.swapchain_parked_now(), 1);
    }

    #[test]
    fn retirement_hold_is_sticky() {
        let faults = Faults::new();
        assert!(!faults.retirement_held());
        faults.set_retirement_hold(true);
        assert!(faults.retirement_held());
        assert!(faults.retirement_held());
        faults.set_retirement_hold(false);
        assert!(!faults.retirement_held());
    }
}
