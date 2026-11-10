//! Sedici's frame generation context: the FFX create/configure/prepare/generate state machine
//! (ports `runtime/context.cpp`).
//!
//! The context turns FFX inputs into pixels through the [`FrameGenerator`] port and never touches
//! D3D12 itself. The C++ kept pointer-shaped state; each becomes a small trait here:
//!
//! - [`Retirement`] stands in for `ID3D12Fence*`. The context borrows a shared probe and never
//!   owns the fence; the swapchain (or the offline host) keeps ownership.
//! - [`QueueDependency`] stands in for `ID3D12CommandQueue*` in the split-recording path, where
//!   the generation queue must wait for the prepare submission before it executes.
//! - [`DeviceLookup`] and [`FrameObjects`] turn the game's opaque FFX handles into the generator's
//!   typed resources. The D3D12 adapter implements them; the application never casts.
//!
//! [`ContextHandle`] is the object-safe face the presentation slice installs as its generation
//! sink: it speaks only in the game's own handles, so a swapchain wrapper can drive generation
//! without knowing the GPU types. It mirrors `GenerateSink` in `presentation/swapchain.h`; the
//! presentation slice is written in parallel, so this trait is the reconciliation point.
//!
//! A rejected call is a local rejection, never permission to forward the context elsewhere. Every
//! rejection logs one line at info level, because a generation that fails inside a Present never
//! reaches the router (Alan Wake 2 black screen, finding 016).
use std::sync::Arc;
use std::time::{Duration, Instant};

use thiserror::Error;

use crate::application::faults::Faults;
use crate::application::ffx_model::{
    ConfigureDesc, CreateContextDesc, DispatchDesc, OpaqueHandle,
};
use crate::application::ports::generator::{
    FrameGenerator, GeneratePhase, GeneratorError, HudSource, InputFrame,
};
use crate::application::ports::gpu::{
    GpuDevice, GpuError, GpuTexture, PixelFormat, ResourceState, TextureFlags,
};
use crate::domain::frame::{ColorFormat, FrameParams, MIDPOINT_ALPHA};

// ---------------------------------------------------------------------------------------------
// FFX create flags (`ffx_framegeneration.h`)
// ---------------------------------------------------------------------------------------------

pub const ENABLE_ASYNC_WORKLOAD_SUPPORT: u32 = 1 << 0;
pub const ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS: u32 = 1 << 1;
pub const ENABLE_DEPTH_INVERTED: u32 = 1 << 3;
pub const ENABLE_DEPTH_INFINITE: u32 = 1 << 4;
pub const ENABLE_HIGH_DYNAMIC_RANGE: u32 = 1 << 5;

/// The create flags Sedici accepts: the C++ mask, unchanged.
pub const SUPPORTED_CREATE_FLAGS: u32 = ENABLE_DEPTH_INVERTED
    | ENABLE_DEPTH_INFINITE
    | ENABLE_ASYNC_WORKLOAD_SUPPORT
    | ENABLE_HIGH_DYNAMIC_RANGE;

/// How long a prepare waits for the previous generated frame's slot (K98: a resolution change
/// took 93-194 ms on the GPU).
pub const PREPARE_SLOT_WAIT_MS: u32 = 250;

/// How long `prepare` polls for retirement before it refuses the slot.
const PREPARE_SLOT_POLL_MS: u64 = 1;

// ---------------------------------------------------------------------------------------------
// The three pointer-shaped ports
// ---------------------------------------------------------------------------------------------

/// A shared retirement probe over a caller-owned GPU fence timeline.
///
/// `completed` is the raw D3D12 value: `u64::MAX` is the removed-device sentinel and never
/// success. The context borrows the probe (`Arc`) and keeps ownership where the C++ kept a
/// `ComPtr`; `device_identity` replaces the C++ `fence->GetDevice() == m_device` check.
pub trait Retirement: Send + Sync + std::fmt::Debug {
    fn completed(&self) -> u64;

    /// A bounded wait. [`GpuError::Timeout`] means the work may still be in flight.
    fn wait(&self, value: u64, timeout: Duration) -> Result<(), GpuError>;

    /// An opaque device identity, comparable with [`DeviceBinding::identity`].
    fn device_identity(&self) -> usize;
}

/// A GPU command queue the generation path can make wait for the prepare submission.
pub trait QueueDependency: Send + Sync {
    fn device_identity(&self) -> usize;

    /// Makes this queue wait until `retirement` reaches `value` before its next work executes.
    fn wait(&self, retirement: &dyn Retirement, value: u64) -> Result<(), GpuError>;
}

/// A resolved backend device: the port device plus the identity its fences and queues report.
///
/// The device is resolved from the FFX backend desc's opaque handle by the composition root; the
/// context never casts a game pointer.
pub struct DeviceBinding<D: GpuDevice> {
    pub device: D,
    pub identity: usize,
}

/// Resolves the game's `ffxCreateBackendDX12Desc.device` handle into a port device.
pub trait DeviceLookup<D: GpuDevice>: Send + Sync {
    fn resolve(&self, handle: OpaqueHandle) -> Option<DeviceBinding<D>>;
}

