//! The production composition: D3D12 device, Stage 1 generator, FFX resource and swapchain seams.
//!
//! The application router speaks only in port types. This module builds the one production router
//! the DLL installs, wiring the D3D12 adapter into [`FrameObjects`], [`DeviceResolver`],
//! [`SwapchainFactory`] and the submission seams the context needs.
//!
//! # Backend device
//!
//! FFX hands Sedici an `ID3D12Device*` in the create desc. The registry keeps one device binding per
//! backing pointer, so a process with several frame generation contexts shares one device wrapper
//! rather than re-wrapping (and AddRef'ing) it per create. The device wrapper holds a COM reference
//! for the process lifetime; the game's device outlives every FFX context by the FFX contract.
//!
//! # The swapchain factory
//!
//! A swapchain create either adopts the chain the game handed over or builds one on the game's
//! factory and queue (the New and HWND forms). Both paths converge on [`DxgiNative`] and
//! [`Swapchain::wrap`], whose generation source is a [`SessionDriven`] over the session's context.
//! The COM object the game receives is [`crate::adapters::driving::dxgi_wrapper::DxgiWrapper`],
//! whose present backend is the same wrapper.
use std::collections::BTreeMap;
use std::sync::{Arc, Mutex};

use windows::core::Interface;
use windows::Win32::Foundation::HWND;
use windows::Win32::Graphics::Direct3D12::{
    ID3D12CommandList, ID3D12CommandQueue, ID3D12Device, ID3D12Fence, ID3D12GraphicsCommandList,
    ID3D12Resource,
};
use windows::Win32::Graphics::Dxgi::Common::{
    DXGI_ALPHA_MODE, DXGI_FORMAT, DXGI_MODE_DESC, DXGI_MODE_SCALING, DXGI_MODE_SCANLINE_ORDER,
    DXGI_RATIONAL, DXGI_SAMPLE_DESC,
};
use windows::Win32::Graphics::Dxgi::{
    IDXGIFactory, IDXGIFactory2, IDXGIOutput, IDXGISwapChain, IDXGISwapChain4, DXGI_SCALING,
    DXGI_SWAP_CHAIN_DESC, DXGI_SWAP_CHAIN_DESC1, DXGI_SWAP_EFFECT,
    DXGI_USAGE,
};

use crate::adapters::driving::dxgi_wrapper::DxgiWrapper;
use crate::application::context::{
    DeviceBinding, FrameObjects, QueueDependency, Retirement,
};
use crate::application::faults::Faults;
use crate::application::ffx_model;
use crate::application::ports::clock::Clock;
use crate::application::ports::gpu::{GpuError, ResourceState};
use crate::application::presentation::swapchain::{
    PresentChoice, SharedPresentQueues, Swapchain, WrapEnvironment, WrapReport,
};
use crate::application::router::{
    AddressReservation, DeviceResolver, Router, SwapchainFactory, TokenSource,
};
use crate::application::session::{Wrapper, WrapOrigin};
use crate::domain::gpu_policy::removed_device_error;

use super::d3d12::device::D3d12Device;
use super::d3d12::queue::D3d12Queue;
use super::d3d12::resources::{D3d12CommandList, D3d12Texture};
use super::dxgi_native::DxgiNative;
use super::overlay_ui::{OverlayHost, OverlayServices};
use super::stage1::Stage1;
use super::wrapper_handle::{SessionDriven, WrapperHandle};

/// The generator the production router drives.
pub type ProductionGenerator = Stage1;
/// The device the production router drives.
pub type ProductionDevice = D3d12Device;

/// Builds the production router, or `None` when no D3D12 backend is linked (the offline host
/// installs its own).
#[cfg(windows)]
pub fn build_service(faults: Arc<Faults>) -> Option<impl ffx_model::FfxService + 'static> {
    let objects = Arc::new(D3d12FrameObjects);
    let devices: Arc<dyn DeviceResolver<ProductionDevice>> = Arc::new(D3d12Devices::default());
    let swapchains: Arc<dyn SwapchainFactory> = Arc::new(Dx12Swapchains::new());
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

// ---------------------------------------------------------------------------------------------
// The swapchain factory
// ---------------------------------------------------------------------------------------------

/// Builds or adopts the native chain and returns the wrapper the game sees.
pub struct Dx12Swapchains {
    /// One present queue per device for the process (K140).
    queues: Arc<SharedPresentQueues<D3d12Device>>,
    clock: Arc<dyn Clock>,
    /// The overlay's settings, timing and stats. Built from the loaded configuration; one per
    /// process, shared by every host and the present hook.
    overlay: Arc<OverlayServices>,
}

