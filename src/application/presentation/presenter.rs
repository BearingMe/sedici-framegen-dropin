//! The "vrr" presenter (ports `presentation/presenter.*`).
//!
//! With `[fg] present = "vrr"`, presentation leaves the game's thread and queue: every output
//! carries a content time on the game's Present clock, and the presenter flips it at content +
//! latency, where the latency is learned from how long after its content time the first image of a
//! frame is actually finished on the GPU. A tear guard can hold a flip until the panel is not
//! scanning out (finding 008).
//!
//! The thread, the bounded job ring ([`PRESENTER_QUEUE_CAPACITY`]), the latency windows and the
//! guard arithmetic are application logic over the [`GpuDevice`], [`NativeSwapchain`], [`Clock`]
//! and [`OverlayHook`] ports. The chain recreation on adopt, the D3DKMT scan-line read and the
//! RTSS check live in adapters.

use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Condvar, Mutex};
use std::thread::JoinHandle;
use std::time::Duration;

use thiserror::Error;

use crate::application::ports::clock::Clock;
use crate::application::ports::gpu::{
    GpuCommandAllocator, GpuCommandList, GpuDevice, GpuError, GpuFence, GpuQueue, QueueKind,
    QueuePriority, ResourceState,
};
use crate::application::ports::overlay::OverlayHook;
use crate::application::ports::swapchain::{
    NativeSwapchain, PresentFlags, PresentParams, Scanline,
};
use crate::application::presentation::pacing::superseded;
use crate::domain::config::TearGuard;
use crate::domain::gpu_policy::PRESENTER_QUEUE_CAPACITY;

/// At most two generated frames plus the real one in one job.
pub const JOB_OUTPUTS: usize = 3;

/// The bounded GPU wait; a removed or hung device must not trap teardown forever.
pub const GPU_TIMEOUT: Duration = Duration::from_millis(5000);


/// The bounded GPU wait a "vrr" presenter gives the game's frame when the swapchain drains it.
pub const DRAIN_TIMEOUT: Duration = Duration::from_millis(5000);

/// Reset/close over the concrete adapters' command lists, without the application naming them.
pub trait ListLifecycle {
    type Allocator;

    /// Resets the list onto `allocator`, which the caller has already reset.
    fn reset_list(&self, allocator: &Self::Allocator) -> Result<(), GpuError>;

    fn close_list(&self) -> Result<(), GpuError>;
}

/// Where the panel's scan is. The D3DKMT implementation is an adapter.
pub trait ScanlineSource: Send + Sync {
    fn read(&self) -> Option<Scanline>;
}

/// A display's mode, as the presenter reads it from the containing output.
#[derive(Clone, Debug, PartialEq)]
pub struct DisplayMode {
    /// Seconds per refresh at the current mode.
    pub period: f64,
    /// Rows the scan traverses; the panel's native rows on a rotated output.
    pub active_lines: u32,
    /// Rows of lead the guard needs so the flip is latched before the scan starts.
    pub lead_lines: u32,
    pub scanline_available: bool,
}

impl Default for DisplayMode {
    fn default() -> Self {
        Self {
            period: Self::DEFAULT_PERIOD,
            active_lines: 0,
            lead_lines: 0,
            scanline_available: false,
        }
    }
}

impl DisplayMode {
    pub const DEFAULT_PERIOD: f64 = 1.0 / 60.0;

    pub fn new(refresh_hz: f64, height: i64, width: i64, rotated_quarter_turn: bool, lead_seconds: f64) -> Self {
        let period = if refresh_hz.is_finite() && refresh_hz > 1.0 {
            1.0 / refresh_hz
        } else {
            Self::DEFAULT_PERIOD
        };
        let rows = if rotated_quarter_turn { width } else { height };
        let active = rows.max(1) as u32;
        let lead = (f64::from(active) * (lead_seconds / period)).round() as u32;
        Self { period, active_lines: active, lead_lines: lead, scanline_available: false }
    }

    pub fn refresh_hz(&self) -> f64 {
        if self.period > 0.0 {
            1.0 / self.period
        } else {
            60.0
        }
    }
}

/// One image to show: the real frame or a generated one, in content-time order.
#[derive(Clone, Debug)]
pub struct JobOutput<T> {
    /// The generated image, in `COPY_SOURCE`; `None` for the real frame.
    pub image: Option<T>,
    pub real: bool,
    /// Seconds on the game's Present clock.
    pub content: f64,
}

impl<T> JobOutput<T> {
    pub fn real(content: f64) -> Self {
        Self { image: None, real: true, content }
    }

    pub fn generated(image: T, content: f64) -> Self {
        Self { image: Some(image), real: false, content }
    }
}

/// One application frame's worth of output. A frame may show nothing of its own (adaptive target
/// below the real rate), only generated frames, or both.
pub struct PresentJob<D: GpuDevice> {
    /// The private snapshot of the game's back buffer, in `PRESENT`.
    pub real: D::Texture,
    pub outputs: [Option<JobOutput<D::Texture>>; JOB_OUTPUTS],
    pub count: usize,
    /// The game's Present for this frame, on the content clock.
    pub entry: f64,
    /// Signalled on the game queue after the frame and its generation work.
    pub ready: Arc<D::Fence>,
    pub ready_value: u64,
    pub sync: u32,
    pub flags: PresentFlags,
    pub params: Option<PresentParams>,
}

impl<D: GpuDevice> PresentJob<D> {
    pub fn new(
        real: D::Texture,
        ready: Arc<D::Fence>,
        ready_value: u64,
        sync: u32,
        flags: PresentFlags,
        params: Option<PresentParams>,
    ) -> Self {
        Self {
            real,
            outputs: [None, None, None],
            count: 0,
            entry: 0.0,
            ready,
            ready_value,
            sync,
            flags,
            params,
        }
    }

    pub fn push(&mut self, output: JobOutput<D::Texture>) {
        if self.count < JOB_OUTPUTS {
            self.outputs[self.count] = Some(output);
            self.count += 1;
        }
    }
}