/// Wraps the game's opaque resource and command-list handles as the generator's typed objects.
///
/// The adapter takes its own COM reference for each returned value, so it is owned and safe to
/// hold for the duration of one call. A null or unrecognised handle returns `None`, which the
/// context refuses with a defined error.
pub trait FrameObjects<D: GpuDevice, L>: Send + Sync {
    fn texture(&self, handle: OpaqueHandle) -> Option<D::Texture>;
    fn list(&self, handle: OpaqueHandle) -> Option<L>;
}

/// One phase of a recorded generation, in the game's own handles.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct PhaseSpec {
    pub output: OpaqueHandle,
    pub output_state: ResourceState,
    /// Time between the previous real frame (0) and the current one (1).
    pub alpha: f32,
}

/// The generation sink a swapchain wrapper drives (ports `GenerateSink`).
///
/// `record` sees one phase; `record_phases` records a whole real frame's phases into one list.
/// `submitted` follows the queue-signalled fence so the owner can register retirement;
/// `needs_submission` reports recorded but untracked work; `abandoned` invalidates CPU-side
/// history after a frame cannot be presented. Callbacks never call back into the wrapper.
pub trait ContextHandle: Send + Sync {
    fn record(
        &self,
        list: OpaqueHandle,
        real: OpaqueHandle,
        output: OpaqueHandle,
        output_state: ResourceState,
    ) -> bool;

    fn record_phases(&self, list: OpaqueHandle, real: OpaqueHandle, phases: &[PhaseSpec]) -> bool;

    fn submitted(&self, fence: Arc<dyn Retirement>, value: u64) -> bool;

    fn needs_submission(&self) -> bool;

    fn abandoned(&self);
}

// ---------------------------------------------------------------------------------------------
// Errors and rejection reasons
// ---------------------------------------------------------------------------------------------

/// The exact reason strings `context.cpp` uses. They are part of the log and the stable
/// `ErrorCode` classification, so they stay verbatim.
pub mod reasons {
    pub const CREATE_WITHOUT_DESC: &str = "create without FrameGenerationCreate";
    pub const CREATE_TWICE: &str = "context already created";
    pub const UNSUPPORTED_CREATE_FLAGS: &str = "unsupported frame generation create flags";
    pub const UNSUPPORTED_BACK_BUFFER: &str = "unsupported back buffer format";
    pub const CREATE_WITHOUT_DEVICE: &str = "create without a DX12 device";
    pub const EMPTY_DISPLAY: &str = "create with an empty display size";
    pub const STAGE_INIT: &str = "stage 1 initialization";
    pub const CONFIGURE_WITHOUT_CONTEXT: &str = "configure without a context";
    pub const CONFIGURE_WITHOUT_DESC: &str = "configure without a frame generation desc";
    pub const PREPARE_WITHOUT_CONTEXT: &str = "prepare without a context";
    pub const PREPARE_SLOT_IN_FLIGHT: &str = "prepare slot is still in flight";
    pub const PREPARE_SLOT_RECORDED: &str = "prepare slot is still recorded or in flight";
    pub const PREPARE_WITHOUT_DESC: &str = "prepare without a prepare desc";
    pub const PREPARE_WITHOUT_INPUTS: &str = "prepare without depth or motion vectors";
    pub const PREPARE_WITHOUT_RENDER_SIZE: &str = "prepare without a render size";
    pub const PREPARE_OUTSIDE_SIZES: &str = "prepare outside the created sizes";
    pub const UNSUPPORTED_DEPTH: &str = "unsupported depth format";
    pub const UNSUPPORTED_MOTION: &str = "unsupported motion vector format";
    pub const UNSUPPORTED_INPUT_GEOMETRY: &str = "unsupported input texture geometry or usage";
    pub const INVALID_MOTION_SCALE: &str = "invalid motion vector scale";
    pub const PREPARE_WITHOUT_LIST: &str = "prepare without a command list";
    pub const PREPARE_STAGE_CONFIG: &str = "prepare stage configuration";
    pub const PREPARE_REJECTED: &str = "prepare snapshot rejected";
    pub const GENERATE_WITHOUT_OUTPUT: &str = "generate without a command list or output";
    pub const GENERATE_DISABLED: &str = "frame generation disabled";
    pub const GENERATE_WITHOUT_PREPARE: &str = "generate without prepare";
    pub const SPLIT_BEFORE_REGISTRATION: &str =
        "split generation before prepare submission registration";
    pub const SPLIT_WITHOUT_DEPENDENCY: &str = "split generation without a prepare queue dependency";
    pub const FRAME_ID_MISMATCH: &str = "configure/prepare frame ID mismatch";
    pub const GENERATE_WITHOUT_HUDLESS: &str = "generate without a HUD-less colour resource";
    pub const HUDLESS_FORMAT: &str = "HUD-less colour does not match the back buffer format";
    pub const STAGE_REJECTED_FRAME: &str = "stage 1 rejected the frame";
    pub const INVALID_SUBMISSION: &str = "invalid submission retirement token";
    pub const FENCE_OTHER_DEVICE: &str = "submission fence belongs to another device";
    pub const TIMELINE_NOT_ADVANCED: &str = "submission fence timeline did not advance";
    pub const PREPARE_NO_SUBMISSION: &str = "prepare has no registered submission";
    pub const QUEUE_OTHER_DEVICE: &str = "generation queue belongs to another device";
    pub const PREPARE_DEVICE_REMOVED: &str = "prepare device removed";
    pub const QUEUE_WAIT_FAILED: &str = "generation queue wait failed";
    pub const RETIREMENT_TIMEOUT: &str = "retirement timed out";
    pub const DESTROY_BEFORE_RETIREMENT: &str = "destroy before GPU retirement";
    pub const GENERATE_OBJECT: &str = "generation object unavailable";
}

