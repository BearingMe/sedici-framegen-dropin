//! The swapchain wrapper's present logic over ports (ports `presentation/swapchain.*`).
//!
//! The game renders into private back buffers; the wrapper copies them into the native chain's
//! presentation buffers and, in "vrr" mode, hands both outputs to the presenter. The schedule, the
//! pacer, the hooks and the submission bookkeeping all live here; the COM surface
//! (`adapters/driving/dxgi_wrapper.rs`) only forwards calls.
//!
//! Safety contract, kept from the C++:
//! - a call Sedici does not implement is answered with a defined error, never forwarded to another
//!   DLL and never a crash;
//! - only the owner-approved exceptions may surface an error: the create-allocation exceptions, the
//!   Present1 metadata allocation, and the context-identity namespace. Every other bookkeeping
//!   failure is logged and the game call still succeeds;
//! - once submission retirement cannot be established, admission stops (K37) until the wrapper is
//!   replaced.
//!
//! [`PresentedContext`] is the narrow view of the frame generation context the wrapper needs. The
//! context slice (a parallel port of `runtime/context.*` and `runtime/integration.*`) implements it;
//! the wrapper never names `Context` or `Session`.
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use thiserror::Error;

use crate::application::ports::clock::Clock;
use crate::application::ports::generator::{GeneratePhase, MAX_PHASES};
use crate::application::ports::gpu::{
    GpuCommandAllocator, GpuCommandList, GpuDevice, GpuError, GpuFence, GpuQueue, GpuTexture,
    PixelFormat, QueueKind, ResourceState, TextureDesc, TextureFlags,
};
use crate::application::ports::overlay::OverlayHook;
use crate::application::ports::swapchain::{NativeSwapchain, PresentFlags, PresentParams, SwapChainDesc};
use crate::application::presentation::pacing::Pacer;
use crate::application::presentation::presenter::{
    ListLifecycle, PresentJob, Presenter, ScanlineSource,
};
pub use crate::application::presentation::presenter::SharedPresentQueues;
use crate::domain::config::{Multiplier, Present, TearGuard};
use crate::domain::frame::MIDPOINT_ALPHA;
use crate::domain::gpu_policy::{acquire_frame_slot, classify_fence, FenceProgress, PRESENTER_QUEUE_CAPACITY};
use crate::domain::pacing::{OutputSchedule, OutputPlan};
use crate::domain::schedule::auto_target_fps;

/// `E_POINTER`.
pub const E_POINTER: u32 = 0x8000_4003;
/// `E_INVALIDARG`.
pub const E_INVALIDARG: u32 = 0x8007_0057;
/// `E_OUTOFMEMORY`.
pub const E_OUTOFMEMORY: u32 = 0x8007_000E;
/// `E_FAIL`.
pub const E_FAIL: u32 = 0x8000_4005;
/// `DXGI_ERROR_INVALID_CALL`.
pub const DXGI_ERROR_INVALID_CALL: u32 = 0x887A_0001;
/// `DXGI_ERROR_NOT_FOUND`.
pub const DXGI_ERROR_NOT_FOUND: u32 = 0x887A_0002;
/// `DXGI_ERROR_UNSUPPORTED`.
pub const DXGI_ERROR_UNSUPPORTED: u32 = 0x887A_0004;
/// `DXGI_ERROR_DEVICE_REMOVED`.
pub const DXGI_ERROR_DEVICE_REMOVED: u32 = 0x887A_0005;
/// `DXGI_ERROR_DEVICE_HUNG`.
pub const DXGI_ERROR_DEVICE_HUNG: u32 = 0x887A_0006;
/// `DXGI_ERROR_WAS_STILL_DRAWING`.
pub const DXGI_ERROR_WAS_STILL_DRAWING: u32 = 0x887A_000A;

const PHASE_OUTPUTS: usize = MAX_PHASES as usize;

/// How many older frames may still be presenting while the game renders the next one. One frame in
/// flight blocked the game on most frames at ~40 fps in adaptive mode (finding 009).
pub const IN_FLIGHT: usize = PRESENTER_QUEUE_CAPACITY - 1;
/// Everything a "vrr" job reads comes from a ring the game never touches.
pub const SLOTS: usize = IN_FLIGHT + 1;

/// The narrow view of the frame generation context the wrapper needs.
///
/// Recording happens while the wrapper's caller holds its Present path; an owner therefore needs no
/// other synchronisation until it detaches, except for the deferred-retirement path below.
pub trait PresentedContext: Send + Sync {
    /// The texture the wrapper's back buffers and generated images are.
    type Texture: GpuTexture;
    /// The command list the wrapper records generation into.
    type List;

    /// True while a prepare is recorded and not yet consumed.
    fn prepared(&self) -> bool;

    /// Records every planned phase of one real frame into one command list. `real` is the game's
    /// current back buffer (it still carries the HUD). A `false` return is an ordinary skipped
    /// frame that may still have recorded a partial list, so the wrapper closes and executes it.
    fn generate_phases(
        &self,
        list: &Self::List,
        real: &Self::Texture,
        phases: &[GeneratePhase<'_, Self::Texture>],
        reset_history: bool,
    ) -> bool;

    /// The recorded prepare was already submitted on the same queue, before the generation: queue
    /// ordering permits split recording, but it is not a retirement token.
    fn note_ordered_submission(&self);

    /// Registers the queue fence covering every work recorded since the last registration. `false`
    /// means no retirement token was accepted; the wrapper keeps the owner and retries later (K103).
    fn track_submission(&self, fence_value: u64) -> bool;

    /// Invalidates CPU-side generation history after a frame cannot be presented. Recorded or
    /// borrowed work stays unretired until a queue token is registered.
    fn abandon_generation(&self);

    /// True while recorded work has not been retired by a registered submission.
    fn has_untracked_work(&self) -> bool;

    /// Bounded wait for the last registered submission; `false` when nothing was submitted or the
    /// wait expired. Never waits unbounded.
    fn wait_for_retirement(&self, timeout: Duration) -> bool;

    /// The state the generated output is in when recording starts. The context must restore it.
    fn output_state(&self) -> ResourceState;
}

/// How the wrapper presents. Read from settings by [`Swapchain::wrap`] unless one is given.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct PresentChoice {
    pub present: Present,
    pub guard: TearGuard,
    pub multiplier: Multiplier,
    /// Adaptive output target in fps; 0 means three below the refresh rate.
    pub target_fps: f64,
}

impl Default for PresentChoice {
    fn default() -> Self {
        Self { present: Present::Vrr, guard: TearGuard::Scanline, multiplier: Multiplier::X2, target_fps: 0.0 }
    }
}

/// The errors a game call may surface. Every one is a defined code the README records.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Error)]
pub enum SwapchainError {
    #[error("unsupported swapchain description")]
    Unsupported,
    #[error("the application asks for a present queue Sedici does not own")]
    ForeignPresentQueue,
    #[error("a back buffer is still referenced by the application")]
    BufferReferenced,
    #[error("a GPU submission failed")]
    SubmissionFailure,
    #[error("invalid argument")]
    InvalidArgument,
    #[error("the device is hung")]
    DeviceHung,
    #[error("the device was removed")]
    DeviceRemoved,
    #[error("the chain is not presentable")]
    InvalidCall,
    #[error("no interpolation texture for the current size")]
    NotFound,
    #[error("recording allocation failed")]
    OutOfMemory,
}

