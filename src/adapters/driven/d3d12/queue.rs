//! D3D12 queue: submit, signal and wait.
//!
//! Only the operations the application needs; a `Wait` is a queue-side dependency, never a CPU wait
//! (a CPU wait is a fence wait and is bounded).

use windows::core::Interface;
use windows::Win32::Graphics::Direct3D12::{
    ID3D12CommandList, ID3D12CommandQueue, ID3D12Fence, ID3D12GraphicsCommandList,
};

use crate::application::ports::gpu::{GpuError, GpuFence, GpuQueue};

use super::fence::D3d12Fence;
use super::resources::D3d12CommandList;

/// An owning command queue.
pub struct D3d12Queue {
    pub(crate) queue: ID3D12CommandQueue,
}

impl std::fmt::Debug for D3d12Queue {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("D3d12Queue")
    }
}

impl D3d12Queue {
    pub fn raw(&self) -> &ID3D12CommandQueue {
        &self.queue
    }

    /// Executes the lists. The caller guarantees every list is closed and fully recorded.
    pub fn execute_raw(&self, lists: &[&ID3D12GraphicsCommandList]) -> Result<(), GpuError> {
        let mut base = Vec::with_capacity(lists.len());
        for list in lists {
            // SAFETY: QueryInterface for a base interface of the same object always succeeds on a
            // live list, and the returned reference is owned by `base`.
            let interface: ID3D12CommandList = list.cast()
                .map_err(|error| GpuError::failed("command list cast", error.code().0 as u32))?;
            base.push(Some(interface));
        }
        // SAFETY: every list is closed and recorded; the slice outlives the call.
        unsafe { self.queue.ExecuteCommandLists(&base) };
        Ok(())
    }

    pub fn signal_raw(&self, fence: &ID3D12Fence, value: u64) -> Result<(), GpuError> {
        // SAFETY: the fence belongs to the same device as this queue.
        unsafe { self.queue.Signal(fence, value) }
            .map_err(|error| GpuError::failed("queue signal", error.code().0 as u32))
    }

    pub fn wait_raw(&self, fence: &ID3D12Fence, value: u64) -> Result<(), GpuError> {
        // SAFETY: the fence belongs to the same device as this queue.
        unsafe { self.queue.Wait(fence, value) }
            .map_err(|error| GpuError::failed("queue wait", error.code().0 as u32))
    }

    /// Signals `fence` then waits for it with a bound, in the policy's signal-then-wait order.
    pub fn drain(&self, fence: &D3d12Fence, value: u64, timeout: std::time::Duration) -> Result<(), GpuError> {
        self.signal_raw(fence.raw(), value)?;
        fence.wait(value, timeout)
    }
}

impl GpuQueue<D3d12CommandList, D3d12Fence> for D3d12Queue {
    fn execute(&self, lists: &[&D3d12CommandList]) -> Result<(), GpuError> {
        let raw: Vec<&ID3D12GraphicsCommandList> = lists.iter().map(|list| list.raw()).collect();
        self.execute_raw(&raw)
    }

    fn signal(&self, fence: &D3d12Fence, value: u64) -> Result<(), GpuError> {
        self.signal_raw(fence.raw(), value)
    }

    fn wait(&self, fence: &D3d12Fence, value: u64) -> Result<(), GpuError> {
        self.wait_raw(fence.raw(), value)
    }
}