/// A refused context call. The reason is a `&'static str` from [`reasons`] so it can drive
/// [`crate::domain::error::ErrorCode::classify`] and the session's `last_error` unchanged.
#[derive(Debug, Clone, Error)]
pub enum ContextError {
    #[error("{0}")]
    Rejected(&'static str),
    #[error("stage 1 initialization: {0}")]
    Generator(#[from] GeneratorError),
}

impl ContextError {
    pub fn reason(&self) -> &str {
        match self {
            Self::Rejected(reason) => reason,
            Self::Generator(error) => error_reason(error),
        }
    }
}

/// The fixed reason the C++ stored for a generator failure; the detailed error is logged at
/// debug level by the caller.
fn error_reason(_error: &GeneratorError) -> &'static str {
    reasons::STAGE_INIT
}

/// The formats a native depth allocation is sampled with (ports `depth_srv_format`): a
/// `D32_FLOAT` depth is viewed as `R32_FLOAT`, and Cyberpunk's two-plane depth as the plane-0
/// float view.
pub fn depth_srv_format(format: PixelFormat) -> Option<PixelFormat> {
    format.depth_srv_format()
}

pub fn is_supported_motion(format: PixelFormat) -> bool {
    format.is_supported_motion()
}

/// The supported input texture geometry: one mip, one sample, one array slice, not denied a shader
/// resource view, and exactly the render size.
fn input_geometry_ok(info: &crate::application::ports::gpu::TextureInfo, size: (u32, u32)) -> bool {
    info.is_single_sample_2d()
        && info.width == size.0
        && info.height == size.1
        && !info.flags.contains(TextureFlags::DENY_SHADER_RESOURCE)
}

/// The FFX `FfxApiSurfaceFormat` a create desc named, and the [`ColorFormat`] it maps to.
fn color_format_of(value: u32) -> Option<ColorFormat> {
    ColorFormat::from_ffx_surface_format(value)
}

/// The context state machine (ports `Context`).
///
/// Generic over the generator, because that is where the GPU types come from. `D` is the port
/// device; `G` records the passes. The context is single-slot: one prepare outstanding at a time,
/// exactly as the C++ was (K139 is the open question about whether a game can outrun that slot).
///
/// The context does not own the backend device: [`Context::create`] receives the resolved
/// [`DeviceBinding`], which is the same device the session looked up for the *whole* process, so
/// a second frame generation context in one process shares it rather than recreating it.
pub struct Context<D: GpuDevice + Send + 'static, G: FrameGenerator<D> + Send + 'static> {
    objects: Arc<dyn FrameObjects<D, G::List>>,
    faults: Arc<Faults>,

    device: Option<DeviceBinding<D>>,
    create_flags: u32,
    color_format: ColorFormat,
    display_width: u32,
    display_height: u32,
    max_render_width: u32,
    max_render_height: u32,

    enabled: bool,
    prepare_valid: bool,
    history_valid: bool,
    reset_requested: bool,

    frame_id: u64,
    prepared_frame_id: u64,
    frames_generated: u64,

    hudless: OpaqueHandle,
    hudless_state: ResourceState,

    depth_state: ResourceState,
    motion_state: ResourceState,
    input_formats_logged: bool,

    render_width: u32,
    render_height: u32,
    motion_scale_x: f32,
    motion_scale_y: f32,

    stage: Option<G>,
    retirement: Option<Arc<dyn Retirement>>,
    retirement_value: u64,
    recorded: bool,
    snapshot: bool,
    prepare_waited: bool,
    /// The prepare command list, identity only. Never dereferenced.
    prepare_list: OpaqueHandle,
    last_error: &'static str,
}

impl<D: GpuDevice + Send + 'static, G: FrameGenerator<D> + Send + 'static> Context<D, G> {
    /// Builds an empty context. Nothing is initialized until [`Context::create`].
    pub fn new(objects: Arc<dyn FrameObjects<D, G::List>>, faults: Arc<Faults>) -> Self {
        Self {
            objects,
            faults,
            device: None,
            create_flags: 0,
            color_format: ColorFormat::R8G8B8A8Unorm,
            display_width: 0,
            display_height: 0,
            max_render_width: 0,
            max_render_height: 0,
            enabled: false,
            prepare_valid: false,
            history_valid: false,
            reset_requested: true,
            frame_id: 0,
            prepared_frame_id: 0,
            frames_generated: 0,
            hudless: 0,
            hudless_state: ResourceState::NON_PIXEL_SHADER_RESOURCE,
            depth_state: ResourceState::NON_PIXEL_SHADER_RESOURCE,
            motion_state: ResourceState::NON_PIXEL_SHADER_RESOURCE,
            input_formats_logged: false,
            render_width: 0,
            render_height: 0,
            motion_scale_x: 1.0,
            motion_scale_y: 1.0,
            stage: None,
            retirement: None,
            retirement_value: 0,
            recorded: false,
            snapshot: false,
            prepare_waited: false,
            prepare_list: 0,
            last_error: "",
        }
    }

