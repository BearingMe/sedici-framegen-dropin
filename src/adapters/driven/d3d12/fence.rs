//! D3D12 fence: a timeline fence with a bounded wait.
//!
//! The wait mirrors `Presenter::wait_fence` and `Context::wait_for_retirement`: a completed value of
//! `u64::MAX` is a removed device and is never success; a timeout is reported as
//! [`GpuError::Timeout`], never as completion.
use std::time::Duration;

use windows::Win32::Foundation::{CloseHandle, HANDLE, WAIT_OBJECT_0, WAIT_TIMEOUT};
use windows::Win32::Graphics::Direct3D12::ID3D12Fence;
use windows::Win32::System::Threading::{CreateEventW, WaitForSingleObject};

use crate::application::ports::gpu::{GpuError, GpuFence};
use crate::domain::gpu_policy::{
    classify_fence, removed_device_error, FenceProgress, DEVICE_REMOVED,
};

/// An auto-reset event used for a single bounded wait. Dropping it closes the handle.
pub(crate) struct WaitEvent(HANDLE);

// SAFETY: a Win32 event handle is process-wide and may be waited on from any thread; the handle is
// uniquely owned by this struct and closed exactly once on drop.
unsafe impl Send for WaitEvent {}
// SAFETY: as above; `WaitForSingleObject` on the same handle from several threads is legal.
unsafe impl Sync for WaitEvent {}

impl WaitEvent {
    pub(crate) fn handle(&self) -> HANDLE {
        self.0
    }
}

impl Drop for WaitEvent {
    fn drop(&mut self) {
        // SAFETY: the handle was created by `CreateEventW` and is closed exactly once here.
        let _ = unsafe { CloseHandle(self.0) };
    }
}

pub(crate) fn create_event() -> Result<WaitEvent, GpuError> {
    // SAFETY: no attributes, not manual reset, initially unsignalled, no name.
    unsafe { CreateEventW(None, false, false, None) }
        .map(WaitEvent)
        .map_err(|error| GpuError::failed("fence event creation", error.code().0 as u32))
}

/// An owning fence plus the event its bounded waits use.
pub struct D3d12Fence {
    pub(crate) fence: ID3D12Fence,
    pub(crate) event: WaitEvent,
}

impl std::fmt::Debug for D3d12Fence {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("D3d12Fence")
    }
}

impl D3d12Fence {
    pub fn raw(&self) -> &ID3D12Fence {
        &self.fence
    }

    /// The value as the D3D12 policy expects: `u64::MAX` means the device was removed.
    pub fn completed(&self) -> u64 {
        // SAFETY: a pure query on a live fence.
        unsafe { self.fence.GetCompletedValue() }
    }

    /// Waits for `value` with a bounded timeout. `device_reason` is consulted when the fence reports
    /// removal so the error carries the real HRESULT rather than the sentinel.
    pub fn wait_with_reason(
        &self,
        value: u64,
        timeout: Duration,
        device_reason: Option<u32>,
    ) -> Result<(), GpuError> {
        match classify_fence(self.completed(), value) {
            FenceProgress::Complete => return Ok(()),
            FenceProgress::DeviceRemoved => {
                return Err(GpuError::DeviceRemoved {
                    reason: removed_device_error(device_reason.unwrap_or(DEVICE_REMOVED)),
                })
            }
            FenceProgress::Pending => {}
        }

        // SAFETY: the event handle is owned by `self` and outlives the registration.
        let registered = unsafe { self.fence.SetEventOnCompletion(value, self.event.handle()) };
        if let Err(error) = registered {
            return Err(GpuError::failed(
                "fence event registration",
                error.code().0 as u32,
            ));
        }

        // WaitForSingleObject takes milliseconds; clamp rather than overflow for very long waits.
        let millis = u32::try_from(timeout.as_millis()).unwrap_or(u32::MAX);
        // SAFETY: waiting on an owned event handle with a finite timeout.
        let wait = unsafe { WaitForSingleObject(self.event.handle(), millis) };
        if wait == WAIT_TIMEOUT {
            return Err(GpuError::Timeout);
        }
        if wait != WAIT_OBJECT_0 {
            return Err(GpuError::failed("fence wait", wait.0));
        }

        match classify_fence(self.completed(), value) {
            FenceProgress::Complete => Ok(()),
            FenceProgress::DeviceRemoved => Err(GpuError::DeviceRemoved {
                reason: removed_device_error(device_reason.unwrap_or(DEVICE_REMOVED)),
            }),
            // A signalled event whose fence has not reached the value cannot be trusted.
            FenceProgress::Pending => Err(GpuError::failed(
                "fence wait",
                crate::domain::gpu_policy::E_FAIL,
            )),
        }
    }
}

impl GpuFence for D3d12Fence {
    fn completed(&self) -> u64 {
        D3d12Fence::completed(self)
    }

    fn wait(&self, value: u64, timeout: Duration) -> Result<(), GpuError> {
        self.wait_with_reason(value, timeout, None)
    }
}