impl Dx12Swapchains {
    pub fn new() -> Self {
        let clock: Arc<dyn Clock> = Arc::new(super::win32::timer::Win32Clock::new());
        let config = crate::entry::settings()
            .map(|loaded| loaded.settings.clone())
            .unwrap_or_default();
        Self {
            // K13/K140: a Present-hooking overlay (RTSS) reuses a destroyed queue across
            // swapchains, so the queue is kept for the process while it is loaded.
            queues: Arc::new(SharedPresentQueues::new(super::win32::modules::rtss_loaded())),
            overlay: OverlayServices::new(config, Arc::clone(&clock)),
            clock,
        }
    }

    /// The chain, queue and window a create names. The New and HWND forms build on the game's
    /// factory; the wrap form adopts the reference the game handed over (K12).
    fn native_chain(
        &self,
        desc: &ffx_model::CreateContextDesc,
        origin: WrapOrigin,
    ) -> Result<(IDXGISwapChain4, ID3D12CommandQueue, HWND), String> {
        if origin == WrapOrigin::Wrapped {
            let wrap = desc.swapchain_wrap.ok_or_else(|| "swapchain wrap without a chain".to_owned())?;
            if wrap.swapchain == 0 {
                return Err("swapchain wrap without a chain".to_owned());
            }
            // SAFETY: the FFX wrap desc hands over exactly one reference to the chain; this adopts
            // it (`Attach` in the C++), so the game never sees that reference again.
            let chain = unsafe { IDXGISwapChain4::from_raw(wrap.swapchain as *mut _) };
            let queue = adopt_queue(wrap.game_queue)?;
            let window = super::win32::identity::chain_window(&chain).unwrap_or_default();
            return Ok((chain, queue, window));
        }
        if let Some(created) = desc.swapchain_new {
            let factory = adopt_factory(created.dxgi_factory)?;
            let request = legacy_desc(created.desc)?;
            let queue = adopt_queue(created.game_queue)?;
            let mut out: Option<IDXGISwapChain> = None;
            // SAFETY: the request is a fully initialised local and the queue is the game's live
            // one; the factory keeps every reference it needs.
            let hr = unsafe { factory.CreateSwapChain(&queue, &request, &mut out) };
            if hr.is_err() {
                return Err(format!("swapchain new: CreateSwapChain failed ({:#010x})", hr.0));
            }
            let chain = out.ok_or_else(|| "swapchain new: CreateSwapChain returned no chain".to_owned())?;
            let chain: IDXGISwapChain4 = chain.cast().map_err(|error| {
                format!("swapchain new: the chain is not IDXGISwapChain4 ({:#010x})", error.code().0)
            })?;
            let window = HWND(created.desc.map_or(std::ptr::null_mut(), |d| d.output_window as *mut _));
            return Ok((chain, queue, window));
        }
        let form = desc.swapchain_for_hwnd.ok_or_else(|| "swapchain create names no form".to_owned())?;
        let factory: IDXGIFactory2 = adopt_factory(form.dxgi_factory)?
            .cast()
            .map_err(|error| format!("swapchain create: the factory is not IDXGIFactory2 ({:#010x})", error.code().0))?;
        let request = modern_desc(form.desc)?;
        if form.fullscreen_desc != 0 {
            // The ABI edge keeps only the pointer, not the fields, so a non-null fullscreen desc
            // cannot be reconstructed safely. Refuse rather than guess (K160).
            return Err("swapchain for hwnd: fullscreenDesc is not modelled by the ABI edge".to_owned());
        }
        let queue = adopt_queue(form.game_queue)?;
        // SAFETY: the request is a local that outlives the call and the queue is the game's live
        // one; no restrict-to-output is wanted.
        let chain = unsafe {
            factory.CreateSwapChainForHwnd(
                &queue,
                HWND(form.hwnd as *mut _),
                &request,
                None,
                None::<&IDXGIOutput>,
            )
        }
        .map_err(|error| format!("swapchain new: CreateSwapChainForHwnd failed ({:#010x})", error.code().0))?;
        let chain = chain.cast().map_err(|error| {
            format!("swapchain create: the chain is not IDXGISwapChain4 ({:#010x})", error.code().0)
        })?;
        Ok((chain, queue, HWND(form.hwnd as *mut _)))
    }

    /// The presentation choice from the loaded configuration, with the shipped defaults.
    fn choice(&self) -> PresentChoice {
        let Some(loaded) = crate::entry::settings() else { return PresentChoice::default() };
        let fg = &loaded.settings.fg;
        PresentChoice {
            present: fg.present,
            guard: fg.tear_guard,
            multiplier: fg.multiplier,
            target_fps: fg.target_fps,
        }
    }