    /// Records the reason and logs one info line, then reports the refusal (ports `Context::fail`).
    fn fail(&mut self, reason: &'static str) -> ContextError {
        self.last_error = reason;
        tracing::info!("fg: rejected: {reason}");
        ContextError::Rejected(reason)
    }

    // -- Queries -------------------------------------------------------------------------------

    /// A removed device executes nothing more, so work recorded or submitted on it counts as
    /// retired (K38); without that, destroy was refused forever after device loss.
    pub fn retired(&self) -> bool {
        if self.device_lost() {
            return true;
        }
        // The offline seam, never set at runtime: a held workout forces "not retired".
        if self.faults.retirement_held() {
            return false;
        }
        if self.recorded {
            return false;
        }
        let Some(retirement) = &self.retirement else {
            return true;
        };
        match retirement.completed() {
            u64::MAX => false, // the policy turns the sentinel into "removed", checked above
            completed => completed >= self.retirement_value,
        }
    }

    pub fn ready(&self) -> bool {
        self.stage.as_ref().is_some_and(FrameGenerator::ready) && self.display_width != 0
    }

    pub fn device_lost(&self) -> bool {
        self.device.as_ref().is_some_and(|binding| binding.device.removed_reason().is_some())
    }

    pub fn enabled(&self) -> bool {
        self.enabled
    }

    pub fn pending(&self) -> bool {
        self.prepare_valid
    }

    pub fn have_history(&self) -> bool {
        self.history_valid
    }

    pub fn has_untracked_work(&self) -> bool {
        self.recorded
    }

    pub fn frames_generated(&self) -> u64 {
        self.frames_generated
    }

    pub fn frame_id(&self) -> u64 {
        self.frame_id
    }

    pub fn last_error(&self) -> &'static str {
        self.last_error
    }

    pub fn color_format(&self) -> ColorFormat {
        self.color_format
    }

    // -- Lifecycle -----------------------------------------------------------------------------

    /// `ffxCreateContext`: FrameGenerationCreate (+ HUDLESS, BACKEND_DX12). `device` is the
    /// binding the session resolved from the backend handle.
    pub fn create(
        &mut self,
        desc: &CreateContextDesc,
        device: DeviceBinding<D>,
    ) -> Result<(), ContextError> {
        let Some(frame_generation) = desc.frame_generation else {
            return Err(self.fail(reasons::CREATE_WITHOUT_DESC));
        };
        if self.device.is_some() {
            return Err(self.fail(reasons::CREATE_TWICE));
        }
        if frame_generation.flags & !SUPPORTED_CREATE_FLAGS != 0 {
            return Err(self.fail(reasons::UNSUPPORTED_CREATE_FLAGS));
        }
        // The HDR flag only says the inputs are HDR-encoded (P4.8, K129); Stage 1 warps, fills and
        // blends in the encoded space it is given, so the flag changes nothing in the passes.
        let Some(color_format) = color_format_of(frame_generation.back_buffer_format.0) else {
            return Err(self.fail(reasons::UNSUPPORTED_BACK_BUFFER));
        };
        if desc.backend_dx12.map_or(true, |backend| backend.device == 0) {
            return Err(self.fail(reasons::CREATE_WITHOUT_DEVICE));
        }
        if frame_generation.display_size.width == 0 || frame_generation.display_size.height == 0 {
            return Err(self.fail(reasons::EMPTY_DISPLAY));
        }

        // Initialize the generator before publishing the device, so a failed pipeline setup leaves
        // nothing half-created behind.
        let stage = match G::create(&device.device) {
            Ok(stage) => stage,
            Err(error) => {
                tracing::debug!("fg: stage 1 initialization failed: {error}");
                return Err(self.fail(reasons::STAGE_INIT));
            }
        };
        self.device = Some(device);
        self.stage = Some(stage);
        self.create_flags = frame_generation.flags;
        self.color_format = color_format;
        self.display_width = frame_generation.display_size.width;
        self.display_height = frame_generation.display_size.height;
        self.max_render_width = frame_generation.max_render_size.width;
        self.max_render_height = frame_generation.max_render_size.height;

        tracing::info!(
            "fg: context created, display {}x{}, max render {}x{}{}{}",
            self.display_width,
            self.display_height,
            self.max_render_width,
            self.max_render_height,
            if self.create_flags & ENABLE_DEPTH_INVERTED != 0 { ", depth inverted" } else { "" },
            if self.create_flags & ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS != 0 {
                ", display-res motion"
            } else {
                ""
            }
        );
        Ok(())
    }

