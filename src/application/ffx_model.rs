//! Safe models of the game-facing FFX calls, and the service the exports call through.
//!
//! The driving adapter `adapters::driving::ffx_abi` owns every raw descriptor pointer, converts
//! the `pNext` chain into these plain values on entry and converts the replies back on the way
//! out. Nothing here is `unsafe`, contains a raw pointer, or knows the numeric FFX type constants:
//! handles are opaque `usize` token values, and the only pointers that survive are the game's own
//! output slots, recorded as opaque handles so the adapter can write through them.
//!
//! The application router (a later slice) implements [`FfxService`] and installs it once with
//! [`install`]. Until then every export answers with the documented FFX error code and never
//! panics (see `adapters::driving::ffx_abi::exports`).
//!
//! # Rejections
//!
//! A rejection carries the FFX return code the export must produce, because the mapping is not a
//! function of the stable [`ErrorCode`] alone: the README's table answers an unknown create chain
//! with `ERROR_UNKNOWN_DESCTYPE` but an unsupported frame generation create with `ERROR`. The
//! router chooses the code together with the reason; [`ErrorCode::classify`] still derives the
//! stable log code from the reason exactly as the C++ does.
use std::ffi::CStr;
use std::sync::OnceLock;

use crate::domain::error::ErrorCode;

/// The FFX API return codes (`ffx_api.h`). The numeric values are the ABI; the exports emit them
/// unchanged.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
#[repr(u32)]
pub enum ReturnCode {
    Ok = 0,
    Error = 1,
    ErrorUnknownDesctype = 2,
    ErrorRuntimeError = 3,
    NoProvider = 4,
    ErrorMemory = 5,
    ErrorParameter = 6,
    ProviderNoSupportNewDesctype = 7,
}

impl ReturnCode {
    pub const fn as_u32(self) -> u32 {
        self as u32
    }

    /// The name the log prints after the numeric result, as `describe.cpp` does.
    pub const fn name(self) -> &'static str {
        match self {
            Self::Ok => "OK",
            Self::Error => "ERROR",
            Self::ErrorUnknownDesctype => "UNKNOWN_DESCTYPE",
            Self::ErrorRuntimeError => "RUNTIME_ERROR",
            Self::NoProvider => "NO_PROVIDER",
            Self::ErrorMemory => "ERROR_MEMORY",
            Self::ErrorParameter => "ERROR_PARAMETER",
            Self::ProviderNoSupportNewDesctype => "PROVIDER_NO_SUPPORT_NEW_DESCTYPE",
        }
    }
}

/// A refused game call: the FFX return code, the stable log code and the reason text.
#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
#[error("{reason} [code={}]", code.name())]
pub struct Rejection {
    pub ffx: ReturnCode,
    pub code: ErrorCode,
    pub reason: String,
}

impl Rejection {
    pub fn new(ffx: ReturnCode, reason: impl Into<String>) -> Self {
        let reason = reason.into();
        Self { ffx, code: ErrorCode::classify(&reason), reason }
    }

    pub fn with_code(ffx: ReturnCode, code: ErrorCode, reason: impl Into<String>) -> Self {
        Self { ffx, code, reason: reason.into() }
    }

    pub fn error(reason: impl Into<String>) -> Self {
        Self::new(ReturnCode::Error, reason)
    }

    pub fn parameter(reason: impl Into<String>) -> Self {
        Self::new(ReturnCode::ErrorParameter, reason)
    }

    pub fn unknown_desctype(reason: impl Into<String>) -> Self {
        Self::new(ReturnCode::ErrorUnknownDesctype, reason)
    }
}

/// An opaque handle value: a Sedici token for a context, or a game pointer (device, queue,
/// swapchain, callback, callback user context) recorded so the router can name it in a log line.
/// Zero is the FFX null handle.
pub type OpaqueHandle = usize;

/// A live Sedici frame generation or swapchain context. Tokens are never reused.
pub type ContextToken = usize;

/// `FfxApiDimensions2D`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Dimensions {
    pub width: u32,
    pub height: u32,
}

/// `FfxApiFloatCoords2D`: jitter, motion-vector scale.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Float2 {
    pub x: f32,
    pub y: f32,
}

/// A three-component camera vector (`cameraPosition` and friends, all `float[3]` in the SDK).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Float3 {
    pub x: f32,
    pub y: f32,
    pub z: f32,
}

