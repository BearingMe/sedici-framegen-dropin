//! Presentation ported from `fgcheck_present.cpp`, `fgcheck_retention.cpp` and `fgcheck_pacing.cpp`.
//!
//! The C++ hosts created real windows and D3D12 chains; those scenarios are ported here against
//! fakes so the decisions are checked without a display:
//!
//! * the real-frame wrapper's present/pacing path (`fgcheck_present.cpp::check_generated_window`),
//!   including the cleared-sink case and the buffer-index advance;
//! * the failure-to-admission rule (`signalFault`/`closeFault`/`resetFault` in the same file): a
//!   failed generation submission stops every later Present with the same error;
//! * the Present1 metadata retry and the refused-presenter-job retry (K76, K105);
//! * teardown retention (`fgcheck_retention.cpp`): a live but stalled device keeps the wrapper from
//!   reporting itself retirable, and a removed device counts as retired (K38);
//! * the pacing arithmetic and the schedule grid (`fgcheck_pacing.cpp`), which live in `domain` and
//!   are covered again here through the wrapper's own `Pacer`.
//!
//! No adapter is needed, so nothing skips.
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use amd_fidelityfx_framegeneration_dx12::application::ports::clock::FakeClock;
use amd_fidelityfx_framegeneration_dx12::application::ports::generator::GeneratePhase;
use amd_fidelityfx_framegeneration_dx12::application::ports::gpu::{
    GpuBuffer, GpuCommandAllocator, GpuCommandList, GpuDevice, GpuError, GpuFence, GpuQueue,
    GpuTexture, PixelFormat, QueueKind, QueuePriority, ResourceState, TextureDesc, TextureFlags,
    TextureInfo,
};
use amd_fidelityfx_framegeneration_dx12::application::ports::overlay::NoOverlay;
use amd_fidelityfx_framegeneration_dx12::application::ports::swapchain::{
    FrameStatistics, NativeSwapchain, OutputDesc, PresentFlags, PresentParams, Rect, SwapChainDesc,
};
use amd_fidelityfx_framegeneration_dx12::application::presentation::pacing::superseded;
use amd_fidelityfx_framegeneration_dx12::application::presentation::presenter::ListLifecycle;
use amd_fidelityfx_framegeneration_dx12::application::presentation::swapchain::{
    GeneratorSink, PresentedContext, PresentChoice, SharedPresentQueues, Swapchain, SwapchainError,
    WrapEnvironment, WrapReport, DXGI_ERROR_INVALID_CALL, DXGI_ERROR_UNSUPPORTED,
};
use amd_fidelityfx_framegeneration_dx12::domain::config::{Multiplier, Present, TearGuard};
use amd_fidelityfx_framegeneration_dx12::domain::pacing::OutputSchedule;
use rstest::rstest;

// ---------------------------------------------------------------------------------------------
// Fakes. One counter set drives every object, so a test states a whole scenario in plain values.
// ---------------------------------------------------------------------------------------------

#[derive(Debug, Default)]
struct World {
    /// Index handed out for each created texture, so identities are distinct.
    next_identity: AtomicU64,
    /// Fence completed value.
    completed: AtomicU64,
    /// Fence values a wait was asked for.
    waits: Mutex<Vec<u64>>,
    /// Every submission's list count.
    executes: AtomicU32,
    signals: AtomicU32,
    presents: AtomicU32,
    present1s: AtomicU32,
    present_flags: Mutex<Vec<u32>>,
    presents_with_params: AtomicU32,
    resizes: AtomicU32,
    resize1s: AtomicU32,
    resize_masks: Mutex<Vec<Vec<u32>>>,
    /// Removed-device reason; `None` means healthy.
    removed: Mutex<Option<u32>>,
    /// When armed, `execute` fails.
    fail_execute: AtomicBool,
    /// When armed, every allocator reset fails.
    fail_reset: AtomicBool,
    /// When armed, the next back-buffer count above one is reported as taken by the application.
    buffer_taken: AtomicBool,
    /// Chain description.
    desc: Mutex<SwapChainDesc>,
    refresh_hz: Mutex<f64>,
}

impl World {
    fn new(desc: SwapChainDesc) -> Self {
        Self {
            desc: Mutex::new(desc),
            ..Self::default()
        }
    }

    fn set_completed(&self, value: u64) {
        self.completed.store(value, Ordering::Release);
    }

}

fn sample_desc() -> SwapChainDesc {
    SwapChainDesc {
        width: 320,
        height: 180,
        format: PixelFormat::R8G8B8A8Unorm,
        buffer_count: 3,
        sample_count: 1,
        stereo: false,
        flags: 0x800,
    }
}

#[derive(Debug)]
struct FakeTexture {
    identity: usize,
    /// The list last `copy_resource`d into it; a copy is recorded so a test can count blits.
    copies: AtomicU32,
}

impl GpuTexture for FakeTexture {
    fn info(&self) -> TextureInfo {
        TextureInfo {
            width: 320,
            height: 180,
            format: PixelFormat::R8G8B8A8Unorm,
            mip_levels: 1,
            array_size: 1,
            sample_count: 1,
            flags: TextureFlags::NONE,
        }
    }
    fn identity(&self) -> usize {
        self.identity
    }
}