    /// Refuses destruction while recorded/submitted GPU work has not retired.
    pub fn destroy(&mut self) -> Result<(), ContextError> {
        if !self.retired() {
            return Err(self.fail(reasons::DESTROY_BEFORE_RETIREMENT));
        }
        if let Some(stage) = self.stage.as_mut() {
            stage.release_targets();
        }
        self.retirement = None;
        self.retirement_value = 0;
        self.snapshot = false;
        self.prepare_waited = false;
        self.prepare_list = 0;
        self.device = None;
        self.hudless = 0;
        self.input_formats_logged = false;
        self.enabled = false;
        self.prepare_valid = false;
        self.history_valid = false;
        self.reset_requested = true;
        self.display_width = 0;
        self.display_height = 0;
        self.render_width = 0;
        self.render_height = 0;
        self.frame_id = 0;
        self.prepared_frame_id = 0;
        self.frames_generated = 0;
        Ok(())
    }

    /// `ffxConfigure`: FrameGenerationConfig, modern or legacy.
    pub fn configure(&mut self, desc: &ConfigureDesc) -> Result<(), ContextError> {
        if self.device.is_none() {
            return Err(self.fail(reasons::CONFIGURE_WITHOUT_CONTEXT));
        }

        let (enabled, hudless, hudless_state, frame_id) =
            if let Some(modern) = desc.frame_generation.as_ref() {
                let state = ResourceState::from_ffx(modern.hud_less_color.state);
                (
                    modern.frame_generation_enabled,
                    modern.hud_less_color.handle,
                    state,
                    modern.frame_id,
                )
            } else if let Some(legacy) = desc.legacy_frame_generation.as_ref() {
                let state = ResourceState::from_ffx(legacy.hud_less_color.state);
                (
                    legacy.frame_generation_enabled,
                    legacy.hud_less_color.handle,
                    state,
                    legacy.frame_id,
                )
            } else {
                return Err(self.fail(reasons::CONFIGURE_WITHOUT_DESC));
            };

        self.enabled = enabled;
        self.hudless = hudless;
        self.hudless_state = hudless_state;
        self.frame_id = frame_id;
        if !self.enabled {
            self.prepare_valid = false;
            self.history_valid = false;
            self.reset_requested = true;
        }
        Ok(())
    }