    /// The environment knobs the application layer may not read itself.
    fn environment(&self) -> WrapEnvironment {
        WrapEnvironment {
            scanline: None,
            rtss_loaded: super::win32::modules::rtss_loaded(),
            lead_seconds: crate::application::presentation::swapchain::scanline_lead_us()
                .map_or(0.00025, |us| us / 1_000_000.0),
            force_sync_override: std::env::var("SEDICI_FG_SYNC").ok().map(|t| t.starts_with('1')),
            align_vblank: std::env::var("SEDICI_FG_ALIGN").ok().is_some_and(|t| t.starts_with('1')),
        }
    }
}

impl Default for Dx12Swapchains {
    fn default() -> Self {
        Self::new()
    }
}

impl SwapchainFactory for Dx12Swapchains {
    fn create_wrapper(
        &self,
        desc: &ffx_model::CreateContextDesc,
        origin: WrapOrigin,
        owner: ffx_model::OpaqueHandle,
    ) -> Result<(ffx_model::OpaqueHandle, Arc<dyn Wrapper>), ffx_model::Rejection> {
        let _ = owner; // The session supplies its own owner key when it installs its sink.
        let (chain, queue, window) = self.native_chain(desc, origin).map_err(ffx_model::Rejection::error)?;
        let native = DxgiNative::attach(&chain, &queue).map_err(|error| {
            ffx_model::Rejection::error(format!("swapchain wrap failed ({:#010x})", error.code()))
        })?;
        let handle = native.chain_handle();
        let device: Arc<D3d12Device> = Arc::new(D3d12Device::from_raw(native.device()));
        let queue = Arc::new(D3d12Queue::from_adopted(&queue));

        // The overlay host owns the game's device and window: the first draw creates the egui/D3D12
        // objects on this device and format, and the window procedure routes its hotkey and input.
        // It is installed over the game's window before the wrapper gives it to the game, so the
        // hotkey and cursor path work from the first Present (K161 resolved), and it disables itself
        // with one log line on any failure (it never fails a game call).
        let window_handle = crate::application::ports::input::WindowHandle(window.0 as isize);
        let overlay: Arc<OverlayHost> = OverlayHost::production(
            Arc::clone(&self.overlay),
            (*device).clone(),
            window_handle,
        );
        self.overlay.on_swapchain_event("create");
        if !window.0.is_null() {
            overlay.install_input();
        }

        let mut report = WrapReport::default();
        let wrapper = Swapchain::<D3d12Device, DxgiNative, SessionDriven<D3d12Device>, OverlayHost>::wrap(
            Arc::new(native),
            device,
            queue,
            Arc::clone(&self.clock),
            self.choice(),
            Arc::clone(&self.queues),
            Arc::clone(&overlay),
            self.environment(),
            // Exactly one reference reaches the wrapper in both forms: the wrap desc's handed-over
            // reference, or the creation reference of a chain Sedici built (`WrapHandle::create(native,
            // queue, 1)` in the C++). Passing 0 for a created chain would leak that reference and make
            // "vrr" refuse to recreate the chain.
            1,
            &mut report,
        )
        .map_err(|error| {
            ffx_model::Rejection::error(format!("swapchain wrap failed ({:#010x})", error.code()))
        })?;
        if let Some(reason) = report.fallback_reason {
            tracing::info!("fg: \"vrr\" present unavailable ({reason}); falling back to \"vsync\"");
        }
        // The overlay's "vrr" presenter draws and presents on its own thread, with the same host.
        wrapper.set_output_hooks(Arc::clone(&overlay));
        // The real-frame timing and limiter hook the wrapper calls once per application Present.
        let services = Arc::clone(&self.overlay);
        wrapper.set_present_hook(Some(Arc::new(move |qpc: u64| services.on_real_present(qpc))));
        let wrapper = Arc::new(wrapper);
        // The session context registers submissions against the wrapper's own fence timeline.
        wrapper.install_retirement(|fence| D3d12Retirement::new(fence.raw()).ok().map(|probe| Arc::new(probe) as Arc<dyn Retirement>));
        // The game receives the COM wrapper, never the native chain's identity.
        let com = DxgiWrapper::com_swapchain(Arc::clone(&wrapper));
        // One reference for the game's output slot; the COM object owns its own above.
        let game_handle = Interface::into_raw(com) as usize;
        let handle = WrapperHandle::<D3d12Device, DxgiNative>::new(wrapper, handle);
        Ok((game_handle, Arc::new(handle)))
    }
}

// ---------------------------------------------------------------------------------------------
// Descriptor and handle conversion
// ---------------------------------------------------------------------------------------------

