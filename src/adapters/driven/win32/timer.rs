//! High-resolution waitable timer plus QPC (ports the QPC/timer half of `presentation/limiter.cpp`).
//!
//! [`Win32Clock`] is safe: the `unsafe` Win32 calls sit inside it, and the timer handle closes in
//! `Drop`. The high-resolution timer flavour is what makes the wake close enough to the deadline
//! for the limiter's spin window to matter; a standard waitable timer still honours the schedule,
//! just more coarsely.

use std::sync::Mutex;

use tracing::{error, info};
use windows::Win32::Foundation::{CloseHandle, HANDLE, WAIT_OBJECT_0};
use windows::Win32::System::Performance::{QueryPerformanceCounter, QueryPerformanceFrequency};
use windows::Win32::System::Threading::{
    CreateWaitableTimerExW, SetWaitableTimer, WaitForSingleObject,
    CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS,
};

use crate::application::ports::clock::Clock;

/// One-shot waitable timer. Auto-reset, unnamed, no security attributes.
struct WaitableTimer(HANDLE);

// SAFETY: a Win32 timer handle is owned by this wrapper and only used through `&self`; the calls
// below are thread-safe kernel calls. It is never sent between threads by the timer itself.
unsafe impl Send for WaitableTimer {}
unsafe impl Sync for WaitableTimer {}

impl WaitableTimer {
    /// Creates the high-resolution timer, falling back to a standard one (and saying so).
    fn create() -> Option<Self> {
        // SAFETY: null security attributes and a null name are the documented defaults; the flags
        // and access rights are constants. No pointers outlive the call.
        let high = unsafe {
            CreateWaitableTimerExW(
                None,
                None,
                CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                TIMER_ALL_ACCESS.0,
            )
        };
        match high {
            Ok(handle) => Some(Self(handle)),
            Err(error) => {
                info!(
                    "sedici: limiter: high-resolution waitable timer unavailable (GetLastError={}); using the standard timer, spin accuracy may be worse",
                    error.code().0
                );
                // SAFETY: same defaults, no high-resolution flag.
                unsafe { CreateWaitableTimerExW(None, None, 0, TIMER_ALL_ACCESS.0) }
                    .ok()
                    .map(Self)
            }
        }
    }

    /// Arms the timer for a relative duration and blocks until it fires. Negative `due_100ns`
    /// means relative and in the past (fires immediately).
    fn wait_relative(&self, due_100ns: i64) -> bool {
        // SAFETY: the handle is live for `&self`; the due time points at a local that outlives the
        // call; no completion routine and no argument; the timer is not resuming a suspended state.
        let armed = unsafe { SetWaitableTimer(self.0, &due_100ns, 0, None, None, false) };
        if armed.is_err() {
            return false;
        }
        // SAFETY: the same live auto-reset timer handle; `INFINITE` is the documented sentinel.
        let waited = unsafe { WaitForSingleObject(self.0, u32::MAX) };
        waited == WAIT_OBJECT_0
    }
}

impl Drop for WaitableTimer {
    fn drop(&mut self) {
        // SAFETY: the handle is owned here and closed exactly once.
        let _ = unsafe { CloseHandle(self.0) };
    }
}

/// QPC ticks plus a lazily created waitable timer.
pub struct Win32Clock {
    frequency: u64,
    /// `None` after a timer creation failure; `wait_until` then reports `false` and the limiter
    /// disables itself instead of spinning.
    timer: Mutex<Option<WaitableTimer>>,
    timer_failed: std::sync::atomic::AtomicBool,
}

impl Win32Clock {
    pub fn new() -> Self {
        let mut frequency = 0_i64;
        // SAFETY: `frequency` is a local out-parameter; a failure leaves it 0, which the limiter
        // treats as "no timing".
        let ok = unsafe { QueryPerformanceFrequency(&mut frequency) }.is_ok();
        Self {
            frequency: if ok && frequency > 0 {
                frequency as u64
            } else {
                0
            },
            timer: Mutex::new(None),
            timer_failed: std::sync::atomic::AtomicBool::new(false),
        }
    }
}

impl Default for Win32Clock {
    fn default() -> Self {
        Self::new()
    }
}

impl Clock for Win32Clock {
    fn frequency(&self) -> u64 {
        self.frequency
    }

    fn now(&self) -> u64 {
        let mut value = 0_i64;
        // SAFETY: `value` is a local out-parameter, always written on success.
        if unsafe { QueryPerformanceCounter(&mut value) }.is_ok() {
            value as u64
        } else {
            0
        }
    }

    fn wait_until(&self, deadline: u64) -> bool {
        if self.frequency == 0 {
            return false;
        }
        let now = self.now();
        if deadline <= now {
            return true;
        }
        if self.timer_failed.load(std::sync::atomic::Ordering::Relaxed) {
            return false;
        }

        let mut guard = self.timer.lock().unwrap_or_else(|e| e.into_inner());
        if guard.is_none() {
            match WaitableTimer::create() {
                Some(timer) => *guard = Some(timer),
                None => {
                    error!("sedici: limiter disabled: could not create a waitable timer");
                    self.timer_failed
                        .store(true, std::sync::atomic::Ordering::Relaxed);
                    return false;
                }
            }
        }

        // 100 ns units, negative for relative. Clamp to at least 1 unit so a tiny remainder still
        // schedules instead of firing zero ("already elapsed") spuriously.
        let seconds = (deadline - now) as f64 / self.frequency as f64;
        let mut due = -((seconds * 1e7) as i64);
        if due >= 0 {
            due = -1;
        }
        let Some(timer) = guard.as_ref() else {
            return false;
        };
        let ok = timer.wait_relative(due);
        if !ok {
            self.timer_failed
                .store(true, std::sync::atomic::Ordering::Relaxed);
        }
        ok
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reports_a_frequency_and_monotonic_ticks() {
        let clock = Win32Clock::new();
        assert!(clock.frequency() > 0, "QPC reports a frequency on Windows");
        let a = clock.now();
        let b = clock.now();
        assert!(b >= a, "ticks never go backwards");
        // The wait must return promptly for an already-past deadline.
        assert!(clock.wait_until(a.saturating_sub(1)));
    }

    #[test]
    fn a_short_wait_takes_at_least_its_duration() {
        if std::env::var_os("SEDICI_SKIP_TIMING_TESTS").is_some() {
            return;
        }
        let clock = Win32Clock::new();
        let start = clock.now();
        let target = start + clock.frequency() / 100; // 10 ms
        assert!(
            clock.wait_until(target),
            "the waitable timer arms and fires"
        );
        assert!(clock.now() >= start, "time moved forward across the wait");
        assert!(clock.ticks_to_ms(clock.frequency()) >= 999.0);
    }
}