    /// `ffxDispatch`: FrameGenerationPrepare / V2.
    pub fn prepare(&mut self, desc: &DispatchDesc) -> Result<(), ContextError> {
        // A rejected prepare must never leave the preceding frame eligible.
        self.prepare_valid = false;
        if self.device.is_none() {
            return Err(self.fail(reasons::PREPARE_WITHOUT_CONTEXT));
        }
        // A previously generated frame owns the single slot until it retires. Wait rather than
        // reject: the game's next prepare follows immediately, and a rejection fails the game's
        // dispatch (K98).
        if !self.retired() && !self.recorded {
            let retired = self.wait_for_retirement(PREPARE_SLOT_WAIT_MS);
            if !retired {
                return Err(self.fail(reasons::PREPARE_SLOT_IN_FLIGHT));
            }
        }
        if !self.retired() {
            return Err(self.fail(reasons::PREPARE_SLOT_RECORDED));
        }
        // An invalidated prepare still owns its descriptors until its submission retires. Only now
        // may it be abandoned, without releasing its textures.
        if self.snapshot {
            if let Some(stage) = self.stage.as_mut() {
                stage.discard_prepare();
            }
            self.snapshot = false;
            self.reset_requested = true;
            self.history_valid = false;
        }

        let (prepare, reset) = if let Some(v2) = desc.prepare_v2.as_ref() {
            (&v2.prepare, v2.reset)
        } else if let Some(v1) = desc.prepare_v1.as_ref() {
            (&v1.prepare, v1.unused_reset)
        } else {
            return Err(self.fail(reasons::PREPARE_WITHOUT_DESC));
        };

        self.depth_state = ResourceState::from_ffx(prepare.depth.state);
        self.motion_state = ResourceState::from_ffx(prepare.motion_vectors.state);

        if prepare.depth.handle == 0 || prepare.motion_vectors.handle == 0 {
            return Err(self.fail(reasons::PREPARE_WITHOUT_INPUTS));
        }
        if prepare.render_size.width == 0 || prepare.render_size.height == 0 {
            return Err(self.fail(reasons::PREPARE_WITHOUT_RENDER_SIZE));
        }
        let render = (prepare.render_size.width, prepare.render_size.height);
        let geometry_ok = render.0 <= self.max_render_width
            && render.1 <= self.max_render_height
            && render.0 <= self.display_width
            && render.1 <= self.display_height;
        if !geometry_ok {
            return Err(self.fail(reasons::PREPARE_OUTSIDE_SIZES));
        }

        let depth = self
            .objects
            .texture(prepare.depth.handle)
            .ok_or(());
        let motion = self
            .objects
            .texture(prepare.motion_vectors.handle)
            .ok_or(());
        let (Ok(depth), Ok(motion)) = (depth, motion) else {
            return Err(self.fail(reasons::UNSUPPORTED_INPUT_GEOMETRY));
        };

        if !self.input_formats_logged {
            self.input_formats_logged = true;
            let depth_info = depth.info();
            let motion_info = motion.info();
            tracing::info!(
                "fg: native inputs depth{{format={:?} flags={} size={}x{}}} motion{{format={:?} flags={} size={}x{}}}",
                depth_info.format,
                depth_info.flags.0,
                depth_info.width,
                depth_info.height,
                motion_info.format,
                motion_info.flags.0,
                motion_info.width,
                motion_info.height,
            );
        }
        let depth_info = depth.info();
        let motion_info = motion.info();
        let Some(depth_format) = depth_srv_format(depth_info.format) else {
            return Err(self.fail(reasons::UNSUPPORTED_DEPTH));
        };
        if !is_supported_motion(motion_info.format) {
            return Err(self.fail(reasons::UNSUPPORTED_MOTION));
        }
        if !input_geometry_ok(&depth_info, render) || !input_geometry_ok(&motion_info, render) {
            return Err(self.fail(reasons::UNSUPPORTED_INPUT_GEOMETRY));
        }

        let scale = &prepare.motion_vector_scale;
        if !scale.x.is_finite() || !scale.y.is_finite() || scale.x == 0.0 || scale.y == 0.0 {
            return Err(self.fail(reasons::INVALID_MOTION_SCALE));
        }
        if self.render_width != render.0 || self.render_height != render.1 {
            self.reset_requested = true;
            self.history_valid = false;
        }
        self.render_width = render.0;
        self.render_height = render.1;
        self.motion_scale_x = scale.x;
        self.motion_scale_y = scale.y;

        if prepare.command_list == 0 {
            return Err(self.fail(reasons::PREPARE_WITHOUT_LIST));
        }
        let Some(command_list) = self.objects.list(prepare.command_list) else {
            return Err(self.fail(reasons::GENERATE_OBJECT));
        };

        let mut params = FrameParams {
            color_format: self.color_format,
            render_width: self.render_width,
            render_height: self.render_height,
            display_width: self.display_width,
            display_height: self.display_height,
            depth_inverted: self.create_flags & ENABLE_DEPTH_INVERTED != 0,
            // The FFX scale is negated before Stage 1 sees it; the sign is load-bearing.
            motion_vector_scale_x: -self.motion_scale_x,
            motion_vector_scale_y: -self.motion_scale_y,
            ..FrameParams::default()
        };
        params.alpha = MIDPOINT_ALPHA;

        let Some(stage) = self.stage.as_mut() else {
            return Err(self.fail(reasons::PREPARE_WITHOUT_CONTEXT));
        };
        if let Err(error) = stage.configure(&params) {
            tracing::debug!("fg: stage configuration failed: {error}");
            return Err(self.fail(reasons::PREPARE_STAGE_CONFIG));
        }

        let inputs = InputFrame {
            color: None,
            depth: Some(&depth),
            motion: Some(&motion),
            color_state: ResourceState::COMMON,
            depth_state: self.depth_state,
            motion_state: self.motion_state,
            depth_format,
            motion_format: motion_info.format,
        };
        if stage.prepare(&command_list, &inputs).is_err() {
            return Err(self.fail(reasons::PREPARE_REJECTED));
        }
        self.recorded = true;
        self.snapshot = true;
        self.prepare_waited = false;
        self.prepare_list = prepare.command_list;
        self.prepare_valid = true;

        if reset || (self.history_valid && prepare.frame_id != self.prepared_frame_id + 1) {
            self.reset_requested = true;
            self.history_valid = false;
        }
        self.prepared_frame_id = prepare.frame_id;
        Ok(())
    }

    // -- Generation ----------------------------------------------------------------------------

    /// Generates the pending midpoint into `output` at the configured alpha.
    pub fn generate(
        &mut self,
        list: OpaqueHandle,
        output: OpaqueHandle,
        output_state: ResourceState,
        hud: Option<OpaqueHandle>,
    ) -> Result<(), ContextError> {
        if list == 0 || output == 0 {
            return Err(self.fail(reasons::GENERATE_WITHOUT_OUTPUT));
        }
        let alpha = self.stage.as_ref().map_or(MIDPOINT_ALPHA, |stage| stage.params().alpha);
        let phase = PhaseSpec { output, output_state, alpha };
        self.generate_phases(list, &[phase], hud)
    }