/// Why a presenter call failed. Mapped to the game's error code by the FFX ABI edge only.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Error)]
pub enum PresentError {
    #[error("the presenter has stopped")]
    Stopped,
    #[error("the presenter's job ring is full")]
    QueueFull,
    #[error("the device is hung")]
    DeviceHung,
    #[error("a GPU step failed")]
    Gpu,
}

impl PresentError {
    /// The HRESULT-shaped value the C++ returned for the same failure.
    pub fn code(self) -> u32 {
        match self {
            // K167: the C++ `Presenter::submit` answers `DXGI_ERROR_INVALID_CALL` when it has
            // stopped (0x887A0001), not `E_INVALIDARG`. The port's `code_error` maps the former
            // to `SwapchainError::InvalidCall`; `E_INVALIDARG` mapped to `InvalidArgument`
            // instead, so a Present after a stopped presenter returned the wrong defined code.
            Self::Stopped => crate::application::presentation::swapchain::DXGI_ERROR_INVALID_CALL,
            Self::QueueFull => 0x887A_000A,               // DXGI_ERROR_WAS_STILL_DRAWING
            Self::DeviceHung => 0x887A_0006,              // DXGI_ERROR_DEVICE_HUNG
            Self::Gpu => 0x8000_4005,                     // E_FAIL
        }
    }
}

/// A per-device shared present queue (K140: released with its last presenter after a successful
/// drain, unless a Present-hooking overlay is loaded, in which case it is kept for the process).
pub struct SharedPresentQueues<D: GpuDevice> {
    entries: Mutex<Vec<SharedQueue<D>>>,
    keep_for_process: bool,
}

struct SharedQueue<D: GpuDevice> {
    key: usize,
    /// Keeps the device alive while the entry exists, so its key (address) cannot be reused.
    _device: Arc<D>,
    queue: Arc<D::Queue>,
    users: u32,
}

impl<D: GpuDevice> SharedPresentQueues<D> {
    pub fn new(keep_for_process: bool) -> Self {
        Self { entries: Mutex::new(Vec::new()), keep_for_process }
    }

    /// One queue per device, shared by that device's presenters. `key` identifies the device; the
    /// adapter passes a stable value (the device pointer) and the entry keeps the device alive.
    pub fn acquire(&self, key: usize, device: &Arc<D>) -> Result<Arc<D::Queue>, GpuError> {
        let mut entries = lock(&self.entries);
        if let Some(entry) = entries.iter_mut().find(|entry| entry.key == key) {
            entry.users += 1;
            return Ok(Arc::clone(&entry.queue));
        }
        // A high-priority queue is a preference, not a requirement.
        let queue = Arc::new(
            device
                .create_queue(QueueKind::Direct, QueuePriority::High)
                .or_else(|_| device.create_queue(QueueKind::Direct, QueuePriority::Normal))?,
        );
        entries.push(SharedQueue { key, _device: Arc::clone(device), queue: Arc::clone(&queue), users: 1 });
        Ok(queue)
    }

    /// Drops one presenter's use. `idle` says its work retired; a failed drain keeps the queue.
    pub fn release(&self, key: usize, idle: bool) {
        let mut entries = lock(&self.entries);
        if let Some(index) = entries.iter().position(|entry| entry.key == key) {
            if entries[index].users > 0 {
                entries[index].users -= 1;
            }
            if entries[index].users == 0 && idle && !self.keep_for_process {
                entries.remove(index);
            }
        }
    }

    /// Devices that currently have a shared present queue (K140's observable count).
    pub fn len(&self) -> usize {
        lock(&self.entries).len()
    }

    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
}

fn lock<T>(mutex: &Mutex<T>) -> std::sync::MutexGuard<'_, T> {
    mutex.lock().unwrap_or_else(|poisoned| poisoned.into_inner())
}

/// The learned content-time-to-flip latency. The structural part is the largest of the last 8
/// schedule offsets; the GPU part the median of the last 16 Present-to-ready times. Short windows
/// on purpose: one hitch must not hold the latency up and lengthen every flip (finding 009).
#[derive(Debug, Clone)]
pub struct LatencyTracker {
    structural_ring: [f64; 8],
    structural_next: usize,
    structural: f64,
    gpu_ring: [f64; 16],
    gpu_next: usize,
    gpu: f64,
    latency: f64,
}

impl Default for LatencyTracker {
    fn default() -> Self {
        Self {
            structural_ring: [0.0; 8],
            structural_next: 0,
            structural: 0.0,
            gpu_ring: [0.0; 16],
            gpu_next: 0,
            gpu: 0.0,
            latency: 0.0,
        }
    }
}

impl LatencyTracker {
    pub const SAFETY: f64 = 0.001;

    /// `measured` false means the frame was already finished when the presenter got to it, so the
    /// GPU value is only an upper bound and may only lower the estimate.
    pub fn track(&mut self, structural: f64, gpu: f64, measured: bool) {
        if structural.is_finite() {
            self.structural_ring[self.structural_next] = structural.clamp(0.0, 0.25);
            self.structural_next = (self.structural_next + 1) % self.structural_ring.len();
            self.structural = self.structural_ring.iter().copied().fold(0.0_f64, f64::max);
        }
        if gpu.is_finite() {
            let gpu = gpu.clamp(0.0, 0.1);
            if measured || gpu < self.gpu {
                self.gpu_ring[self.gpu_next] = gpu;
                self.gpu_next = (self.gpu_next + 1) % self.gpu_ring.len();
                let mut sorted = self.gpu_ring;
                sorted.sort_by(|a, b| a.partial_cmp(b).unwrap_or(std::cmp::Ordering::Equal));
                self.gpu = sorted[sorted.len() / 2];
            }
        }
        self.latency = self.structural + self.gpu + Self::SAFETY;
    }

    pub fn latency(&self) -> f64 {
        self.latency
    }
}

