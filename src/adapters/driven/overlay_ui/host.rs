//! The production overlay host: the [`OverlayHook`] the swapchain wrapper installs (ports
//! `ui/ui.cpp`'s `ui_on_present`/`ui_output_presented` and `ui/overlay.cpp`'s `record_present`).
//!
//! `draw` runs once per presented output, on the copy list the wrapper is about to submit, after
//! the frame has been generated and copied into the native back buffer. `presented` follows each
//! successful native Present (K156: a failed Present is never reported).
//!
//! # Never fail the game's call
//!
//! Every path here is bookkeeping. A renderer or settings failure disables the overlay for the
//! session with one `sedici: overlay disabled` line pair, gives the input back and keeps
//! presenting; the game never sees an error. The one exception in the C++ — allocation during
//! `Present1` metadata — belongs to the wrapper, not here.
//!
//! # Threads
//!
//! `draw`/`presented` run on whichever thread presents (the "vrr" presenter uses its own). The
//! window procedure runs on the game's window thread and only ever touches the input queue, the
//! live mode and the cursor guard, as the C++ did from `kHotkeyMessage`. Capture is entered only from
//! the window thread, because `CursorInterception` requires it.
//!
//! # Renderer lifetime
//!
//! The egui/D3D12 objects need the game's device and the back-buffer format, known only at the
//! first draw. They are created lazily there; a device or format change disables the overlay for
//! the session, as the C++ `ensure_imgui` did.
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};

use tracing::{error, info};

use crate::adapters::driven::d3d12::device::D3d12Device;
use crate::adapters::driven::settings_worker::NativeWorkerHost;
use crate::adapters::driving::window_input::{client_size, post_capture_message};
use crate::application::overlay::{
    active_line, cycle_mode, ActionPlan, OutputCadence, OverlayAction, OverlayFrame, OverlayState,
    OverlayTiming, OverlayView,
};
use crate::application::ports::clock::Clock;
use crate::application::ports::gpu::{GpuCommandList, GpuTexture};
use crate::application::ports::input::{CursorGuard, Hotkey, HotkeyEvent, KeyAction, WindowHandle};
use crate::application::ports::overlay::OverlayHook;
use crate::application::ports::ui_renderer::{
    OutputFormat, OverlayRenderer, RenderTarget, UiFrameInfo, UiInput,
};
use crate::application::settings::io::SettingsIo;
use crate::application::settings::live::Live;
use crate::application::settings::tx::SettingsTx;
use crate::application::stats::Stats;
use crate::domain::config::{Settings, UiMode};

use super::OverlayUi;

/// The rate limit for a render-path capture request; the C++ used half a QPC second.
const CAPTURE_RETRY_MS: f64 = 500.0;

/// What a drawn output looked like, so the first `overlay present callback active` line names the
/// resources the hook was given (the C++ printed the callback desc's fields).
#[derive(Clone, Debug)]
struct LastDraw {
    device: String,
    list: String,
    output: String,
    size: Option<(u32, u32)>,
    format: u32,
    state: u32,
}

/// The process-wide settings, timing and stats the overlay shares with the present hook.
///
/// One instance per DLL. The swapchain factory builds it from the loaded configuration and hands the
/// same `Arc` to every host and every present hook.
pub struct OverlayServices {
    pub live: Arc<Live>,
    pub tx: Arc<SettingsTx>,
    pub io: Arc<SettingsIo>,
    stats: Mutex<Stats>,
    timing: Mutex<OverlayTiming>,
    clock: Arc<dyn Clock>,
}

impl OverlayServices {
    /// Builds the services over the loaded configuration. No file is touched until the menu asks
    /// for an edit baseline; the transaction starts from the acknowledged values.
    pub fn new(config: Settings, clock: Arc<dyn Clock>) -> Arc<Self> {
        let store: Arc<dyn crate::application::ports::config_store::ConfigStore + Send + Sync> =
            Arc::new(crate::adapters::driven::config_toml::TomlConfigStore::beside_module());
        Self::with_store(config, clock, store)
    }

