//! Our swapchain as a COM object (ports the `IDXGISwapChain4` surface of
//! `presentation/swapchain.*`).
//!
//! The game is handed this object instead of the native chain. Everything Sedici does not intercept
//! forwards to the native chain; `Present`, `Present1`, `ResizeBuffers`, `ResizeBuffers1`,
//! `GetBuffer`, `GetCurrentBackBufferIndex` and `SetFullscreenState` go through the application
//! logic in [`crate::application::presentation::swapchain`], which owns the private back buffers,
//! the generation schedule and the submission bookkeeping.
//!
//! `QueryInterface` answers only the interfaces this object implements (and `IUnknown`): the native
//! chain's identity is never leaked, so the game cannot bypass the wrapper by casting.

use std::sync::Arc;

use windows::core::{implement, GUID, Interface, Result as WinResult, HRESULT};
use windows::core::BOOL;
use windows::Win32::Foundation::{HANDLE, HWND};
use windows::Win32::Graphics::Direct3D12::{ID3D12GraphicsCommandList, ID3D12Resource};
use windows::Win32::Graphics::Dxgi::Common::{
    DXGI_COLOR_SPACE_TYPE, DXGI_FORMAT, DXGI_MODE_DESC, DXGI_MODE_ROTATION,
};
use windows::Win32::Graphics::Dxgi::{
    IDXGIDeviceSubObject_Impl, IDXGIObject_Impl, IDXGIOutput, IDXGISwapChain1_Impl,
    IDXGISwapChain2_Impl, IDXGISwapChain3_Impl, IDXGISwapChain4, IDXGISwapChain4_Impl,
    IDXGISwapChain_Impl,
    DXGI_FRAME_STATISTICS, DXGI_HDR_METADATA_TYPE, DXGI_MATRIX_3X2_F, DXGI_PRESENT,
    DXGI_PRESENT_PARAMETERS, DXGI_RGBA, DXGI_SWAP_CHAIN_DESC, DXGI_SWAP_CHAIN_DESC1,
    DXGI_SWAP_CHAIN_FLAG, DXGI_SWAP_CHAIN_FULLSCREEN_DESC,
};
use windows::core::IUnknown;

use crate::application::ports::gpu::PixelFormat;
use crate::application::ports::swapchain::{
    Offset, PresentFlags, PresentParams, Rect,
};
use crate::application::presentation::swapchain::SwapchainError;

/// The application logic the COM surface delegates to. Implemented below for the production
/// [`crate::application::presentation::swapchain::Swapchain`]; tests implement it directly.
pub trait PresentBackend: Send + Sync {
    /// `Present`/`Present1`. `params` is `Some` when the game called `Present1`.
    fn present(&self, sync: u32, flags: PresentFlags, params: Option<PresentParams>) -> Result<(), SwapchainError>;

    fn resize(
        &self,
        count: u32,
        width: u32,
        height: u32,
        format: Option<PixelFormat>,
        flags: u32,
    ) -> Result<(), SwapchainError>;

    fn resize_buffers1(
        &self,
        count: u32,
        width: u32,
        height: u32,
        format: Option<PixelFormat>,
        flags: u32,
        node_masks: &[u32],
    ) -> Result<(), SwapchainError>;

    fn current_back_buffer_index(&self) -> u32;

    /// The game's private buffer for `index`, with its own reference and in `PRESENT`.
    fn back_buffer(&self, index: u32) -> Option<ID3D12Resource>;

    /// The scratch interpolation list and texture the game may record into (K131). The pointers stay
    /// valid for the wrapper's life; nothing recorded into them is executed or presented.
    fn interpolation_list(&self) -> Option<ID3D12GraphicsCommandList>;
    fn interpolation_texture(&self) -> Option<ID3D12Resource>;

    /// Called before a mode change so a flip from the presenter thread cannot race it.
    fn before_fullscreen_change(&self);
}

/// The wrapper COM object.
#[implement(IDXGISwapChain4)]
pub struct DxgiWrapper {
    native: IDXGISwapChain4,
    backend: Arc<dyn PresentBackend>,
}

impl std::fmt::Debug for DxgiWrapper {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("DxgiWrapper")
    }
}

