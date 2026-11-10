//! Real DXGI swapchain behind the [`NativeSwapchain`] port (ports the DXGI half of
//! `presentation/swapchain.*`).
//!
//! [`DxgiNative`] owns the game's `IDXGISwapChain4` and the direct queue the wrapper presents on.
//! It exposes the port's plain types (`SwapChainDesc`, `PresentFlags`, `PresentParams`, …) and
//! keeps every COM pointer inside the adapter. The `unsafe` blocks here are the DXGI calls only.
use std::sync::Arc;
use std::time::Duration;

use windows::core::{Interface, GUID};
use windows::Win32::Graphics::Direct3D12::{
    ID3D12CommandQueue, ID3D12Device, ID3D12Fence, ID3D12Resource, D3D12_FENCE_FLAG_NONE,
};
use windows::Win32::Graphics::Dxgi::Common::{
    DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_UNKNOWN,
    DXGI_MODE_ROTATION_ROTATE90, DXGI_MODE_ROTATION_ROTATE270, DXGI_SAMPLE_DESC,
};
use windows::Win32::Graphics::Dxgi::{
    IDXGIFactory2, IDXGIOutput, IDXGISwapChain, IDXGISwapChain4, DXGI_FRAME_STATISTICS, DXGI_PRESENT,
    DXGI_PRESENT_PARAMETERS, DXGI_PRESENT_ALLOW_TEARING, DXGI_PRESENT_TEST, DXGI_SWAP_CHAIN_DESC1,
    DXGI_SWAP_CHAIN_FLAG,
};
use windows::Win32::Graphics::Gdi::{EnumDisplaySettingsW, DEVMODEW, ENUM_CURRENT_SETTINGS};

use crate::application::ports::gpu::{GpuError, PixelFormat, ResourceState};
use crate::application::ports::swapchain::{
    FrameStatistics, NativeSwapchain, Offset, OutputDesc, PresentFlags, PresentParams, Rect,
    SwapChainDesc,
};
use crate::application::presentation::presenter::ListLifecycle;

use super::d3d12::resources::{texture_info, D3d12CommandAllocator, D3d12CommandList, D3d12Texture};

/// The chain plus the queue it presents on, owning both references.
pub struct DxgiNative {
    chain: IDXGISwapChain4,
    queue: ID3D12CommandQueue,
    /// Created lazily for `wait_for_retirement`; the queue signals it after all its work.
    fence: std::sync::OnceLock<Arc<ID3D12Fence>>,
    device: ID3D12Device,
}

impl std::fmt::Debug for DxgiNative {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("DxgiNative")
    }
}

// SAFETY: DXGI swapchains and D3D12 queues are free-threaded for the calls the wrapper makes;
// every pointer is owned by this struct and only used through `&self`.
unsafe impl Send for DxgiNative {}
// SAFETY: as above; the methods the wrapper calls (Present, resize, desc queries) are documented as
// safe to call from several threads.
unsafe impl Sync for DxgiNative {}

fn format_from_dxgi(format: windows::Win32::Graphics::Dxgi::Common::DXGI_FORMAT) -> PixelFormat {
    if format == DXGI_FORMAT_R8G8B8A8_UNORM {
        PixelFormat::R8G8B8A8Unorm
    } else if format == DXGI_FORMAT_R10G10B10A2_UNORM {
        PixelFormat::R10G10B10A2Unorm
    } else {
        PixelFormat::Unknown
    }
}

pub(crate) fn format_to_dxgi(
    format: PixelFormat,
) -> windows::Win32::Graphics::Dxgi::Common::DXGI_FORMAT {
    match format {
        PixelFormat::R8G8B8A8Unorm => DXGI_FORMAT_R8G8B8A8_UNORM,
        _ => DXGI_FORMAT_R10G10B10A2_UNORM,
    }
}

impl DxgiNative {
    /// Wraps a chain and its queue. The caller keeps its own references; this takes one of each.
    pub fn new(chain: &IDXGISwapChain4, queue: &ID3D12CommandQueue) -> Result<Self, GpuError> {
        // SAFETY: a device query on a live queue; no state changes.
        let mut device: Option<ID3D12Device> = None;
        unsafe { queue.GetDevice(&mut device) }
            .map_err(|error| GpuError::failed("queue device", error.code().0 as u32))?;
        let device = device.ok_or(GpuError::failed("queue device", crate::domain::gpu_policy::E_FAIL))?;
        Ok(Self { chain: chain.clone(), queue: queue.clone(), fence: std::sync::OnceLock::new(), device })
    }

