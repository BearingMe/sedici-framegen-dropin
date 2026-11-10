//! The production composition: D3D12 device, Stage 1 generator, FFX resource and swapchain seams.
//!
//! The application router speaks only in port types. This module builds the one production router
//! the DLL installs, wiring the D3D12 adapter into [`FrameObjects`], [`DeviceResolver`],
//! [`SwapchainFactory`] and the two submission seams the context needs.
//!
//! # Backend device
//!
//! FFX hands Sedici an `ID3D12Device*` in the create desc. The registry keeps one device binding per
//! backing pointer, so a process with several frame generation contexts shares one device wrapper
//! rather than re-wrapping (and AddRef'ing) it per create. The device wrapper holds a COM reference
//! for the process lifetime; the game's device outlives every FFX context by the FFX contract.
use std::collections::BTreeMap;
use std::sync::{Arc, Mutex};

use windows::core::Interface;
use windows::Win32::Graphics::Direct3D12::{
    ID3D12CommandList, ID3D12CommandQueue, ID3D12Device, ID3D12Fence, ID3D12GraphicsCommandList,
    ID3D12Resource,
};
use windows::Win32::Graphics::Dxgi::IDXGISwapChain4;

use crate::application::context::{DeviceBinding, FrameObjects, QueueDependency, Retirement};
use crate::application::ffx_model;
use crate::application::ports::gpu::{GpuError, ResourceState};
use crate::application::router::{
    AddressReservation, DeviceResolver, Router, SwapchainFactory, TokenSource,
};
use crate::application::session::Wrapper;
use crate::domain::gpu_policy::removed_device_error;
use crate::application::faults::Faults;

use super::d3d12::device::D3d12Device;
use super::d3d12::resources::{D3d12CommandList, D3d12Texture};
use super::stage1::Stage1;

/// The generator the production router drives.
pub type ProductionGenerator = Stage1;
/// The device the production router drives.
pub type ProductionDevice = D3d12Device;

/// Builds the production router, or `None` when no D3D12 backend is linked (the offline host
/// installs its own).
#[cfg(windows)]
pub fn build_service(
    faults: Arc<Faults>,
) -> Option<impl ffx_model::FfxService + 'static> {
    let objects = Arc::new(D3d12FrameObjects);
    let devices: Arc<dyn DeviceResolver<ProductionDevice>> = Arc::new(D3d12Devices::default());
    // The swapchain factory and the address reserver belong to the presentation slice; until it
    // lands they answer with defined errors, which keeps every other path exercised.
    let swapchains: Arc<dyn SwapchainFactory> = Arc::new(UnsupportedSwapchains);
    let tokens: Arc<dyn TokenSource> = Arc::new(crate::application::router::ProcessTokens::new(
        Arc::clone(&faults),
        Arc::new(VirtualAllocReservation),
    ));
    Some(Router::<ProductionDevice, ProductionGenerator>::new(
        objects, devices, swapchains, tokens, faults,
    ))
}

/// Resolves the game's device handle into a port device. One binding per device pointer, shared by
/// every context in the process.
#[derive(Default)]
pub struct D3d12Devices {
    known: Mutex<BTreeMap<usize, DeviceBinding<D3d12Device>>>,
}

impl DeviceResolver<D3d12Device> for D3d12Devices {
    fn resolve(&self, handle: usize) -> Option<DeviceBinding<D3d12Device>> {
        if handle == 0 {
            return None;
        }
        let mut known = self.known.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        if let Some(binding) = known.get(&handle) {
            return Some(DeviceBinding { device: binding.device.clone(), identity: binding.identity });
        }
        // SAFETY: the FFX create desc hands Sedici a live `ID3D12Device*`; the FFX contract keeps it
        // alive while the context exists. `from_raw` takes a COM reference, so the wrapper owns its
        // own and never depends on the game's reference for lifetime.
        let device = unsafe { ID3D12Device::from_raw(handle as *mut _) };
        let wrapper = D3d12Device::from_raw(&device);
        let binding = DeviceBinding { device: wrapper, identity: handle };
        known.insert(handle, DeviceBinding { device: binding.device.clone(), identity: handle });
        Some(binding)
    }
}