    /// As [`new`](Self::new) over an injected store (the offline tests use a memory store so they
    /// never touch a real `sedici.toml`).
    pub fn with_store(
        config: Settings,
        clock: Arc<dyn Clock>,
        store: Arc<dyn crate::application::ports::config_store::ConfigStore + Send + Sync>,
    ) -> Arc<Self> {
        let live = Arc::new(Live::new());
        live.initialize(&config);
        // P4.5: the log level follows the live value without diagnostics reading settings.
        live.set_level_sink(|level| {
            crate::adapters::driven::logging::set_level(
                crate::adapters::driven::logging::LogLevel::from_int(level),
            );
        });
        let tx = Arc::new(SettingsTx::new(Arc::clone(&live), config, store));
        let port: Arc<dyn crate::application::settings::io::SettingsIoPort> =
            Arc::clone(&tx) as Arc<dyn crate::application::settings::io::SettingsIoPort>;
        let io = Arc::new(SettingsIo::new(port, Arc::new(NativeWorkerHost::new())));
        Arc::new(Self {
            live,
            tx,
            io,
            stats: Mutex::new(Stats::new(clock.frequency())),
            timing: Mutex::new(OverlayTiming::new()),
            clock,
        })
    }

    /// One application Present: the real-frame timing ring and the present-interval stats, in the
    /// order the C++ `sedici_game_present` used. Installed as the wrapper's per-Present hook.
    pub fn on_real_present(&self, qpc: u64) {
        let frequency = self.clock.frequency();
        lock(&self.timing).on_real_present(qpc, frequency);
        lock(&self.stats).on_present(qpc);
    }

    /// A swapchain create: starts a fresh cadence window so a report never mixes sizes (K123).
    pub fn on_swapchain_event(&self, kind: &'static str) {
        let now = self.clock.now();
        lock(&self.stats).on_swapchain_event(kind, now);
    }
}

/// The host's menu state, cadence and last draw, behind one lock (the C++ `g_ui` + `g_window`).
struct Menu {
    state: OverlayState,
    cadence: OutputCadence,
    window: WindowHandle,
    /// QPC of the previous overlay frame, for egui's animation clock.
    last_frame_qpc: u64,
    /// The last render-path capture request, for the C++ half-second gate.
    last_capture_request_ms: f64,
    last_draw: Option<LastDraw>,
}

/// The production overlay host. One per wrapped swapchain.
pub struct OverlayHost {
    services: Option<Arc<OverlayServices>>,
    device: Option<D3d12Device>,
    renderer: Mutex<OverlayUi>,
    menu: Mutex<Menu>,
    input: Mutex<Vec<crate::adapters::driving::window_input::InputMessage>>,
    guard: Arc<dyn CursorGuard>,
    disabled: AtomicBool,
    disabled_reason: Mutex<Option<String>>,
    draws_real: AtomicU64,
    draws_generated: AtomicU64,
    presented_real: AtomicU64,
    presented_generated: AtomicU64,
}

impl Default for OverlayHost {
    fn default() -> Self {
        Self::new(None)
    }
}

impl std::fmt::Debug for OverlayHost {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("OverlayHost")
            .field("disabled", &self.disabled.load(Ordering::Relaxed))
            .field("counters", &self.counters())
            .finish()
    }
}

/// The counters a status query (and the offline tests) read, porting `ui_render_status`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct OverlayStatus {
    pub disabled: bool,
    /// Successful overlay records, `(real, generated)`.
    pub draws: (u64, u64),
    /// Successful Presents the hook saw, `(real, generated)`.
    pub presented: (u64, u64),
}

impl OverlayHost {
    /// A host with no services: the empty hook a cleared wrapper could install. It draws nothing
    /// and counts what it was shown.
    pub fn new(services: Option<Arc<OverlayServices>>) -> Self {
        Self {
            services,
            device: None,
            renderer: Mutex::new(OverlayUi::new()),
            menu: Mutex::new(Menu {
                state: OverlayState::new(),
                cadence: OutputCadence::new(),
                window: WindowHandle::NONE,
                last_frame_qpc: 0,
                last_capture_request_ms: f64::NEG_INFINITY,
                last_draw: None,
            }),
            input: Mutex::new(Vec::new()),
            guard: Arc::new(crate::adapters::driven::win32::iat::shared()),
            disabled: AtomicBool::new(false),
            disabled_reason: Mutex::new(None),
            draws_real: AtomicU64::new(0),
            draws_generated: AtomicU64::new(0),
            presented_real: AtomicU64::new(0),
            presented_generated: AtomicU64::new(0),
        }
    }