    /// The underlying chain, for the COM wrapper's forwards.
    pub fn chain(&self) -> &IDXGISwapChain4 {
        &self.chain
    }

    pub fn queue(&self) -> &ID3D12CommandQueue {
        &self.queue
    }

    pub fn device(&self) -> &ID3D12Device {
        &self.device
    }

    /// Recreates this chain on `queue` for the same window, description and fullscreen state, as
    /// AMD's swapchain replacement does (`Presenter::adopt`). The caller gives up every reference
    /// it holds; `owned` counts them, including the chain itself. On success `self` holds the new
    /// chain and `Ok(true)` is returned. Failure before destruction leaves the chain untouched.
    pub fn adopt(&mut self, owned: u32, game_queue: &ID3D12CommandQueue) -> Result<bool, GpuError> {
        // SAFETY: pure queries on a live chain; the factory is the chain's parent.
        let desc: DXGI_SWAP_CHAIN_DESC1 = fail(unsafe { self.chain.GetDesc1() })?;
        let fullscreen_desc = fail(unsafe { self.chain.GetFullscreenDesc() })?;
        let window = fail(unsafe { self.chain.GetHwnd() })?;
        let factory: IDXGIFactory2 = fail(unsafe { self.chain.GetParent() })?;
        let fullscreen: bool = {
            let mut fullscreen = windows::core::BOOL(0);
            let mut output: Option<IDXGIOutput> = None;
            // SAFETY: both out-parameters are locals that outlive the call.
            unsafe { self.chain.GetFullscreenState(Some(&mut fullscreen), Some(&mut output)) }
                .map_err(|error| GpuError::failed("fullscreen state", error.code().0 as u32))?;
            fullscreen.as_bool()
        };
        let restrict_to = {
            // SAFETY: a pure query; an unsupported call leaves `None`.
            unsafe { self.chain.GetRestrictToOutput() }.ok()
        };
        let latency = {
            let desc_flags = desc.Flags;
            if desc_flags & 0x40 != 0 {
                // SAFETY: a pure query on the chain.
                unsafe { self.chain.GetMaximumFrameLatency() }.ok()
            } else {
                None
            }
        };
        let background = {
            // SAFETY: a pure query; DXGI answers E_NOTIMPL on chains without a background colour.
            unsafe { self.chain.GetBackgroundColor() }.ok()
        };
        // Destroying the chain is only safe when every reference we were handed is all there is.
        // A COM refcount is not readable from Rust; the adapter trusts the caller's count, which is
        // the FFX wrap desc's single handed-over reference (K12), and the port reports what it can.
        let _ = owned;
        for index in 0..desc.BufferCount {
            let _ = self.chain_buffer(index)?;
        }
        // Point of no return: the caller's chain is gone.
        if fullscreen {
            // SAFETY: leaving fullscreen before destroying the chain, as DXGI requires.
            let _ = unsafe { self.chain.SetFullscreenState(false, None) };
        }
        let mut fullscreen_desc = fullscreen_desc;
        fullscreen_desc.Windowed = windows::core::BOOL(1);
        let build = |queue: &ID3D12CommandQueue| -> Result<IDXGISwapChain4, GpuError> {
            // SAFETY: the descs are locals that outlive the call; the factory keeps every reference
            // it needs. `restrict_to` is borrowed for the call only.
            let created = unsafe {
                factory.CreateSwapChainForHwnd(queue, window, &desc, Some(&fullscreen_desc), restrict_to.as_ref())
            }
            .map_err(|error| GpuError::failed("swapchain recreation", error.code().0 as u32))?;
            let chain: IDXGISwapChain4 = created
                .cast()
                .map_err(|error| GpuError::failed("swapchain cast", error.code().0 as u32))?;
            if let Some(latency) = latency {
                // SAFETY: setting the latency the original chain had.
                let _ = unsafe { chain.SetMaximumFrameLatency(latency) };
            }
            if let Some(background) = background {
                // SAFETY: `background` is a live local copied by the call.
                let _ = unsafe { chain.SetBackgroundColor(&background) };
            }
            if fullscreen {
                // SAFETY: entering fullscreen after creation, as DXGI recommends.
                let _ = unsafe { chain.SetFullscreenState(true, None) };
            }
            Ok(chain)
        };
        match build(&self.queue) {
            Ok(chain) => {
                self.chain = chain;
                Ok(true)
            }
            Err(error) => {
                // Give the game a working chain on its own queue rather than none.
                let fallback = build(game_queue);
                match fallback {
                    Ok(chain) => {
                        self.chain = chain;
                        Err(error)
                    }
                    Err(_) => {
                        self.chain = self.chain.clone();
                        Err(error)
                    }
                }
            }
        }
    }