/// The earliest moment the timer guard allows a flip: one refresh after the previous flip plus a
/// jitter margin. The scan-line guard may reach back to just before the previous image's scan can
/// have ended, because right after Present the scan still reports the previous blank (finding 009).
pub fn guard_floor(
    last_flip: f64,
    period: f64,
    lead_seconds: f64,
    guard: TearGuard,
    scanline_available: bool,
) -> f64 {
    if !(last_flip > 0.0) {
        return 0.0;
    }
    let timer_guard = guard == TearGuard::Timer || (guard == TearGuard::Scanline && !scanline_available);
    let scan_guard = guard == TearGuard::Scanline && scanline_available;
    if timer_guard {
        last_flip + period + 0.0002
    } else if scan_guard {
        last_flip + period - lead_seconds - 0.0005
    } else {
        0.0
    }
}

/// On a fixed-refresh panel the scan-line guard can only flip at a blank; accepting a blank up to
/// this much before the target avoids waiting a whole refresh, which at ~36 fps on 75 Hz would
/// drop every midpoint.
pub fn guard_early(scanline_available: bool, guard: TearGuard, period: f64, not_before: f64) -> f64 {
    let scan_guard = guard == TearGuard::Scanline && scanline_available;
    if scan_guard && not_before > 0.0 {
        0.0015_f64.min(period * 0.12)
    } else {
        0.0
    }
}

/// The presenter window's running diagnostics, reported every 300 jobs and then reset.
#[derive(Debug, Default, Clone)]
pub struct PresentWindow {
    pub jobs: u32,
    pub generated_shown: u32,
    pub generated_dropped: u32,
    pub reals_shown: u32,
    pub reals_dropped: u32,
    pub late: u32,
    pub latency_sum: f64,
    pub latency_samples: u32,
    pub gpu_waits: u32,
    pub flips: u32,
    pub short_gaps: u32,
    pub back_to_back: u32,
    pub gap_min: f64,
    pub gpu_wait_sum: f64,
    pub gpu_wait_max: f64,
    pub guard_holds: u32,
    pub guard_sum: f64,
    pub guard_max: f64,
    pub guard_timeouts: u32,
    pub call_sum: f64,
    pub call_max: f64,
    /// At the flip: blank, then 0-25/25-50/50-75/75-100 %, then unread.
    pub scan: [u32; 6],
    pub idle_blank: u32,
    pub idle_samples: u32,
}

/// Presenter-side counters the wrapper reads.
#[derive(Debug, Default)]
pub struct PresenterCounters {
    generated_shown: AtomicU64,
    generated_dropped: AtomicU64,
    back_to_back: AtomicU64,
}

impl PresenterCounters {
    pub fn generated_shown(&self) -> u64 {
        self.generated_shown.load(Ordering::Relaxed)
    }
    pub fn generated_dropped(&self) -> u64 {
        self.generated_dropped.load(Ordering::Relaxed)
    }
    pub fn back_to_back(&self) -> u64 {
        self.back_to_back.load(Ordering::Relaxed)
    }
}

/// The presenter thread's own state.
struct ThreadState<D: GpuDevice>
where
    D::CommandList: ListLifecycle<Allocator = D::CommandAllocator>,
{
    allocators: [D::CommandAllocator; 2],
    lists: [D::CommandList; 2],
    next_list: usize,
    latency: LatencyTracker,
    window: PresentWindow,
    last_flip: f64,
    last_result: u32,
    stop: bool,
    cancel: bool,
}

struct Queue<D: GpuDevice> {
    jobs: std::collections::VecDeque<PresentJob<D>>,
    submitted: u64,
    completed: u64,
}

struct Shared<D: GpuDevice, N: NativeSwapchain>
where
    D::CommandList: ListLifecycle<Allocator = D::CommandAllocator>,
{
    /// Keeps the device alive for the life of the presenter (the shared queue entry also does).
    _device: Arc<D>,
    queue: Arc<D::Queue>,
    native: Arc<N>,
    fence: Arc<D::Fence>,
    clock: Arc<dyn Clock>,
    scanline: Option<Arc<dyn ScanlineSource>>,
    overlay: Mutex<Arc<dyn OverlayHook<D::Texture>>>,
    guard: TearGuard,
    mode: Mutex<DisplayMode>,
    lead_seconds: f64,
    state: Mutex<ThreadState<D>>,
    jobs: Mutex<Queue<D>>,
    wake: Condvar,
    done: Condvar,
    counters: Arc<PresenterCounters>,
    fence_value: AtomicU64,
}

/// The presenter thread and the state it shares with the game thread.
pub struct Presenter<D, N>
where
    D: GpuDevice + Send + Sync + 'static,
    D::Texture: Send + 'static,
    D::CommandList: ListLifecycle<Allocator = D::CommandAllocator> + GpuCommandList<D::Texture> + Send + Sync + 'static,
    D::CommandAllocator: Send + 'static,
    D::Fence: Send + Sync + 'static,
    D::Queue: Send + Sync + 'static,
    N: NativeSwapchain<Buffer = D::Texture> + Send + Sync + 'static,
{
    shared: Arc<Shared<D, N>>,
    thread: Mutex<Option<JoinHandle<()>>>,
    queue_key: usize,
    queues: Arc<SharedPresentQueues<D>>,
}