/// Wraps `native` and returns the game-facing object. The wrapper holds its own reference to the
/// chain; the caller keeps whichever reference the FFX contract gives it.
pub fn wrap(native: &IDXGISwapChain4, backend: Arc<dyn PresentBackend>) -> IDXGISwapChain4 {
    DxgiWrapper { native: native.clone(), backend }.into()
}

fn hresult(error: SwapchainError) -> HRESULT {
    HRESULT(error.code() as i32)
}

fn present_flags(raw: DXGI_PRESENT) -> PresentFlags {
    PresentFlags(raw.0)
}

/// Copies the game's pointer-based Present1 metadata into plain values. The pointers are only valid
/// for this call; a failed copy is the owner-approved Present1 metadata error (K76).
///
/// # Safety
/// `params` must point at a `DXGI_PRESENT_PARAMETERS` whose `pDirtyRects` names `DirtyRectsCount`
/// rects and whose scroll pointers, when non-null, name one rect/point each, as DXGI documents.
unsafe fn copy_params(params: *const DXGI_PRESENT_PARAMETERS) -> Option<PresentParams> {
    if params.is_null() {
        return None;
    }
    // SAFETY: the caller guarantees `params` is valid for the duration of the call.
    let params = unsafe { &*params };
    let dirty = if params.pDirtyRects.is_null() || params.DirtyRectsCount == 0 {
        Vec::new()
    } else {
        // SAFETY: DXGI documents `pDirtyRects` as `DirtyRectsCount` rects.
        unsafe { std::slice::from_raw_parts(params.pDirtyRects, params.DirtyRectsCount as usize) }
            .iter()
            .map(|rect| Rect { left: rect.left, top: rect.top, right: rect.right, bottom: rect.bottom })
            .collect()
    };
    Some(PresentParams {
        dirty_rects: dirty,
        // SAFETY: a non-null scroll rect points at one rect.
        scroll_rect: (!params.pScrollRect.is_null()).then(|| {
            let rect = unsafe { *params.pScrollRect };
            Rect { left: rect.left, top: rect.top, right: rect.right, bottom: rect.bottom }
        }),
        // SAFETY: a non-null scroll offset points at one point.
        scroll_offset: (!params.pScrollOffset.is_null()).then(|| {
            let point = unsafe { *params.pScrollOffset };
            Offset { x: point.x, y: point.y }
        }),
    })
}

/// The `IDXGIObject` methods forward to the native chain.
impl IDXGIObject_Impl for DxgiWrapper_Impl {
    fn SetPrivateData(&self, name: *const GUID, datasize: u32, pdata: *const core::ffi::c_void) -> WinResult<()> {
        // SAFETY: the parameters are the caller's; the native chain validates them.
        unsafe { self.native.SetPrivateData(name, datasize, pdata) }
    }

    fn SetPrivateDataInterface(&self, name: *const GUID, punknown: windows::core::Ref<'_, IUnknown>) -> WinResult<()> {
        // SAFETY: passing the caller's interface reference through unchanged.
        unsafe { self.native.SetPrivateDataInterface(name, punknown.as_ref()) }
    }

    fn GetPrivateData(&self, name: *const GUID, pdatasize: *mut u32, pdata: *mut core::ffi::c_void) -> WinResult<()> {
        // SAFETY: the caller's out-parameters are forwarded unchanged.
        unsafe { self.native.GetPrivateData(name, pdatasize, pdata) }
    }

    fn GetParent(&self, riid: *const GUID, ppparent: *mut *mut core::ffi::c_void) -> WinResult<()> {
        // The returned interface is the native parent, never this wrapper.
        let parent = unsafe { self.native.GetParent::<IUnknown>() }.ok();
        query_interface_of(parent.as_ref(), riid, ppparent)
    }
}

impl IDXGIDeviceSubObject_Impl for DxgiWrapper_Impl {
    fn GetDevice(&self, riid: *const GUID, ppdevice: *mut *mut core::ffi::c_void) -> WinResult<()> {
        let device = unsafe { self.native.GetDevice::<IUnknown>() }.ok();
        query_interface_of(device.as_ref(), riid, ppdevice)
    }
}

impl IDXGISwapChain_Impl for DxgiWrapper_Impl {
    fn Present(&self, syncinterval: u32, flags: DXGI_PRESENT) -> HRESULT {
        match self.backend.present(syncinterval, present_flags(flags), None) {
            Ok(()) => HRESULT(0),
            Err(error) => hresult(error),
        }
    }