/// `FfxApiRect2D`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Rect {
    pub left: i32,
    pub top: i32,
    pub width: i32,
    pub height: i32,
}

/// `FfxApiSurfaceFormat`, as the raw value the game passed. Unknown values are kept verbatim;
/// the log prints them in hex like the C++ does.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub struct SurfaceFormat(pub u32);

/// `FfxApiResourceType`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum ResourceKind {
    #[default]
    Buffer,
    Texture1D,
    Texture2D,
    TextureCube,
    Texture3D,
    Unknown(u32),
}

/// `FfxApiResourceDescription` flattened: the width/height/depth unions also name a buffer's
/// size/stride/alignment, so all six fields are kept.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ResourceDescription {
    pub kind: ResourceKind,
    pub format: SurfaceFormat,
    pub width: u32,
    pub height: u32,
    pub depth: u32,
    pub mip_count: u32,
    pub flags: u32,
    pub usage: u32,
}

/// `FfxApiResource`: the game's device resource plus its FFX description and state.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Resource {
    /// The `ID3D12Resource*` the game passed, or zero for an empty resource.
    pub handle: OpaqueHandle,
    pub description: ResourceDescription,
    pub state: u32,
}

impl Resource {
    pub fn is_empty(&self) -> bool {
        self.handle == 0
    }

    /// For a buffer description, `width`/`height` are `size`/`stride`.
    pub fn buffer_size(&self) -> (u32, u32) {
        (self.description.width, self.description.height)
    }

    pub fn pixel_size(&self) -> (u32, u32) {
        (self.description.width, self.description.height)
    }
}

/// `FfxApiEffectMemoryUsage`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct MemoryUsage {
    pub total_usage_in_bytes: u64,
    pub aliasable_usage_in_bytes: u64,
}

/// `FfxApiSwapchainFramePacingTuning`, read from the pointer a swapchain key-value configure
/// carries for `FRAMEPACINGTUNING`.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct FramePacingTuning {
    pub safety_margin_in_ms: f32,
    pub variance_factor: f32,
    pub allow_hybrid_spin: bool,
    pub hybrid_spin_time: u32,
    pub allow_wait_for_single_object_on_fence: bool,
}

// -------------------------------------------------------------------------------------------------
// Create
// -------------------------------------------------------------------------------------------------

/// `ffxCreateContextDescFrameGeneration`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct FrameGenerationCreate {
    pub flags: u32,
    pub display_size: Dimensions,
    pub max_render_size: Dimensions,
    pub back_buffer_format: SurfaceFormat,
}

/// `ffxCreateContextDescFrameGenerationHudless`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct FrameGenerationHudless {
    pub hudless_back_buffer_format: SurfaceFormat,
}

/// `ffxCreateContextDescFrameGenerationSwapChainWrapDX12`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SwapchainWrap {
    /// The game's `IDXGISwapChain4**` output slot: the address the wrapper must be written to.
    pub swapchain_slot: OpaqueHandle,
    /// `*swapchain_slot` on entry: the game's native chain.
    pub swapchain: OpaqueHandle,
    pub game_queue: OpaqueHandle,
}

/// The fields of `DXGI_SWAP_CHAIN_DESC` the log prints.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SwapchainDesc {
    pub buffer_count: u32,
    pub width: u32,
    pub height: u32,
    pub format: u32,
    pub refresh_numerator: u32,
    pub refresh_denominator: u32,
    pub usage: u32,
    pub swap_effect: u32,
    pub flags: u32,
    pub windowed: bool,
    pub output_window: OpaqueHandle,
}

/// The fields of `DXGI_SWAP_CHAIN_DESC1` the log prints.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SwapchainDesc1 {
    pub width: u32,
    pub height: u32,
    pub format: u32,
    pub stereo: bool,
    pub sample_count: u32,
    pub sample_quality: u32,
    pub usage: u32,
    pub buffer_count: u32,
    pub scaling: u32,
    pub swap_effect: u32,
    pub alpha_mode: u32,
    pub flags: u32,
}

/// `ffxCreateContextDescFrameGenerationSwapChainNewDX12`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SwapchainNew {
    pub swapchain_slot: OpaqueHandle,
    pub desc: Option<SwapchainDesc>,
    pub dxgi_factory: OpaqueHandle,
    pub game_queue: OpaqueHandle,
}