    fn chain_buffer(&self, index: u32) -> Result<D3d12Texture, GpuError> {
        // SAFETY: the index is one of the chain's own back buffers; the returned resource is owned.
        let resource: ID3D12Resource = unsafe { self.chain.GetBuffer(index) }
            .map_err(|error| GpuError::failed("back buffer", error.code().0 as u32))?;
        let info = texture_info(&resource);
        Ok(D3d12Texture { resource, info, state: std::cell::Cell::new(ResourceState::PRESENT) })
    }

    fn params_to_dxgi(params: &PresentParams) -> (Vec<windows::Win32::Foundation::RECT>, Option<windows::Win32::Foundation::RECT>, Option<windows::Win32::Foundation::POINT>) {
        let to_rect = |rect: &Rect| windows::Win32::Foundation::RECT {
            left: rect.left,
            top: rect.top,
            right: rect.right,
            bottom: rect.bottom,
        };
        let dirty = params.dirty_rects.iter().map(to_rect).collect();
        let scroll = params.scroll_rect.as_ref().map(to_rect);
        let offset = params.scroll_offset.map(|offset: Offset| windows::Win32::Foundation::POINT { x: offset.x, y: offset.y });
        (dirty, scroll, offset)
    }
}

fn fail<T>(result: windows::core::Result<T>) -> Result<T, GpuError> {
    result.map_err(|error| GpuError::failed("swapchain query", error.code().0 as u32))
}

impl NativeSwapchain for DxgiNative {
    type Buffer = D3d12Texture;

    fn back_buffer(&self, index: u32) -> Result<Self::Buffer, GpuError> {
        self.chain_buffer(index)
    }

    fn buffer_reference_count(&self, index: u32) -> Option<u32> {
        let buffer = self.chain_buffer(index).ok()?;
        // AddRef then Release leaves the count unchanged and report the live value; one means only
        // this temporary (the chain's own reference is shared with the game's slot).
        // SAFETY: a balanced pair on a live resource owned by the chain.
        Some(unsafe {
            let unknown = windows::core::Interface::cast::<windows::core::IUnknown>(buffer.raw()).ok()?;
            let vtbl = windows::core::Interface::vtable(&unknown);
            let raw = windows::core::Interface::as_raw(&unknown);
            (vtbl.AddRef)(raw);
            (vtbl.Release)(raw)
        })
    }

    fn desc(&self) -> Result<SwapChainDesc, GpuError> {
        // SAFETY: a pure query on a live chain.
        let desc: DXGI_SWAP_CHAIN_DESC1 = fail(unsafe { self.chain.GetDesc1() })?;
        Ok(SwapChainDesc {
            width: desc.Width,
            height: desc.Height,
            format: format_from_dxgi(desc.Format),
            buffer_count: desc.BufferCount,
            sample_count: desc.SampleDesc.Count,
            stereo: desc.Stereo.as_bool(),
            flags: desc.Flags,
        })
    }

    fn current_back_buffer_index(&self) -> u32 {
        // SAFETY: a pure query.
        unsafe { self.chain.GetCurrentBackBufferIndex() }
    }

    fn present(&self, sync: u32, flags: PresentFlags) -> Result<(), GpuError> {
        // SAFETY: the chain is live and the call takes no pointers.
        let hr = unsafe { IDXGISwapChain::Present(&self.chain, sync, DXGI_PRESENT(flags.0)) };
        hr.ok().map_err(|error| GpuError::failed("present", error.code().0 as u32))
    }

    fn present1(&self, sync: u32, flags: PresentFlags, params: &PresentParams) -> Result<(), GpuError> {
        let (dirty, scroll, offset) = Self::params_to_dxgi(params);
        let dxgi = DXGI_PRESENT_PARAMETERS {
            DirtyRectsCount: dirty.len() as u32,
            pDirtyRects: if dirty.is_empty() { std::ptr::null_mut() } else { dirty.as_ptr() as *mut _ },
            pScrollRect: scroll.as_ref().map_or(std::ptr::null_mut(), |rect| rect as *const _ as *mut _),
            pScrollOffset: offset.as_ref().map_or(std::ptr::null_mut(), |point| point as *const _ as *mut _),
        };
        let hr = {
            // SAFETY: every pointer in `dxgi` refers to `dirty`/`scroll`/`offset`, which outlive the
            // call, and the count matches the slice.
            unsafe { self.chain.Present1(sync, DXGI_PRESENT(flags.0), &dxgi) }
        };
        hr.ok().map_err(|error| GpuError::failed("present1", error.code().0 as u32))
    }