impl SwapchainError {
    pub const fn code(self) -> u32 {
        match self {
            Self::Unsupported => DXGI_ERROR_UNSUPPORTED,
            Self::ForeignPresentQueue => DXGI_ERROR_UNSUPPORTED,
            Self::BufferReferenced => DXGI_ERROR_INVALID_CALL,
            Self::SubmissionFailure => E_FAIL,
            Self::InvalidArgument => E_INVALIDARG,
            Self::DeviceHung => DXGI_ERROR_DEVICE_HUNG,
            Self::DeviceRemoved => DXGI_ERROR_DEVICE_REMOVED,
            Self::InvalidCall => DXGI_ERROR_INVALID_CALL,
            Self::NotFound => DXGI_ERROR_NOT_FOUND,
            Self::OutOfMemory => E_OUTOFMEMORY,
        }
    }
}

/// What `wrap` could not do; kept so the fallback is logged without failing the game.
#[derive(Clone, Debug, Default)]
pub struct WrapReport {
    pub presenter: bool,
    pub fallback_reason: Option<String>,
}

/// What `wrap` learns from the process environment and the adapters, passed in rather than read
/// here, so the application layer stays free of platform calls.
#[derive(Clone)]
pub struct WrapEnvironment {
    /// The D3DKMT scan-line reader, when an adapter provides one.
    pub scanline: Option<Arc<dyn ScanlineSource>>,
    /// True when a Present-hooking overlay (RTSS) is loaded; "vrr" refuses to start then (K13).
    pub rtss_loaded: bool,
    /// `SEDICI_FG_SCANLINE_LEAD_US`, in seconds.
    pub lead_seconds: f64,
    /// `SEDICI_FG_SYNC=0/1`, the legacy A/B switch, when set.
    pub force_sync_override: Option<bool>,
    /// `SEDICI_FG_ALIGN=1`, the opt-in vblank alignment.
    pub align_vblank: bool,
}

impl Default for WrapEnvironment {
    fn default() -> Self {
        Self { scanline: None, rtss_loaded: false, lead_seconds: 0.00025, force_sync_override: None, align_vblank: false }
    }
}

/// The wrapper's counters, read by diagnostics and tests.
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub struct SwapchainCounters {
    pub generated_presents: u64,
    pub skipped_generates: u64,
    pub present_cycles: u64,
}

/// The owner of the installed generation sink, with the lease needed for deferred retirement.
pub struct GeneratorSink<C: PresentedContext> {
    pub context: Arc<C>,
    /// Identifies the owner for [`Swapchain::clear_generator_if_owner`].
    pub owner: usize,
    /// A lease on the owner, required for callbacks after detachment (K103).
    pub lease: Option<Arc<dyn std::any::Any + Send + Sync>>,
}

/// One "vrr" ring slot: the generated images and the real-frame snapshot the presenter reads.
pub struct VrrSlot<D: GpuDevice> {
    pub phase_outputs: [D::Texture; PHASE_OUTPUTS],
    pub snapshot: D::Texture,
    pub snapshot_fence: u64,
}

struct Scratch<D: GpuDevice> {
    /// The game's own recording target (P4.8, K131). Its own lock, never the wrapper's Present
    /// path: a game thread asking for it while another sits in Present would otherwise block
    /// (K138). Nothing recorded into it is executed or presented.
    allocator: Option<D::CommandAllocator>,
    list: Option<D::CommandList>,
    texture: Option<D::Texture>,
    /// Replaced by a resize; kept because the game may still hold the pointer.
    retired: Vec<D::Texture>,
    format: Option<PixelFormat>,
    width: u32,
    height: u32,
}

fn lock<T>(mutex: &Mutex<T>) -> std::sync::MutexGuard<'_, T> {
    mutex.lock().unwrap_or_else(|poisoned| poisoned.into_inner())
}

/// The swapchain wrapper.
pub struct Swapchain<D, N, C, O>
where
    D: GpuDevice + Send + Sync + 'static,
    D::Texture: Clone + Send + 'static,
    D::CommandList: ListLifecycle<Allocator = D::CommandAllocator> + GpuCommandList<D::Texture> + Send + Sync + 'static,
    D::CommandAllocator: Send + 'static,
    D::CommandList: Clone,
    D::Fence: Send + Sync + 'static,
    D::Queue: Send + Sync + 'static,
    N: NativeSwapchain<Buffer = D::Texture> + Send + Sync + 'static,
    C: PresentedContext<Texture = D::Texture, List = D::CommandList> + 'static,
    O: OverlayHook<D::Texture> + 'static,
{
    native: Arc<N>,
    device: Arc<D>,
    queue: Arc<D::Queue>,
    fence: Arc<D::Fence>,
    clock: Arc<dyn Clock>,
    buffers: Vec<D::Texture>,
    generated: D::Texture,
    copy_allocators: [D::CommandAllocator; 2],
    copy_lists: [D::CommandList; 2],
    next_list: AtomicU64,
    generate_allocator: D::CommandAllocator,
    generate_list: <D as GpuDevice>::CommandList,
    scratch: Mutex<Scratch<D>>,
    sink: Mutex<Option<GeneratorSink<C>>>,
    untracked: Mutex<Option<GeneratorSink<C>>>,
    /// The probe a session-owned generation source registers its submissions against. The wrapper's
    /// own fence is a `D::Fence`; the composition root wraps the same timeline in a
    /// [`crate::application::context::Retirement`] and installs it here.
    sink_retirement: Mutex<Option<Arc<dyn crate::application::context::Retirement>>>,
    generator_closed: AtomicBool,
    present_hook: Mutex<Option<Arc<dyn Fn(u64) + Send + Sync>>>,
    overlay: Mutex<Arc<O>>,
    pacer: Mutex<Pacer>,
    schedule: Mutex<OutputSchedule>,
    choice: Mutex<PresentChoice>,
    presenter: Option<Arc<Presenter<D, N>>>,
    force_sync: bool,
    align_vblank: bool,
    generated_ready: AtomicBool,
    counters: Mutex<SwapchainCounters>,
    fence_value: AtomicU64,
    /// Once submission retirement cannot be established, no further frames are admitted (K37).
    submission_failure: Mutex<Option<u32>>,
    game_index: Mutex<u32>,
    vrr_next_slot: Mutex<u32>,
    vrr_slots: Mutex<Vec<VrrSlot<D>>>,
    /// The rate window the "vrr" path reports every 300 game frames.
    rate_window: Mutex<(f64, u32, u32)>,
}