impl<D, N> Presenter<D, N>
where
    D: GpuDevice + Send + Sync + 'static,
    D::Texture: Send + 'static,
    D::CommandList: ListLifecycle<Allocator = D::CommandAllocator> + GpuCommandList<D::Texture> + Send + Sync + 'static,
    D::CommandAllocator: Send + 'static,
    D::Fence: Send + Sync + 'static,
    D::Queue: Send + Sync + 'static,
    N: NativeSwapchain<Buffer = D::Texture> + Send + Sync + 'static,
{
    /// Creates everything that can fail (queue, lists, fence, thread) without touching a
    /// swapchain. `why` receives a reason on failure.
    #[allow(clippy::too_many_arguments)]
    pub fn create(
        device: Arc<D>,
        queues: Arc<SharedPresentQueues<D>>,
        queue_key: usize,
        native: Arc<N>,
        clock: Arc<dyn Clock>,
        guard: TearGuard,
        scanline: Option<Arc<dyn ScanlineSource>>,
        lead_seconds: f64,
        overlay: Arc<dyn OverlayHook<D::Texture>>,
        why: &mut String,
    ) -> Option<Self> {
        let queue = match queues.acquire(queue_key, &device) {
            Ok(queue) => queue,
            Err(_) => {
                *why = "present queue setup failed".to_owned();
                return None;
            }
        };
        let mut allocators = Vec::with_capacity(2);
        let mut lists = Vec::with_capacity(2);
        for _ in 0..2 {
            let allocator = match device.create_command_allocator(QueueKind::Direct) {
                Ok(allocator) => allocator,
                Err(_) => {
                    queues.release(queue_key, true);
                    *why = "present command allocator creation failed".to_owned();
                    return None;
                }
            };
            let list = match device.create_command_list(&allocator) {
                Ok(list) => {
                    list
                }
                Err(_) => {
                    queues.release(queue_key, true);
                    *why = "present command list creation failed".to_owned();
                    return None;
                }
            };
            allocators.push(allocator);
            lists.push(list);
        }
        let fence = match device.create_fence(0) {
            Ok(fence) => Arc::new(fence),
            Err(_) => {
                queues.release(queue_key, true);
                *why = "present fence creation failed".to_owned();
                return None;
            }
        };
        let output = native
            .containing_output()
            .map(|desc| DisplayMode::new(desc.refresh_hz, desc.height, desc.width, desc.rotated_quarter_turn, lead_seconds))
            .unwrap_or_default();
        let shared = Arc::new(Shared {
            _device: Arc::clone(&device),
            queue,
            native,
            fence,
            clock,
            scanline,
            overlay: Mutex::new(overlay),
            guard,
            mode: Mutex::new(output),
            lead_seconds,
            state: Mutex::new(ThreadState {
                allocators: [allocators.remove(0), allocators.remove(0)],
                lists: [lists.remove(0), lists.remove(0)],
                next_list: 0,
                latency: LatencyTracker::default(),
                window: PresentWindow::default(),
                last_flip: 0.0,
                last_result: 0,
                stop: false,
                cancel: false,
            }),
            jobs: Mutex::new(Queue { jobs: std::collections::VecDeque::with_capacity(PRESENTER_QUEUE_CAPACITY), submitted: 0, completed: 0 }),
            wake: Condvar::new(),
            done: Condvar::new(),
            counters: Arc::new(PresenterCounters::default()),
            fence_value: AtomicU64::new(0),
        });
        let worker = Arc::clone(&shared);
        let thread = match std::thread::Builder::new()
            .name("Sedici FG presenter".to_owned())
            .spawn(move || run(worker))
        {
            Ok(thread) => thread,
            Err(_) => {
                queues.release(queue_key, true);
                *why = "presenter thread creation failed".to_owned();
                return None;
            }
        };
        Some(Self { shared, thread: Mutex::new(Some(thread)), queue_key, queues })
    }

    /// Jobs submitted and not yet finished, including the one being presented.
    pub fn outstanding(&self) -> usize {
        let jobs = lock(&self.shared.jobs);
        (jobs.submitted - jobs.completed) as usize
    }

    /// Blocks until at most `max` jobs are outstanding; false on timeout.
    pub fn wait_outstanding(&self, max: usize, timeout: Duration) -> bool {
        let jobs = lock(&self.shared.jobs);
        let waited = self
            .shared
            .done
            .wait_timeout_while(jobs, timeout, |jobs| jobs.submitted - jobs.completed > max as u64)
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        waited.0.submitted - waited.0.completed <= max as u64
    }

    /// Rejects stopped or unqueued jobs rather than silently dropping them.
    pub fn submit(&self, job: PresentJob<D>) -> Result<(), PresentError> {
        {
            let state = lock(&self.shared.state);
            let mut jobs = lock(&self.shared.jobs);
            if state.stop {
                return Err(PresentError::Stopped);
            }
            if jobs.jobs.len() >= PRESENTER_QUEUE_CAPACITY {
                return Err(PresentError::QueueFull);
            }
            jobs.jobs.push_back(job);
            jobs.submitted += 1;
        }
        self.shared.wake.notify_one();
        Ok(())
    }

    /// Requests cancellation without waiting for a worker blocked in the driver.
    pub fn request_stop(&self) {
        {
            let mut state = lock(&self.shared.state);
            state.stop = true;
            state.cancel = true;
        }
        self.shared.wake.notify_all();
    }

    pub fn set_output_hooks(&self, hooks: Arc<dyn OverlayHook<D::Texture>>) {
        *lock(&self.shared.overlay) = hooks;
    }

    /// The most recent native Present's result.
    pub fn last_result(&self) -> Result<(), u32> {
        let state = lock(&self.shared.state);
        if state.last_result == 0 {
            Ok(())
        } else {
            Err(state.last_result)
        }
    }

    /// Waits for the GPU to retire everything the presenter queued. The caller must have waited
    /// for zero outstanding jobs first.
    pub fn drain_gpu(&self) -> Result<(), GpuError> {
        let value = self.shared.fence_value.fetch_add(1, Ordering::AcqRel) + 1;
        self.shared.queue.signal(self.shared.fence.as_ref(), value)?;
        self.shared.fence.as_ref().wait(value, GPU_TIMEOUT)
    }

    /// ResizeBuffers1 on the native chain, keeping it on the present queue. Only call with zero
    /// outstanding jobs and a drained GPU.
    pub fn resize_native(
        &self,
        count: u32,
        width: u32,
        height: u32,
        format: crate::application::ports::gpu::PixelFormat,
        flags: u32,
    ) -> Result<(), GpuError> {
        let count = if count == 0 { self.shared.native.desc()?.buffer_count } else { count };
        let masks = vec![1_u32; count as usize];
        self.shared.native.resize_buffers1(count, width, height, format, flags, &masks)?;
        self.refresh_output();
        Ok(())
    }

    /// Re-reads the containing output: refresh period, active rows and lead rows.
    pub fn refresh_output(&self) {
        let Some(output) = self.shared.native.containing_output() else { return };
        let mode = DisplayMode::new(
            output.refresh_hz,
            output.height,
            output.width,
            output.rotated_quarter_turn,
            self.shared.lead_seconds,
        );
        let mut current = lock(&self.shared.mode);
        *current = DisplayMode { scanline_available: current.scanline_available, ..mode };
    }

    pub fn refresh_hz(&self) -> f64 {
        lock(&self.shared.mode).refresh_hz()
    }

    pub fn counters(&self) -> Arc<PresenterCounters> {
        Arc::clone(&self.shared.counters)
    }

    /// The current presenter window, for tests and the periodic stats.
    pub fn window(&self) -> PresentWindow {
        lock(&self.shared.state).window.clone()
    }
}