    fn GetBuffer(&self, buffer: u32, riid: *const GUID, ppsurface: *mut *mut core::ffi::c_void) -> WinResult<()> {
        if ppsurface.is_null() {
            return Err(windows::core::Error::from(windows::Win32::Foundation::E_POINTER));
        }
        // SAFETY: the slot is non-null and valid for one write.
        unsafe { *ppsurface = std::ptr::null_mut() };
        let Some(resource) = self.backend.back_buffer(buffer) else {
            return Err(windows::core::Error::from(HRESULT(
                SwapchainError::InvalidCall.code() as i32,
            )));
        };
        let object = resource.cast::<IUnknown>().ok();
        query_interface_of(object.as_ref(), riid, ppsurface)
    }

    fn SetFullscreenState(&self, fullscreen: BOOL, ptarget: windows::core::Ref<'_, IDXGIOutput>) -> WinResult<()> {
        // A mode change must not race a flip issued from the presenter thread.
        self.backend.before_fullscreen_change();
        // SAFETY: the target reference is forwarded unchanged.
        unsafe { self.native.SetFullscreenState(fullscreen.as_bool(), ptarget.as_ref()) }
    }

    fn GetFullscreenState(&self, pfullscreen: *mut BOOL, pptarget: windows::core::OutRef<'_, IDXGIOutput>) -> WinResult<()> {
        // SAFETY: the caller's out-parameters are forwarded unchanged. The output slot is
        // optional, so a null one stays null.
        // SAFETY: `OutRef` is `#[repr(transparent)]` over the interface's out pointer, so the cast
        // hands the native chain the same slot the caller gave us.
        let target: *mut Option<IDXGIOutput> = unsafe { std::mem::transmute(pptarget) };
        unsafe {
            self.native.GetFullscreenState(
                (!pfullscreen.is_null()).then_some(pfullscreen),
                Some(target),
            )
        }
    }

    fn GetDesc(&self) -> WinResult<DXGI_SWAP_CHAIN_DESC> {
        // SAFETY: a pure query.
        unsafe { self.native.GetDesc() }
    }

    fn ResizeBuffers(
        &self,
        buffercount: u32,
        width: u32,
        height: u32,
        newformat: DXGI_FORMAT,
        swapchainflags: &DXGI_SWAP_CHAIN_FLAG,
    ) -> WinResult<()> {
        let format = (newformat != windows::Win32::Graphics::Dxgi::Common::DXGI_FORMAT_UNKNOWN)
            .then(|| pixel_format(newformat));
        self.backend
            .resize(buffercount, width, height, format, swapchainflags.0 as u32)
            .map_err(|error| windows::core::Error::from(hresult(error)))
    }

    fn ResizeTarget(&self, pnewtargetparam: *const DXGI_MODE_DESC) -> WinResult<()> {
        // SAFETY: the caller's descriptor pointer is forwarded unchanged.
        unsafe { self.native.ResizeTarget(pnewtargetparam) }
    }

    fn GetContainingOutput(&self) -> WinResult<IDXGIOutput> {
        // SAFETY: a pure query.
        unsafe { self.native.GetContainingOutput() }
    }

    fn GetFrameStatistics(&self, pstats: *mut DXGI_FRAME_STATISTICS) -> WinResult<()> {
        // SAFETY: the caller's out-parameter is forwarded unchanged.
        unsafe { self.native.GetFrameStatistics(pstats) }
    }

    fn GetLastPresentCount(&self) -> WinResult<u32> {
        // SAFETY: a pure query.
        unsafe { self.native.GetLastPresentCount() }
    }
}

impl IDXGISwapChain1_Impl for DxgiWrapper_Impl {
    fn GetDesc1(&self) -> WinResult<DXGI_SWAP_CHAIN_DESC1> {
        // SAFETY: a pure query.
        unsafe { self.native.GetDesc1() }
    }

    fn GetFullscreenDesc(&self) -> WinResult<DXGI_SWAP_CHAIN_FULLSCREEN_DESC> {
        // SAFETY: a pure query.
        unsafe { self.native.GetFullscreenDesc() }
    }