    /// Generates `count` (1..=MAX_PHASES) frames between the same two real frames from one
    /// prepare, each at its own alpha. With a HUD source, its HUD layer is composited onto each.
    pub fn generate_phases(
        &mut self,
        list: OpaqueHandle,
        phases: &[PhaseSpec],
        hud: Option<OpaqueHandle>,
    ) -> Result<(), ContextError> {
        if list == 0 || phases.is_empty() {
            return Err(self.fail(reasons::GENERATE_WITHOUT_OUTPUT));
        }
        if !self.enabled {
            return Err(self.fail(reasons::GENERATE_DISABLED));
        }
        if !self.prepare_valid {
            return Err(self.fail(reasons::GENERATE_WITHOUT_PREPARE));
        }
        if self.recorded && list != self.prepare_list && !self.prepare_waited {
            return Err(self.fail(reasons::SPLIT_BEFORE_REGISTRATION));
        }
        if !self.recorded && !self.prepare_waited {
            return Err(self.fail(reasons::SPLIT_WITHOUT_DEPENDENCY));
        }
        if self.prepared_frame_id != self.frame_id {
            return Err(self.fail(reasons::FRAME_ID_MISMATCH));
        }
        if self.hudless == 0 {
            return Err(self.fail(reasons::GENERATE_WITHOUT_HUDLESS));
        }
        let Some(hudless) = self.objects.texture(self.hudless) else {
            return Err(self.fail(reasons::GENERATE_OBJECT));
        };
        if hudless.info().format != PixelFormat::from(self.color_format) {
            return Err(self.fail(reasons::HUDLESS_FORMAT));
        }
        let Some(command_list) = self.objects.list(list) else {
            return Err(self.fail(reasons::GENERATE_OBJECT));
        };

        // Resolve every phase output before recording anything: a missing output must not leave a
        // half-recorded command list for the caller to submit.
        let mut outputs = Vec::with_capacity(phases.len());
        for phase in phases {
            let Some(texture) = self.objects.texture(phase.output) else {
                return Err(self.fail(reasons::GENERATE_OBJECT));
            };
            outputs.push((texture, *phase));
        }

        let reset = self.reset_requested || !self.history_valid;
        let frame = InputFrame {
            color: Some(&hudless),
            depth: None,
            motion: None,
            color_state: self.hudless_state,
            depth_state: ResourceState::NON_PIXEL_SHADER_RESOURCE,
            motion_state: ResourceState::NON_PIXEL_SHADER_RESOURCE,
            depth_format: PixelFormat::R32Float,
            motion_format: PixelFormat::R16G16B16A16Float,
        };
        let hud_texture = hud.and_then(|handle| self.objects.texture(handle));
        let hud_ref = hud_texture
            .as_ref()
            .map(|texture| HudSource { real: texture, real_state: ResourceState::PRESENT });

        let recorded: Vec<GeneratePhase<'_, D::Texture>> = outputs
            .iter()
            .map(|(texture, phase)| GeneratePhase {
                output: texture,
                output_state: phase.output_state,
                alpha: phase.alpha,
            })
            .collect();

        let Some(stage) = self.stage.as_mut() else {
            return Err(self.fail(reasons::GENERATE_WITHOUT_OUTPUT));
        };
        if let Err(error) = stage.generate(&command_list, &frame, &recorded, reset, hud_ref, None) {
            tracing::debug!("fg: stage 1 rejected the frame: {error}");
            return Err(self.fail(reasons::STAGE_REJECTED_FRAME));
        }

        self.history_valid = true;
        self.reset_requested = false;
        self.prepare_valid = false;
        self.snapshot = false;
        self.recorded = true;
        self.prepare_list = 0;
        self.frames_generated += 1;
        Ok(())
    }

    // -- Submission and retirement -------------------------------------------------------------

    /// After ExecuteCommandLists, registers a queue-signalled fence covering all work recorded
    /// since the last registration. No CPU-signalled completion.
    pub fn track_submission(
        &mut self,
        retirement: Arc<dyn Retirement>,
        value: u64,
    ) -> Result<(), ContextError> {
        if !self.recorded || value == 0 || value == u64::MAX {
            return Err(self.fail(reasons::INVALID_SUBMISSION));
        }
        let device_identity = self.device.as_ref().map(|binding| binding.identity);
        if Some(retirement.device_identity()) != device_identity {
            return Err(self.fail(reasons::FENCE_OTHER_DEVICE));
        }
        if let Some(current) = &self.retirement {
            if Arc::ptr_eq(current, &retirement) && value <= self.retirement_value {
                return Err(self.fail(reasons::TIMELINE_NOT_ADVANCED));
            }
        }
        self.retirement = Some(retirement);
        self.retirement_value = value;
        self.recorded = false;
        Ok(())
    }

    /// Makes the generation queue wait for the prepare submission before its next work. Used for
    /// split recording; the caller guarantees the recorded prepare work was submitted first.
    pub fn wait_prepare(&mut self, queue: &dyn QueueDependency) -> Result<(), ContextError> {
        let Some(retirement) = &self.retirement else {
            return Err(self.fail(reasons::PREPARE_NO_SUBMISSION));
        };
        if !self.snapshot || self.recorded {
            return Err(self.fail(reasons::PREPARE_NO_SUBMISSION));
        }
        let device_identity = self.device.as_ref().map(|binding| binding.identity);
        if Some(queue.device_identity()) != device_identity {
            return Err(self.fail(reasons::QUEUE_OTHER_DEVICE));
        }
        if retirement.completed() == u64::MAX {
            return Err(self.fail(reasons::PREPARE_DEVICE_REMOVED));
        }
        if queue.wait(retirement.as_ref(), self.retirement_value).is_err() {
            return Err(self.fail(reasons::QUEUE_WAIT_FAILED));
        }
        self.prepare_waited = true;
        Ok(())
    }