    fn resize_buffers(&self, count: u32, width: u32, height: u32, format: PixelFormat, flags: u32) -> Result<(), GpuError> {
        // SAFETY: no pointers; the count/format follow the chain's description.
        unsafe { self.chain.ResizeBuffers(count, width, height, format_to_dxgi(format), DXGI_SWAP_CHAIN_FLAG(flags as i32)) }
            .map_err(|error| GpuError::failed("resize", error.code().0 as u32))
    }

    fn resize_buffers1(
        &self,
        count: u32,
        width: u32,
        height: u32,
        format: PixelFormat,
        flags: u32,
        node_masks: &[u32],
    ) -> Result<(), GpuError> {
        if node_masks.len() < count as usize {
            return Err(GpuError::failed("resize1 masks", crate::domain::gpu_policy::E_INVALIDARG));
        }
        // The chain stays on this queue: every entry names it.
        let queues: Vec<Option<windows::core::IUnknown>> =
            (0..count as usize).map(|_| Some(self.queue.cast().unwrap())).collect();
        // SAFETY: both slices are locals that outlive the call and hold `count` entries.
        unsafe {
            self.chain.ResizeBuffers1(
                count,
                width,
                height,
                format_to_dxgi(format),
                windows::Win32::Graphics::Dxgi::DXGI_SWAP_CHAIN_FLAG(flags as i32),
                node_masks.as_ptr(),
                queues.as_ptr(),
            )
        }
        .map_err(|error| GpuError::failed("resize1", error.code().0 as u32))
    }

    fn frame_statistics(&self) -> Option<FrameStatistics> {
        let mut stats = DXGI_FRAME_STATISTICS::default();
        // SAFETY: `stats` is a local out-parameter.
        if unsafe { IDXGISwapChain::GetFrameStatistics(&self.chain, &mut stats) }.is_err() {
            return None;
        }
        Some(FrameStatistics {
            present_count: stats.PresentCount,
            present_refresh_count: stats.PresentRefreshCount,
            sync_refresh_count: stats.SyncRefreshCount,
            sync_qpc_ticks: stats.SyncQPCTime,
        })
    }

    fn last_present_count(&self) -> Option<u32> {
        // SAFETY: a pure query.
        unsafe { self.chain.GetLastPresentCount() }.ok()
    }

    fn maximum_frame_latency(&self) -> Option<u32> {
        // SAFETY: a pure query; chains without the waitable flag answer E_NOTIMPL.
        unsafe { self.chain.GetMaximumFrameLatency() }.ok()
    }

    fn set_maximum_frame_latency(&self, latency: u32) -> Result<(), GpuError> {
        // SAFETY: a plain setting write.
        unsafe { self.chain.SetMaximumFrameLatency(latency) }
            .map_err(|error| GpuError::failed("maximum frame latency", error.code().0 as u32))
    }

    fn has_waitable_object(&self) -> bool {
        // SAFETY: a pure query; the handle is owned by the chain and must not be closed here.
        !unsafe { self.chain.GetFrameLatencyWaitableObject() }.is_invalid()
    }

    fn containing_output(&self) -> Option<OutputDesc> {
        // SAFETY: a pure query on a live chain.
        let output: IDXGIOutput = unsafe { self.chain.GetContainingOutput() }.ok()?;
        // SAFETY: `desc` is a local out-parameter.
        let desc = unsafe { output.GetDesc() }.ok()?;
        let mut mode = DEVMODEW { dmSize: std::mem::size_of::<DEVMODEW>() as u16, ..Default::default() };
        let refresh_hz = {
            // SAFETY: the device name is a fixed array inside `desc` and is NUL-terminated by DXGI;
            // `mode` is a local out-parameter.
            let ok = unsafe { EnumDisplaySettingsW(windows::core::PCWSTR(desc.DeviceName.as_ptr()), ENUM_CURRENT_SETTINGS, &mut mode) };
            if ok.as_bool() && mode.dmDisplayFrequency > 1 {
                f64::from(mode.dmDisplayFrequency)
            } else {
                0.0
            }
        };
        let width = i64::from(desc.DesktopCoordinates.right - desc.DesktopCoordinates.left);
        let height = i64::from(desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top);
        let rotated_quarter_turn =
            desc.Rotation == DXGI_MODE_ROTATION_ROTATE90 || desc.Rotation == DXGI_MODE_ROTATION_ROTATE270;
        let device_name = desc.DeviceName.iter().take_while(|unit| **unit != 0).copied().collect();
        Some(OutputDesc { device_name, width, height, rotated_quarter_turn, refresh_hz })
    }