fn adopt_factory(handle: ffx_model::OpaqueHandle) -> Result<IDXGIFactory, String> {
    if handle == 0 {
        return Err("swapchain create without a DXGI factory".to_owned());
    }
    let raw = handle as *mut std::ffi::c_void;
    // SAFETY: the FFX desc's factory is owned by the game and stays alive for the call; a borrowed
    // reference cloned here takes a legitimate AddRef, so this value cannot outlive it. Cloning
    // before `raw` goes out of scope keeps the owned value independent of the borrow.
    match unsafe { IDXGIFactory::from_raw_borrowed(&raw) } {
        Some(borrowed) => Ok(borrowed.clone()),
        None => Err("swapchain create without a DXGI factory".to_owned()),
    }
}

fn adopt_queue(handle: ffx_model::OpaqueHandle) -> Result<ID3D12CommandQueue, String> {
    if handle == 0 {
        return Err("swapchain create without a queue".to_owned());
    }
    let raw = handle as *mut std::ffi::c_void;
    // SAFETY: the FFX contract keeps the game's queue alive for the context's lifetime; cloning the
    // borrowed reference is a legitimate AddRef, never a release of the game's own.
    match unsafe { ID3D12CommandQueue::from_raw_borrowed(&raw) } {
        Some(borrowed) => Ok(borrowed.clone()),
        None => Err("swapchain create without a queue".to_owned()),
    }
}


/// Builds the legacy `DXGI_SWAP_CHAIN_DESC` the New form carries from the safe model.
fn legacy_desc(desc: Option<ffx_model::SwapchainDesc>) -> Result<DXGI_SWAP_CHAIN_DESC, String> {
    let desc = desc.ok_or_else(|| "swapchain new without a description".to_owned())?;
    Ok(DXGI_SWAP_CHAIN_DESC {
        BufferDesc: DXGI_MODE_DESC {
            Width: desc.width,
            Height: desc.height,
            RefreshRate: DXGI_RATIONAL {
                Numerator: desc.refresh_numerator,
                Denominator: desc.refresh_denominator,
            },
            Format: DXGI_FORMAT(desc.format as i32),
            ScanlineOrdering: DXGI_MODE_SCANLINE_ORDER(0),
            Scaling: DXGI_MODE_SCALING(0),
        },
        SampleDesc: DXGI_SAMPLE_DESC { Count: 1, Quality: 0 },
        BufferUsage: DXGI_USAGE(desc.usage),
        BufferCount: desc.buffer_count,
        OutputWindow: HWND(desc.output_window as *mut _),
        Windowed: windows::core::BOOL(i32::from(desc.windowed)),
        SwapEffect: DXGI_SWAP_EFFECT(desc.swap_effect as i32),
        Flags: desc.flags,
    })
}

/// Builds the modern `DXGI_SWAP_CHAIN_DESC1` the HWND form carries from the safe model.
fn modern_desc(desc: Option<ffx_model::SwapchainDesc1>) -> Result<DXGI_SWAP_CHAIN_DESC1, String> {
    let desc = desc.ok_or_else(|| "swapchain for hwnd without a description".to_owned())?;
    Ok(DXGI_SWAP_CHAIN_DESC1 {
        Width: desc.width,
        Height: desc.height,
        Format: DXGI_FORMAT(desc.format as i32),
        Stereo: windows::core::BOOL(i32::from(desc.stereo)),
        SampleDesc: DXGI_SAMPLE_DESC { Count: desc.sample_count, Quality: desc.sample_quality },
        BufferUsage: DXGI_USAGE(desc.usage),
        BufferCount: desc.buffer_count,
        Scaling: DXGI_SCALING(desc.scaling as i32),
        SwapEffect: DXGI_SWAP_EFFECT(desc.swap_effect as i32),
        AlphaMode: DXGI_ALPHA_MODE(desc.alpha_mode as i32),
        Flags: desc.flags,
    })
}

// ---------------------------------------------------------------------------------------------
// Retirement, queue dependency, token namespace
// ---------------------------------------------------------------------------------------------

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
        // The context checks the fence's device before calling this. The production path records
        // generation on the game's own queue, where the prepare already holds by ordering, so this
        // side is not reached today; it reports the dependency unavailable instead of guessing at a
        // fence it cannot name.
        let _ = (retirement, value);
        let _ = &self.queue;
        Err(GpuError::failed("queue wait", crate::domain::gpu_policy::E_FAIL))
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

#[allow(dead_code)]
fn _unused(_: Option<&ID3D12CommandList>, _: Option<&DxgiWrapper>) {}