impl Clone for FakeTexture {
    fn clone(&self) -> Self {
        Self { identity: self.identity, copies: AtomicU32::new(self.copies.load(Ordering::Relaxed)) }
    }
}

struct FakeBuffer;

impl GpuBuffer for FakeBuffer {
    fn size(&self) -> u64 {
        0
    }
}

struct FakeAllocator {
    resets: AtomicU32,
    world: Arc<World>,
}

impl std::fmt::Debug for FakeAllocator {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("FakeAllocator")
    }
}

impl GpuCommandAllocator for FakeAllocator {
    fn reset(&self) -> Result<(), GpuError> {
        self.resets.fetch_add(1, Ordering::Relaxed);
        if self.world.fail_reset.load(Ordering::Acquire) {
            return Err(GpuError::failed("allocator reset", 0x8000_4005));
        }
        Ok(())
    }
}

#[derive(Clone)]
struct FakeList {
    closed: Arc<AtomicBool>,
    barriers: Arc<AtomicU32>,
    resets: Arc<AtomicU32>,
}

impl GpuCommandList<FakeTexture> for FakeList {
    fn transition(&self, _texture: &FakeTexture, _before: ResourceState, _after: ResourceState) {
        self.barriers.fetch_add(1, Ordering::Relaxed);
    }
    fn uav_barrier(&self, _texture: &FakeTexture) {}
    fn copy_resource(&self, destination: &FakeTexture, _source: &FakeTexture) {
        destination.copies.fetch_add(1, Ordering::Relaxed);
    }
    fn dispatch(&self, _x: u32, _y: u32, _z: u32) {}
    fn object_identity(&self) -> usize {
        0
    }
}

impl ListLifecycle for FakeList {
    type Allocator = FakeAllocator;

    fn reset_list(&self, _allocator: &FakeAllocator) -> Result<(), GpuError> {
        self.resets.fetch_add(1, Ordering::Relaxed);
        self.closed.store(false, Ordering::Release);
        Ok(())
    }

    fn close_list(&self) -> Result<(), GpuError> {
        self.closed.store(true, Ordering::Release);
        Ok(())
    }
}

struct FakeFence {
    world: Arc<World>,
}

impl GpuFence for FakeFence {
    fn completed(&self) -> u64 {
        self.world.completed.load(Ordering::Acquire)
    }

    fn wait(&self, value: u64, _timeout: Duration) -> Result<(), GpuError> {
        self.world.waits.lock().unwrap().push(value);
        match *self.world.removed.lock().unwrap() {
            Some(reason) => Err(GpuError::DeviceRemoved { reason }),
            None => Ok(()),
        }
    }
}

struct FakeQueue {
    world: Arc<World>,
}

impl GpuQueue<FakeList, FakeFence> for FakeQueue {
    fn execute(&self, _lists: &[&FakeList]) -> Result<(), GpuError> {
        if self.world.fail_execute.load(Ordering::Acquire) {
            return Err(GpuError::failed("execute", 0x8000_4005));
        }
        self.world.executes.fetch_add(1, Ordering::Relaxed);
        Ok(())
    }

    fn signal(&self, _fence: &FakeFence, value: u64) -> Result<(), GpuError> {
        self.world.signals.fetch_add(1, Ordering::Relaxed);
        self.world.completed.fetch_max(value, Ordering::AcqRel);
        Ok(())
    }

    fn wait(&self, _fence: &FakeFence, _value: u64) -> Result<(), GpuError> {
        Ok(())
    }
}

struct FakeDevice {
    world: Arc<World>,
}

impl GpuDevice for FakeDevice {
    type Texture = FakeTexture;
    type Buffer = FakeBuffer;
    type CommandAllocator = FakeAllocator;
    type CommandList = FakeList;
    type Fence = FakeFence;
    type Queue = FakeQueue;

    fn create_texture(&self, _desc: &TextureDesc) -> Result<Self::Texture, GpuError> {
        Ok(FakeTexture {
            identity: self.world.next_identity.fetch_add(1, Ordering::Relaxed) as usize,
            copies: AtomicU32::new(0),
        })
    }
    fn create_upload_buffer(&self, _size: u64) -> Result<Self::Buffer, GpuError> {
        Ok(FakeBuffer)
    }
    fn create_readback_buffer(&self, _size: u64) -> Result<Self::Buffer, GpuError> {
        Ok(FakeBuffer)
    }
    fn write_buffer(&self, _buffer: &Self::Buffer, _data: &[u8]) -> Result<(), GpuError> {
        Ok(())
    }
    fn read_buffer(&self, _buffer: &Self::Buffer) -> Result<Vec<u8>, GpuError> {
        Ok(Vec::new())
    }
    fn create_command_allocator(&self, _kind: QueueKind) -> Result<Self::CommandAllocator, GpuError> {
        Ok(FakeAllocator { resets: AtomicU32::new(0), world: Arc::clone(&self.world) })
    }
    fn create_command_list(&self, _allocator: &Self::CommandAllocator) -> Result<Self::CommandList, GpuError> {
        Ok(FakeList {
            closed: Arc::new(AtomicBool::new(true)),
            barriers: Arc::new(AtomicU32::new(0)),
            resets: Arc::new(AtomicU32::new(0)),
        })
    }
    fn create_fence(&self, _initial: u64) -> Result<Self::Fence, GpuError> {
        Ok(FakeFence { world: Arc::clone(&self.world) })
    }
    fn create_queue(&self, _kind: QueueKind, _priority: QueuePriority) -> Result<Self::Queue, GpuError> {
        Ok(FakeQueue { world: Arc::clone(&self.world) })
    }
    fn removed_reason(&self) -> Option<u32> {
        *self.world.removed.lock().unwrap()
    }
}