    fn GetHwnd(&self) -> WinResult<HWND> {
        // SAFETY: a pure query.
        unsafe { self.native.GetHwnd() }
    }

    fn GetCoreWindow(&self, refiid: *const GUID, ppunk: *mut *mut core::ffi::c_void) -> WinResult<()> {
        let window = unsafe { self.native.GetCoreWindow::<IUnknown>() }.ok();
        query_interface_of(window.as_ref(), refiid, ppunk)
    }

    fn Present1(&self, syncinterval: u32, presentflags: DXGI_PRESENT, ppresentparameters: *const DXGI_PRESENT_PARAMETERS) -> HRESULT {
        if ppresentparameters.is_null() {
            return hresult(SwapchainError::InvalidCall);
        }
        // SAFETY: the caller passes a valid Present1 parameter block for the duration of the call.
        let params = unsafe { copy_params(ppresentparameters) };
        match self.backend.present(syncinterval, present_flags(presentflags), params) {
            Ok(()) => HRESULT(0),
            Err(error) => hresult(error),
        }
    }

    fn IsTemporaryMonoSupported(&self) -> BOOL {
        // SAFETY: a pure query.
        unsafe { self.native.IsTemporaryMonoSupported() }
    }

    fn GetRestrictToOutput(&self) -> WinResult<IDXGIOutput> {
        // SAFETY: a pure query.
        unsafe { self.native.GetRestrictToOutput() }
    }

    fn SetBackgroundColor(&self, pcolor: *const DXGI_RGBA) -> WinResult<()> {
        // SAFETY: the caller's colour pointer is forwarded unchanged.
        unsafe { self.native.SetBackgroundColor(pcolor) }
    }

    fn GetBackgroundColor(&self) -> WinResult<DXGI_RGBA> {
        // SAFETY: a pure query.
        unsafe { self.native.GetBackgroundColor() }
    }

    fn SetRotation(&self, rotation: DXGI_MODE_ROTATION) -> WinResult<()> {
        // SAFETY: a plain setting write.
        unsafe { self.native.SetRotation(rotation) }
    }

    fn GetRotation(&self) -> WinResult<DXGI_MODE_ROTATION> {
        // SAFETY: a pure query.
        unsafe { self.native.GetRotation() }
    }
}

impl IDXGISwapChain2_Impl for DxgiWrapper_Impl {
    fn SetSourceSize(&self, width: u32, height: u32) -> WinResult<()> {
        // SAFETY: a plain setting write.
        unsafe { self.native.SetSourceSize(width, height) }
    }

    fn GetSourceSize(&self, pwidth: *mut u32, pheight: *mut u32) -> WinResult<()> {
        // SAFETY: the caller's out-parameters are forwarded unchanged.
        unsafe { self.native.GetSourceSize(pwidth, pheight) }
    }

    fn SetMaximumFrameLatency(&self, maxlatency: u32) -> WinResult<()> {
        // SAFETY: a plain setting write.
        unsafe { self.native.SetMaximumFrameLatency(maxlatency) }
    }

    fn GetMaximumFrameLatency(&self) -> WinResult<u32> {
        // SAFETY: a pure query.
        unsafe { self.native.GetMaximumFrameLatency() }
    }

    fn GetFrameLatencyWaitableObject(&self) -> HANDLE {
        // SAFETY: a pure query; the handle belongs to the native chain and must not be closed.
        unsafe { self.native.GetFrameLatencyWaitableObject() }
    }

    fn SetMatrixTransform(&self, pmatrix: *const DXGI_MATRIX_3X2_F) -> WinResult<()> {
        // SAFETY: the caller's matrix pointer is forwarded unchanged.
        unsafe { self.native.SetMatrixTransform(pmatrix) }
    }

    fn GetMatrixTransform(&self, pmatrix: *mut DXGI_MATRIX_3X2_F) -> WinResult<()> {
        // SAFETY: the caller owns the out-parameter; the native chain writes it.
        unsafe { self.native.GetMatrixTransform(pmatrix) }
            .map_err(|error| windows::core::Error::from_hresult(error.code()))
    }
}

impl IDXGISwapChain3_Impl for DxgiWrapper_Impl {
    fn GetCurrentBackBufferIndex(&self) -> u32 {
        self.backend.current_back_buffer_index()
    }