/// `ffxCreateContextDescFrameGenerationSwapChainForHwndDX12`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SwapchainForHwnd {
    pub swapchain_slot: OpaqueHandle,
    pub hwnd: OpaqueHandle,
    pub desc: Option<SwapchainDesc1>,
    /// Non-null when the game passed a `DXGI_SWAP_CHAIN_FULLSCREEN_DESC`.
    pub fullscreen_desc: OpaqueHandle,
    pub dxgi_factory: OpaqueHandle,
    pub game_queue: OpaqueHandle,
}

/// `ffxCreateBackendDX12Desc`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct BackendDx12 {
    pub device: OpaqueHandle,
}

/// `ffxCreateBackendDX12AllocationCallbacksDesc`: pointer identity only; Sedici never calls them.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct BackendDx12AllocationCallbacks {
    pub resource_allocator: OpaqueHandle,
    pub resource_deallocator: OpaqueHandle,
    pub heap_allocator: OpaqueHandle,
    pub heap_deallocator: OpaqueHandle,
    pub constant_buffer_allocator: OpaqueHandle,
}

/// A flattened create chain. Each linked desc appears at most once; a repeated type keeps the
/// first occurrence, which is what the C++ `find_desc` returns.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct CreateContextDesc {
    pub frame_generation: Option<FrameGenerationCreate>,
    pub frame_generation_hudless: Option<FrameGenerationHudless>,
    /// `ffxCreateContextDescFrameGenerationVersion.version`.
    pub frame_generation_version: Option<u32>,
    pub swapchain_wrap: Option<SwapchainWrap>,
    pub swapchain_new: Option<SwapchainNew>,
    pub swapchain_for_hwnd: Option<SwapchainForHwnd>,
    /// `ffxCreateContextDescFrameGenerationSwapChainVersionDX12.version`.
    pub swapchain_version: Option<u32>,
    pub backend_dx12: Option<BackendDx12>,
    pub backend_dx12_allocation_callbacks: Option<BackendDx12AllocationCallbacks>,
    /// `ffxOverrideVersion.versionId`.
    pub override_version: Option<u64>,
    /// Every other type in the chain, in walk order, so the router can log or refuse them.
    pub unknown_types: Vec<u64>,
}

impl CreateContextDesc {
    /// True when the chain names a context Sedici could own: frame generation or a swapchain
    /// create form. An unknown chain is answered with `ERROR_UNKNOWN_DESCTYPE` by the export.
    pub fn names_a_sedici_context(&self) -> bool {
        self.frame_generation.is_some()
            || self.swapchain_wrap.is_some()
            || self.swapchain_new.is_some()
            || self.swapchain_for_hwnd.is_some()
    }
}

/// The successful answer to `ffxCreateContext`: the issued token, and for the swapchain forms the
/// wrapper chain that must be written through the game's output slot.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CreatedContext {
    pub context: ContextToken,
    /// `None` for a frame generation context. For a swapchain form the handle already carries
    /// the reference the game receives; the adapter writes it through the slot and does not
    /// `AddRef` again.
    pub swapchain: Option<OpaqueHandle>,
}

// -------------------------------------------------------------------------------------------------
// Configure
// -------------------------------------------------------------------------------------------------

/// A present or frame generation callback: only whether one was set is observable here.
pub type Callback = Option<OpaqueHandle>;

/// `ffxConfigureDescFrameGeneration`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct FrameGenerationConfigure {
    pub swapchain: OpaqueHandle,
    pub present_callback: Callback,
    pub present_callback_user_context: OpaqueHandle,
    pub frame_generation_callback: Callback,
    pub frame_generation_callback_user_context: OpaqueHandle,
    pub frame_generation_enabled: bool,
    pub allow_async_workloads: bool,
    pub hud_less_color: Resource,
    pub flags: u32,
    pub only_present_generated: bool,
    pub generation_rect: Rect,
    pub frame_id: u64,
}