/// Wraps the game's FFX resource and command-list handles as the generator's typed objects.
///
/// The FFX resource's `state` is the state the game reported; Stage 1 transitions it and restores
/// it before returning. A null or unreadable handle returns `None`, which the context refuses with
/// a defined error.
pub struct D3d12FrameObjects;

impl FrameObjects<D3d12Device, D3d12CommandList> for D3d12FrameObjects {
    fn texture(&self, handle: usize) -> Option<D3d12Texture> {
        if handle == 0 {
            return None;
        }
        // SAFETY: every non-null handle reaching here came from a validated FFX descriptor's
        // `resource` field. The FFX contract keeps it alive for the call; `borrow` takes a COM
        // reference so the value outlives the game's reference.
        let resource = unsafe { ID3D12Resource::from_raw(handle as *mut _) };
        Some(D3d12Texture::borrow(&resource, ResourceState::COMMON))
    }

    fn list(&self, handle: usize) -> Option<D3d12CommandList> {
        if handle == 0 {
            return None;
        }
        // SAFETY: the handle came from a validated prepare desc's `commandList` field, which the
        // FFX contract keeps live until the dispatch returns.
        let list = unsafe { ID3D12GraphicsCommandList::from_raw(handle as *mut _) };
        Some(D3d12CommandList::borrow(&list))
    }
}

/// A shared retirement probe over the game's or swapchain's fence. It owns a COM reference so a
/// late registration after the game released its own cannot dangle.
pub struct D3d12Retirement {
    fence: ID3D12Fence,
    device: usize,
}

impl D3d12Retirement {
    /// Wraps a live fence. The device identity is the fence's owning device address.
    pub fn new(fence: &ID3D12Fence) -> Result<Self, GpuError> {
        let device = device_of(fence)?;
        Ok(Self { fence: fence.clone(), device })
    }
}

/// The owning device's address, or a GPU error when the query fails.
fn device_of<T: GetDevice>(object: &T) -> Result<usize, GpuError> {
    let mut device: Option<ID3D12Device> = None;
    object.get_device(&mut device)?;
    let device = device
        .ok_or_else(|| GpuError::failed("fence device query", crate::domain::gpu_policy::E_FAIL))?;
    Ok(Interface::as_raw(&device) as usize)
}

/// The raw D3D12 `GetDevice` on objects that have one.
trait GetDevice {
    fn get_device(&self, out: &mut Option<ID3D12Device>) -> Result<(), GpuError>;
}

impl GetDevice for ID3D12Fence {
    fn get_device(&self, out: &mut Option<ID3D12Device>) -> Result<(), GpuError> {
        // SAFETY: a pure query on a live fence; `out` is an owned, correctly typed out-parameter.
        unsafe { self.GetDevice(out) }
            .map_err(|error| GpuError::failed("fence device query", error.code().0 as u32))
    }
}

impl GetDevice for ID3D12CommandQueue {
    fn get_device(&self, out: &mut Option<ID3D12Device>) -> Result<(), GpuError> {
        // SAFETY: a pure query on a live queue; `out` is an owned, correctly typed out-parameter.
        unsafe { self.GetDevice(out) }
            .map_err(|error| GpuError::failed("queue device query", error.code().0 as u32))
    }
}

impl std::fmt::Debug for D3d12Retirement {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("D3d12Retirement")
    }
}

impl Retirement for D3d12Retirement {
    fn completed(&self) -> u64 {
        // SAFETY: a pure query on a live fence.
        unsafe { self.fence.GetCompletedValue() }
    }