impl<D, N, C, O> Swapchain<D, N, C, O>
where
    D: GpuDevice + Send + Sync + 'static,
    D::Texture: Clone + Send + 'static,
    D::CommandList: ListLifecycle<Allocator = D::CommandAllocator> + GpuCommandList<D::Texture> + Send + Sync + 'static,
    D::CommandAllocator: Send + 'static,
    D::CommandList: Clone,
    D::Fence: Send + Sync + 'static,
    D::Queue: Send + Sync + 'static,
    N: NativeSwapchain<Buffer = D::Texture> + Send + Sync + 'static,
    C: PresentedContext<Texture = D::Texture, List = D::CommandList> + 'static,
    O: OverlayHook<D::Texture> + 'static,
{
    /// Creates the wrapper. Every fallible allocation happens before success, and neither the
    /// caller's handle nor the underlying chain is mutated on failure (the create-allocation
    /// exception). "vrr" falls back to "vsync" rather than failing the game.
    #[allow(clippy::too_many_arguments)]
    pub fn wrap(
        native: Arc<N>,
        device: Arc<D>,
        queue: Arc<D::Queue>,
        clock: Arc<dyn Clock>,
        choice: PresentChoice,
        queues: Arc<SharedPresentQueues<D>>,
        overlay: Arc<O>,
        environment: WrapEnvironment,
        handed_refs: u32,
        report: &mut WrapReport,
    ) -> Result<Self, SwapchainError> {
        let WrapEnvironment { scanline, rtss_loaded, lead_seconds, force_sync_override, align_vblank } = environment;
        let desc = native.desc().map_err(|_| SwapchainError::Unsupported)?;
        if !desc.is_supported() {
            return Err(SwapchainError::Unsupported);
        }
        let mut chosen = choice;
        if chosen.present == Present::Vrr {
            if rtss_loaded && std::env::var_os("SEDICI_FG_ALLOW_RTSS").is_none() {
                report.fallback_reason = Some(
                    "RTSS is injected and would write the chain from the wrong queue; close RTSS or set SEDICI_FG_ALLOW_RTSS=1"
                        .to_owned(),
                );
                chosen.present = Present::Vsync;
            } else if handed_refs == 0 {
                report.fallback_reason =
                    Some("the caller keeps its swapchain reference, so the chain cannot be recreated".to_owned());
                chosen.present = Present::Vsync;
            }
        }

        let copy_allocators = [
            device.create_command_allocator(QueueKind::Direct).map_err(|_| SwapchainError::OutOfMemory)?,
            device.create_command_allocator(QueueKind::Direct).map_err(|_| SwapchainError::OutOfMemory)?,
        ];
        let generate_allocator =
            device.create_command_allocator(QueueKind::Direct).map_err(|_| SwapchainError::OutOfMemory)?;
        let copy_raw = [
            device.create_command_list(&copy_allocators[0]).map_err(|_| SwapchainError::OutOfMemory)?,
            device.create_command_list(&copy_allocators[1]).map_err(|_| SwapchainError::OutOfMemory)?,
        ];
        let generate_raw =
            device.create_command_list(&generate_allocator).map_err(|_| SwapchainError::OutOfMemory)?;
        let copy_lists = copy_raw;
        let generate_list = generate_raw;
        let fence = Arc::new(device.create_fence(0).map_err(|_| SwapchainError::OutOfMemory)?);
        let buffers = create_back_buffers(device.as_ref(), &desc).map_err(|_| SwapchainError::OutOfMemory)?;
        let generated = create_generated(device.as_ref(), &desc).map_err(|_| SwapchainError::OutOfMemory)?;

        let mut wrapper = Self {
            native,
            device,
            queue,
            fence,
            clock: Arc::clone(&clock),
            buffers,
            generated,
            copy_allocators,
            copy_lists,
            next_list: AtomicU64::new(0),
            generate_allocator,
            generate_list,
            scratch: Mutex::new(Scratch {
                allocator: None,
                list: None,
                texture: None,
                retired: Vec::new(),
                format: Some(desc.format),
                width: desc.width,
                height: desc.height,
            }),
            sink: Mutex::new(None),
            sink_retirement: Mutex::new(None),
            untracked: Mutex::new(None),
            generator_closed: AtomicBool::new(false),
            present_hook: Mutex::new(None),
            overlay: Mutex::new(overlay),
            pacer: Mutex::new(Pacer::new()),
            schedule: Mutex::new(OutputSchedule::new()),
            choice: Mutex::new(chosen),
            presenter: None,
            force_sync: chosen.present == Present::Vsync,
            align_vblank,
            generated_ready: AtomicBool::new(false),
            counters: Mutex::new(SwapchainCounters::default()),
            fence_value: AtomicU64::new(0),
            submission_failure: Mutex::new(None),
            game_index: Mutex::new(0),
            vrr_next_slot: Mutex::new(0),
            vrr_slots: Mutex::new(Vec::new()),
            rate_window: Mutex::new((0.0, 0, 0)),
        };
        // A legacy A/B switch: `SEDICI_FG_SYNC=0/1` overrides the mode for "game" and "vrr".
        if chosen.present != Present::Vsync {
            if let Some(forced) = force_sync_override {
                wrapper.force_sync = forced;
            }
        }
        if chosen.present == Present::Vrr {
            wrapper.create_vrr_outputs(&desc);
            // The shared queue entry keeps the device alive, so its address is a stable key.
            let device_key = Arc::as_ptr(&wrapper.device) as usize;
            let mut why = String::new();
            let presenter = Presenter::create(
                Arc::clone(&wrapper.device),
                queues,
                device_key,
                Arc::clone(&wrapper.native),
                Arc::clone(&wrapper.clock),
                chosen.guard,
                scanline,
                lead_seconds,
                wrapper.overlay_hook(),
                &mut why,
            );
            match presenter {
                Some(presenter) => {
                    wrapper.presenter = Some(Arc::new(presenter));
                    report.presenter = true;
                }
                None => {
                    chosen.present = Present::Vsync;
                    wrapper.force_sync = true;
                    wrapper.vrr_slots = Mutex::new(Vec::new());
                    report.fallback_reason = Some(why);
                }
            }
        }
        *lock(&wrapper.choice) = chosen;
        wrapper.configure_schedule(chosen);
        Ok(wrapper)
    }

    fn create_vrr_outputs(&mut self, desc: &SwapChainDesc) {
        let generated = TextureDesc::texture2d(
            desc.width,
            desc.height,
            desc.format,
            TextureFlags::ALLOW_UNORDERED_ACCESS,
        );
        let snapshot = TextureDesc::texture2d(desc.width, desc.height, desc.format, TextureFlags::NONE);
        let mut slots = Vec::with_capacity(SLOTS);
        for _ in 0..SLOTS {
            let mut built = Vec::with_capacity(PHASE_OUTPUTS);
            for _ in 0..PHASE_OUTPUTS {
                match self.device.create_texture(&generated) {
                    Ok(texture) => built.push(texture),
                    Err(_) => return,
                }
            }
            let Some(snapshot_texture) = self.device.create_texture(&snapshot).ok() else { return };
            let mut iter = built.into_iter();
            let phase_outputs = std::array::from_fn(|_| iter.next().expect("counted above"));
            slots.push(VrrSlot { phase_outputs, snapshot: snapshot_texture, snapshot_fence: 0 });
        }
        *lock(&self.vrr_slots) = slots;
    }

    fn overlay_hook(&self) -> Arc<dyn OverlayHook<D::Texture>> {
        let overlay: &Arc<O> = &lock(&self.overlay);
        Arc::clone(overlay) as Arc<dyn OverlayHook<D::Texture>>
    }

    /// The "vrr" presenter, when one is active.
    pub fn presenter(&self) -> Option<&Arc<Presenter<D, N>>> {
        self.presenter.as_ref()
    }

    /// True when "vrr" is active (presenter thread and queue in use).
    pub fn async_present(&self) -> bool {
        self.presenter.is_some()
    }

    /// Installs the probe a session-owned generation source registers submissions against.
    ///
    /// The wrapper's own queue fence is a `D::Fence`; the composition root wraps **that same fence**
    /// in a [`crate::application::context::Retirement`] through `make`, so a session context's
    /// retirement is the wrapper's timeline rather than a second, never-signalled one. A `None`
    /// from `make` leaves admission closed (K37) instead of claiming a token the context never
    /// received.
    pub fn install_retirement(
        &self,
        make: impl FnOnce(&D::Fence) -> Option<Arc<dyn crate::application::context::Retirement>>,
    ) {
        if let Some(probe) = make(&self.fence) {
            *lock(&self.sink_retirement) = Some(probe);
        }
    }

    /// The probe installed by [`Swapchain::install_retirement`].
    pub fn sink_retirement(&self) -> Option<Arc<dyn crate::application::context::Retirement>> {
        lock(&self.sink_retirement).clone()
    }

    /// Installs the frame generation context. Without one the wrapper presents real frames only.
    /// False after [`Swapchain::close_generator`]: a destroyed owner may not be reattached.
    pub fn set_generator(&self, sink: Option<GeneratorSink<C>>) -> bool {
        if self.generator_closed.load(Ordering::Acquire) {
            return false;
        }
        let mut current = lock(&self.sink);
        let replaces = current.as_ref().map(|sink| sink.owner) != sink.as_ref().map(|sink| sink.owner);
        *current = sink;
        drop(current);
        if replaces {
            self.generated_ready.store(false, Ordering::Release);
            lock(&self.pacer).reset();
        }
        true
    }

    /// Clears only the sink installed by this owner; a stale Session must not detach a newer one.
    pub fn clear_generator_if_owner(&self, owner: usize) -> bool {
        let mut current = lock(&self.sink);
        match current.as_ref() {
            Some(sink) if sink.owner == owner => {
                *current = None;
                drop(current);
                self.generated_ready.store(false, Ordering::Release);
                lock(&self.pacer).reset();
                true
            }
            _ => false,
        }
    }

    /// Detaches and forbids later attachment. A pending midpoint is discarded.
    pub fn close_generator(&self) {
        self.generator_closed.store(true, Ordering::Release);
        *lock(&self.sink) = None;
        self.generated_ready.store(false, Ordering::Release);
        lock(&self.pacer).reset();
    }

    /// Called once per application Present, before any output, with the tick timestamp taken on
    /// entry. This is the one place that sees a game frame rather than an output frame.
    pub fn set_present_hook(&self, hook: Option<Arc<dyn Fn(u64) + Send + Sync>>) {
        *lock(&self.present_hook) = hook;
    }

    pub fn set_output_hooks(&self, overlay: Arc<O>) {
        *lock(&self.overlay) = Arc::clone(&overlay);
        let hook: Arc<dyn OverlayHook<D::Texture>> = overlay;
        if let Some(presenter) = &self.presenter {
            presenter.set_output_hooks(hook);
        }
    }

    pub fn counters(&self) -> SwapchainCounters {
        let mut counters = *lock(&self.counters);
        if let Some(presenter) = &self.presenter {
            counters.generated_presents =
                counters.generated_presents.saturating_add(presenter.counters().generated_shown());
            counters.skipped_generates =
                counters.skipped_generates.saturating_add(presenter.counters().generated_dropped());
        }
        counters
    }

    /// The game-facing buffer index. Advances once per accepted cycle.
    pub fn current_back_buffer_index(&self) -> u32 {
        *lock(&self.game_index)
    }

    /// The game's private back buffer for `index`, for the COM surface's `GetBuffer`.
    pub fn back_buffer(&self, index: u32) -> Option<&D::Texture> {
        self.buffers.get(index as usize)
    }

    // -- Names the COM-surface adapter uses, so it never collides with a trait method it must
    // implement under the same name ----------------------------------------------------------

    /// [`Swapchain::present`], under a name the COM adapter can call from its own `present`.
    pub fn present_chain(
        &self,
        sync: u32,
        flags: PresentFlags,
        params: Option<&PresentParams>,
    ) -> Result<(), SwapchainError> {
        self.present(sync, flags, params)
    }

    /// [`Swapchain::resize`], under a name the COM adapter can call from its own `resize`.
    pub fn resize_chain(
        &self,
        count: u32,
        width: u32,
        height: u32,
        format: Option<PixelFormat>,
        flags: u32,
    ) -> Result<(), SwapchainError> {
        self.resize(count, width, height, format, flags)
    }

    /// [`Swapchain::resize_buffers1`], under a name the COM adapter can call.
    pub fn resize_chain1(
        &self,
        count: u32,
        width: u32,
        height: u32,
        format: Option<PixelFormat>,
        flags: u32,
        node_masks: &[u32],
    ) -> Result<(), SwapchainError> {
        self.resize_buffers1(count, width, height, format, flags, node_masks)
    }

    /// [`Swapchain::current_back_buffer_index`], under a name the COM adapter can call.
    pub fn game_buffer_index(&self) -> u32 {
        self.current_back_buffer_index()
    }

    /// [`Swapchain::back_buffer`], under a name the COM adapter can call.
    pub fn private_back_buffer(&self, index: u32) -> Option<&D::Texture> {
        self.back_buffer(index)
    }

    /// [`Swapchain::drain`], under a name the COM adapter's `before_fullscreen_change` can call.
    pub fn drain_chain(&self) -> Result<(), SwapchainError> {
        self.drain()
    }

    /// The chain format and size the private buffers were made with.
    pub fn buffer_count(&self) -> u32 {
        self.buffers.len() as u32
    }

    /// The native chain the wrapper presents through, for the adapter that must write its handle
    /// into the game's output slot.
    pub fn native(&self) -> &Arc<N> {
        &self.native
    }

    /// The interpolation scratch texture (P4.8, K131). Nothing recorded into it is presented. The
    /// pointer stays valid for the life of the wrapper.
    pub fn interpolation_texture(&self) -> Result<D::Texture, SwapchainError> {
        let mut scratch = lock(&self.scratch);
        let Some(format) = scratch.format else { return Err(SwapchainError::NotFound) };
        if let Some(texture) = &scratch.texture {
            let info = texture.info();
            if info.width == scratch.width && info.height == scratch.height && info.format == format {
                return Ok(texture.clone());
            }
            let old = scratch.texture.take();
            if let Some(old) = old {
                scratch.retired.push(old);
            }
        }
        let desc = TextureDesc::texture2d(
            scratch.width,
            scratch.height,
            format,
            TextureFlags::ALLOW_UNORDERED_ACCESS,
        );
        let texture = self.device.create_texture(&desc).map_err(|_| SwapchainError::OutOfMemory)?;
        scratch.texture = Some(texture);
        Ok(scratch.texture.as_ref().expect("just stored").clone())
    }

    /// The interpolation command list (P4.8, K131). The list is created open the first time and
    /// reset for every later ask, because the previous recording was never executed; the pointer
    /// stays valid for the life of the wrapper.
    pub fn interpolation_list(&self) -> Result<D::CommandList, SwapchainError> {
        let mut scratch = lock(&self.scratch);
        if scratch.list.is_none() {
            let allocator = self
                .device
                .create_command_allocator(QueueKind::Direct)
                .map_err(|_| SwapchainError::OutOfMemory)?;
            let list = self
                .device
                .create_command_list(&allocator)
                .map_err(|_| SwapchainError::OutOfMemory)?;
            scratch.allocator = Some(allocator);
            scratch.list = Some(list);
            // The port's `create_command_list` hands back a closed list; a reset begins the
            // recording and leaves it open, which is what the C++ hands the game (K152).
            let new_list = scratch.list.as_ref().expect("just stored");
            let allocator = scratch.allocator.as_ref().expect("just stored");
            new_list.reset_list(allocator).map_err(|_| SwapchainError::OutOfMemory)?;
            return Ok(new_list.clone());
        }
        let allocator = scratch.allocator.as_ref().ok_or(SwapchainError::OutOfMemory)?;
        let list = scratch.list.as_ref().ok_or(SwapchainError::OutOfMemory)?;
        // The previous recording was never executed, so it is still open; a list must be closed
        // before its allocator can be reset. K168: without this close, the re-ask failed every
        // time (`allocator reset`) and the game was told the interpolation list was unavailable.
        let _ = list.close_list();
        allocator.reset().map_err(|_| SwapchainError::OutOfMemory)?;
        list.reset_list(allocator).map_err(|_| SwapchainError::OutOfMemory)?;
        Ok(list.clone())
    }

    /// True once the interpolation texture exists for the current size.
    pub fn has_interpolation_texture(&self) -> bool {
        lock(&self.scratch).texture.is_some()
    }

    /// The Present entry point. `params` `Some` means Present1 was called; an empty metadata set is
    /// itself a distinct call.
    pub fn present(&self, sync: u32, flags: PresentFlags, params: Option<&PresentParams>) -> Result<(), SwapchainError> {
        let entered = self.clock.now();
        if flags.contains(PresentFlags::TEST) {
            return self.call_native(sync, flags, params);
        }
        if let Some(code) = *lock(&self.submission_failure) {
            return Err(code_error(code));
        }
        if self.buffers.is_empty() {
            return Err(SwapchainError::InvalidCall);
        }
        if self.presenter.is_some() {
            return self.present_async(sync, flags, params, entered);
        }
        if flags.contains(PresentFlags::DO_NOT_WAIT) && self.fence.completed() < self.fence_value.load(Ordering::Acquire) {
            return Err(code_error(DXGI_ERROR_WAS_STILL_DRAWING));
        }
        self.drain()?;
        // The limiter caps the game's real frames here: exactly one call per application frame. A
        // DO_NOT_WAIT present must not block (K52), so it is skipped for such calls.
        if !flags.contains(PresentFlags::DO_NOT_WAIT) {
            if let Some(hook) = lock(&self.present_hook).clone() {
                hook(entered);
            }
        }
        let now = self.seconds();
        let out_sync = if self.force_sync { 1 } else { sync };
        let out_flags = if self.force_sync { flags.without(PresentFlags::ALLOW_TEARING) } else { flags };
        // Generate from THIS prepare before any native Present or sleep can let the application
        // recycle its inputs. The next outputs must be midpoint(N-1,N) and real N, not last
        // cycle's midpoint followed by real N.
        self.generated_ready.store(false, Ordering::Release);
        self.record_generated()?;
        let ready = self.generated_ready.load(Ordering::Acquire);
        let plan = { lock(&self.pacer).plan(now, ready) };
        let mut presented_generated = false;

        if plan.present_generated {
            let mut generated_state = ResourceState::COPY_SOURCE;
            if self.blit(&self.generated, &mut generated_state, true).is_ok() {
                let _ = self.signal_after_copy();
                self.generated_ready.store(false, Ordering::Release);
                if self.call_native(out_sync, out_flags, params).is_ok() {
                    lock(&self.overlay).presented(true);
                    lock(&self.counters).generated_presents += 1;
                    presented_generated = true;
                }
            } else {
                self.generated_ready.store(false, Ordering::Release);
                lock(&self.counters).skipped_generates += 1;
            }
            if presented_generated {
                let interval = self.pacer_interval();
                self.wait_until_seconds(plan.real_deadline, interval * 0.5);
            }
        }

        let index = self.current_back_buffer_index() as usize;
        let Some(real) = self.buffers.get(index) else { return Err(SwapchainError::InvalidCall) };
        let mut real_state = ResourceState::PRESENT;
        self.blit(real, &mut real_state, false)?;
        if self.align_vblank {
            let target = self.vblank_deadline(now);
            self.wait_until_seconds(target, 0.05);
        }
        let result = self.call_native(out_sync, out_flags, params);
        if result.is_ok() {
            lock(&self.overlay).presented(false);
        }
        // Retires our copy AND any work DXGI queues for Present; a fence before Present is
        // insufficient for releasing native buffers on resize.
        if self.signal_after_copy().is_err() {
            self.set_submission_failure(E_FAIL);
            return Err(SwapchainError::SubmissionFailure);
        }
        self.resolve_untracked()?;
        if result.is_ok() {
            lock(&self.counters).present_cycles += 1;
            self.generated_ready.store(false, Ordering::Release);
            let mut index = lock(&self.game_index);
            *index = (*index + 1) % self.buffers.len() as u32;
        }
        result
    }

    fn call_native(&self, sync: u32, flags: PresentFlags, params: Option<&PresentParams>) -> Result<(), SwapchainError> {
        match params {
            Some(params) => self.native.present1(sync, flags, params),
            None => self.native.present(sync, flags),
        }
        .map_err(|error| code_error(error.code()))
    }

    /// Records a submission failure. Only the first one is kept: it is the one the game sees.
    fn set_submission_failure(&self, code: u32) {
        let mut failure = lock(&self.submission_failure);
        if failure.is_none() {
            *failure = Some(code);
        }
    }

    /// Records the midpoint into `generated` from the current prepare.
    fn record_generated(&self) -> Result<(), SwapchainError> {
        let context = match lock(&self.sink).as_ref() {
            Some(sink) => Arc::clone(&sink.context),
            None => return Ok(()),
        };
        if self.generated_ready.load(Ordering::Acquire) {
            return Ok(());
        }
        let index = self.current_back_buffer_index() as usize;
        let Some(real) = self.buffers.get(index) else { return Ok(()) };
        self.generate_allocator.reset().map_err(|_| SwapchainError::SubmissionFailure)?;
        self.generate_list
            .reset_list(&self.generate_allocator)
            .map_err(|_| SwapchainError::SubmissionFailure)?;
        context.note_ordered_submission();
        let phases = [GeneratePhase {
            output: &self.generated,
            output_state: context.output_state(),
            alpha: MIDPOINT_ALPHA,
        }];
        let recorded = context.generate_phases(&self.generate_list, real, &phases, false);
        // Even a refusal may leave a partially recorded list; a Close failure is a submission-path
        // error, not an ordinary skipped frame.
        if self.generate_list.close_list().is_err() {
            self.hold_untracked_generator();
            self.set_submission_failure(E_FAIL);
            return Err(SwapchainError::SubmissionFailure);
        }
        if !recorded {
            self.hold_untracked_generator();
            lock(&self.counters).skipped_generates += 1;
            return Ok(());
        }
        if self.queue.execute(&[&self.generate_list]).is_err() {
            self.hold_untracked_generator();
            self.set_submission_failure(E_FAIL);
            return Err(SwapchainError::SubmissionFailure);
        }
        let value = self.fence_value.fetch_add(1, Ordering::AcqRel) + 1;
        if self.queue.signal(self.fence.as_ref(), value).is_err() {
            self.hold_untracked_generator();
            lock(&self.counters).skipped_generates += 1;
            self.set_submission_failure(E_FAIL);
            return Err(SwapchainError::SubmissionFailure);
        }
        if !context.track_submission(value) {
            self.hold_untracked_generator();
            self.set_submission_failure(E_FAIL);
            return Err(SwapchainError::SubmissionFailure);
        }
        self.generated_ready.store(true, Ordering::Release);
        Ok(())
    }

    /// Keeps an owner whose submission was not registered, with its lease, until a queue fence is
    /// accepted (K103). Admission is serialized, so an unresolved owner is never replaced.
    fn hold_untracked_generator(&self) {
        // Admission is serialized by the wrapper, so an unresolved owner is never replaced.
        if lock(&self.untracked).is_some() {
            return;
        }
        let held = {
            let sink = lock(&self.sink);
            let Some(sink) = sink.as_ref() else { return };
            if sink.lease.is_none() || !sink.context.has_untracked_work() {
                return;
            }
            GeneratorSink {
                context: Arc::clone(&sink.context),
                owner: sink.owner,
                lease: sink.lease.clone(),
            }
        };
        held.context.abandon_generation();
        *lock(&self.untracked) = Some(held);
    }

    /// Registers the held owner's retirement once a queue fence covers it. An unaccepted token
    /// keeps the lease.
    fn resolve_untracked(&self) -> Result<(), SwapchainError> {
        let mut untracked = lock(&self.untracked);
        let Some(sink) = untracked.as_ref() else { return Ok(()) };
        let value = self.fence_value.load(Ordering::Acquire);
        if !sink.context.track_submission(value) {
            return Err(SwapchainError::SubmissionFailure);
        }
        *untracked = None;
        Ok(())
    }

    /// The "vrr" Present: generate on the game thread, hand both outputs to the presenter.
    fn present_async(
        &self,
        sync: u32,
        flags: PresentFlags,
        params: Option<&PresentParams>,
        entered: u64,
    ) -> Result<(), SwapchainError> {
        let Some(presenter) = self.presenter.as_ref() else { return Err(SwapchainError::InvalidCall) };
        if flags.contains(PresentFlags::DO_NOT_WAIT) && presenter.outstanding() > IN_FLIGHT {
            return Err(code_error(DXGI_ERROR_WAS_STILL_DRAWING));
        }
        if !presenter.wait_outstanding(IN_FLIGHT, Duration::from_millis(5000)) {
            return Err(SwapchainError::DeviceHung);
        }
        // Presentation is asynchronous, so the game sees the outcome one frame later.
        presenter.last_result().map_err(code_error)?;
        // Own the variable Present1 metadata before advancing the schedule or recording GPU work:
        // allocation failure is a game-visible error (the owner-approved Present1 exception), but
        // it must not leave partial GPU or history ownership behind (K76).
        let owned = params.map(|params| PresentParams {
            dirty_rects: params.dirty_rects.clone(),
            scroll_rect: params.scroll_rect,
            scroll_offset: params.scroll_offset,
        });
        if !flags.contains(PresentFlags::DO_NOT_WAIT) {
            if let Some(hook) = lock(&self.present_hook).clone() {
                hook(entered);
            }
        }
        let now = self.seconds();
        let slot = self.acquire_vrr_slot()?;
        // Planning follows admission so a failed slot wait cannot consume a grid point. `now`
        // remains anchored before the wait.
        let (schedule_before, planned) = {
            let mut schedule = lock(&self.schedule);
            let before = schedule.clone();
            let planned = schedule.plan(now);
            (before, planned)
        };
        // Generate every planned phase in one submission. The generator advances its history once
        // per real frame, so a frame with nothing generated to show still runs one phase.
        let (generated, phases) = self.record_vrr_phases(&planned, slot)?;
        let plan = if generated { planned } else { OutputPlan::real_only(now) };
        if !generated && planned.generated() > 0 && lock(&self.sink).is_some() {
            // Nothing usable was generated: the real frame is shown on its own.
            lock(&self.counters).skipped_generates += 1;
        }
        // The presenter reads the snapshot, so the game's own buffer is free once this copy has run;
        // queue order guarantees it before the buffer's next use.
        self.snapshot_real(slot)?;
        // Everything the game and the generator queued for this frame.
        if self.signal_after_copy().is_err() {
            self.set_submission_failure(E_FAIL);
            return Err(SwapchainError::SubmissionFailure);
        }
        self.resolve_untracked()?;
        let ready_value = self.fence_value.load(Ordering::Acquire);
        // The job takes this slot's images; the slot is not reused until its fence retires.
        let snapshot = {
            let mut slots = lock(&self.vrr_slots);
            match slots.get_mut(slot as usize) {
                Some(entry) => {
                    entry.snapshot_fence = ready_value;
                    entry.snapshot.clone()
                }
                None => return Err(SwapchainError::InvalidCall),
            }
        };
        let mut job = PresentJob::new(
            snapshot,
            Arc::clone(&self.fence),
            ready_value,
            sync,
            flags.without(PresentFlags::DO_NOT_WAIT),
            owned,
        );
        job.entry = now;
        let mut phase_index = 0usize;
        for output in plan.outputs() {
            if output.real {
                job.push(crate::application::presentation::presenter::JobOutput::real(output.content));
            } else {
                let image = phases[phase_index.min(phases.len() - 1)].clone();
                phase_index += 1;
                job.push(crate::application::presentation::presenter::JobOutput::generated(image, output.content));
            }
        }

        if job.count == 0 {
            job.push(crate::application::presentation::presenter::JobOutput::real(now));
        }
        if let Err(error) = presenter.submit(job) {
            // Do not advance ownership for an unqueued job (K51); the planned content time is
            // returned so a retry does not skip a slot (K105).
            *lock(&self.schedule) = schedule_before;
            return Err(code_error(error.code()));
        }
        {
            let mut rate = lock(&self.rate_window);
            if rate.0 == 0.0 {
                rate.0 = now;
            }
            rate.1 += plan.count() as u32;
            rate.2 += plan.generated() as u32;
        }
        let mut index = lock(&self.game_index);
        *index = (*index + 1) % self.buffers.len() as u32;
        Ok(())
    }

    fn acquire_vrr_slot(&self) -> Result<u32, SwapchainError> {
        let mut next = lock(&self.vrr_next_slot);
        let result = acquire_frame_slot(&mut next, SLOTS as u32, |candidate| {
            let target = lock(&self.vrr_slots).get(candidate as usize).map(|slot| slot.snapshot_fence).unwrap_or(0);
            match classify_fence(self.fence.completed(), target) {
                FenceProgress::Complete => Ok(true),
                FenceProgress::DeviceRemoved => Err(crate::domain::gpu_policy::GpuFailure(DXGI_ERROR_DEVICE_REMOVED)),
                FenceProgress::Pending => Ok(false),
            }
        });
        match result {
            Ok(slot) => Ok(slot),
            Err(failure) if failure.0 == DXGI_ERROR_DEVICE_REMOVED => Err(SwapchainError::DeviceHung),
            Err(failure) => Err(code_error(failure.0)),
        }
    }

    /// Records every planned phase in one submission. The generator advances history once per
    /// real frame, so a frame with nothing to show still runs one phase. Returns the images it
    /// recorded into, in order.
    fn record_vrr_phases(
        &self,
        plan: &OutputPlan,
        slot: u32,
    ) -> Result<(bool, Vec<D::Texture>), SwapchainError> {
        let context = match lock(&self.sink).as_ref() {
            Some(sink) => Arc::clone(&sink.context),
            None => return Ok((false, Vec::new())),
        };
        let index = self.current_back_buffer_index() as usize;
        let Some(real) = self.buffers.get(index) else { return Ok((false, Vec::new())) };
        let alphas: Vec<f32> = plan.outputs().iter().filter(|output| !output.real).map(|output| output.alpha).collect();
        let images: Vec<D::Texture> = {
            let slots = lock(&self.vrr_slots);
            let Some(entry) = slots.get(slot as usize) else { return Err(SwapchainError::InvalidCall) };
            let wanted = alphas.len().min(PHASE_OUTPUTS).max(1);
            entry.phase_outputs.iter().take(wanted).cloned().collect()
        };
        let phases: Vec<GeneratePhase<'_, D::Texture>> = images
            .iter()
            .enumerate()
            .map(|(position, image)| GeneratePhase {
                output: image,
                output_state: ResourceState::COPY_SOURCE,
                alpha: alphas.get(position).copied().unwrap_or(MIDPOINT_ALPHA),
            })
            .collect();
        if self.generate_allocator.reset().is_err() {
            self.set_submission_failure(E_FAIL);
            return Err(SwapchainError::SubmissionFailure);
        }
        if self.generate_list.reset_list(&self.generate_allocator).is_err() {
            self.set_submission_failure(E_FAIL);
            return Err(SwapchainError::SubmissionFailure);
        }
        context.note_ordered_submission();
        let recorded = context.generate_phases(&self.generate_list, real, &phases, false);
        if self.generate_list.close_list().is_err() {
            self.hold_untracked_generator();
            self.set_submission_failure(E_FAIL);
            return Err(SwapchainError::SubmissionFailure);
        }
        if !recorded {
            self.hold_untracked_generator();
            return Ok((false, Vec::new()));
        }
        if self.queue.execute(&[&self.generate_list]).is_err() {
            self.hold_untracked_generator();
            self.set_submission_failure(E_FAIL);
            return Err(SwapchainError::SubmissionFailure);
        }
        let value = self.signal_after_copy()?;
        if !context.track_submission(value) {
            self.hold_untracked_generator();
            self.set_submission_failure(E_FAIL);
            return Err(SwapchainError::SubmissionFailure);
        }
        Ok((true, images))
    }

    /// Copies the game's back buffer into `slot`'s snapshot on the game queue, so the game's own
    /// buffer is free once this copy has run (queue order guarantees it before its next use).
    fn snapshot_real(&self, slot: u32) -> Result<(), SwapchainError> {
        let index = self.current_back_buffer_index() as usize;
        let Some(source) = self.buffers.get(index) else { return Ok(()) };
        self.generate_allocator.reset().map_err(|_| SwapchainError::SubmissionFailure)?;
        self.generate_list
            .reset_list(&self.generate_allocator)
            .map_err(|_| SwapchainError::SubmissionFailure)?;
        {
            let slots = lock(&self.vrr_slots);
            let Some(entry) = slots.get(slot as usize) else { return Err(SwapchainError::InvalidCall) };
            self.generate_list.transition(source, ResourceState::PRESENT, ResourceState::COPY_SOURCE);
            self.generate_list.transition(&entry.snapshot, ResourceState::PRESENT, ResourceState::COPY_DEST);
            self.generate_list.copy_resource(&entry.snapshot, source);
            self.generate_list.transition(source, ResourceState::COPY_SOURCE, ResourceState::PRESENT);
            self.generate_list.transition(&entry.snapshot, ResourceState::COPY_DEST, ResourceState::PRESENT);
        }
        if self.generate_list.close_list().is_err() {
            self.set_submission_failure(E_FAIL);
            return Err(SwapchainError::SubmissionFailure);
        }
        if self.queue.execute(&[&self.generate_list]).is_err() {
            self.set_submission_failure(E_FAIL);
            return Err(SwapchainError::SubmissionFailure);
        }
        Ok(())
    }

    /// Copies `source` into the current native back buffer, restoring both states, and draws the
    /// overlay on the final image. The copy is executed before this returns.
    fn blit(&self, source: &D::Texture, source_state: &mut ResourceState, generated: bool) -> Result<(), SwapchainError> {
        let destination =
            self.native.back_buffer(self.native.current_back_buffer_index()).map_err(|error| code_error(error.code()))?;
        // Alternating allocators: both copies of a cycle execute before either can be reset.
        let slot = (self.next_list.fetch_add(1, Ordering::AcqRel) & 1) as usize;
        self.copy_allocators[slot].reset().map_err(|_| SwapchainError::SubmissionFailure)?;
        let list = &self.copy_lists[slot];
        list.reset_list(&self.copy_allocators[slot]).map_err(|_| SwapchainError::SubmissionFailure)?;
        let restore = *source_state;
        list.transition(source, restore, ResourceState::COPY_SOURCE);
        list.transition(&destination, ResourceState::PRESENT, ResourceState::COPY_DEST);
        list.copy_resource(&destination, source);
        list.transition(&destination, ResourceState::COPY_DEST, ResourceState::PRESENT);
        list.transition(source, ResourceState::COPY_SOURCE, restore);
        // The overlay draws on the final image, as it does from AMD's callback.
        lock(&self.overlay).draw(list, &destination, generated);
        if list.close_list().is_err() {
            return Err(SwapchainError::SubmissionFailure);
        }
        self.queue.execute(&[list]).map_err(|_| SwapchainError::SubmissionFailure)?;
        Ok(())
    }

    fn signal_after_copy(&self) -> Result<u64, SwapchainError> {
        let value = self.fence_value.fetch_add(1, Ordering::AcqRel) + 1;
        self.queue.signal(self.fence.as_ref(), value).map_err(|_| SwapchainError::SubmissionFailure)?;
        Ok(value)
    }

    /// The bounded teardown drain. A removed or hung device must not trap teardown forever.
    pub fn drain(&self) -> Result<(), SwapchainError> {
        if let Some(presenter) = &self.presenter {
            if !presenter.wait_outstanding(0, Duration::from_millis(5000)) {
                return Err(SwapchainError::DeviceHung);
            }
            presenter.drain_gpu().map_err(|error| code_error(error.code()))?;
        }
        if let Some(recovery) = *lock(&self.submission_failure) {
            // A failed signal left submitted work without a retirement token. A later queue-signalled
            // fence can cover it; admission stays closed even if this bounded attempt succeeds.
            let value = self.fence_value.fetch_add(1, Ordering::AcqRel) + 1;
            if self.queue.signal(self.fence.as_ref(), value).is_err() {
                return Err(code_error(recovery));
            }
            self.resolve_untracked()?;
        }
        let target = self.fence_value.load(Ordering::Acquire);
        if target == 0 {
            return Ok(());
        }
        match classify_fence(self.fence.completed(), target) {
            FenceProgress::DeviceRemoved => {
                return Err(code_error(crate::domain::gpu_policy::removed_device_error(
                    self.device.removed_reason().unwrap_or(DXGI_ERROR_DEVICE_REMOVED),
                )))
            }
            FenceProgress::Complete => return Ok(()),
            FenceProgress::Pending => {}
        }
        match self.fence.wait(target, Duration::from_millis(5000)) {
            Ok(()) => Ok(()),
            Err(GpuError::Timeout) => match self.device.removed_reason() {
                Some(reason) => Err(code_error(crate::domain::gpu_policy::removed_device_error(reason))),
                None => Err(SwapchainError::DeviceHung),
            },
            Err(error) => Err(code_error(error.code())),
        }
    }

    /// True when no GPU work or presenter job can still touch this object. Never blocks.
    pub fn retirable(&self) -> bool {
        if self.device.removed_reason().is_some() {
            return true; // nothing can run any more
        }
        if lock(&self.submission_failure).is_some() {
            return false; // untracked work: no token covers it
        }
        if let Some(presenter) = &self.presenter {
            if presenter.outstanding() != 0 {
                return false;
            }
        }
        self.fence.completed() >= self.fence_value.load(Ordering::Acquire)
    }

    /// Resize: refuses while the application owns a back buffer, drains, resizes the chain and
    /// recreates the private buffers.
    pub fn resize(
        &self,
        count: u32,
        width: u32,
        height: u32,
        format: Option<PixelFormat>,
        flags: u32,
    ) -> Result<(), SwapchainError> {
        if let Some(format) = format {
            if !matches!(format, PixelFormat::R8G8B8A8Unorm | PixelFormat::R10G10B10A2Unorm) {
                return Err(SwapchainError::Unsupported);
            }
        }
        if let Some(presenter) = &self.presenter {
            // A job still on the presenter holds its own buffer reference; let it finish so only the
            // application's references are counted below.
            if !presenter.wait_outstanding(0, Duration::from_millis(5000)) {
                return Err(SwapchainError::DeviceHung);
            }
        }
        // Resize must fail while the application owns a back buffer reference, as native DXGI does.
        for index in 0..self.buffers.len() as u32 {
            if self.native.buffer_reference_count(index).is_some_and(|count| count > 1) {
                return Err(SwapchainError::BufferReferenced);
            }
        }
        self.drain()?;
        let current = self.native.desc().map_err(|_| SwapchainError::InvalidCall)?;
        let chain_format = format.unwrap_or(current.format);
        match &self.presenter {
            Some(presenter) => presenter.resize_native(count, width, height, chain_format, flags).map_err(|error| code_error(error.code()))?,
            None => self
                .native
                .resize_buffers(count, width, height, chain_format, flags)
                .map_err(|error| code_error(error.code()))?,
        }
        self.generated_ready.store(false, Ordering::Release);
        lock(&self.pacer).reset();
        let desc = self.native.desc().map_err(|_| SwapchainError::InvalidCall)?;
        {
            let mut scratch = lock(&self.scratch);
            if let Some(texture) = scratch.texture.take() {
                scratch.retired.push(texture);
            }
            // The list and its allocator follow the chain but keep their pointers (K131).
            scratch.allocator = None;
            scratch.list = None;
            scratch.format = Some(desc.format);
            scratch.width = desc.width;
            scratch.height = desc.height;
        }
        Ok(())
    }

    /// `ResizeBuffers1`: one node and one direct queue only. The chain stays on Sedici's present
    /// queue, so every buffer must be named for node 0.
    pub fn resize_buffers1(
        &self,
        count: u32,
        width: u32,
        height: u32,
        format: Option<PixelFormat>,
        flags: u32,
        node_masks: &[u32],
    ) -> Result<(), SwapchainError> {
        let buffers = if count == 0 { self.buffers.len() as u32 } else { count };
        if node_masks.len() < buffers as usize {
            return Err(SwapchainError::InvalidArgument);
        }
        if node_masks.iter().take(buffers as usize).any(|mask| *mask > 1) {
            return Err(SwapchainError::ForeignPresentQueue);
        }
        self.resize(count, width, height, format, flags)
    }

    fn configure_schedule(&mut self, choice: PresentChoice) {
        if self.presenter.is_none() {
            if choice.multiplier != Multiplier::X2 {
                tracing::error!(
                    "fg: multiplier \"{}\" needs present = \"vrr\"; presenting 2x",
                    choice.multiplier.name()
                );
            }
            return;
        }
        let mut schedule = lock(&self.schedule);
        match choice.multiplier {
            Multiplier::X1_75 => schedule.set_ratio(7, 4),
            Multiplier::X1_5 => schedule.set_ratio(3, 2),
            Multiplier::X1_25 => schedule.set_ratio(5, 4),
            Multiplier::Adaptive => {
                let target = self.adaptive_target(choice);
                schedule.set_target(target);
            }
            Multiplier::X2 => schedule.set_ratio(2, 1),
        }
    }

    /// Adaptive needs a rate to aim for: the one target, or auto when it is uncapped. Without a
    /// known refresh rate keep the old safe fallback.
    pub fn adaptive_target(&self, choice: PresentChoice) -> f64 {
        let hz = self.display_refresh_hz();
        let target = if choice.target_fps > 0.0 { choice.target_fps } else { auto_target_fps(hz) };
        if target > 0.0 {
            target
        } else {
            (hz - 3.0).max(1.0)
        }
    }

    pub fn display_refresh_hz(&self) -> f64 {
        let mut hz = self.native.containing_output().map(|output| output.refresh_hz).unwrap_or(0.0);
        if hz <= 0.0 {
            if let Some(presenter) = &self.presenter {
                hz = presenter.refresh_hz();
            }
        }
        hz
    }

    /// The output ratio and refresh rate the settings layer needs for the one target fps.
    pub fn output_context(&self) -> (f64, f64) {
        let schedule = lock(&self.schedule);
        let ratio = if self.presenter.is_none() {
            2.0
        } else if schedule.adaptive() {
            1.0
        } else {
            f64::from(schedule.numerator()) / f64::from(schedule.denominator().max(1))
        };
        drop(schedule);
        (ratio, self.display_refresh_hz())
    }

    fn pacer_interval(&self) -> f64 {
        lock(&self.pacer).interval()
    }

    /// The interval between two outputs, for the pacing wait. 1/60 before the first cycle.
    pub fn pacing_interval(&self) -> f64 {
        lock(&self.pacer).interval()
    }

    fn seconds(&self) -> f64 {
        let frequency = self.clock.frequency();
        if frequency == 0 {
            0.0
        } else {
            self.clock.now() as f64 / frequency as f64
        }
    }

    fn wait_until_seconds(&self, deadline: f64, max_wait: f64) {
        let now = self.seconds();
        if !deadline.is_finite() || !(max_wait > 0.0) {
            return;
        }
        let remaining = (deadline - now).clamp(0.0, max_wait.min(0.05));
        if !(remaining > 0.0) {
            return;
        }
        let frequency = self.clock.frequency();
        if frequency == 0 {
            return;
        }
        let target = self.clock.now().saturating_add((remaining * frequency as f64) as u64);
        let _ = self.clock.wait_until(target);
    }

    /// The absolute deadline for arming a flip just before the next vertical blank (opt-in,
    /// `SEDICI_FG_ALIGN=1`). Returns `now` when frame statistics are unavailable.
    fn vblank_deadline(&self, now: f64) -> f64 {
        let Some(stats) = self.native.frame_statistics() else { return now };
        let frequency = self.clock.frequency();
        if frequency == 0 || stats.sync_qpc_ticks <= 0 {
            return now;
        }
        let epoch = stats.sync_qpc_ticks as f64 / frequency as f64;
        // Arm shortly before the next blank so the flip lands at the scan start.
        const MARGIN: f64 = 0.0012;
        let lead = now + MARGIN;
        let period = 1.0 / 75.0;
        let target = if lead >= epoch { epoch + ((lead - epoch) / period).ceil() * period } else { epoch };
        target - MARGIN
    }
}

