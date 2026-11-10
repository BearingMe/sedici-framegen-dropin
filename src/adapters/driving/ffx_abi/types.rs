//! `repr(C)` FFX descriptor types, hand-written from the vendored SDK headers in
//! `third_party/ffx/` (reference only; never modified, never run through bindgen).
//!
//! The types are reproduced exactly as the C declarations lay them out, including the C `bool`
//! (one byte) and the anonymous unions in `FfxApiResourceDescription`. `const` assertions on
//! `size_of`, `align_of` and the offsets of every field pin the layout to the header: a change in
//! either side fails the build rather than misreading a game descriptor.
//!
//! These raw types never leave this module and [`super::desc_walk`]; the exports convert them
//! into the safe values in [`crate::application::ffx_model`].
#![allow(non_camel_case_types, non_snake_case, dead_code)]

use std::mem::{align_of, offset_of, size_of};

// -------------------------------------------------------------------------------------------------
// Base types (ffx_api.h)
// -------------------------------------------------------------------------------------------------

pub type ffxContext = *mut std::ffi::c_void;
pub type ffxStructType_t = u64;
pub type ffxReturnCode_t = u32;

/// The FFX API return codes. `ffxReturnCode_t` is a `uint32_t` in the header, so the ABI value is
/// the bare integer; these constants name the integers.
pub const FFX_API_RETURN_OK: ffxReturnCode_t = 0;
pub const FFX_API_RETURN_ERROR: ffxReturnCode_t = 1;
pub const FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE: ffxReturnCode_t = 2;
pub const FFX_API_RETURN_ERROR_RUNTIME_ERROR: ffxReturnCode_t = 3;
pub const FFX_API_RETURN_NO_PROVIDER: ffxReturnCode_t = 4;
pub const FFX_API_RETURN_ERROR_MEMORY: ffxReturnCode_t = 5;
pub const FFX_API_RETURN_ERROR_PARAMETER: ffxReturnCode_t = 6;
pub const FFX_API_RETURN_PROVIDER_NO_SUPPORT_NEW_DESCTYPE: ffxReturnCode_t = 7;

/// `ffxApiHeader`: every descriptor starts with this.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ffxApiHeader {
    pub type_: ffxStructType_t,
    pub pNext: *mut ffxApiHeader,
}

pub type ffxCreateContextDescHeader = ffxApiHeader;
pub type ffxConfigureDescHeader = ffxApiHeader;
pub type ffxQueryDescHeader = ffxApiHeader;
pub type ffxDispatchDescHeader = ffxApiHeader;

/// `ffxAllocationCallbacks`. Sedici never allocates through the game's callbacks; the pointer is
/// accepted and ignored, as the C++ did.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ffxAllocationCallbacks {
    pub pUserData: *mut std::ffi::c_void,
    pub alloc: Option<unsafe extern "C" fn(*mut std::ffi::c_void, u64) -> *mut std::ffi::c_void>,
    pub dealloc: Option<unsafe extern "C" fn(*mut std::ffi::c_void, *mut std::ffi::c_void)>,
}