impl<D, N> Drop for Presenter<D, N>
where
    D: GpuDevice + Send + Sync + 'static,
    D::Texture: Send + 'static,
    D::CommandList: ListLifecycle<Allocator = D::CommandAllocator> + GpuCommandList<D::Texture> + Send + Sync + 'static,
    D::CommandAllocator: Send + 'static,
    D::Fence: Send + Sync + 'static,
    D::Queue: Send + Sync + 'static,
    N: NativeSwapchain<Buffer = D::Texture> + Send + Sync + 'static,
{
    fn drop(&mut self) {
        self.request_stop();
        if let Some(thread) = lock(&self.thread).take() {
            let _ = thread.join();
        }
        let idle = self.drain_gpu().is_ok();
        let window = lock(&self.shared.state).window.clone();
        if window.jobs > 0 {
            log_window(&window);
        }
        self.queues.release(self.queue_key, idle);
    }
}

fn now_seconds(clock: &dyn Clock) -> f64 {
    let frequency = clock.frequency();
    if frequency == 0 {
        0.0
    } else {
        clock.now() as f64 / frequency as f64
    }
}

/// Sleeps most of a bounded wait, then spins the last window against the absolute deadline. Never
/// parks the thread past 0.1 s, so a broken interval cannot stall presentation.
fn wait_until(clock: &dyn Clock, deadline: f64) {
    let frequency = clock.frequency();
    if frequency == 0 || !deadline.is_finite() {
        return;
    }
    let now = clock.now();
    let remaining = deadline - now as f64 / frequency as f64;
    if !(remaining > 0.0) {
        return;
    }
    let remaining = remaining.min(0.1);
    let target = now.saturating_add(((remaining * frequency as f64) as u64).max(1));
    if !clock.wait_until(target) {
        return;
    }
    let spin = (0.0002 * frequency as f64) as u64 + 1;
    let limit = clock.now().saturating_add(spin);
    while (clock.now() as f64 / frequency as f64) < deadline && clock.now() < limit {
        std::hint::spin_loop();
    }
}

fn run<D, N>(shared: Arc<Shared<D, N>>)
where
    D: GpuDevice + Send + Sync + 'static,
    D::Texture: Send + 'static,
    D::CommandList: ListLifecycle<Allocator = D::CommandAllocator> + GpuCommandList<D::Texture> + Send + Sync + 'static,
    D::CommandAllocator: Send + 'static,
    D::Fence: Send + Sync + 'static,
    D::Queue: Send + Sync + 'static,
    N: NativeSwapchain<Buffer = D::Texture> + Send + Sync + 'static,
{
    loop {
        let job = {
            let mut jobs = lock(&shared.jobs);
            loop {
                let cancel = lock(&shared.state).cancel;
                if cancel {
                    // Cancelled (parking or teardown): jobs that never started are discarded and
                    // counted as completed, so a parked swapchain can still be reaped (K48).
                    while jobs.jobs.pop_front().is_some() {
                        jobs.completed += 1;
                    }
                    drop(jobs);
                    shared.done.notify_all();
                    return;
                }
                if let Some(job) = jobs.jobs.pop_front() {
                    break job;
                }
                if lock(&shared.state).stop {
                    return;
                }
                jobs = shared.wake.wait(jobs).unwrap_or_else(|poisoned| poisoned.into_inner());
            }
        };
        let result = present_job(&shared, job);
        {
            let mut state = lock(&shared.state);
            state.last_result = match result {
                Ok(()) => 0,
                Err(error) => error.code(),
            };
            state.window.jobs += 1;
        }
        {
            lock(&shared.jobs).completed += 1;
        }
        shared.done.notify_all();
        let log_now = {
            let mut state = lock(&shared.state);
            if state.window.jobs >= 300 {
                log_window(&state.window);
                state.window = PresentWindow::default();
                true
            } else {
                false
            }
        };
        let _ = log_now;
    }
}