    /// The production host over the composition root's services, the game's device and window, and
    /// the real cursor interception.
    pub fn production(
        services: Arc<OverlayServices>,
        device: D3d12Device,
        window: WindowHandle,
    ) -> Arc<Self> {
        let mut host = Self::new(Some(services));
        host.device = Some(device);
        lock(&host.menu).window = window;
        Arc::new(host)
    }

    /// A host over injected services, device, window and guard; the offline-test constructor.
    pub fn for_test(
        services: Arc<OverlayServices>,
        device: D3d12Device,
        window: WindowHandle,
        guard: Arc<dyn CursorGuard>,
    ) -> Self {
        let mut host = Self::new(Some(services));
        host.device = Some(device);
        host.guard = guard;
        lock(&host.menu).window = window;
        host
    }

    /// Builds services over an injected store, so a test never reads or writes `sedici.toml`
    /// beside the test binary.
    #[allow(dead_code)] // the offline host supplies it; the production factory uses `new`
    pub fn services_with_store(
        config: Settings,
        clock: Arc<dyn Clock>,
        store: Arc<dyn crate::application::ports::config_store::ConfigStore + Send + Sync>,
    ) -> Arc<OverlayServices> {
        OverlayServices::with_store(config, clock, store)
    }

    /// The hotkey the window subclass matches, from the loaded `[ui] hotkey`.
    pub fn hotkey(&self) -> Hotkey {
        self.services
            .as_ref()
            .map_or_else(Hotkey::default, |services| {
                Hotkey::parse(&services.tx.config().ui.hotkey)
            })
    }

    pub fn window(&self) -> WindowHandle {
        lock(&self.menu).window
    }

    /// Binds the game window the menu input and cursor capture belong to.
    #[allow(dead_code)] // a second swapchain rebinds inside `production`; kept for the offline host
    pub fn bind_window(&self, window: WindowHandle) {
        lock(&self.menu).window = window;
    }

    /// The counters the status query reads, `((real, generated) draws, (real, generated) presents)`.
    pub fn counters(&self) -> ((u64, u64), (u64, u64)) {
        (
            (
                self.draws_real.load(Ordering::Relaxed),
                self.draws_generated.load(Ordering::Relaxed),
            ),
            (
                self.presented_real.load(Ordering::Relaxed),
                self.presented_generated.load(Ordering::Relaxed),
            ),
        )
    }

    pub fn status(&self) -> OverlayStatus {
        let (draws, presented) = self.counters();
        OverlayStatus {
            disabled: self.disabled.load(Ordering::Acquire),
            draws,
            presented,
        }
    }

    pub fn disabled(&self) -> bool {
        self.disabled.load(Ordering::Acquire)
    }

    pub fn disabled_reason(&self) -> Option<String> {
        lock(&self.disabled_reason).clone()
    }

    /// Permanently disables the overlay for the session (ports `ui_disable`): stops drawing and
    /// input capture, publishes Off, asks the window thread to give the cursor back, and logs the
    /// one line pair `tools/analyze_log.ps1` counts.
    pub fn disable(&self, reason: &str) {
        if self.disabled.swap(true, Ordering::AcqRel) {
            return;
        }
        {
            let mut slot = lock(&self.disabled_reason);
            if slot.is_none() {
                *slot = Some(reason.to_owned());
            }
        }
        if let Some(services) = &self.services {
            services.live.set_ui_mode(UiMode::Off);
        }
        self.guard.disable();
        // The process-wide window subclass stops consuming input too (the C++ `ui_input_disable`).
        crate::adapters::driving::window_input::disable_global();
        let window = self.window();
        if !window.is_none() {
            post_capture_message(window);
        }
        error!("sedici: overlay disabled");
        error!("{reason}");
    }

    /// The mode the live settings hold.
    pub fn mode(&self) -> UiMode {
        self.services
            .as_ref()
            .map_or(UiMode::Off, |services| services.live.ui_mode())
    }

    /// Installs the window subclass over this host's window (ports `ui_capture_window`).
    pub fn install_input(self: &Arc<Self>) {
        let window = self.window();
        if window.is_none() {
            return;
        }
        crate::adapters::driving::window_input::capture_window_global(
            self.hotkey(),
            window,
            Arc::clone(self) as Arc<dyn crate::adapters::driving::window_input::OverlayInputHost>,
        );
    }