    fn CheckColorSpaceSupport(&self, colorspace: DXGI_COLOR_SPACE_TYPE) -> WinResult<u32> {
        // SAFETY: a pure query.
        unsafe { self.native.CheckColorSpaceSupport(colorspace) }
    }

    fn SetColorSpace1(&self, colorspace: DXGI_COLOR_SPACE_TYPE) -> WinResult<()> {
        // SAFETY: a plain setting write.
        unsafe { self.native.SetColorSpace1(colorspace) }
    }

    fn ResizeBuffers1(
        &self,
        buffercount: u32,
        width: u32,
        height: u32,
        format: DXGI_FORMAT,
        swapchainflags: &DXGI_SWAP_CHAIN_FLAG,
        pcreationnodemask: *const u32,
        pppresentqueue: *const Option<IUnknown>,
    ) -> WinResult<()> {
        let buffers = buffercount.max(1) as usize;
        // SAFETY: DXGI documents both arrays with one entry per buffer requested.
        let masks = if pcreationnodemask.is_null() {
            Vec::new()
        } else {
            unsafe { std::slice::from_raw_parts(pcreationnodemask, buffers) }.to_vec()
        };
        // The chain stays on Sedici's present queue; a buffer named for another queue is refused.
        let foreign = if pppresentqueue.is_null() {
            false
        } else {
            // SAFETY: one optional interface per buffer, as DXGI documents.
            unsafe { std::slice::from_raw_parts(pppresentqueue, buffers) }
                .iter()
                .any(|queue| queue.is_none())
        };
        if foreign {
            return Err(windows::core::Error::from(hresult(SwapchainError::ForeignPresentQueue)));
        }
        let format = (format != windows::Win32::Graphics::Dxgi::Common::DXGI_FORMAT_UNKNOWN)
            .then(|| pixel_format(format));
        self.backend
            .resize_buffers1(buffercount, width, height, format, swapchainflags.0 as u32, &masks)
            .map_err(|error| windows::core::Error::from(hresult(error)))
    }
}

impl IDXGISwapChain4_Impl for DxgiWrapper_Impl {
    fn SetHDRMetaData(&self, r#type: DXGI_HDR_METADATA_TYPE, size: u32, pmetadata: *const core::ffi::c_void) -> WinResult<()> {
        // SAFETY: the metadata buffer is the caller's and valid for `size` bytes for the call.
        unsafe {
            let bytes = if pmetadata.is_null() {
                None
            } else {
                Some(std::slice::from_raw_parts(pmetadata.cast::<u8>(), size as usize))
            };
            self.native.SetHDRMetaData(r#type, bytes)
                .map_err(|error| windows::core::Error::from_hresult(error.code()))
        }
    }
}

/// Answers a QueryInterface for an interface the native object holds, never for this wrapper: the
/// wrapper's own `#[implement]` surface answers its own identifiers before this is reached.
fn query_interface_of(
    object: Option<&IUnknown>,
    riid: *const GUID,
    out: *mut *mut core::ffi::c_void,
) -> WinResult<()> {
    match object {
        Some(object) => {
            // SAFETY: the object is live and the caller owns the identifier and the out-slot.
            let vtbl = windows::core::Interface::vtable(object);
            let hr = unsafe {
                (vtbl.QueryInterface)(windows::core::Interface::as_raw(object), riid, out)
            };
            hr.ok()
        }
        None => Err(windows::core::Error::from_hresult(HRESULT(0x8000_4005_u32 as i32))),
    }
}


fn pixel_format(format: DXGI_FORMAT) -> PixelFormat {
    if format == windows::Win32::Graphics::Dxgi::Common::DXGI_FORMAT_R8G8B8A8_UNORM {
        PixelFormat::R8G8B8A8Unorm
    } else if format == windows::Win32::Graphics::Dxgi::Common::DXGI_FORMAT_R10G10B10A2_UNORM {
        PixelFormat::R10G10B10A2Unorm
    } else {
        PixelFormat::Unknown
    }
}

/// The scratch objects the game may query (P4.8, K131); not part of the wrapper's presentation.
pub fn interpolation_objects(
    backend: &dyn PresentBackend,
) -> (Option<ID3D12GraphicsCommandList>, Option<ID3D12Resource>) {
    (backend.interpolation_list(), backend.interpolation_texture())
}