fn present_job<D, N>(shared: &Arc<Shared<D, N>>, job: PresentJob<D>) -> Result<(), PresentError>
where
    D: GpuDevice,
    D::CommandList: ListLifecycle<Allocator = D::CommandAllocator> + GpuCommandList<D::Texture>,
    N: NativeSwapchain<Buffer = D::Texture>,
{
    // Whether the frame was already finished before the presenter got to it: its readiness time is
    // then only an upper bound, and arrival time would count queueing as GPU latency.
    let already_ready = job.ready.as_ref().completed() >= job.ready_value;
    let dequeued = now_seconds(shared.clock.as_ref());
    shared.queue.wait(job.ready.as_ref(), job.ready_value).map_err(|error| match error {
        GpuError::DeviceRemoved { .. } => PresentError::DeviceHung,
        _ => PresentError::Gpu,
    })?;

    note_idle_scan(shared);

    for index in 0..job.count {
        let Some(output) = job.outputs[index].as_ref() else { continue };
        let real = output.real;
        let content = output.content;
        let gpu_start = now_seconds(shared.clock.as_ref());
        match output.image.as_ref() {
            None => copy_to_native(shared, &job.real, ResourceState::PRESENT, false)?,
            Some(image) => copy_to_native(shared, image, ResourceState::COPY_SOURCE, true)?,
        }
        wait_presenter_fence(shared)?;
        let ready = now_seconds(shared.clock.as_ref());
        let latency = {
            let mut state = lock(&shared.state);
            if index == 0 {
                let gpu_wait = ready - gpu_start;
                state.window.gpu_waits += 1;
                state.window.gpu_wait_sum += gpu_wait;
                state.window.gpu_wait_max = state.window.gpu_wait_max.max(gpu_wait);
                if already_ready {
                    state.latency.track(job.entry - content, dequeued - job.entry, false);
                } else {
                    state.latency.track(job.entry - content, ready - job.entry, true);
                }
            }
            state.latency.latency()
        };
        // Each output keeps its place on the content clock, so on-screen spacing follows the
        // game's frame times and the schedule's grid.
        let target = content + latency;
        if later_output_due(shared, &job, index, latency, ready) {
            // A later output is already due: showing this one would put the schedule behind.
            let mut state = lock(&shared.state);
            if real {
                state.window.reals_dropped += 1;
            } else {
                state.window.generated_dropped += 1;
            }
            drop(state);
            if !real {
                shared.counters.generated_dropped.fetch_add(1, Ordering::Relaxed);
            }
            continue;
        }
        hold(shared, target);
        let flip = now_seconds(shared.clock.as_ref());
        {
            let mut state = lock(&shared.state);
            if flip > target + 0.001 {
                state.window.late += 1;
            }
        }
        native_present(shared, &job, !real)?;
        {
            let mut state = lock(&shared.state);
            if real {
                state.window.reals_shown += 1;
            } else {
                state.window.generated_shown += 1;
                state.window.latency_sum += latency;
                state.window.latency_samples += 1;
            }
        }
        if !real {
            shared.counters.generated_shown.fetch_add(1, Ordering::Relaxed);
        }
    }

    // Retirement point for drain_gpu; also covers DXGI's own present work.
    let value = shared.fence_value.fetch_add(1, Ordering::AcqRel) + 1;
    shared.queue.signal(shared.fence.as_ref(), value).map_err(|_| PresentError::Gpu)?;
    Ok(())
}

fn note_idle_scan<D, N>(shared: &Arc<Shared<D, N>>)
where
    D: GpuDevice,
    D::CommandList: ListLifecycle<Allocator = D::CommandAllocator> + GpuCommandList<D::Texture>,
    N: NativeSwapchain<Buffer = D::Texture>,
{
    // VRR evidence: after more than one refresh without a flip a VRR panel sits in its blank
    // waiting, while a fixed-refresh panel is in blank about 5% of the time.
    let (last_flip, period) = {
        let state = lock(&shared.state);
        (state.last_flip, lock(&shared.mode).period)
    };
    if last_flip > 0.0 && now_seconds(shared.clock.as_ref()) - last_flip > period + 0.0005 {
        if let Some(scanline) = read_scanline(shared) {
            let mut state = lock(&shared.state);
            state.window.idle_samples += 1;
            if scanline.in_vertical_blank {
                state.window.idle_blank += 1;
            }
        }
    }
}

fn later_output_due<D, N>(
    shared: &Arc<Shared<D, N>>,
    job: &PresentJob<D>,
    index: usize,
    latency: f64,
    now: f64,
) -> bool
where
    D: GpuDevice,
    D::CommandList: ListLifecycle<Allocator = D::CommandAllocator> + GpuCommandList<D::Texture>,
    N: NativeSwapchain<Buffer = D::Texture>,
{
    if let Some(next) = job.outputs.get(index + 1).and_then(Option::as_ref) {
        return superseded(true, next.content, latency, now);
    }
    let jobs = lock(&shared.jobs);
    match jobs.jobs.front().and_then(|next| next.outputs.first()).and_then(Option::as_ref) {
        Some(next) => superseded(true, next.content, latency, now),
        None => superseded(false, 0.0, latency, now),
    }
}

fn read_scanline<D, N>(shared: &Arc<Shared<D, N>>) -> Option<Scanline>
where
    D: GpuDevice,
    D::CommandList: ListLifecycle<Allocator = D::CommandAllocator> + GpuCommandList<D::Texture>,
    N: NativeSwapchain<Buffer = D::Texture>,
{
    let source = shared.scanline.as_ref()?;
    if !lock(&shared.mode).scanline_available {
        return None;
    }
    source.read()
}

/// Waits for the guard's floor and `not_before` and, with the scan-line guard, until the panel is
/// in its blank or within `lead` of it.
fn hold<D, N>(shared: &Arc<Shared<D, N>>, not_before: f64)
where
    D: GpuDevice,
    D::CommandList: ListLifecycle<Allocator = D::CommandAllocator> + GpuCommandList<D::Texture>,
    N: NativeSwapchain<Buffer = D::Texture>,
{
    let (mode, guard, last_flip) = {
        let state = lock(&shared.state);
        (lock(&shared.mode).clone(), shared.guard, state.last_flip)
    };
    let scanline_available = mode.scanline_available && shared.scanline.is_some();
    let scheduled = not_before.max(now_seconds(shared.clock.as_ref()));
    let floor = guard_floor(last_flip, mode.period, shared.lead_seconds, guard, scanline_available);
    let early = guard_early(scanline_available, guard, mode.period, not_before);
    wait_until(shared.clock.as_ref(), (not_before - early).max(floor));
    let waited = (now_seconds(shared.clock.as_ref()) - scheduled).max(0.0);
    if floor > scheduled && waited > 0.0001 {
        let mut state = lock(&shared.state);
        state.window.guard_holds += 1;
        state.window.guard_sum += waited;
        state.window.guard_max = state.window.guard_max.max(waited);
    }

    if !(guard == TearGuard::Scanline && scanline_available) {
        return;
    }
    let hold_start = now_seconds(shared.clock.as_ref());
    let give_up = hold_start + mode.period + 0.002;
    let open = mode.active_lines.saturating_sub(mode.lead_lines);
    loop {
        match read_scanline(shared) {
            Some(scanline) if scanline.in_vertical_blank || scanline.line >= open => break,
            Some(scanline) => {
                let now = now_seconds(shared.clock.as_ref());
                if now > give_up {
                    lock(&shared.state).window.guard_timeouts += 1;
                    break;
                }
                let eta = (f64::from(open.saturating_sub(scanline.line)) / f64::from(mode.active_lines.max(1)))
                    * mode.period;
                if eta > 0.002 {
                    wait_until(shared.clock.as_ref(), now + eta - 0.001);
                } else {
                    std::hint::spin_loop();
                }
            }
            None => {
                let mut current = lock(&shared.mode);
                if current.scanline_available {
                    current.scanline_available = false;
                    drop(current);
                    tracing::info!("fg: scan-line read failed; tear guard falls back to \"timer\"");
                }
                break;
            }
        }
    }
    let held = now_seconds(shared.clock.as_ref()) - hold_start;
    if held > 0.0001 {
        let mut state = lock(&shared.state);
        state.window.guard_holds += 1;
        state.window.guard_sum += held;
        state.window.guard_max = state.window.guard_max.max(held);
    }
}