/// C `bool` is one byte, and the SDK uses it in every `bool` field.
pub type c_bool = u8;

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ffxConfigureDescGlobalDebug {
    pub header: ffxConfigureDescHeader,
    pub effectId: u64,
    pub fpMessage: Option<unsafe extern "C" fn(u32, *const u16)>,
    pub debugLevel: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ffxConfigureDescGlobalDebug1 {
    pub header: ffxConfigureDescHeader,
    pub fpMessage: Option<unsafe extern "C" fn(u32, *const u16)>,
    pub debugLevel: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ffxQueryDescGetVersions {
    pub header: ffxQueryDescHeader,
    pub createDescType: u64,
    pub device: *mut std::ffi::c_void,
    pub outputCount: *mut u64,
    pub versionIds: *mut u64,
    pub versionNames: *mut *const std::ffi::c_char,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ffxOverrideVersion {
    pub header: ffxApiHeader,
    pub versionId: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ffxQueryGetProviderVersion {
    pub header: ffxQueryDescHeader,
    pub versionId: u64,
    pub versionName: *const std::ffi::c_char,
}

// -------------------------------------------------------------------------------------------------
// Shared value types (ffx_api_types.h)
// -------------------------------------------------------------------------------------------------

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct FfxApiDimensions2D {
    pub width: u32,
    pub height: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct FfxApiFloatCoords2D {
    pub x: f32,
    pub y: f32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct FfxApiRect2D {
    pub left: i32,
    pub top: i32,
    pub width: i32,
    pub height: i32,
}

/// `FfxApiResourceDescription` with its three anonymous unions kept as the layout gives them:
/// each member is a `uint32_t`, so the union is a `uint32_t` and the `repr(C)` struct below is
/// byte-identical. The model names `width`/`height`/`depth` and treats them as `size`/`stride`/
/// `alignment` for buffers.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct FfxApiResourceDescription {
    pub type_: u32,
    pub format: u32,
    pub width_or_size: u32,
    pub height_or_stride: u32,
    pub depth_or_alignment: u32,
    pub mipCount: u32,
    pub flags: u32,
    pub usage: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct FfxApiResource {
    pub resource: *mut std::ffi::c_void,
    pub description: FfxApiResourceDescription,
    pub state: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct FfxApiEffectMemoryUsage {
    pub totalUsageInBytes: u64,
    pub aliasableUsageInBytes: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct FfxApiSwapchainFramePacingTuning {
    pub safetyMarginInMs: f32,
    pub varianceFactor: f32,
    pub allowHybridSpin: c_bool,
    pub hybridSpinTime: u32,
    pub allowWaitForSingleObjectOnFence: c_bool,
}

// -------------------------------------------------------------------------------------------------
// DXGI types the swapchain descriptors point at (dxgi.h / dxgi1_5.h)
// -------------------------------------------------------------------------------------------------

pub type IDXGISwapChain4 = *mut std::ffi::c_void;
pub type ID3D12CommandQueue = *mut std::ffi::c_void;
pub type ID3D12Device = *mut std::ffi::c_void;
pub type IDXGIFactory = *mut std::ffi::c_void;
pub type HWND = *mut std::ffi::c_void;

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct DXGI_RATIONAL {
    pub Numerator: u32,
    pub Denominator: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct DXGI_MODE_DESC {
    pub Width: u32,
    pub Height: u32,
    pub RefreshRate: DXGI_RATIONAL,
    pub Format: u32,
    pub ScanlineOrdering: u32,
    pub Scaling: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct DXGI_SAMPLE_DESC {
    pub Count: u32,
    pub Quality: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct DXGI_SWAP_CHAIN_DESC {
    pub BufferDesc: DXGI_MODE_DESC,
    pub SampleDesc: DXGI_SAMPLE_DESC,
    pub BufferUsage: u32,
    pub BufferCount: u32,
    pub OutputWindow: HWND,
    pub Windowed: i32, // BOOL: 4 bytes
    pub SwapEffect: u32,
    pub Flags: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct DXGI_SWAP_CHAIN_DESC1 {
    pub Width: u32,
    pub Height: u32,
    pub Format: u32,
    pub Stereo: i32, // BOOL: 4 bytes
    pub SampleDesc: DXGI_SAMPLE_DESC,
    pub BufferUsage: u32,
    pub BufferCount: u32,
    pub Scaling: u32,
    pub SwapEffect: u32,
    pub AlphaMode: u32,
    pub Flags: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct DXGI_SWAP_CHAIN_FULLSCREEN_DESC {
    pub RefreshRate: DXGI_RATIONAL,
    pub ScanlineOrdering: u32,
    pub Scaling: u32,
    pub Windowed: i32, // BOOL: 4 bytes
}

/// `PfnFfxConstantBufferAllocator`'s target; the desc stores bare function pointers, never
/// called by Sedici.
pub type FfxApiConstantBufferAllocator =
    Option<unsafe extern "C" fn(*mut std::ffi::c_void, u64) -> FfxApiConstantBufferAllocation>;

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct FfxApiConstantBufferAllocation {
    pub resource: FfxApiResource,
    pub handle: u64,
}

// -------------------------------------------------------------------------------------------------
// Backend descriptors (ffx_api_dx12.h)
// -------------------------------------------------------------------------------------------------

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ffxCreateBackendDX12Desc {
    pub header: ffxCreateContextDescHeader,
    pub device: *mut ID3D12Device,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ffxCreateBackendDX12AllocationCallbacksDesc {
    pub header: ffxCreateContextDescHeader,
    pub pfnFfxResourceAllocator: *mut std::ffi::c_void,
    pub pfnFfxResourceDeallocator: *mut std::ffi::c_void,
    pub pfnFfxHeapAllocator: *mut std::ffi::c_void,
    pub pfnFfxHeapDeallocator: *mut std::ffi::c_void,
    pub pfnFfxConstantBufferAllocator: FfxApiConstantBufferAllocator,
}

// -------------------------------------------------------------------------------------------------
// Frame generation descriptors (ffx_framegeneration.h)
// -------------------------------------------------------------------------------------------------

pub type FfxApiPresentCallbackFunc = Option<
    unsafe extern "C" fn(
        *mut ffxCallbackDescFrameGenerationPresent,
        *mut std::ffi::c_void,
    ) -> ffxReturnCode_t,
>;
pub type FfxApiFrameGenerationDispatchFunc = Option<
    unsafe extern "C" fn(
        *mut ffxDispatchDescFrameGeneration,
        *mut std::ffi::c_void,
    ) -> ffxReturnCode_t,
>;

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ffxCreateContextDescFrameGeneration {
    pub header: ffxCreateContextDescHeader,
    pub flags: u32,
    pub displaySize: FfxApiDimensions2D,
    pub maxRenderSize: FfxApiDimensions2D,
    pub backBufferFormat: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ffxCallbackDescFrameGenerationPresent {
    pub header: ffxDispatchDescHeader,
    pub device: *mut std::ffi::c_void,
    pub commandList: *mut std::ffi::c_void,
    pub currentBackBuffer: FfxApiResource,
    pub currentUI: FfxApiResource,
    pub outputSwapChainBuffer: FfxApiResource,
    pub isGeneratedFrame: c_bool,
    pub frameID: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ffxDispatchDescFrameGeneration {
    pub header: ffxDispatchDescHeader,
    pub commandList: *mut std::ffi::c_void,
    pub presentColor: FfxApiResource,
    pub outputs: [FfxApiResource; 4],
    pub numGeneratedFrames: u32,
    pub reset: c_bool,
    pub backbufferTransferFunction: u32,
    pub minMaxLuminance: [f32; 2],
    pub generationRect: FfxApiRect2D,
    pub frameID: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ffxConfigureDescFrameGeneration {
    pub header: ffxConfigureDescHeader,
    pub swapChain: *mut std::ffi::c_void,
    pub presentCallback: FfxApiPresentCallbackFunc,
    pub presentCallbackUserContext: *mut std::ffi::c_void,
    pub frameGenerationCallback: FfxApiFrameGenerationDispatchFunc,
    pub frameGenerationCallbackUserContext: *mut std::ffi::c_void,
    pub frameGenerationEnabled: c_bool,
    pub allowAsyncWorkloads: c_bool,
    pub HUDLessColor: FfxApiResource,
    pub flags: u32,
    pub onlyPresentGenerated: c_bool,
    pub generationRect: FfxApiRect2D,
    pub frameID: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ffxDispatchDescFrameGenerationPrepare {
    pub header: ffxDispatchDescHeader,
    pub frameID: u64,
    pub flags: u32,
    pub commandList: *mut std::ffi::c_void,
    pub renderSize: FfxApiDimensions2D,
    pub jitterOffset: FfxApiFloatCoords2D,
    pub motionVectorScale: FfxApiFloatCoords2D,
    pub frameTimeDelta: f32,
    pub unused_reset: c_bool,
    pub cameraNear: f32,
    pub cameraFar: f32,
    pub cameraFovAngleVertical: f32,
    pub viewSpaceToMetersFactor: f32,
    pub depth: FfxApiResource,
    pub motionVectors: FfxApiResource,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ffxConfigureDescFrameGenerationKeyValue {
    pub header: ffxConfigureDescHeader,
    pub key: u64,
    pub u64_: u64,
    pub ptr: *mut std::ffi::c_void,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ffxQueryDescFrameGenerationGetGPUMemoryUsage {
    pub header: ffxQueryDescHeader,
    pub gpuMemoryUsageFrameGeneration: *mut FfxApiEffectMemoryUsage,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ffxConfigureDescFrameGenerationRegisterDistortionFieldResource {
    pub header: ffxConfigureDescHeader,
    pub distortionField: FfxApiResource,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ffxCreateContextDescFrameGenerationHudless {
    pub header: ffxCreateContextDescHeader,
    pub hudlessBackBufferFormat: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ffxDispatchDescFrameGenerationPrepareCameraInfo {
    pub header: ffxConfigureDescHeader,
    pub cameraPosition: [f32; 3],
    pub cameraUp: [f32; 3],
    pub cameraRight: [f32; 3],
    pub cameraForward: [f32; 3],
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ffxQueryDescFrameGenerationGetGPUMemoryUsageV2 {
    pub header: ffxQueryDescHeader,
    pub device: *mut std::ffi::c_void,
    pub maxRenderSize: FfxApiDimensions2D,
    pub displaySize: FfxApiDimensions2D,
    pub createFlags: u32,
    pub dispatchFlags: u32,
    pub backBufferFormat: u32,
    pub hudlessBackBufferFormat: u32,
    pub gpuMemoryUsageFrameGeneration: *mut FfxApiEffectMemoryUsage,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ffxDispatchDescFrameGenerationPrepareV2 {
    pub header: ffxDispatchDescHeader,
    pub frameID: u64,
    pub flags: u32,
    pub commandList: *mut std::ffi::c_void,
    pub renderSize: FfxApiDimensions2D,
    pub jitterOffset: FfxApiFloatCoords2D,
    pub motionVectorScale: FfxApiFloatCoords2D,
    pub frameTimeDelta: f32,
    pub reset: c_bool,
    pub cameraNear: f32,
    pub cameraFar: f32,
    pub cameraFovAngleVertical: f32,
    pub viewSpaceToMetersFactor: f32,
    pub depth: FfxApiResource,
    pub motionVectors: FfxApiResource,
    pub cameraPosition: [f32; 3],
    pub cameraUp: [f32; 3],
    pub cameraRight: [f32; 3],
    pub cameraForward: [f32; 3],
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ffxCallbackDescFrameGenerationPresentPremulAlpha {
    pub header: ffxApiHeader,
    pub usePremulAlpha: c_bool,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ffxCreateContextDescFrameGenerationVersion {
    pub header: ffxCreateContextDescHeader,
    pub version: u32,
}

/// The deprecated `FfxFrameGenerationConfig`, still sent by older games.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct FfxFrameGenerationConfig {
    pub header: ffxApiHeader,
    pub swapChain: *mut std::ffi::c_void,
    pub presentCallback: FfxApiPresentCallbackFunc,
    pub presentCallbackContext: *mut std::ffi::c_void,
    pub frameGenerationCallback: FfxApiFrameGenerationDispatchFunc,
    pub frameGenerationCallbackContext: *mut std::ffi::c_void,
    pub frameGenerationEnabled: c_bool,
    pub allowAsyncWorkloads: c_bool,
    pub allowAsyncPresent: c_bool,
    pub HUDLessColor: FfxApiResource,
    pub flags: u32,
    pub onlyPresentInterpolated: c_bool,
    pub interpolationRect: FfxApiRect2D,
    pub frameID: u64,
    pub drawDebugPacingLines: c_bool,
}

// -------------------------------------------------------------------------------------------------
// Swapchain DX12 descriptors (ffx_api_framegeneration_dx12.h)
// -------------------------------------------------------------------------------------------------

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ffxCreateContextDescFrameGenerationSwapChainWrapDX12 {
    pub header: ffxCreateContextDescHeader,
    pub swapchain: *mut IDXGISwapChain4,
    pub gameQueue: *mut ID3D12CommandQueue,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ffxCreateContextDescFrameGenerationSwapChainNewDX12 {
    pub header: ffxCreateContextDescHeader,
    pub swapchain: *mut IDXGISwapChain4,
    pub desc: *mut DXGI_SWAP_CHAIN_DESC,
    pub dxgiFactory: *mut IDXGIFactory,
    pub gameQueue: *mut ID3D12CommandQueue,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 {
    pub header: ffxCreateContextDescHeader,
    pub swapchain: *mut IDXGISwapChain4,
    pub hwnd: HWND,
    pub desc: *mut DXGI_SWAP_CHAIN_DESC1,
    pub fullscreenDesc: *mut DXGI_SWAP_CHAIN_FULLSCREEN_DESC,
    pub dxgiFactory: *mut IDXGIFactory,
    pub gameQueue: *mut ID3D12CommandQueue,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12 {
    pub header: ffxConfigureDescHeader,
    pub uiResource: FfxApiResource,
    pub flags: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ffxQueryDescFrameGenerationSwapChainInterpolationCommandListDX12 {
    pub header: ffxQueryDescHeader,
    pub pOutCommandList: *mut *mut std::ffi::c_void,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ffxQueryDescFrameGenerationSwapChainInterpolationTextureDX12 {
    pub header: ffxQueryDescHeader,
    pub pOutTexture: *mut FfxApiResource,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ffxDispatchDescFrameGenerationSwapChainWaitForPresentsDX12 {
    pub header: ffxDispatchDescHeader,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ffxConfigureDescFrameGenerationSwapChainKeyValueDX12 {
    pub header: ffxConfigureDescHeader,
    pub key: u64,
    pub u64_: u64,
    pub ptr: *mut std::ffi::c_void,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ffxQueryFrameGenerationSwapChainGetGPUMemoryUsageDX12 {
    pub header: ffxQueryDescHeader,
    pub gpuMemoryUsageFrameGenerationSwapchain: *mut FfxApiEffectMemoryUsage,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ffxQueryFrameGenerationSwapChainGetGPUMemoryUsageDX12V2 {
    pub header: ffxQueryDescHeader,
    pub device: *mut std::ffi::c_void,
    pub displaySize: FfxApiDimensions2D,
    pub backBufferFormat: u32,
    pub backBufferCount: u32,
    pub uiResourceSize: FfxApiDimensions2D,
    pub uiResourceFormat: u32,
    pub flags: u32,
    pub gpuMemoryUsageFrameGenerationSwapchain: *mut FfxApiEffectMemoryUsage,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ffxCreateContextDescFrameGenerationSwapChainVersionDX12 {
    pub header: ffxCreateContextDescHeader,
    pub version: u32,
}

// -------------------------------------------------------------------------------------------------
// Descriptor type constants. Values are the macros of the headers, spelled out here so the raw
// module is self-contained; `tests/abi.rs` re-derives them from the headers' formulas.
// -------------------------------------------------------------------------------------------------

pub const FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION: u64 = 0x0002_0001;
pub const FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS: u64 = 0x0002_0009;
pub const FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION: u64 = 0x0002_000e;
pub const FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WRAP_DX12: u64 = 0x0003_0001;
pub const FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_NEW_DX12: u64 = 0x0003_0005;
pub const FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12: u64 =
    0x0003_0006;
pub const FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12: u64 = 0x0003_000b;
pub const FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12: u64 = 0x0000_0002;
pub const FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12_ALLOCATION_CALLBACKS: u64 = 0x0000_0003;
pub const FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION: u64 = 0x0002_0002;
pub const FFX_API_FRAME_GENERATION_CONFIG: u64 = 0x0002_000f;
pub const FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION_KEYVALUE: u64 = 0x0002_0006;
pub const FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION_REGISTERDISTORTIONRESOURCE: u64 = 0x0002_0008;
pub const FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_REGISTERUIRESOURCE_DX12: u64 =
    0x0003_0002;
pub const FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_KEYVALUE_DX12: u64 = 0x0003_0008;
pub const FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG: u64 = 7;
pub const FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG1: u64 = 1;
pub const FFX_API_DESC_TYPE_OVERRIDE_VERSION: u64 = 5;
pub const FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE: u64 = 0x0002_0004;
pub const FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2: u64 = 0x0002_000c;
pub const FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_CAMERAINFO: u64 = 0x0002_000a;
pub const FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION: u64 = 0x0002_0003;
pub const FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WAIT_FOR_PRESENTS_DX12: u64 =
    0x0003_0007;
pub const FFX_API_CALLBACK_DESC_TYPE_FRAMEGENERATION_PRESENT: u64 = 0x0002_0005;
pub const FFX_API_CALLBACK_DESC_TYPE_FRAMEGENERATION_PRESENT_PREMUL_ALPHA: u64 = 0x0002_000d;
pub const FFX_API_QUERY_DESC_TYPE_GET_VERSIONS: u64 = 4;
pub const FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION: u64 = 6;
pub const FFX_API_QUERY_DESC_TYPE_FRAMEGENERATION_GPU_MEMORY_USAGE: u64 = 0x0002_0007;
pub const FFX_API_QUERY_DESC_TYPE_FRAMEGENERATION_GPU_MEMORY_USAGE_V2: u64 = 0x0002_000b;
pub const FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_INTERPOLATIONCOMMANDLIST_DX12: u64 =
    0x0003_0003;
pub const FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_INTERPOLATIONTEXTURE_DX12: u64 =
    0x0003_0004;
pub const FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_GPU_MEMORY_USAGE_DX12: u64 = 0x0003_0009;
pub const FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_GPU_MEMORY_USAGE_DX12_V2: u64 =
    0x0003_000a;

/// Frame generation configure keys (`FfxApiConfigureFrameGenerationKey`).
pub const FFX_API_CONFIGURE_FRAMEGENERATION_KEY_DEBUG_VIEW_MODE: u64 = 0;
pub const FFX_API_CONFIGURE_FRAMEGENERATION_KEY_DEBUG_VIEW_FLOW_SCALE: u64 = 1;
/// Swapchain configure keys (`FfxApiConfigureFrameGenerationSwapChainKeyDX12`).
pub const FFX_API_CONFIGURE_FG_SWAPCHAIN_KEY_WAITCALLBACK: u64 = 0;
pub const FFX_API_CONFIGURE_FG_SWAPCHAIN_KEY_FRAMEPACINGTUNING: u64 = 2;

/// Resource type values (`FfxApiResourceType`).
pub const FFX_API_RESOURCE_TYPE_BUFFER: u32 = 0;
pub const FFX_API_RESOURCE_TYPE_TEXTURE1D: u32 = 1;
pub const FFX_API_RESOURCE_TYPE_TEXTURE2D: u32 = 2;
pub const FFX_API_RESOURCE_TYPE_TEXTURE_CUBE: u32 = 3;
pub const FFX_API_RESOURCE_TYPE_TEXTURE3D: u32 = 4;

/// The FFX frame generation and swapchain versions the C++ reports.
pub const FFX_FRAMEGENERATION_VERSION: u32 = (4 << 22) | 2;

// -------------------------------------------------------------------------------------------------
// Layout assertions, derived from the headers.
// -------------------------------------------------------------------------------------------------

macro_rules! assert_layout {
    ($t:ty, $size:expr, $align:expr) => {
        const _: () = {
            assert!(size_of::<$t>() == $size, concat!(stringify!($t), ": size"));
            assert!(
                align_of::<$t>() == $align,
                concat!(stringify!($t), ": align")
            );
        };
    };
}

macro_rules! assert_offset {
    ($t:ty, $field:ident, $offset:expr) => {
        const _: () = assert!(
            offset_of!($t, $field) == $offset,
            concat!(stringify!($t), ".", stringify!($field))
        );
    };
}

// ffx_api.h
assert_layout!(ffxApiHeader, 16, 8);
assert_offset!(ffxApiHeader, type_, 0);
assert_offset!(ffxApiHeader, pNext, 8);
assert_layout!(ffxAllocationCallbacks, 24, 8);
assert_layout!(ffxConfigureDescGlobalDebug, 40, 8);
assert_offset!(ffxConfigureDescGlobalDebug, effectId, 16);
assert_offset!(ffxConfigureDescGlobalDebug, fpMessage, 24);
assert_offset!(ffxConfigureDescGlobalDebug, debugLevel, 32);
assert_layout!(ffxConfigureDescGlobalDebug1, 32, 8);
assert_layout!(ffxQueryDescGetVersions, 56, 8);
assert_offset!(ffxQueryDescGetVersions, createDescType, 16);
assert_offset!(ffxQueryDescGetVersions, device, 24);
assert_offset!(ffxQueryDescGetVersions, outputCount, 32);
assert_offset!(ffxQueryDescGetVersions, versionIds, 40);
assert_offset!(ffxQueryDescGetVersions, versionNames, 48);
assert_layout!(ffxOverrideVersion, 24, 8);
assert_layout!(ffxQueryGetProviderVersion, 32, 8);
assert_offset!(ffxQueryGetProviderVersion, versionId, 16);
assert_offset!(ffxQueryGetProviderVersion, versionName, 24);

// ffx_api_types.h
assert_layout!(FfxApiDimensions2D, 8, 4);
assert_layout!(FfxApiFloatCoords2D, 8, 4);
assert_layout!(FfxApiRect2D, 16, 4);
assert_layout!(FfxApiResourceDescription, 32, 4);
assert_offset!(FfxApiResourceDescription, type_, 0);
assert_offset!(FfxApiResourceDescription, format, 4);
assert_offset!(FfxApiResourceDescription, width_or_size, 8);
assert_offset!(FfxApiResourceDescription, height_or_stride, 12);
assert_offset!(FfxApiResourceDescription, depth_or_alignment, 16);
assert_offset!(FfxApiResourceDescription, mipCount, 20);
assert_offset!(FfxApiResourceDescription, flags, 24);
assert_offset!(FfxApiResourceDescription, usage, 28);
assert_layout!(FfxApiResource, 48, 8);
assert_offset!(FfxApiResource, resource, 0);
assert_offset!(FfxApiResource, description, 8);
assert_offset!(FfxApiResource, state, 40);
assert_layout!(FfxApiEffectMemoryUsage, 16, 8);
// f32, f32, bool, u32, bool -> 4 + 4 + 1 + (3 pad) + 4 + 1 + (3 pad)
assert_layout!(FfxApiSwapchainFramePacingTuning, 20, 4);
assert_offset!(FfxApiSwapchainFramePacingTuning, allowHybridSpin, 8);
assert_offset!(FfxApiSwapchainFramePacingTuning, hybridSpinTime, 12);
assert_offset!(
    FfxApiSwapchainFramePacingTuning,
    allowWaitForSingleObjectOnFence,
    16
);

// dxgi.h (x64)
assert_layout!(DXGI_RATIONAL, 8, 4);
assert_layout!(DXGI_MODE_DESC, 28, 4);
assert_layout!(DXGI_SAMPLE_DESC, 8, 4);
assert_layout!(DXGI_SWAP_CHAIN_DESC, 72, 8);
assert_offset!(DXGI_SWAP_CHAIN_DESC, BufferDesc, 0);
assert_offset!(DXGI_SWAP_CHAIN_DESC, SampleDesc, 28);
assert_offset!(DXGI_SWAP_CHAIN_DESC, BufferUsage, 36);
assert_offset!(DXGI_SWAP_CHAIN_DESC, BufferCount, 40);
assert_offset!(DXGI_SWAP_CHAIN_DESC, OutputWindow, 48);
assert_offset!(DXGI_SWAP_CHAIN_DESC, Windowed, 56);
assert_offset!(DXGI_SWAP_CHAIN_DESC, SwapEffect, 60);
assert_offset!(DXGI_SWAP_CHAIN_DESC, Flags, 64);
assert_layout!(DXGI_SWAP_CHAIN_DESC1, 48, 4);
assert_offset!(DXGI_SWAP_CHAIN_DESC1, Stereo, 12);
assert_offset!(DXGI_SWAP_CHAIN_DESC1, SampleDesc, 16);
assert_offset!(DXGI_SWAP_CHAIN_DESC1, BufferUsage, 24);
assert_offset!(DXGI_SWAP_CHAIN_DESC1, BufferCount, 28);
assert_offset!(DXGI_SWAP_CHAIN_DESC1, Scaling, 32);
assert_offset!(DXGI_SWAP_CHAIN_DESC1, SwapEffect, 36);
assert_offset!(DXGI_SWAP_CHAIN_DESC1, AlphaMode, 40);
assert_offset!(DXGI_SWAP_CHAIN_DESC1, Flags, 44);
assert_layout!(DXGI_SWAP_CHAIN_FULLSCREEN_DESC, 20, 4);

// ffx_api_dx12.h
assert_layout!(ffxCreateBackendDX12Desc, 24, 8);
assert_layout!(ffxCreateBackendDX12AllocationCallbacksDesc, 56, 8);

// ffx_framegeneration.h
assert_layout!(ffxCreateContextDescFrameGeneration, 40, 8);
assert_offset!(ffxCreateContextDescFrameGeneration, flags, 16);
assert_offset!(ffxCreateContextDescFrameGeneration, displaySize, 20);
assert_offset!(ffxCreateContextDescFrameGeneration, maxRenderSize, 28);
assert_offset!(ffxCreateContextDescFrameGeneration, backBufferFormat, 36);
assert_layout!(ffxCallbackDescFrameGenerationPresent, 192, 8);
assert_layout!(ffxDispatchDescFrameGeneration, 312, 8);
assert_offset!(ffxDispatchDescFrameGeneration, presentColor, 24);
assert_offset!(ffxDispatchDescFrameGeneration, outputs, 72);
assert_offset!(ffxDispatchDescFrameGeneration, numGeneratedFrames, 264);
assert_offset!(ffxDispatchDescFrameGeneration, minMaxLuminance, 276);
assert_offset!(ffxDispatchDescFrameGeneration, frameID, 304);
assert_layout!(ffxConfigureDescFrameGeneration, 144, 8);
assert_offset!(ffxConfigureDescFrameGeneration, swapChain, 16);
assert_offset!(ffxConfigureDescFrameGeneration, presentCallback, 24);
assert_offset!(
    ffxConfigureDescFrameGeneration,
    presentCallbackUserContext,
    32
);
assert_offset!(ffxConfigureDescFrameGeneration, frameGenerationCallback, 40);
assert_offset!(
    ffxConfigureDescFrameGeneration,
    frameGenerationCallbackUserContext,
    48
);
assert_offset!(ffxConfigureDescFrameGeneration, frameGenerationEnabled, 56);
assert_offset!(ffxConfigureDescFrameGeneration, allowAsyncWorkloads, 57);
assert_offset!(ffxConfigureDescFrameGeneration, HUDLessColor, 64);
assert_offset!(ffxConfigureDescFrameGeneration, flags, 112);
assert_offset!(ffxConfigureDescFrameGeneration, onlyPresentGenerated, 116);
assert_offset!(ffxConfigureDescFrameGeneration, generationRect, 120);
assert_offset!(ffxConfigureDescFrameGeneration, frameID, 136);
assert_layout!(ffxDispatchDescFrameGenerationPrepare, 184, 8);
assert_offset!(ffxDispatchDescFrameGenerationPrepare, frameTimeDelta, 64);
assert_offset!(ffxDispatchDescFrameGenerationPrepare, cameraNear, 72);
assert_offset!(ffxDispatchDescFrameGenerationPrepare, depth, 88);
assert_offset!(ffxDispatchDescFrameGenerationPrepare, motionVectors, 136);
assert_layout!(ffxConfigureDescFrameGenerationKeyValue, 40, 8);
assert_layout!(ffxQueryDescFrameGenerationGetGPUMemoryUsage, 24, 8);
assert_layout!(
    ffxConfigureDescFrameGenerationRegisterDistortionFieldResource,
    64,
    8
);
assert_layout!(ffxCreateContextDescFrameGenerationHudless, 24, 8);
assert_layout!(ffxDispatchDescFrameGenerationPrepareCameraInfo, 64, 8);
assert_layout!(ffxQueryDescFrameGenerationGetGPUMemoryUsageV2, 64, 8);
assert_layout!(ffxDispatchDescFrameGenerationPrepareV2, 232, 8);
assert_offset!(ffxDispatchDescFrameGenerationPrepareV2, cameraPosition, 184);
assert_offset!(ffxDispatchDescFrameGenerationPrepareV2, cameraForward, 220);
assert_layout!(ffxCallbackDescFrameGenerationPresentPremulAlpha, 24, 8);
assert_layout!(ffxCreateContextDescFrameGenerationVersion, 24, 8);
assert_layout!(FfxFrameGenerationConfig, 152, 8);

// ffx_api_framegeneration_dx12.h
assert_layout!(ffxCreateContextDescFrameGenerationSwapChainWrapDX12, 32, 8);
assert_offset!(
    ffxCreateContextDescFrameGenerationSwapChainWrapDX12,
    swapchain,
    16
);
assert_offset!(
    ffxCreateContextDescFrameGenerationSwapChainWrapDX12,
    gameQueue,
    24
);
assert_layout!(ffxCreateContextDescFrameGenerationSwapChainNewDX12, 48, 8);
assert_layout!(
    ffxCreateContextDescFrameGenerationSwapChainForHwndDX12,
    64,
    8
);
assert_layout!(
    ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12,
    72,
    8
);
assert_layout!(
    ffxQueryDescFrameGenerationSwapChainInterpolationCommandListDX12,
    24,
    8
);
assert_layout!(
    ffxQueryDescFrameGenerationSwapChainInterpolationTextureDX12,
    24,
    8
);
assert_layout!(
    ffxDispatchDescFrameGenerationSwapChainWaitForPresentsDX12,
    16,
    8
);
assert_layout!(ffxConfigureDescFrameGenerationSwapChainKeyValueDX12, 40, 8);
assert_layout!(ffxQueryFrameGenerationSwapChainGetGPUMemoryUsageDX12, 24, 8);
assert_layout!(
    ffxQueryFrameGenerationSwapChainGetGPUMemoryUsageDX12V2,
    64,
    8
);
assert_layout!(
    ffxCreateContextDescFrameGenerationSwapChainVersionDX12,
    24,
    8
);

#[cfg(test)]
mod tests {
    use super::*;

    /// The `FFX_API_MAKE_*` macros from the headers, recomputed so a typo in a constant above is a
    /// test failure rather than a silent misroute. Kept here (not in `tests/abi.rs`) so the raw
    /// constants stay private to this module.
    #[test]
    fn desc_type_constants_match_the_header_macros() {
        const EFFECT_MASK: u64 = 0x00ff_0000;
        const BACKEND_MASK: u64 = 0xff00_0000;
        let effect = |id: u64, sub: u64| (id & EFFECT_MASK) | (sub & !EFFECT_MASK);
        let backend_effect = |backend: u64, id: u64, sub: u64| {
            (sub & !EFFECT_MASK)
                | (id & EFFECT_MASK)
                | (backend & BACKEND_MASK)
                | (sub & !(BACKEND_MASK | EFFECT_MASK))
        };
        const GENERAL: u64 = 0x0000_0000;
        const FG: u64 = 0x0002_0000;
        const SWAP: u64 = 0x0003_0000;
        const DX12: u64 = 0x0000_0000;

        assert_eq!(
            FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION,
            effect(FG, 0x01)
        );
        assert_eq!(
            FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS,
            effect(FG, 0x09)
        );
        assert_eq!(
            FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION,
            effect(FG, 0x0e)
        );
        assert_eq!(
            FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION,
            effect(FG, 0x02)
        );
        assert_eq!(FFX_API_FRAME_GENERATION_CONFIG, effect(FG, 0x0f));
        assert_eq!(
            FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION_KEYVALUE,
            effect(FG, 0x06)
        );
        assert_eq!(
            FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION_REGISTERDISTORTIONRESOURCE,
            effect(FG, 0x08)
        );
        assert_eq!(
            FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE,
            effect(FG, 0x04)
        );
        assert_eq!(
            FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2,
            effect(FG, 0x0c)
        );
        assert_eq!(
            FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_CAMERAINFO,
            effect(FG, 0x0a)
        );
        assert_eq!(FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION, effect(FG, 0x03));
        assert_eq!(
            FFX_API_CALLBACK_DESC_TYPE_FRAMEGENERATION_PRESENT,
            effect(FG, 0x05)
        );
        assert_eq!(
            FFX_API_CALLBACK_DESC_TYPE_FRAMEGENERATION_PRESENT_PREMUL_ALPHA,
            effect(FG, 0x0d)
        );
        assert_eq!(
            FFX_API_QUERY_DESC_TYPE_FRAMEGENERATION_GPU_MEMORY_USAGE,
            effect(FG, 0x07)
        );
        assert_eq!(
            FFX_API_QUERY_DESC_TYPE_FRAMEGENERATION_GPU_MEMORY_USAGE_V2,
            effect(FG, 0x0b)
        );

        assert_eq!(
            FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WRAP_DX12,
            backend_effect(DX12, SWAP, 0x01)
        );
        assert_eq!(
            FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_NEW_DX12,
            backend_effect(DX12, SWAP, 0x05)
        );
        assert_eq!(
            FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12,
            backend_effect(DX12, SWAP, 0x06)
        );
        assert_eq!(
            FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12,
            backend_effect(DX12, SWAP, 0x0b)
        );
        assert_eq!(
            FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_REGISTERUIRESOURCE_DX12,
            backend_effect(DX12, SWAP, 0x02)
        );
        assert_eq!(
            FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_KEYVALUE_DX12,
            backend_effect(DX12, SWAP, 0x08)
        );
        assert_eq!(
            FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_INTERPOLATIONCOMMANDLIST_DX12,
            backend_effect(DX12, SWAP, 0x03)
        );
        assert_eq!(
            FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_INTERPOLATIONTEXTURE_DX12,
            backend_effect(DX12, SWAP, 0x04)
        );
        assert_eq!(
            FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WAIT_FOR_PRESENTS_DX12,
            backend_effect(DX12, SWAP, 0x07)
        );
        assert_eq!(
            FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_GPU_MEMORY_USAGE_DX12,
            backend_effect(DX12, SWAP, 0x09)
        );
        assert_eq!(
            FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_GPU_MEMORY_USAGE_DX12_V2,
            backend_effect(DX12, SWAP, 0x0a)
        );
        assert_eq!(
            FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12,
            backend_effect(DX12, GENERAL, 0x02)
        );
        assert_eq!(
            FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12_ALLOCATION_CALLBACKS,
            backend_effect(DX12, GENERAL, 0x03)
        );

        assert_eq!(FFX_API_QUERY_DESC_TYPE_GET_VERSIONS, 4);
        assert_eq!(FFX_API_DESC_TYPE_OVERRIDE_VERSION, 5);
        assert_eq!(FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION, 6);
        assert_eq!(FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG, 7);
        assert_eq!(FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG1, 1);
    }

    #[test]
    fn version_constant_matches_the_header_macro() {
        assert_eq!(FFX_FRAMEGENERATION_VERSION, (4u32 << 22) | 2);
        assert_eq!(FFX_API_RETURN_OK, 0);
        assert_eq!(FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE, 2);
        assert_eq!(FFX_API_RETURN_ERROR_PARAMETER, 6);
    }
}