    // -- Per-output work -------------------------------------------------------------------

    /// The menu pass for one output: process I/O completions, apply the mode transition, start a
    /// pending baseline job, and build the view. The caller records the view afterwards.
    fn frame(&self, target: &RenderTarget, frame: UiFrameInfo) -> Option<OverlayView> {
        let services = self.services.as_ref()?;
        let mode = services.live.ui_mode();
        // Only Full capture suppresses the game's cursor calls (the C++ `set_full_mode`).
        crate::adapters::driven::win32::iat::set_full_mode(mode == UiMode::Full);
        let window = lock(&self.menu).window;
        let output = {
            let stats = lock(&services.stats);
            let timing = lock(&services.timing);
            let mut menu = lock(&self.menu);
            menu.state.frame(OverlayFrame {
                mode,
                window,
                output_size: (target.width, target.height),
                client_size: client_size(window),
                delta_seconds: frame.delta_seconds,
                frame_generation_on: crate::application::ffx_model::service()
                    .is_some_and(|service| service.any_generation_enabled()),
                live: &services.live,
                tx: &services.tx,
                stats: &stats,
                timing: &timing,
                io: &services.io,
            })
        };
        if let Some(mode) = output.mode {
            services.live.set_ui_mode(mode);
        }
        if let Some((generation, epoch)) = output.begin {
            if !services.io.start_begin(generation, epoch) {
                lock(&self.menu).state.mark_begin_failed();
            }
        }
        if let Some(status) = output.status {
            info!("{status}");
        }
        if output.mode.is_some() && !window.is_none() {
            post_capture_message(window);
        }
        Some(output.view)
    }

    /// Applies the actions one drawn menu produced, in click order.
    fn apply_actions(&self, actions: Vec<OverlayAction>, mode: UiMode) {
        let Some(services) = &self.services else {
            return;
        };
        for action in actions {
            let plan: ActionPlan = {
                let mut menu = lock(&self.menu);
                menu.state.act(action, mode, &services.tx)
            };
            if let Some(mode) = plan.mode {
                services.live.set_ui_mode(mode);
            }
            if let Some(ticket) = plan.save {
                let epoch = lock(&self.menu).state.edit_epoch();
                let started = services.io.start_save(ticket, epoch);
                lock(&self.menu).state.note_save_started(started);
                if !started {
                    error!("sedici: Cannot start settings save");
                }
            }
            if let Some(status) = plan.status {
                info!("{status}");
            }
            if plan.mode.is_some() || plan.transition_from == Some(UiMode::Full) {
                let window = self.window();
                if !window.is_none() {
                    post_capture_message(window);
                }
            }
        }
    }

    /// Builds the render target for one output, if the frame names the objects a draw needs. The
    /// wrapper leaves the destination in `PRESENT` after its copy, which is the state the C++
    /// reported to the overlay callback.
    fn target<T: GpuTexture>(
        &self,
        list: &dyn GpuCommandList<T>,
        back_buffer: &T,
        generated: bool,
    ) -> Option<RenderTarget> {
        let info = back_buffer.info();
        if info.width == 0 || info.height == 0 {
            return None;
        }
        let device = self.device.as_ref()?;
        let identity = list.object_identity();
        if identity == 0 || back_buffer.identity() == 0 {
            return None;
        }
        Some(RenderTarget {
            command_list: identity,
            resource: back_buffer.identity(),
            device: crate::adapters::driven::overlay_ui::device_identity_of(device),
            width: info.width,
            height: info.height,
            format: OutputFormat(dxgi_format(info.format)),
            state: crate::application::ports::gpu::ResourceState::PRESENT,
            generated,
        })
    }