    fn wait(&self, value: u64, timeout: std::time::Duration) -> Result<(), GpuError> {
        use windows::Win32::Foundation::{CloseHandle, WAIT_OBJECT_0, WAIT_TIMEOUT};
        use windows::Win32::System::Threading::{CreateEventW, WaitForSingleObject};
        use crate::domain::gpu_policy::{classify_fence, FenceProgress};

        match classify_fence(self.completed(), value) {
            FenceProgress::Complete => return Ok(()),
            FenceProgress::DeviceRemoved => {
                return Err(GpuError::DeviceRemoved { reason: removed_device_error(0) })
            }
            FenceProgress::Pending => {}
        }
        // SAFETY: an auto-reset event with no attributes, initially unsignalled, no name.
        let event = unsafe { CreateEventW(None, false, false, None) }
            .map_err(|error| GpuError::failed("fence event creation", error.code().0 as u32))?;
        // SAFETY: the fence is live and the event is owned by this call.
        let registered = unsafe { self.fence.SetEventOnCompletion(value, event) };
        let result = if let Err(error) = registered {
            Err(GpuError::failed("fence event registration", error.code().0 as u32))
        } else {
            let millis = u32::try_from(timeout.as_millis()).unwrap_or(u32::MAX);
            // SAFETY: an owned event handle with a finite timeout.
            match unsafe { WaitForSingleObject(event, millis) } {
                WAIT_OBJECT_0 => match classify_fence(self.completed(), value) {
                    FenceProgress::Complete => Ok(()),
                    FenceProgress::DeviceRemoved => {
                        Err(GpuError::DeviceRemoved { reason: removed_device_error(0) })
                    }
                    FenceProgress::Pending => {
                        Err(GpuError::failed("fence wait", crate::domain::gpu_policy::E_FAIL))
                    }
                },
                WAIT_TIMEOUT => Err(GpuError::Timeout),
                other => Err(GpuError::failed("fence wait", other.0)),
            }
        };
        // SAFETY: the event was created above and is closed exactly once here.
        let _ = unsafe { CloseHandle(event) };
        result
    }

    fn device_identity(&self) -> usize {
        self.device
    }
}

/// A queue dependency over the game's `ID3D12CommandQueue` (split recording).
pub struct D3d12QueueDependency {
    queue: ID3D12CommandQueue,
    device: usize,
}

impl D3d12QueueDependency {
    /// Wraps a live queue and reads its device identity.
    pub fn new(queue: &ID3D12CommandQueue) -> Result<Self, GpuError> {
        let device = device_of(queue)?;
        Ok(Self { queue: queue.clone(), device })
    }
}

impl QueueDependency for D3d12QueueDependency {
    fn device_identity(&self) -> usize {
        self.device
    }

    fn wait(&self, retirement: &dyn Retirement, value: u64) -> Result<(), GpuError> {
        // The context checks the fence's device before calling this; the queue-side wait itself is
        // the raw `ID3D12CommandQueue::Wait`, which needs the fence interface rather than the port
        // trait. The production sink path uses `note_ordered_submission` instead because the game
        // records generation on the same queue it presented on, so this wait is only for a split
        // queue the game never uses today. Returning an error keeps a caller from silently skipping
        // the dependency until the swapchain slice supplies the fence.
        let _ = (retirement, value);
        let _ = &self.queue;
        Err(GpuError::failed("queue wait", crate::domain::gpu_policy::E_FAIL))
    }
}

/// The swapchain factory arrives with the presentation slice. Until then a swapchain create is a
/// defined `ERROR` and never a half-built chain.
pub struct UnsupportedSwapchains;

impl SwapchainFactory for UnsupportedSwapchains {
    fn create_wrapper(
        &self,
        _desc: &ffx_model::CreateContextDesc,
        _origin: crate::application::session::WrapOrigin,
    ) -> Result<(usize, Arc<dyn Wrapper>), ffx_model::Rejection> {
        Err(ffx_model::Rejection::error("swapchain wrap backend allocation failed"))
    }
}

/// A process-lifetime address reservation for the handle namespace.
pub struct VirtualAllocReservation;

impl AddressReservation for VirtualAllocReservation {
    fn reserve(&self, bytes: usize) -> Option<usize> {
        use windows::Win32::System::Memory::{VirtualAlloc, MEM_RESERVE, PAGE_NOACCESS};
        // SAFETY: a reservation for `bytes` at a system-chosen address; nothing is committed or
        // dereferenced, and the C++ also never released the range.
        let base = unsafe { VirtualAlloc(None, bytes, MEM_RESERVE, PAGE_NOACCESS) } as usize;
        (base != 0).then_some(base)
    }
}

/// Compile-time proof that the production pair satisfies the router's bounds.
const _: () = {
    fn assert_service<T: ffx_model::FfxService>() {}
    fn check() {
        assert_service::<Router<ProductionDevice, ProductionGenerator>>();
    }
    let _ = check;
};

// Silence unused imports that the conditional compilation above can leave behind.
#[allow(dead_code)]
fn _unused(_: Option<&ID3D12CommandList>, _: Option<&IDXGISwapChain4>) {}