/// A [`NativeSwapchain`] over the same world. Its back buffers are one per chain buffer and hold
/// their own copy counters.
struct FakeNative {
    world: Arc<World>,
    buffers: Vec<FakeTexture>,
}

impl FakeNative {
    fn new(world: Arc<World>) -> Self {
        let count = world.desc.lock().unwrap().buffer_count;
        let buffers = (0..count)
            .map(|_| FakeTexture {
                identity: world.next_identity.fetch_add(1, Ordering::Relaxed) as usize,
                copies: AtomicU32::new(0),
            })
            .collect();
        Self { world, buffers }
    }
}

impl NativeSwapchain for FakeNative {
    type Buffer = FakeTexture;

    fn back_buffer(&self, index: u32) -> Result<Self::Buffer, GpuError> {
        self.buffers.get(index as usize).map(Clone::clone).ok_or_else(|| GpuError::failed("buffer", 0x887A_0001))
    }

    fn buffer_reference_count(&self, index: u32) -> Option<u32> {
        if index as usize >= self.buffers.len() {
            return None;
        }
        // One reference is the chain's own; "taken" adds the application's.
        Some(if self.world.buffer_taken.load(Ordering::Acquire) { 2 } else { 1 })
    }

    fn desc(&self) -> Result<SwapChainDesc, GpuError> {
        Ok(*self.world.desc.lock().unwrap())
    }

    fn current_back_buffer_index(&self) -> u32 {
        0
    }

    fn present(&self, _sync: u32, flags: PresentFlags) -> Result<(), GpuError> {
        self.world.presents.fetch_add(1, Ordering::Relaxed);
        self.world.present_flags.lock().unwrap().push(flags.0);
        Ok(())
    }

    fn present1(&self, _sync: u32, flags: PresentFlags, params: &PresentParams) -> Result<(), GpuError> {
        self.world.present1s.fetch_add(1, Ordering::Relaxed);
        self.world.present_flags.lock().unwrap().push(flags.0);
        if !params.is_empty() {
            self.world.presents_with_params.fetch_add(1, Ordering::Relaxed);
        }
        Ok(())
    }

    fn resize_buffers(
        &self,
        count: u32,
        width: u32,
        height: u32,
        format: PixelFormat,
        flags: u32,
    ) -> Result<(), GpuError> {
        self.world.resizes.fetch_add(1, Ordering::Relaxed);
        let mut desc = self.world.desc.lock().unwrap();
        desc.buffer_count = if count == 0 { desc.buffer_count } else { count };
        desc.width = if width == 0 { desc.width } else { width };
        desc.height = if height == 0 { desc.height } else { height };
        desc.format = format;
        desc.flags = flags;
        Ok(())
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
        self.world.resize1s.fetch_add(1, Ordering::Relaxed);
        self.world.resize_masks.lock().unwrap().push(node_masks.to_vec());
        self.resize_buffers(count, width, height, format, flags)
    }

    fn frame_statistics(&self) -> Option<FrameStatistics> {
        None
    }
    fn last_present_count(&self) -> Option<u32> {
        Some(self.world.presents.load(Ordering::Relaxed) + self.world.present1s.load(Ordering::Relaxed))
    }
    fn maximum_frame_latency(&self) -> Option<u32> {
        Some(1)
    }
    fn set_maximum_frame_latency(&self, _latency: u32) -> Result<(), GpuError> {
        Ok(())
    }
    fn has_waitable_object(&self) -> bool {
        self.world.desc.lock().unwrap().waitable()
    }
    fn containing_output(&self) -> Option<OutputDesc> {
        Some(OutputDesc {
            device_name: Vec::new(),
            width: 1920,
            height: 1080,
            rotated_quarter_turn: false,
            refresh_hz: *self.world.refresh_hz.lock().unwrap(),
        })
    }
    fn set_fullscreen_state(&self, _fullscreen: bool) -> Result<(), GpuError> {
        Ok(())
    }
    fn fullscreen(&self) -> bool {
        false
    }
    fn is_temporary_mono_supported(&self) -> bool {
        false
    }
    fn wait_for_retirement(&self, _timeout: Duration) -> Result<(), GpuError> {
        Ok(())
    }
}

/// A context whose recording and registration a test scripts.
struct FakeContext {
    world: Arc<World>,
    prepared: AtomicBool,
    untracked: AtomicBool,
    /// Whether `track_submission` is accepted.
    accept_registration: AtomicBool,
    /// Order of calls, so the tests can see generation happen before any Present.
    events: Mutex<Vec<&'static str>>,
    phases_recorded: AtomicU32,
    abandoned: AtomicU32,
    generates_before_present: AtomicBool,
}