fn copy_to_native<D, N>(
    shared: &Arc<Shared<D, N>>,
    source: &D::Texture,
    state: ResourceState,
    generated: bool,
) -> Result<(), PresentError>
where
    D: GpuDevice,
    D::CommandList: ListLifecycle<Allocator = D::CommandAllocator> + GpuCommandList<D::Texture>,
    N: NativeSwapchain<Buffer = D::Texture>,
{
    let destination = shared
        .native
        .back_buffer(shared.native.current_back_buffer_index())
        .map_err(|_| PresentError::Gpu)?;
    let cancel = lock(&shared.state).cancel;
    {
        let mut thread = lock(&shared.state);
        let slot = thread.next_list;
        thread.next_list ^= 1;
        thread.allocators[slot].reset().map_err(|_| PresentError::Gpu)?;
        let list = &thread.lists[slot];
        list.reset_list(&thread.allocators[slot]).map_err(|_| PresentError::Gpu)?;
        list.transition(&destination, ResourceState::PRESENT, ResourceState::COPY_DEST);
        if state != ResourceState::COPY_SOURCE {
            list.transition(source, state, ResourceState::COPY_SOURCE);
        }
        list.copy_resource(&destination, source);
        list.transition(&destination, ResourceState::COPY_DEST, ResourceState::PRESENT);
        if state != ResourceState::COPY_SOURCE {
            list.transition(source, ResourceState::COPY_SOURCE, state);
        }
        if !cancel {
            let overlay = Arc::clone(&lock(&shared.overlay));
            overlay.draw(list, &destination, generated);
        }
        list.close_list().map_err(|_| PresentError::Gpu)?;
        shared.queue.execute(&[list]).map_err(|_| PresentError::Gpu)?;
    }
    let value = shared.fence_value.fetch_add(1, Ordering::AcqRel) + 1;
    shared.queue.signal(shared.fence.as_ref(), value).map_err(|_| PresentError::Gpu)?;
    Ok(())
}

fn wait_presenter_fence<D, N>(shared: &Arc<Shared<D, N>>) -> Result<(), PresentError>
where
    D: GpuDevice,
    D::CommandList: ListLifecycle<Allocator = D::CommandAllocator> + GpuCommandList<D::Texture>,
    N: NativeSwapchain<Buffer = D::Texture>,
{
    let value = shared.fence_value.load(Ordering::Acquire);
    shared.fence.as_ref().wait(value, GPU_TIMEOUT).map_err(|error| match error {
        GpuError::Timeout | GpuError::DeviceRemoved { .. } => PresentError::DeviceHung,
        _ => PresentError::Gpu,
    })
}

fn native_present<D, N>(shared: &Arc<Shared<D, N>>, job: &PresentJob<D>, generated: bool) -> Result<(), PresentError>
where
    D: GpuDevice,
    D::CommandList: ListLifecycle<Allocator = D::CommandAllocator> + GpuCommandList<D::Texture>,
    N: NativeSwapchain<Buffer = D::Texture>,
{
    let before = now_seconds(shared.clock.as_ref());
    let mode = lock(&shared.mode).clone();
    match read_scanline(shared) {
        Some(scanline) => {
            let quarter = (4 * scanline.line / mode.active_lines.max(1)).min(3) as usize;
            let mut state = lock(&shared.state);
            let index = if scanline.in_vertical_blank { 0 } else { 1 + quarter };
            state.window.scan[index] += 1;
        }
        None => lock(&shared.state).window.scan[5] += 1,
    }
    {
        let mut state = lock(&shared.state);
        if state.last_flip > 0.0 {
            let gap = before - state.last_flip;
            if gap < mode.period - 0.0001 {
                state.window.short_gaps += 1;
            }
            if gap < 0.002 {
                state.window.back_to_back += 1;
                shared.counters.back_to_back.fetch_add(1, Ordering::Relaxed);
            }
            if state.window.flips == 0 || gap < state.window.gap_min {
                state.window.gap_min = gap;
            }
        }
        state.window.flips += 1;
        state.last_flip = before;
    }
    let result = match job.params.as_ref() {
        Some(params) => shared.native.present1(job.sync, job.flags, params),
        None => shared.native.present(job.sync, job.flags),
    };
    let call = now_seconds(shared.clock.as_ref()) - before;
    {
        let mut state = lock(&shared.state);
        state.window.call_sum += call;
        state.window.call_max = state.window.call_max.max(call);
    }
    result.map_err(|_| PresentError::DeviceHung)?;
    if !lock(&shared.state).cancel {
        let overlay = Arc::clone(&lock(&shared.overlay));
        overlay.presented(generated);
    }
    Ok(())
}