/// The legacy `FfxFrameGenerationConfig` (`FFX_API_FRAME_GENERATION_CONFIG`, deprecated) that
/// older games still send. Kept because the C++ logs both forms.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct FrameGenerationConfigureLegacy {
    pub swapchain: OpaqueHandle,
    pub present_callback: Callback,
    pub present_callback_context: OpaqueHandle,
    pub frame_generation_callback: Callback,
    pub frame_generation_callback_context: OpaqueHandle,
    pub frame_generation_enabled: bool,
    pub allow_async_workloads: bool,
    pub allow_async_present: bool,
    pub hud_less_color: Resource,
    pub flags: u32,
    pub only_present_interpolated: bool,
    pub interpolation_rect: Rect,
    pub frame_id: u64,
    pub draw_debug_pacing_lines: bool,
}

/// `ffxConfigureDescFrameGenerationKeyValue` and its swapchain counterpart.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct KeyValue {
    pub key: u64,
    pub u64_value: u64,
    pub ptr: OpaqueHandle,
    /// For the swapchain `FRAMEPACINGTUNING` key, the tuning struct the pointer named.
    pub tuning: Option<FramePacingTuning>,
}

/// `ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct UiResourceRegister {
    pub ui_resource: Resource,
    pub flags: u32,
}

/// `ffxConfigureDescGlobalDebug`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct GlobalDebug {
    pub effect_id: u64,
    pub message_callback: Callback,
    pub debug_level: u32,
}

/// `ffxConfigureDescGlobalDebug1` (deprecated).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct GlobalDebug1 {
    pub message_callback: Callback,
    pub debug_level: u32,
}

/// A flattened configure chain.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct ConfigureDesc {
    /// The export passes a null descriptor through so the router can answer with the documented
    /// `FFX_API_RETURN_ERROR`, as the C++ route does on a Sedici-owned context.
    pub null_descriptor: bool,
    pub frame_generation: Option<FrameGenerationConfigure>,
    pub legacy_frame_generation: Option<FrameGenerationConfigureLegacy>,
    pub frame_generation_key_value: Option<KeyValue>,
    pub distortion_field: Option<Resource>,
    pub ui_resource: Option<UiResourceRegister>,
    pub swapchain_key_value: Option<KeyValue>,
    pub global_debug: Option<GlobalDebug>,
    pub global_debug1: Option<GlobalDebug1>,
    pub override_version: Option<u64>,
    pub unknown_types: Vec<u64>,
}

// -------------------------------------------------------------------------------------------------
// Dispatch
// -------------------------------------------------------------------------------------------------

/// The fields shared by `ffxDispatchDescFrameGenerationPrepare` and its V2 replacement.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Prepare {
    pub frame_id: u64,
    pub flags: u32,
    pub command_list: OpaqueHandle,
    pub render_size: Dimensions,
    pub jitter: Float2,
    pub motion_vector_scale: Float2,
    pub frame_time_delta: f32,
    pub camera_near: f32,
    pub camera_far: f32,
    pub camera_fov_vertical: f32,
    pub view_space_to_meters: f32,
    pub depth: Resource,
    pub motion_vectors: Resource,
}

/// The deprecated prepare desc's reset flag is named `unused_reset` in the SDK and is logged as
/// `reset`.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct PrepareV1 {
    pub prepare: Prepare,
    pub unused_reset: bool,
}

/// `ffxDispatchDescFrameGenerationPrepareV2`, the form FSR 3.1.4+ uses.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct PrepareV2 {
    pub prepare: Prepare,
    pub reset: bool,
    pub camera_position: Float3,
    pub camera_up: Float3,
    pub camera_right: Float3,
    pub camera_forward: Float3,
}

/// `ffxDispatchDescFrameGenerationPrepareCameraInfo` (deprecated; linked after the prepare desc).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct PrepareCameraInfo {
    pub camera_position: Float3,
    pub camera_up: Float3,
    pub camera_right: Float3,
    pub camera_forward: Float3,
}

/// `ffxDispatchDescFrameGeneration`: the game's own generation dispatch.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct FrameGenerationDispatch {
    pub command_list: OpaqueHandle,
    pub present_color: Resource,
    pub outputs: [Resource; 4],
    pub num_generated_frames: u32,
    pub reset: bool,
    pub backbuffer_transfer_function: u32,
    pub min_max_luminance: [f32; 2],
    pub generation_rect: Rect,
    pub frame_id: u64,
}

impl Default for FrameGenerationDispatch {
    fn default() -> Self {
        Self {
            command_list: 0,
            present_color: Resource::default(),
            outputs: [Resource::default(); 4],
            num_generated_frames: 0,
            reset: false,
            backbuffer_transfer_function: 0,
            min_max_luminance: [0.0; 2],
            generation_rect: Rect::default(),
            frame_id: 0,
        }
    }
}

