//! GPU retirement, slot and queue policies, independent of D3D12 so success and failure can both
//! be exercised without manufacturing a device failure.
use std::collections::VecDeque;

/// The swapchain admits at most this many presenter jobs, including the one executing.
pub const PRESENTER_QUEUE_CAPACITY: usize = 3;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FenceProgress {
    Pending,
    Complete,
    DeviceRemoved,
}

/// A completed value of `u64::MAX` is D3D12's removed-device sentinel.
pub fn classify_fence(completed: u64, target: u64) -> FenceProgress {
    if completed == u64::MAX {
        FenceProgress::DeviceRemoved
    } else if completed >= target {
        FenceProgress::Complete
    } else {
        FenceProgress::Pending
    }
}

/// A failed slot or retirement step, carrying the HRESULT the adapter understands.
#[derive(Debug, Clone, Copy, PartialEq, Eq, thiserror::Error)]
#[error("gpu step failed with code {0:#010x}")]
pub struct GpuFailure(pub u32);

pub const E_FAIL: u32 = 0x8000_4005;
pub const E_INVALIDARG: u32 = 0x8007_0057;
pub const DEVICE_REMOVED: u32 = 0x887A_0005;

/// Some drivers return success from the removed-reason query before removal is observable. A
/// removed-fence sentinel must never become success.
pub fn removed_device_error(reason: u32) -> u32 {
    if reason & 0x8000_0000 != 0 { reason } else { DEVICE_REMOVED }
}

/// A retirement wait is valid only after its queue signal was accepted.
pub fn signal_then_wait(
    signal: impl FnOnce() -> Result<(), GpuFailure>,
    wait: impl FnOnce() -> Result<(), GpuFailure>,
) -> Result<(), GpuFailure> {
    signal()?;
    wait()
}

/// Advance a bounded slot ring only after the current slot is reusable. `wait` returns `Ok(true)`
/// when ready; `Ok(false)` (neither failure nor ready) is `E_FAIL`.
pub fn acquire_frame_slot(
    next: &mut u32,
    count: u32,
    wait: impl FnOnce(u32) -> Result<bool, GpuFailure>,
) -> Result<u32, GpuFailure> {
    if count == 0 || *next >= count {
        return Err(GpuFailure(E_INVALIDARG));
    }
    let candidate = *next;
    if !wait(candidate)? {
        return Err(GpuFailure(E_FAIL));
    }
    *next = (candidate + 1) % count;
    Ok(candidate)
}

/// Bounded FIFO whose insertion never allocates after construction.
#[derive(Debug)]
pub struct FixedQueue<T, const CAPACITY: usize> {
    entries: VecDeque<T>,
}

impl<T, const CAPACITY: usize> Default for FixedQueue<T, CAPACITY> {
    fn default() -> Self {
        Self { entries: VecDeque::with_capacity(CAPACITY) }
    }
}

impl<T, const CAPACITY: usize> FixedQueue<T, CAPACITY> {
    pub fn len(&self) -> usize {
        self.entries.len()
    }
    pub fn is_empty(&self) -> bool {
        self.entries.is_empty()
    }
    pub fn front(&self) -> Option<&T> {
        self.entries.front()
    }
    /// Gives the value back when the queue is full.
    pub fn push_back(&mut self, value: T) -> Result<(), T> {
        if self.entries.len() == CAPACITY {
            return Err(value);
        }
        self.entries.push_back(value);
        Ok(())
    }
    pub fn pop_front(&mut self) -> Option<T> {
        self.entries.pop_front()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use rstest::rstest;

    #[rstest]
    #[case(u64::MAX, 5, FenceProgress::DeviceRemoved)]
    #[case(5, 5, FenceProgress::Complete)]
    #[case(4, 5, FenceProgress::Pending)]
    fn classifies_fences(#[case] done: u64, #[case] target: u64, #[case] want: FenceProgress) {
        assert_eq!(classify_fence(done, target), want);
    }

    #[test]
    fn wait_is_skipped_when_signal_fails() {
        let mut waited = false;
        let r = signal_then_wait(|| Err(GpuFailure(E_FAIL)), || {
            waited = true;
            Ok(())
        });
        assert_eq!(r, Err(GpuFailure(E_FAIL)));
        assert!(!waited);
    }

    #[test]
    fn slot_ring_advances_only_when_ready() {
        let mut next = 1;
        assert_eq!(acquire_frame_slot(&mut next, 3, |_| Ok(false)), Err(GpuFailure(E_FAIL)));
        assert_eq!(next, 1);
        assert_eq!(acquire_frame_slot(&mut next, 3, |_| Ok(true)), Ok(1));
        assert_eq!(next, 2);
        assert_eq!(acquire_frame_slot(&mut 3, 3, |_| Ok(true)), Err(GpuFailure(E_INVALIDARG)));
    }

    #[test]
    fn fixed_queue_refuses_when_full() {
        let mut q = FixedQueue::<u8, PRESENTER_QUEUE_CAPACITY>::default();
        for i in 0..3 {
            q.push_back(i).unwrap();
        }
        assert_eq!(q.push_back(9), Err(9));
        assert_eq!(q.pop_front(), Some(0));
        assert!(q.push_back(9).is_ok());
    }
}