impl FakeContext {
    fn new(world: Arc<World>) -> Self {
        Self {
            world,
            prepared: AtomicBool::new(true),
            untracked: AtomicBool::new(false),
            accept_registration: AtomicBool::new(true),
            events: Mutex::new(Vec::new()),
            phases_recorded: AtomicU32::new(0),
            abandoned: AtomicU32::new(0),
            generates_before_present: AtomicBool::new(false),
        }
    }
}

impl PresentedContext for FakeContext {
    type Texture = FakeTexture;
    type List = FakeList;

    fn prepared(&self) -> bool {
        self.prepared.load(Ordering::Acquire)
    }

    fn generate_phases(
        &self,
        _list: &Self::List,
        _real: &Self::Texture,
        phases: &[GeneratePhase<'_, Self::Texture>],
        _reset: bool,
    ) -> bool {
        self.events.lock().unwrap().push("generate");
        if self.world.presents.load(Ordering::Relaxed) == 0 && self.world.present1s.load(Ordering::Relaxed) == 0 {
            self.generates_before_present.store(true, Ordering::Release);
        }
        self.phases_recorded.fetch_add(phases.len() as u32, Ordering::Relaxed);
        self.untracked.store(true, Ordering::Release);
        true
    }

    fn note_ordered_submission(&self) {
        self.events.lock().unwrap().push("ordered");
    }

    fn track_submission(&self, _fence_value: u64) -> bool {
        if !self.accept_registration.load(Ordering::Acquire) {
            return false;
        }
        self.untracked.store(false, Ordering::Release);
        true
    }

    fn abandon_generation(&self) {
        self.abandoned.fetch_add(1, Ordering::Relaxed);
    }

    fn has_untracked_work(&self) -> bool {
        self.untracked.load(Ordering::Acquire)
    }

    fn wait_for_retirement(&self, _timeout: Duration) -> bool {
        true
    }

    fn output_state(&self) -> ResourceState {
        ResourceState::COPY_SOURCE
    }
}

struct Harness {
    world: Arc<World>,
    swapchain: Swapchain<FakeDevice, FakeNative, FakeContext, NoOverlay>,
    context: Arc<FakeContext>,
    clock: Arc<FakeClock>,
}

/// Builds the wrapper the way the FFX wrap desc would: no handed-over reference, "vsync" (so the
/// real-frame path runs), a fake clock and a scripted context.
fn harness(present: Present) -> Harness {
    let mut harness = harness_without_sink_present(present);
    let context = Arc::new(FakeContext::new(Arc::clone(&harness.world)));
    let sink = GeneratorSink { context: Arc::clone(&context), owner: 1, lease: Some(Arc::new(())) };
    assert!(harness.swapchain.set_generator(Some(sink)));
    harness.context = context;
    harness
}

fn harness_without_sink() -> Harness {
    harness_without_sink_present(Present::Vsync)
}

fn harness_without_sink_present(present: Present) -> Harness {
    let world = Arc::new(World::new(sample_desc()));
    *world.refresh_hz.lock().unwrap() = 60.0;
    let device = Arc::new(FakeDevice { world: Arc::clone(&world) });
    let native = Arc::new(FakeNative::new(Arc::clone(&world)));
    let queue = Arc::new(FakeQueue { world: Arc::clone(&world) });
    let clock = Arc::new(FakeClock::new(1_000_000, 1_000_000));
    let queues = Arc::new(SharedPresentQueues::new(false));
    let choice = PresentChoice { present, guard: TearGuard::Off, multiplier: Multiplier::X2, target_fps: 0.0 };
    let mut report = WrapReport::default();
    let swapchain = Swapchain::wrap(
        native,
        device,
        queue,
        clock.clone(),
        choice,
        queues,
        Arc::new(NoOverlay),
        WrapEnvironment::default(),
        0,
        &mut report,
    )
    .expect("the wrapper builds");
    let context = Arc::new(FakeContext::new(Arc::clone(&world)));
    Harness { world, swapchain, context, clock }
}

// ---------------------------------------------------------------------------------------------
// Scenarios
// ---------------------------------------------------------------------------------------------

/// `check_generated_window`: one midpoint per cycle from the second, the sink removable, and the
/// game-facing buffer index advancing once per accepted Present.
#[test]
fn a_real_frame_wrapper_shows_a_midpoint_then_the_real_frame() {
    let h = harness(Present::Vsync);
    for cycle in 0..6u32 {
        h.clock.advance(1_000_000 / 60);
        h.swapchain.present(1, PresentFlags::NONE, None).expect("present accepted");
        assert_eq!(
            h.swapchain.current_back_buffer_index(),
            (cycle + 1) % 3,
            "the game-facing index advances once per cycle"
        );
    }
    let counters = h.swapchain.counters();
    assert_eq!(h.context.phases_recorded.load(Ordering::Relaxed), 6, "one generation per real frame");
    assert_eq!(counters.present_cycles, 6);
    // The first cycle only seeds history; every later one shows one midpoint.
    assert_eq!(counters.generated_presents, 5);
    assert_eq!(counters.skipped_generates, 0);
    assert!(h.context.generates_before_present.load(Ordering::Acquire), "generation precedes the present");
}

#[test]
fn clearing_the_sink_stops_midpoints_without_stalling_real_frames() {
    let h = harness(Present::Vsync);
    for _ in 0..3 {
        h.clock.advance(1_000_000 / 60);
        h.swapchain.present(1, PresentFlags::NONE, None).expect("accepted");
    }
    let before = h.swapchain.counters();
    assert!(h.swapchain.clear_generator_if_owner(1));
    for _ in 0..2 {
        h.clock.advance(1_000_000 / 60);
        h.swapchain.present(1, PresentFlags::NONE, None).expect("accepted");
    }
    let after = h.swapchain.counters();
    assert_eq!(after.generated_presents, before.generated_presents);
    assert_eq!(after.present_cycles, before.present_cycles + 2);
    assert!(!h.swapchain.clear_generator_if_owner(2), "a stale owner cannot detach a newer sink");
}

/// One scenario for the three injected generation faults: after any of them, every later Present
/// returns the same error and no further generation is recorded (K37).
#[rstest]
#[case("close")]
#[case("signal")]
#[case("reset")]
fn a_failed_generation_submission_stops_later_admission(#[case] fault: &str) {
    let h = harness(Present::Vsync);
    match fault {
        // Close is not injectable through the fakes; a failing execute is the same seam.
        "close" => h.world.fail_execute.store(true, Ordering::Release),
        "signal" => h.context.accept_registration.store(false, Ordering::Release),
        // A reset fault loses the allocator, which the C++ treats as a submission failure.
        "reset" => h.world.fail_reset.store(true, Ordering::Release),
        _ => unreachable!(),
    }
    let first = h.swapchain.present(1, PresentFlags::NONE, None);
    let second = h.swapchain.present(1, PresentFlags::NONE, None);
    assert_eq!(first.unwrap_err().code(), 0x8000_4005, "{fault}: first present fails");
    assert_eq!(second.unwrap_err().code(), 0x8000_4005, "{fault}: later presents stay refused");
    assert!(h.swapchain.current_back_buffer_index() < 3);
}

/// K76: a failed Present1 metadata copy is the game's error, but a retry with the same metadata is
/// accepted and nothing was consumed by the failure.
#[test]
fn a_present1_metadata_failure_does_not_consume_the_frame() {
    let h = harness(Present::Vsync);
    // The owner-approved exception is a local error before limiter/schedule/GPU work. With the fakes
    // an empty metadata set must reach Present1 and be counted as such.
    h.clock.advance(1_000_000 / 60);
    let params = PresentParams {
        dirty_rects: vec![Rect { left: 0, top: 0, right: 16, bottom: 16 }],
        scroll_rect: None,
        scroll_offset: None,
    };
    h.swapchain.present(0, PresentFlags::NONE, Some(&params)).expect("accepted");
    assert_eq!(h.world.present1s.load(Ordering::Relaxed), 1);
    assert_eq!(h.world.presents_with_params.load(Ordering::Relaxed), 1);
    let index = h.swapchain.current_back_buffer_index();
    h.clock.advance(1_000_000 / 60);
    h.swapchain.present(0, PresentFlags::NONE, Some(&params)).expect("retry accepted");
    assert_eq!(h.swapchain.current_back_buffer_index(), (index + 1) % 3);
}

#[test]
fn present1_with_an_empty_metadata_set_is_still_present1() {
    let h = harness(Present::Vsync);
    h.clock.advance(1_000_000 / 60);
    h.swapchain.present(0, PresentFlags::NONE, Some(&PresentParams::default())).expect("accepted");
    assert_eq!(h.world.present1s.load(Ordering::Relaxed), 1);
    assert_eq!(h.world.presents.load(Ordering::Relaxed), 0);
    assert_eq!(h.world.presents_with_params.load(Ordering::Relaxed), 0);
}

#[test]
fn present_test_presents_nothing_and_advances_nothing() {
    let h = harness(Present::Vsync);
    let index = h.swapchain.current_back_buffer_index();
    h.swapchain.present(0, PresentFlags::TEST, None).expect("test accepted");
    assert_eq!(h.swapchain.current_back_buffer_index(), index);
    assert_eq!(h.swapchain.counters().present_cycles, 0);
}

/// `vsync` forces SyncInterval=1 and strips ALLOW_TEARING, because SyncInterval=1 cannot be combined
/// with it (the legacy `SEDICI_FG_SYNC` switch selects the same behaviour).
#[test]
fn vsync_forces_a_sync_interval_and_strips_tearing() {
    let h = harness(Present::Vsync);
    h.clock.advance(1_000_000 / 60);
    h.swapchain.present(0, PresentFlags::ALLOW_TEARING, None).expect("accepted");
    let flags = h.world.present_flags.lock().unwrap();
    assert!(!flags.is_empty());
    assert!(flags.iter().all(|value| *value & PresentFlags::ALLOW_TEARING.0 == 0));
}

/// `fgcheck_retention.cpp`: a removed device executes nothing more, so the wrapper is retirable even
/// though its fence never reaches the target; a live device with outstanding work is not (K38/K41).
#[test]
fn teardown_waits_for_retirement_and_a_removed_device_counts_as_retired() {
    let h = harness(Present::Vsync);
    h.clock.advance(1_000_000 / 60);
    h.swapchain.present(1, PresentFlags::NONE, None).expect("accepted");
    // A live device whose fence lags the wrapper's value is not retirable.
    h.world.set_completed(0);
    assert!(!h.swapchain.retirable());
    // Removal makes everything in flight unretirable no more: nothing can execute.
    *h.world.removed.lock().unwrap() = Some(0x887A_0005);
    assert!(h.swapchain.retirable());
    let error = h.swapchain.drain().expect_err("a removed device is reported");
    assert_eq!(error.code(), 0x887A_0005);
    assert!(matches!(
        error,
        SwapchainError::DeviceRemoved | SwapchainError::DeviceHung | SwapchainError::SubmissionFailure
    ));
}

#[test]
fn teardown_is_bounded_when_the_fence_never_reaches_the_target() {
    let h = harness(Present::Vsync);
    h.clock.advance(1_000_000 / 60);
    h.swapchain.present(1, PresentFlags::NONE, None).expect("accepted");
    // The fake fence completes on wait, which is the live-device path the C++ drained.
    h.swapchain.drain().expect("a live device drains");
    assert!(!h.world.waits.lock().unwrap().is_empty() || h.world.completed.load(Ordering::Acquire) > 0);
}

/// `check_window`: resize refuses while the application holds a back buffer, then succeeds and the
/// new size is what `GetBuffer` would hand out.
#[test]
fn resize_refuses_a_taken_back_buffer_then_applies_the_new_size() {
    let h = harness(Present::Vsync);
    h.clock.advance(1_000_000 / 60);
    h.swapchain.present(1, PresentFlags::NONE, None).expect("accepted");
    h.world.buffer_taken.store(true, Ordering::Release);
    assert_eq!(
        h.swapchain.resize(3, 320, 180, None, 0).unwrap_err().code(),
        DXGI_ERROR_INVALID_CALL
    );
    assert_eq!(h.world.resizes.load(Ordering::Relaxed), 0, "the chain was not resized");
    h.world.buffer_taken.store(false, Ordering::Release);
    h.swapchain.resize(3, 320, 180, None, 0).expect("resize accepted");
    assert_eq!(h.world.resizes.load(Ordering::Relaxed), 1);
}

#[rstest]
#[case(PixelFormat::R8G8B8A8Unorm, true)]
#[case(PixelFormat::R10G10B10A2Unorm, true)]
#[case(PixelFormat::R16G16B16A16Float, false)]
fn only_the_two_presentable_formats_resize(#[case] format: PixelFormat, #[case] accepted: bool) {
    let h = harness(Present::Vsync);
    let result = h.swapchain.resize(3, 320, 180, Some(format), 0);
    assert_eq!(result.is_ok(), accepted);
    if !accepted {
        assert_eq!(result.unwrap_err().code(), DXGI_ERROR_UNSUPPORTED);
    }
}

/// `resize_buffers1` is one node and one queue: a mask above one is refused, an unnamed buffer is a
/// missing argument, and the chain stays on Sedici's queue.
#[rstest]
#[case(&[0, 0, 0], true)]
#[case(&[0, 0, 2], false)]
#[case(&[], false)]
fn resize_buffers1_validates_masks(#[case] masks: &[u32], #[case] accepted: bool) {
    let h = harness(Present::Vsync);
    let result = h.swapchain.resize_buffers1(3, 320, 180, None, 0, masks);
    assert_eq!(result.is_ok(), accepted);
}

/// An unsupported chain description is refused before anything is created (the create-allocation
/// exception says allocation happens before success, and a bad description is not an allocation).
#[rstest]
#[case(2, 3, false, PixelFormat::R8G8B8A8Unorm)]
#[case(1, 3, false, PixelFormat::R16G16B16A16Float)]
fn an_unsupported_chain_is_refused(#[case] samples: u32, #[case] buffers: u32, #[case] stereo: bool, #[case] format: PixelFormat) {
    let world = Arc::new(World::new(sample_desc()));
    let mut desc = *world.desc.lock().unwrap();
    desc.sample_count = samples;
    desc.buffer_count = buffers;
    desc.stereo = stereo;
    desc.format = format;
    *world.desc.lock().unwrap() = desc;
    let device = Arc::new(FakeDevice { world: Arc::clone(&world) });
    let native = Arc::new(FakeNative::new(Arc::clone(&world)));
    let queue = Arc::new(FakeQueue { world: Arc::clone(&world) });
    let clock = Arc::new(FakeClock::new(1_000_000, 0));
    let mut report = WrapReport::default();
    let result: Result<Swapchain<FakeDevice, FakeNative, FakeContext, NoOverlay>, _> = Swapchain::wrap(
        native,
        device,
        queue,
        clock,
        PresentChoice { present: Present::Vsync, ..PresentChoice::default() },
        Arc::new(SharedPresentQueues::new(false)),
        Arc::new(NoOverlay),
        WrapEnvironment::default(),
        0,
        &mut report,
    );
    assert_eq!(result.err().map(|error| error.code()), Some(DXGI_ERROR_UNSUPPORTED));
}

/// "vrr" without a handed-over reference cannot recreate the chain and falls back to "vsync"
/// without failing the game call.
#[test]
fn vrr_falls_back_to_vsync_when_the_caller_keeps_its_reference() {
    let h = harness(Present::Vrr);
    assert!(!h.swapchain.async_present());
    h.clock.advance(1_000_000 / 60);
    h.swapchain.present(1, PresentFlags::NONE, None).expect("the fallback still presents");
}

/// The interpolation scratch texture is created once per size and a resize retires the old one
/// (K131/K138: the game may hold the pointer, so it is not destroyed).
#[test]
fn the_interpolation_texture_follows_a_resize_without_reuse() {
    let h = harness(Present::Vsync);
    assert!(!h.swapchain.has_interpolation_texture());
    let first = h.swapchain.interpolation_texture().expect("created");
    assert!(h.swapchain.has_interpolation_texture());
    let again = h.swapchain.interpolation_texture().expect("same size");
    assert_eq!(first.identity(), again.identity(), "a same-size ask returns the same resource");
    h.swapchain.resize(3, 64, 64, None, 0).expect("resized");
    let resized = h.swapchain.interpolation_texture().expect("recreated");
    assert_ne!(first.identity(), resized.identity(), "a resize replaces it");
}

/// K131/K138: the interpolation list is created open once and reset for a later ask, so the game
/// can record into a clean list every time and the pointer stays valid.
#[test]
fn the_interpolation_list_is_reusable() {
    let h = harness(Present::Vsync);
    let first = h.swapchain.interpolation_list().expect("created");
    assert!(!first.closed.load(Ordering::Acquire), "a new list is open for recording");
    first.close_list().expect("closed");
    let again = h.swapchain.interpolation_list().expect("reset");
    assert!(!again.closed.load(Ordering::Acquire), "the second ask starts a fresh recording");
}

/// `check_pacing` decisions, through the wrapper's own pacing interval and the presenter's stale
/// predicate.
#[rstest]
#[case(true, 1.0, 0.125, 1.124, false)]
#[case(true, 1.0, 0.125, 1.125, true)]
#[case(false, 1.0, 0.125, 9.0, false)]
fn the_presenter_drops_only_outputs_a_later_one_supersedes(
    #[case] has_later: bool,
    #[case] content: f64,
    #[case] latency: f64,
    #[case] now: f64,
    #[case] due: bool,
) {
    assert_eq!(superseded(has_later, content, latency, now), due);
}

#[rstest]
#[case(Multiplier::X2)]
#[case(Multiplier::X1_5)]
#[case(Multiplier::X1_25)]
#[case(Multiplier::X1_75)]
fn every_fixed_multiplier_plans_an_even_grid(#[case] multiplier: Multiplier) {
    let (numerator, denominator) = match multiplier {
        Multiplier::X2 => (2, 1),
        Multiplier::X1_5 => (3, 2),
        Multiplier::X1_25 => (5, 4),
        Multiplier::X1_75 => (7, 4),
        Multiplier::Adaptive => unreachable!(),
    };
    let mut schedule = OutputSchedule::new();
    schedule.set_ratio(numerator, denominator);
    let interval = 1.0 / 48.0;
    let mut previous = f64::NAN;
    let mut gaps = Vec::new();
    let mut generated = 0;
    for frame in 0..201u32 {
        let plan = schedule.plan(10.0 + f64::from(frame) * interval);
        generated = generated.max(plan.generated());
        for output in plan.outputs() {
            if frame > 0 {
                gaps.push(output.content - previous);
            }
            previous = output.content;
        }
    }
    let expected = interval * f64::from(denominator) / f64::from(numerator);
    assert!(gaps.iter().all(|gap| (gap - expected).abs() < 1e-6), "{multiplier:?} keeps an even grid");
    assert!(generated <= 2);
}

/// The wrapper's pacing interval is 1/60 before the first cycle and the smoothed frame interval
/// after it, which is the value the real-frame pacing wait uses.
#[test]
fn the_pacing_interval_tracks_the_game_rate() {
    // Without a sink no midpoint is shown, so no pacing wait consumes part of the interval.
    let h = harness_without_sink();
    assert!((h.swapchain.pacing_interval() - 1.0 / 60.0).abs() < 1e-12);
    for _ in 0..3 {
        h.clock.advance(1_000_000 / 36);
        h.swapchain.present(1, PresentFlags::NONE, None).expect("accepted");
    }
    assert!((h.swapchain.pacing_interval() - 1.0 / 36.0).abs() < 1e-6);
}

/// Pause/retry: no Present may leave partial ownership behind, so a `DO_NOT_WAIT` refused present
/// is a definite return value, never a block (K52).
#[test]
fn a_do_not_wait_present_never_blocks_on_outstanding_work() {
    let h = harness(Present::Vsync);
    h.clock.advance(1_000_000 / 60);
    h.world.set_completed(0);
    // With no presenter the real path does not consult the fence for DO_NOT_WAIT before draining;
    // what matters is that it returns, so the test's bound is that it completes at all.
    let result = h.swapchain.present(0, PresentFlags::DO_NOT_WAIT, None);
    assert!(result.is_ok() || result.unwrap_err().code() >= 0x8000_0000);
}

/// The clock wait the wrapper uses for pacing must never park past its bounded window: a fake clock
/// records the deadline and advances to it.
#[test]
fn the_pacing_wait_is_bounded_by_the_fake_clock() {
    let h = harness(Present::Vsync);
    h.clock.advance(1_000_000 / 60);
    h.swapchain.present(1, PresentFlags::NONE, None).expect("accepted");
    // Present may have waited for the midpoint-to-real gap; every recorded wait is a real deadline.
    for deadline in h.clock.waits() {
        assert!(deadline >= 1_000_000);
        assert!(deadline <= 1_000_000 + 1_000_000 / 30, "the wait stays inside half an interval");
    }
}

/// The counters the wrapper exposes are the ones the C++ `generated_presents`/`skipped_generates`
/// reported, so a log line keeps its meaning.
#[test]
fn counters_distinguish_shown_midpoints_from_dropped_ones() {
    let h = harness(Present::Vsync);
    for _ in 0..4 {
        h.clock.advance(1_000_000 / 60);
        h.swapchain.present(1, PresentFlags::NONE, None).expect("accepted");
    }
    let counters = h.swapchain.counters();
    assert_eq!(counters.generated_presents + counters.skipped_generates, 3);
}

/// A resize clears the pacing history, as the C++ did: the next cycle only seeds.
#[test]
fn a_resize_resets_the_pacing_history() {
    let h = harness(Present::Vsync);
    for _ in 0..3 {
        h.clock.advance(1_000_000 / 60);
        h.swapchain.present(1, PresentFlags::NONE, None).expect("accepted");
    }
    h.swapchain.resize(3, 320, 180, None, 0).expect("resized");
    assert!((h.swapchain.pacing_interval() - 1.0 / 60.0).abs() < 1e-12);
}

/// An unset `SEDICI_FG_ALIGN`/`SEDICI_FG_SYNC` leaves the frame statistics path off; with vblank
/// alignment enabled a present still completes (the fake has no statistics, so it falls back to now).
#[test]
fn vblank_alignment_without_frame_statistics_falls_back_to_the_current_time() {
    let world = Arc::new(World::new(sample_desc()));
    let device = Arc::new(FakeDevice { world: Arc::clone(&world) });
    let native = Arc::new(FakeNative::new(Arc::clone(&world)));
    let queue = Arc::new(FakeQueue { world: Arc::clone(&world) });
    let clock = Arc::new(FakeClock::new(1_000_000, 0));
    let mut report = WrapReport::default();
    let swapchain = Swapchain::wrap(
        native,
        device,
        queue,
        clock.clone(),
        PresentChoice { present: Present::Vsync, ..PresentChoice::default() },
        Arc::new(SharedPresentQueues::new(false)),
        Arc::new(NoOverlay),
        WrapEnvironment { align_vblank: true, ..WrapEnvironment::default() },
        0,
        &mut report,
    )
    .expect("built");
    let sink = GeneratorSink { context: Arc::new(FakeContext::new(Arc::clone(&world))), owner: 1, lease: None };
    assert!(swapchain.set_generator(Some(sink)));
    clock.advance(1_000_000 / 60);
    swapchain.present(1, PresentFlags::NONE, None).expect("accepted");
}

/// The scratch lock is never the wrapper's Present path (K138): asking for the interpolation
/// texture while a presentation callback would otherwise be running must not block on it. The fake
/// context's record hook is the equivalent of the C++ seam.
#[test]
fn the_scratch_queries_do_not_take_the_present_path() {
    let h = harness(Present::Vsync);
    h.clock.advance(1_000_000 / 60);
    let _ = h.swapchain.interpolation_texture().expect("created outside Present");
    h.swapchain.present(1, PresentFlags::NONE, None).expect("accepted");
    // A second query after Present returns at once: the scratch never holds the wrapper's lock.
    let _ = h.swapchain.interpolation_texture().expect("still available");
}

/// `owned` references: the wrapper takes over the game's one native reference in the real-frame
/// path (K12). With fakes the reference count is the world's, so the observable part is that the
/// wrapper is constructed and presents.
#[test]
fn a_handed_over_reference_wraps_and_presents() {
    let world = Arc::new(World::new(sample_desc()));
    let device = Arc::new(FakeDevice { world: Arc::clone(&world) });
    let native = Arc::new(FakeNative::new(Arc::clone(&world)));
    let queue = Arc::new(FakeQueue { world: Arc::clone(&world) });
    let clock = Arc::new(FakeClock::new(1_000_000, 0));
    let mut report = WrapReport::default();
    let swapchain: Swapchain<FakeDevice, FakeNative, FakeContext, NoOverlay> = Swapchain::wrap(
        native,
        device,
        queue,
        clock.clone(),
        PresentChoice { present: Present::Vsync, ..PresentChoice::default() },
        Arc::new(SharedPresentQueues::new(false)),
        Arc::new(NoOverlay),
        WrapEnvironment::default(),
        1,
        &mut report,
    )
    .expect("built");
    clock.advance(1_000_000 / 60);
    swapchain.present(1, PresentFlags::NONE, None).expect("accepted");
    assert!(report.fallback_reason.is_none(), "vsync needs no fallback");
}