    /// Draws one output; returns whether a draw was recorded.
    fn draw_output<T: GpuTexture>(
        &self,
        list: &dyn GpuCommandList<T>,
        back_buffer: &T,
        generated: bool,
    ) -> bool {
        if self.disabled() {
            return false;
        }
        let Some(services) = &self.services else {
            return false;
        };
        let Some(target) = self.target(list, back_buffer, generated) else {
            return false;
        };
        let window = self.window();
        let (client_width, client_height) = client_size(window);
        let delta = self.frame_delta(services);
        let frame = UiFrameInfo {
            output_width: target.width,
            output_height: target.height,
            client_width,
            client_height,
            delta_seconds: delta,
        };
        let mode = services.live.ui_mode();
        let Some(view) = self.frame(&target, frame) else {
            return false;
        };
        {
            let mut menu = lock(&self.menu);
            menu.last_draw = Some(LastDraw {
                device: format!("0x{:x}", target.device),
                list: format!("0x{:x}", target.command_list),
                output: format!("0x{:x}", target.resource),
                size: Some((target.width, target.height)),
                format: target.format.0,
                state: target.state.0,
            });
        }
        if matches!(view, OverlayView::Off) {
            return false;
        }
        let input = self.take_input();
        let mut renderer = lock(&self.renderer);
        if !renderer.disabled() {
            if let Some(device) = &self.device {
                // The first draw is where the device and the back-buffer format are known (the C++
                // `ensure_imgui`). A failure disables the overlay for the session.
                if let Err(error) = renderer.initialize(device, target.format) {
                    renderer.disable(&error.reason());
                }
            }
        }
        let before = renderer.draw_counts();
        let actions = match renderer.draw(&target, &view, &input, frame) {
            Ok(actions) => actions,
            Err(error) => {
                let reason = error.reason();
                renderer.disable(&reason);
                drop(renderer);
                if error.is_permanent() {
                    self.disable(&reason);
                } else {
                    error!("sedici: overlay draw failed: {reason}");
                }
                return false;
            }
        };
        let after = renderer.draw_counts();
        drop(renderer);
        if generated {
            self.draws_generated
                .fetch_add(after.1.saturating_sub(before.1), Ordering::Relaxed);
        } else {
            self.draws_real
                .fetch_add(after.0.saturating_sub(before.0), Ordering::Relaxed);
        }
        self.apply_actions(actions, mode);
        after != before
    }

    /// Seconds since the previous overlay frame, clamped to the C++ 1..=100 ms window.
    fn frame_delta(&self, services: &OverlayServices) -> Option<f32> {
        let now = services.clock.now();
        let mut menu = lock(&self.menu);
        let previous = menu.last_frame_qpc;
        menu.last_frame_qpc = now;
        if previous == 0 || now <= previous {
            return None;
        }
        let elapsed = (now - previous) as f64 / services.clock.frequency().max(1) as f64;
        Some(elapsed.clamp(0.001, 0.1) as f32)
    }

    fn take_input(&self) -> Vec<UiInput> {
        std::mem::take(&mut *lock(&self.input))
            .into_iter()
            .map(|message| UiInput {
                msg: message.msg,
                wparam: message.wparam as u64,
                lparam: message.lparam as i64,
            })
            .collect()
    }

    /// One presented output: the output timing ring, the cadence window and the report lines
    /// (ports `record_present`).
    fn record_present(&self, generated: bool) {
        let Some(services) = &self.services else {
            return;
        };
        let now = services.clock.now();
        let frequency = services.clock.frequency();
        lock(&services.timing).on_output_present(now, frequency);
        let events = lock(&services.stats).swapchain_event_count();
        let report = {
            let mut menu = lock(&self.menu);
            menu.cadence.record(now, generated, events, frequency)
        };
        if report.first_output {
            // The C++ printed the callback's device, list, output, size, format and state once.
            let line = {
                let draw = lock(&self.menu).last_draw.clone();
                match draw {
                    Some(draw) => active_line(
                        &draw.device,
                        &draw.list,
                        &draw.output,
                        draw.size,
                        draw.format,
                        draw.state,
                        false,
                    ),
                    None => active_line("n/a", "n/a", "n/a", None, 0, 0, false),
                }
            };
            info!("{line}");
        }
        if let Some(line) = report.report {
            info!("{line}");
        }
    }

    /// The window procedure's decision on `kHotkeyMessage` (the C++ `ui_window_message`).
    pub fn on_capture_message(&self, window: WindowHandle) {
        let mode = self.mode();
        if self.disabled() || mode != UiMode::Full {
            self.guard.leave(true);
            return;
        }
        if !self.guard.available() {
            error!(
                "sedici: Full menu unavailable: the executable's cursor calls could not be \
                 intercepted; using Mini"
            );
            if let Some(services) = &self.services {
                services.live.set_ui_mode(UiMode::Mini);
            }
            return;
        }
        if !self.guard.active() && foreground_is(window) && !self.guard.enter(window) {
            tracing::debug!("sedici: Full menu capture refused for now");
        }
    }