/// `SEDICI_FG_SCANLINE_LEAD_US` in microseconds; `None` uses the 0.00025 s default.
pub fn scanline_lead_us() -> Option<f64> {
    let text = std::env::var("SEDICI_FG_SCANLINE_LEAD_US").ok()?;
    let value: f64 = text.trim().parse().ok()?;
    (value >= 0.0 && value <= 5000.0).then_some(value)
}

fn create_back_buffers<D: GpuDevice>(device: &D, desc: &SwapChainDesc) -> Result<Vec<D::Texture>, GpuError> {
    // The game renders into these, so they need render-target access (D3D12's
    // D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET is 0x1). The port's `TextureFlags` has no named
    // constant for it (K152), so the raw bit is spelled here instead of widening another slice.
    let texture = TextureDesc::texture2d(desc.width, desc.height, desc.format, TextureFlags(0x1));
    (0..desc.buffer_count).map(|_| device.create_texture(&texture)).collect()
}

fn create_generated<D: GpuDevice>(device: &D, desc: &SwapChainDesc) -> Result<D::Texture, GpuError> {
    // Written by a compute pass and copied to a native back buffer, so UAV access and no RT flag.
    let texture = TextureDesc::texture2d(desc.width, desc.height, desc.format, TextureFlags::ALLOW_UNORDERED_ACCESS);
    device.create_texture(&texture)
}

fn code_error(code: u32) -> SwapchainError {
    match code {
        DXGI_ERROR_WAS_STILL_DRAWING | DXGI_ERROR_DEVICE_HUNG => SwapchainError::DeviceHung,
        DXGI_ERROR_DEVICE_REMOVED => SwapchainError::DeviceRemoved,
        E_OUTOFMEMORY => SwapchainError::OutOfMemory,
        E_INVALIDARG => SwapchainError::InvalidArgument,
        DXGI_ERROR_NOT_FOUND => SwapchainError::NotFound,
        _ => SwapchainError::SubmissionFailure,
    }
}