fn log_window(window: &PresentWindow) {
    let average = |sum: f64, count: u32| if count > 0 { sum / f64::from(count) } else { 0.0 };
    let ms = |seconds: f64| seconds * 1000.0;
    tracing::info!(
        "fg: vrr present jobs={} generated{{shown={} dropped={}}} real{{shown={} dropped={}}} late={} latency_ms={:.2} short_gaps={}/{} back_to_back={} gap_min_ms={:.2} gpu_wait_ms{{avg={:.2} max={:.2}}} guard{{holds={} avg_ms={:.2} max_ms={:.2} timeouts={}}} present_call_ms{{avg={:.2} max={:.2}}} scan_at_flip{{blank={} q1={} q2={} q3={} q4={} unread={}}} idle_in_blank={}/{}",
        window.jobs,
        window.generated_shown,
        window.generated_dropped,
        window.reals_shown,
        window.reals_dropped,
        window.late,
        ms(average(window.latency_sum, window.latency_samples)),
        window.short_gaps,
        window.flips,
        window.back_to_back,
        ms(window.gap_min),
        ms(average(window.gpu_wait_sum, window.gpu_waits)),
        ms(window.gpu_wait_max),
        window.guard_holds,
        ms(average(window.guard_sum, window.guard_holds)),
        ms(window.guard_max),
        window.guard_timeouts,
        ms(average(window.call_sum, window.flips)),
        ms(window.call_max),
        window.scan[0],
        window.scan[1],
        window.scan[2],
        window.scan[3],
        window.scan[4],
        window.scan[5],
        window.idle_blank,
        window.idle_samples,
    );
}

#[cfg(test)]
mod tests {
    use super::*;
    use rstest::rstest;

    #[test]
    fn latency_uses_the_structural_maximum_and_gpu_median() {
        let mut tracker = LatencyTracker::default();
        for index in 0..8 {
            tracker.track(f64::from(index) * 0.001, 0.002, true);
        }
        assert!((tracker.latency() - (0.007 + 0.002 + LatencyTracker::SAFETY)).abs() < 1e-9);
        // The structural window is the last 8 frames, so one hitch holds the estimate only until it
        // ages out; the C++ behaviour is the same (finding 009).
        tracker.track(0.2, 0.002, true);
        assert!(tracker.latency() <= 0.2 + 0.01);
        for _ in 0..8 {
            tracker.track(0.0, 0.002, true);
        }
        assert!(tracker.latency() < 0.01, "the hitch ages out of the window");
    }

    #[test]
    fn an_unmeasured_gpu_estimate_only_lowers_the_latency() {
        let mut tracker = LatencyTracker::default();
        for _ in 0..16 {
            tracker.track(0.0, 0.010, true);
        }
        let high = tracker.latency();
        tracker.track(0.0, 0.020, false);
        assert!(tracker.latency() <= high);
        // The median only drops once enough lowering samples fill the window.
        for _ in 0..16 {
            tracker.track(0.0, 0.0, false);
        }
        assert!(tracker.latency() < high);
    }

    #[rstest]
    #[case(TearGuard::Timer, true, 1.0 / 60.0 + 0.0002)]
    #[case(TearGuard::Scanline, false, 1.0 / 60.0 + 0.0002)]
    fn timer_guard_keeps_a_full_refresh(#[case] guard: TearGuard, #[case] scanline: bool, #[case] want: f64) {
        let floor = guard_floor(10.0, 1.0 / 60.0, 0.00025, guard, scanline);
        assert!((floor - (10.0 + want)).abs() < 1e-9);
    }

    #[test]
    fn scanline_guard_reaches_back_before_the_previous_scan_ends() {
        let floor = guard_floor(10.0, 1.0 / 60.0, 0.00025, TearGuard::Scanline, true);
        assert!((floor - (10.0 + 1.0 / 60.0 - 0.00025 - 0.0005)).abs() < 1e-9);
        assert_eq!(guard_floor(0.0, 1.0 / 60.0, 0.00025, TearGuard::Scanline, true), 0.0);
    }

    #[rstest]
    #[case(TearGuard::Scanline, true, true)]
    #[case(TearGuard::Timer, true, false)]
    #[case(TearGuard::Scanline, false, false)]
    fn only_the_scanline_guard_accepts_an_early_blank(
        #[case] guard: TearGuard,
        #[case] scanline: bool,
        #[case] applies: bool,
    ) {
        let early = guard_early(scanline, guard, 1.0 / 60.0, 10.0);
        assert_eq!(early > 0.0, applies);
        assert!(early <= 0.0015 + 1e-9);
    }

    #[test]
    fn display_mode_derives_the_period_and_lead_rows() {
        let mode = DisplayMode::new(75.0, 1080, 1920, false, 0.00025);
        assert!((mode.period - 1.0 / 75.0).abs() < 1e-12);
        assert_eq!(mode.active_lines, 1080);
        assert_eq!(mode.lead_lines, (1080.0_f64 * (0.00025 * 75.0)).round() as u32);
        let rotated = DisplayMode::new(60.0, 1080, 1920, true, 0.00025);
        assert_eq!(rotated.active_lines, 1920);
        assert!((DisplayMode::new(0.0, 0, 0, false, 0.0).period - DisplayMode::DEFAULT_PERIOD).abs() < 1e-12);
    }

    /// K167: the C++ `Presenter::submit` returns `DXGI_ERROR_INVALID_CALL` when it has stopped and
    /// `DXGI_ERROR_WAS_STILL_DRAWING` when its ring is full. The port's `Stopped` briefly answered
    /// `E_INVALIDARG`, which the wrapper maps to `SwapchainError::InvalidArgument` instead of
    /// `InvalidCall`, so a game that failed a Present after a stopped presenter saw the wrong code.
    #[rstest]
    #[case(PresentError::Stopped, 0x887A_0001)]
    #[case(PresentError::QueueFull, 0x887A_000A)]
    #[case(PresentError::DeviceHung, 0x887A_0006)]
    #[case(PresentError::Gpu, 0x8000_4005)]
    fn present_errors_map_to_the_cpp_codes(#[case] error: PresentError, #[case] code: u32) {
        assert_eq!(error.code(), code);
    }
}