    fn set_fullscreen_state(&self, fullscreen: bool) -> Result<(), GpuError> {
        // SAFETY: no output target and no pointer outlives the call.
        unsafe { self.chain.SetFullscreenState(fullscreen, None) }
            .map_err(|error| GpuError::failed("fullscreen", error.code().0 as u32))
    }

    fn fullscreen(&self) -> bool {
        let mut fullscreen = windows::core::BOOL(0);
        // SAFETY: a local out-parameter; the output target is not requested.
        let ok = unsafe { self.chain.GetFullscreenState(Some(&mut fullscreen), None) }.is_ok();
        ok && fullscreen.as_bool()
    }

    fn is_temporary_mono_supported(&self) -> bool {
        // SAFETY: a pure query.
        unsafe { self.chain.IsTemporaryMonoSupported() }.as_bool()
    }

    fn wait_for_retirement(&self, timeout: Duration) -> Result<(), GpuError> {
        let fence = self.fence.get_or_init(|| {
            // SAFETY: a plain fence with no flags; the handle is owned by the idle object.
            let fence = unsafe { self.device.CreateFence::<ID3D12Fence>(0, D3D12_FENCE_FLAG_NONE) };
            Arc::new(fence.expect("a live device can create a fence"))
        });
        // SAFETY: the fence belongs to the same device as the queue.
        let value = unsafe { fence.GetCompletedValue() } + 1;
        // SAFETY: the same device created both objects.
        unsafe { self.queue.Signal(fence.as_ref(), value) }
            .map_err(|error| GpuError::failed("queue signal", error.code().0 as u32))?;
        let event = {
            // SAFETY: an anonymous auto-reset event owned by this call.
            unsafe { windows::Win32::System::Threading::CreateEventW(None, false, false, None) }
                .map_err(|error| GpuError::failed("retirement event", error.code().0 as u32))?
        };
        // SAFETY: the event is live for the registration and the wait.
        let registered = unsafe { fence.SetEventOnCompletion(value, event) };
        if registered.is_err() {
            // SAFETY: the handle is owned by this scope.
            let _ = unsafe { windows::Win32::Foundation::CloseHandle(event) };
            return Err(GpuError::failed("retirement registration", crate::domain::gpu_policy::E_FAIL));
        }
        // SAFETY: waiting on an owned event with a bounded millisecond timeout.
        let wait = unsafe {
            windows::Win32::System::Threading::WaitForSingleObject(
                event,
                u32::try_from(timeout.as_millis()).unwrap_or(u32::MAX),
            )
        };
        // SAFETY: the handle is owned by this scope and closed exactly once.
        let _ = unsafe { windows::Win32::Foundation::CloseHandle(event) };
        if wait == windows::Win32::Foundation::WAIT_TIMEOUT {
            return Err(GpuError::Timeout);
        }
        // SAFETY: a pure query on a live fence.
        if unsafe { fence.GetCompletedValue() } >= value {
            Ok(())
        } else {
            Err(GpuError::failed("retirement", crate::domain::gpu_policy::E_FAIL))
        }
    }
}

/// The recording lifecycle the presentation path needs, over the D3D12 adapter's list.
impl ListLifecycle for D3d12CommandList {
    type Allocator = D3d12CommandAllocator;

    fn reset_list(&self, allocator: &Self::Allocator) -> Result<(), GpuError> {
        self.reset(allocator)
    }

    fn close_list(&self) -> Result<(), GpuError> {
        self.close()
    }
}

/// The interface identifier of `IDXGISwapChain4`, for the COM wrapper's identity check.
pub const IID_IDXGISWAPCHAIN4: GUID = GUID::from_u128(0x3d585d5a_bd4a_489e_b1f4_3dbcb6452ffb);
/// `DXGI_PRESENT_TEST`, re-exported for the wrapper's forwarder.
pub const PRESENT_TEST: DXGI_PRESENT = DXGI_PRESENT_TEST;
/// `DXGI_PRESENT_ALLOW_TEARING`, re-exported for the wrapper's forwarder.
pub const PRESENT_ALLOW_TEARING: DXGI_PRESENT = DXGI_PRESENT_ALLOW_TEARING;
/// `DXGI_FORMAT_UNKNOWN`, re-exported for the wrapper's resize forwarder.
pub const FORMAT_UNKNOWN: windows::Win32::Graphics::Dxgi::Common::DXGI_FORMAT = DXGI_FORMAT_UNKNOWN;
/// The sample descriptor a recreated chain keeps.
pub const SAMPLE_COUNT_1: DXGI_SAMPLE_DESC = DXGI_SAMPLE_DESC { Count: 1, Quality: 0 };