/// `ffxCallbackDescFrameGenerationPresent`, which Sedici receives as a dispatch desc.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct PresentCallbackDesc {
    pub device: OpaqueHandle,
    pub command_list: OpaqueHandle,
    pub current_back_buffer: Resource,
    pub current_ui: Resource,
    pub output_swap_chain_buffer: Resource,
    pub is_generated_frame: bool,
    pub frame_id: u64,
}

/// A flattened dispatch chain.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct DispatchDesc {
    /// The first desc type, which is what the router routes on.
    pub root_type: Option<u64>,
    pub null_descriptor: bool,
    pub prepare_v1: Option<PrepareV1>,
    pub prepare_v2: Option<PrepareV2>,
    pub camera_info: Option<PrepareCameraInfo>,
    pub frame_generation: Option<FrameGenerationDispatch>,
    pub present_callback: Option<PresentCallbackDesc>,
    /// `ffxCallbackDescFrameGenerationPresentPremulAlpha.usePremulAlpha`.
    pub present_premul_alpha: Option<bool>,
    pub wait_for_presents: bool,
    pub unknown_types: Vec<u64>,
}

// -------------------------------------------------------------------------------------------------
// Query
// -------------------------------------------------------------------------------------------------

/// `ffxQueryDescGetVersions`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct GetVersions {
    pub create_desc_type: u64,
    pub device: OpaqueHandle,
    /// `uint64_t* outputCount`: input capacity and output count.
    pub output_count_slot: OpaqueHandle,
    pub version_ids_slot: OpaqueHandle,
    pub version_names_slot: OpaqueHandle,
    /// `*outputCount` on entry, when the slot is readable.
    pub output_count: u64,
}

/// `ffxQueryDescFrameGenerationGetGPUMemoryUsage`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct MemoryUsageQuery {
    pub output_slot: OpaqueHandle,
}

/// `ffxQueryDescFrameGenerationGetGPUMemoryUsageV2`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct MemoryUsageQueryV2 {
    pub device: OpaqueHandle,
    pub max_render_size: Dimensions,
    pub display_size: Dimensions,
    pub create_flags: u32,
    pub dispatch_flags: u32,
    pub back_buffer_format: SurfaceFormat,
    pub hudless_back_buffer_format: SurfaceFormat,
    pub output_slot: OpaqueHandle,
}

/// `ffxQueryFrameGenerationSwapChainGetGPUMemoryUsageDX12V2`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SwapchainMemoryUsageQueryV2 {
    pub device: OpaqueHandle,
    pub display_size: Dimensions,
    pub back_buffer_format: SurfaceFormat,
    pub back_buffer_count: u32,
    pub ui_resource_size: Dimensions,
    pub ui_resource_format: SurfaceFormat,
    pub flags: u32,
    pub output_slot: OpaqueHandle,
}

/// The two interpolation queries a game uses once it received an interpolation list or texture.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct CommandListQuery {
    pub output_slot: OpaqueHandle,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct TextureQuery {
    pub output_slot: OpaqueHandle,
}

/// A flattened query chain.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct QueryDesc {
    pub root_type: Option<u64>,
    pub null_descriptor: bool,
    pub get_versions: Option<GetVersions>,
    /// `ffxQueryGetProviderVersion`: the query writes both the id and the name into the desc.
    pub get_provider_version: Option<()>,
    pub gpu_memory_usage: Option<MemoryUsageQuery>,
    pub gpu_memory_usage_v2: Option<MemoryUsageQueryV2>,
    pub interpolation_command_list: Option<CommandListQuery>,
    pub interpolation_texture: Option<TextureQuery>,
    pub swapchain_gpu_memory_usage: Option<MemoryUsageQuery>,
    pub swapchain_gpu_memory_usage_v2: Option<SwapchainMemoryUsageQueryV2>,
    pub override_version: Option<u64>,
    pub unknown_types: Vec<u64>,
}