    /// Asks the window thread to (re)evaluate capture; rate limited, as the C++ render-path retry.
    fn request_capture(&self, window: WindowHandle) {
        let Some(services) = &self.services else {
            return;
        };
        let now_ms = services.clock.ticks_to_ms(services.clock.now());
        {
            let mut menu = lock(&self.menu);
            if now_ms - menu.last_capture_request_ms < CAPTURE_RETRY_MS {
                return;
            }
            menu.last_capture_request_ms = now_ms;
        }
        post_capture_message(window);
    }

    /// A hotkey press: Off -> Mini -> Full -> Off, then the window thread re-evaluates capture.
    pub fn on_hotkey(&self, event: HotkeyEvent) {
        if event.action != KeyAction::Pressed || self.disabled() {
            return;
        }
        let Some(services) = &self.services else {
            return;
        };
        let next = cycle_mode(services.live.ui_mode());
        services.live.set_ui_mode(next);
        post_capture_message(event.window);
    }
}

impl<T: GpuTexture> OverlayHook<T> for OverlayHost {
    fn draw(&self, list: &dyn GpuCommandList<T>, back_buffer: &T, generated: bool) {
        // A panic in the overlay must never unwind into Present (the C++ caught every exception).
        let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            self.draw_output(list, back_buffer, generated)
        }));
    }

    fn presented(&self, generated: bool) {
        let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            // The C++ `overlay_output_presented` recorded the output cadence even after `ui_disable`:
            // the line is a property of presentation, not of the menu. Drawing stops instead.
            if generated {
                self.presented_generated.fetch_add(1, Ordering::Relaxed);
            } else {
                self.presented_real.fetch_add(1, Ordering::Relaxed);
            }
            self.record_present(generated);
            if self.disabled() {
                return;
            }
            // Full without capture (focus arrived after the hotkey) retries from the render path.
            let window = self.window();
            if self.mode() == UiMode::Full && !window.is_none() && !self.guard.active() {
                self.request_capture(window);
            }
        }));
    }
}

impl crate::adapters::driving::window_input::OverlayInputHost for OverlayHost {
    fn on_capture_message(&self, window: WindowHandle) {
        OverlayHost::on_capture_message(self, window);
    }
}

impl crate::adapters::driving::window_input::WindowInputHost for OverlayHost {
    fn cursor_active(&self) -> bool {
        self.guard.active()
    }

    fn disabled(&self) -> bool {
        OverlayHost::disabled(self)
    }

    fn on_hotkey(&self, event: HotkeyEvent) {
        OverlayHost::on_hotkey(self, event);
    }

    fn on_input(&self, message: crate::adapters::driving::window_input::InputMessage) {
        // The window procedure's queued menu input, in client pixels already mapped to output.
        if OverlayHost::disabled(self) {
            return;
        }
        let mut queue = lock(&self.input);
        if queue.len() >= crate::adapters::driving::window_input::MAX_QUEUED_MESSAGES {
            queue.remove(0);
        }
        queue.push(message);
    }

    fn on_destroyed(&self, window: WindowHandle) {
        let mut menu = lock(&self.menu);
        if menu.window == window {
            menu.window = WindowHandle::NONE;
        }
    }
}

/// Whether `window` is the foreground window; a plain query, never fatal.
fn foreground_is(window: WindowHandle) -> bool {
    if window.is_none() {
        return false;
    }
    use windows::Win32::Foundation::HWND;
    use windows::Win32::UI::WindowsAndMessaging::GetForegroundWindow;
    // SAFETY: no arguments; returns the foreground window or null.
    unsafe { GetForegroundWindow() == HWND(window.0 as *mut std::ffi::c_void) }
}

/// The DXGI format value the overlay renderer needs. It supports R8G8B8A8 only; anything else
/// reaches it as a defined `UnsupportedFormat` and disables the overlay for the session.
fn dxgi_format(format: crate::application::ports::gpu::PixelFormat) -> u32 {
    use crate::application::ports::gpu::PixelFormat;
    match format {
        PixelFormat::R8G8B8A8Unorm => 28,
        PixelFormat::R10G10B10A2Unorm => 24,
        _ => 0,
    }
}

fn lock<T>(mutex: &Mutex<T>) -> std::sync::MutexGuard<'_, T> {
    mutex
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
}