    /// The caller guarantees the recorded prepare work was already submitted on the same queue
    /// as the following generation, before it. Queue ordering permits split recording, but it is
    /// not a retirement token: the prepare stays marked recorded until a fence is registered.
    pub fn note_ordered_submission(&mut self) {
        self.prepare_waited = true;
    }

    /// Invalidates CPU-side generation history after a frame cannot be presented. Recorded or
    /// borrowed work remains unretired until a queue token is registered.
    pub fn abandon_generation(&mut self) {
        self.prepare_valid = false;
        self.history_valid = false;
        self.reset_requested = true;
    }

    /// A bounded wait for the last registered submission. `false` when nothing was submitted yet
    /// or the wait timed out; never unbounded.
    pub fn wait_for_retirement(&mut self, timeout_ms: u32) -> bool {
        if self.recorded {
            return false; // not submitted yet: nothing to wait for
        }
        // The offline seam: while the hold is set, no work reports as retired. Returning at once
        // keeps a test from waiting out the real bound; the runtime never sets it.
        if self.faults.retirement_held() {
            return false;
        }
        let Some(retirement) = self.retirement.clone() else {
            return true;
        };
        if self.retired() {
            return true;
        }
        // Poll the actual queue-signalled fence: a timed-out SetEventOnCompletion registration can
        // outlive the waiting handle, and there is no CPU-signalled completion to retain.
        let deadline = Instant::now() + Duration::from_millis(u64::from(timeout_ms));
        loop {
            if self.retired() {
                return true;
            }
            if Instant::now() >= deadline {
                let _ = self.fail(reasons::RETIREMENT_TIMEOUT);
                return false;
            }
            // One bounded slice, then re-check: a removed device or a completed fence ends early.
            let slice = Duration::from_millis(PREPARE_SLOT_POLL_MS);
            match retirement.wait(self.retirement_value, slice) {
                Ok(()) => {}
                Err(GpuError::Timeout) => {}
                Err(_) => {
                    // A removed device executes nothing more; retired() reports it.
                    if self.device_lost() {
                        return true;
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------
// The session-owned sink
// ---------------------------------------------------------------------------------------------

/// A mutex-guarded context shared by the session object and the generation sink it installs.
///
/// The C++ let the swapchain call back into the session through raw `void*` user pointers while a
/// separate mutex serialized the calls. Here the same mutex lives in the operating system's
/// `Mutex` and the sink closures share one [`SharedContext`]. Every operation that mutates the
/// context takes the lock; the wrapper never holds it while it presents.
///
/// The `Send` bounds are what a `MutexGuard` across threads requires; the generator adapters own
/// COM objects, which `windows` marks sendable, and the context never shares the device across
/// threads without the lock.
pub type SharedContext<D, G> = Arc<std::sync::Mutex<Context<D, G>>>;

/// The generation sink the session installs for a shared context. It is the object-safe
/// [`ContextHandle`] the presentation slice forwards its `GenerateSink` calls to.
///
/// Each method takes the context lock for the duration of the call, exactly as `Session::record`
/// takes `m_context_mutex`. The callbacks never call back into the swapchain (the C++ contract).
pub struct ContextSink<D: GpuDevice + Send + 'static, G: FrameGenerator<D> + Send + 'static> {
    context: SharedContext<D, G>,
}

impl<D: GpuDevice + Send + 'static, G: FrameGenerator<D> + Send + 'static> ContextSink<D, G> {
    pub fn new(context: SharedContext<D, G>) -> Self {
        Self { context }
    }

    fn lock(&self) -> std::sync::MutexGuard<'_, Context<D, G>> {
        // A panic elsewhere while holding the lock must not fail a game call: recover the state
        // rather than propagate the poison (bookkeeping never fails a game call).
        self.context.lock().unwrap_or_else(|poisoned| poisoned.into_inner())
    }
}

impl<D: GpuDevice + Send + 'static, G: FrameGenerator<D> + Send + 'static> ContextHandle for ContextSink<D, G> {
    fn record(
        &self,
        list: OpaqueHandle,
        real: OpaqueHandle,
        output: OpaqueHandle,
        output_state: ResourceState,
    ) -> bool {
        let alpha = self.lock().stage.as_ref().map_or(MIDPOINT_ALPHA, |stage| stage.params().alpha);
        self.record_phases(
            list,
            real,
            &[PhaseSpec { output, output_state, alpha }],
        )
    }

    fn record_phases(&self, list: OpaqueHandle, real: OpaqueHandle, phases: &[PhaseSpec]) -> bool {
        let mut context = self.lock();
        if context.device_lost() {
            return false;
        }
        // The wrapper generates before either native Present, so the HUD and colour belong to the
        // current prepare and are consumed before returning to the game. Generation is recorded on
        // the same queue the game presents on, after the prepare it submitted for this frame.
        context.note_ordered_submission();
        context.generate_phases(list, phases, Some(real)).is_ok()
    }

    fn submitted(&self, fence: Arc<dyn Retirement>, value: u64) -> bool {
        self.lock().track_submission(fence, value).is_ok()
    }

    fn needs_submission(&self) -> bool {
        self.lock().has_untracked_work()
    }

    fn abandoned(&self) {
        self.lock().abandon_generation();
    }
}