/// The successful answer to a query. The adapter writes it through the descriptor's output
/// pointers; the router never touches game memory.
#[derive(Clone, Debug, PartialEq)]
pub enum QueryReply {
    /// `GetVersions`: exactly one provider for a recognised create type.
    Versions { count: u64, id: Option<u64>, name: Option<&'static CStr> },
    /// `GetProviderVersion`.
    ProviderVersion { id: u64, name: Option<&'static CStr> },
    /// `InterpolationCommandListDX12`: a scratch list the game may record into.
    CommandList(OpaqueHandle),
    /// `InterpolationTextureDX12`: a scratch texture the game may render into.
    Texture(Resource),
    /// A GPU memory usage query.
    MemoryUsage(MemoryUsage),
    /// A query Sedici accepted without producing a value.
    Acknowledged,
}

/// The successful answer to a dispatch.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DispatchReply {
    /// A prepare was consumed by the generator.
    Prepared,
    /// The game's own `FrameGeneration` dispatch was acknowledged; Sedici generates the frame.
    FrameGenerationAcknowledged,
}

// -------------------------------------------------------------------------------------------------
// The service
// -------------------------------------------------------------------------------------------------

/// What the exports call. Implemented by the application router and installed once at load.
///
/// Every method receives the flattened, safe descriptor model and either a reply to write back or
/// a [`Rejection`] carrying the exact FFX return code. Errors must never escape as panics; a panic
/// is caught at the ABI edge and answered with `FFX_API_RETURN_ERROR`.
pub trait FfxService: Send + Sync {
    fn create(&self, desc: &CreateContextDesc) -> Result<CreatedContext, Rejection>;
    fn destroy(&self, context: ContextToken) -> Result<(), Rejection>;
    fn configure(&self, context: ContextToken, desc: &ConfigureDesc) -> Result<(), Rejection>;
    fn query(&self, context: Option<ContextToken>, desc: &QueryDesc) -> Result<QueryReply, Rejection>;
    fn dispatch(&self, context: ContextToken, desc: &DispatchDesc) -> Result<DispatchReply, Rejection>;

    /// `route_any_generation_enabled()`: whether a live session was last configured with frame
    /// generation on, for the menu's first line. A service that keeps no sessions says no.
    fn any_generation_enabled(&self) -> bool {
        false
    }
}

/// Returned by [`install`] when a service is already installed. Installing is a one-shot load
/// step; a second install is a composition-root bug, not a game-facing failure.
#[derive(Debug, Clone, Copy, PartialEq, Eq, thiserror::Error)]
#[error("an FFX service is already installed")]
pub struct ServiceAlreadyInstalled;

static SERVICE: OnceLock<Box<dyn FfxService>> = OnceLock::new();

/// Installs the router. The first caller wins; later calls never replace it.
pub fn install(service: impl FfxService + 'static) -> Result<(), ServiceAlreadyInstalled> {
    SERVICE.set(Box::new(service)).map_err(|_| ServiceAlreadyInstalled)
}

/// The installed router, if the composition root has run.
pub fn service() -> Option<&'static dyn FfxService> {
    SERVICE.get().map(|boxed| boxed.as_ref())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn return_codes_match_the_c_header() {
        let all = [
            (ReturnCode::Ok, 0, "OK"),
            (ReturnCode::Error, 1, "ERROR"),
            (ReturnCode::ErrorUnknownDesctype, 2, "UNKNOWN_DESCTYPE"),
            (ReturnCode::ErrorRuntimeError, 3, "RUNTIME_ERROR"),
            (ReturnCode::NoProvider, 4, "NO_PROVIDER"),
            (ReturnCode::ErrorMemory, 5, "ERROR_MEMORY"),
            (ReturnCode::ErrorParameter, 6, "ERROR_PARAMETER"),
            (ReturnCode::ProviderNoSupportNewDesctype, 7, "PROVIDER_NO_SUPPORT_NEW_DESCTYPE"),
        ];
        for (code, value, name) in all {
            assert_eq!(code.as_u32(), value);
            assert_eq!(code.name(), name);
        }
    }

    #[test]
    fn a_rejection_keeps_the_explicit_ffx_code_and_classifies_the_reason() {
        let rejected = Rejection::unknown_desctype("create with an unrecognised descriptor chain");
        assert_eq!(rejected.ffx, ReturnCode::ErrorUnknownDesctype);
        assert_eq!(rejected.code, ErrorCode::Rejected);

        let stale = Rejection::error("configure on a destroyed context");
        assert_eq!(stale.ffx, ReturnCode::Error);
        assert_eq!(stale.code, ErrorCode::StaleHandle);
    }
}
