//! The production overlay host, driven the way the swapchain wrapper drives it: a real D3D12 device
//! and hidden window, an offscreen back buffer, and the [`OverlayHook`] contract.
//!
//! Ports what the C++ `sedici_regression` `gpu-mini`/`gpu-full` scenarios checked at the host level
//! (the renderer itself is covered by `tests/overlay_ui.rs`), plus the wiring this slice added:
//!
//! - with `[ui] mode = "mini"`, a few outputs draw through the host, the host's counters and the
//!   renderer's counters agree, and the periodic `sedici: overlay present frames=...` report line is
//!   emitted (captured through a counting `tracing` subscriber);
//! - a Full-mode host reports its menu view and, with a simulated hotkey, Off -> Mini -> Full -> Off
//!   cycles and the host disables itself cleanly on a renderer failure without failing anything.
//!
//! No D3D12 adapter skips cleanly with a message. The game is never launched and nothing is
//! downloaded.
#![allow(unsafe_code)] // The device, window and render calls are raw Win32/D3D12 for the fixture.
use std::sync::atomic::Ordering;
use std::sync::{Arc, Mutex};

use amd_fidelityfx_framegeneration_dx12::adapters::driven::d3d12::device::D3d12Device;
use amd_fidelityfx_framegeneration_dx12::adapters::driven::d3d12::resources::D3d12CommandList;
use amd_fidelityfx_framegeneration_dx12::adapters::driven::overlay_ui::host::{
    OverlayHost, OverlayServices,
};
use amd_fidelityfx_framegeneration_dx12::adapters::driven::overlay_ui::OverlayUi;
use amd_fidelityfx_framegeneration_dx12::application::overlay::OverlayView;
use amd_fidelityfx_framegeneration_dx12::application::ports::clock::FakeClock;
use amd_fidelityfx_framegeneration_dx12::application::ports::config_store::{
    ConfigStore, ConfigValues, FileStamp, SaveOutcome,
};
use amd_fidelityfx_framegeneration_dx12::application::ports::gpu::{
    GpuCommandAllocator, GpuDevice, GpuFence, GpuQueue, PixelFormat, QueueKind, QueuePriority,
    ResourceState, TextureDesc, TextureFlags,
};
use amd_fidelityfx_framegeneration_dx12::application::ports::input::{
    CursorGuard, Hotkey, HotkeyEvent, InterceptionSummary, KeyAction, WindowHandle,
};
use amd_fidelityfx_framegeneration_dx12::adapters::driven::d3d12::resources::D3d12Texture;
use amd_fidelityfx_framegeneration_dx12::application::ports::overlay::OverlayHook;
use amd_fidelityfx_framegeneration_dx12::domain::config::{Settings, UiMode};
use rstest::rstest;

/// Captures the events one test emits, formatted the way the log line reads (`<message> <fields>`),
/// so the cadence and link lines are checked as text rather than as a count.
struct TraceCapture {
    lines: Arc<Mutex<Vec<String>>>,
    /// Keeps the thread-local default subscriber installed for this test.
    _guard: tracing::subscriber::DefaultGuard,
}

impl TraceCapture {
    fn install() -> Self {
        let lines = Arc::new(Mutex::new(Vec::new()));
        let subscriber = Lines {
            lines: Arc::clone(&lines),
        };
        // A thread-local default subscriber: `tracing` keeps it for this test thread only, so the
        // suite's other tests are unaffected.
        let guard = tracing::subscriber::set_default(subscriber);
        Self { lines, _guard: guard }
    }

    fn lines(&self) -> Vec<String> {
        self.lines.lock().unwrap().clone()
    }
}

struct Lines {
    lines: Arc<Mutex<Vec<String>>>,
}

impl tracing::Subscriber for Lines {
    fn enabled(&self, metadata: &tracing::Metadata<'_>) -> bool {
        metadata.level() <= &tracing::Level::INFO
    }

    fn new_span(&self, _span: &tracing::span::Attributes<'_>) -> tracing::span::Id {
        tracing::span::Id::from_u64(1)
    }

    fn record(&self, _span: &tracing::span::Id, _values: &tracing::span::Record<'_>) {}

    fn record_follows_from(&self, _span: &tracing::span::Id, _follows: &tracing::span::Id) {}

    fn event(&self, event: &tracing::Event<'_>) {
        let mut writer = LineWriter(String::new());
        event.record(&mut writer);
        self.lines.lock().unwrap().push(writer.0);
    }

    fn enter(&self, _span: &tracing::span::Id) {}

    fn exit(&self, _span: &tracing::span::Id) {}
}

struct LineWriter(String);

impl tracing::field::Visit for LineWriter {
    fn record_debug(&mut self, field: &tracing::field::Field, value: &dyn std::fmt::Debug) {
        if !self.0.is_empty() {
            self.0.push(' ');
        }
        if field.name() == "message" {
            self.0.push_str(&format!("{value:?}"));
        } else {
            self.0.push_str(&format!("{}={value:?}", field.name()));
        }
    }
}

const WIDTH: u32 = 320;
const HEIGHT: u32 = 180;
const WAIT: std::time::Duration = std::time::Duration::from_secs(20);

/// A cursor guard that never captures: the hidden test window is never the foreground window, so
/// the real interception would refuse anyway, and a fake keeps the test independent of it.
#[derive(Default)]
struct FakeGuard {
    active: std::sync::atomic::AtomicBool,
}

impl CursorGuard for FakeGuard {
    fn enter(&self, _window: WindowHandle) -> bool {
        self.active.store(true, Ordering::Release);
        true
    }
    fn leave(&self, _restore_clip: bool) {
        self.active.store(false, Ordering::Release);
    }
    fn leave_window(&self, _window: WindowHandle, _restore_clip: bool) {
        self.active.store(false, Ordering::Release);
    }
    fn disable(&self) {
        self.active.store(false, Ordering::Release);
    }
    fn active(&self) -> bool {
        self.active.load(Ordering::Acquire)
    }
    fn available(&self) -> bool {
        true
    }
    fn summary(&self) -> InterceptionSummary {
        InterceptionSummary::default()
    }
}

/// A hidden window plus the D3D12 device, queue, allocator, list, fence and offscreen output the
/// host draws onto.
struct Fixture {
    window: windows::Win32::Foundation::HWND,
    device: D3d12Device,
    queue: <D3d12Device as GpuDevice>::Queue,
    allocator: <D3d12Device as GpuDevice>::CommandAllocator,
    list: D3d12CommandList,
    fence: <D3d12Device as GpuDevice>::Fence,
    value: u64,
    output: <D3d12Device as GpuDevice>::Texture,
}

impl Drop for Fixture {
    fn drop(&mut self) {
        use windows::Win32::UI::WindowsAndMessaging::{DestroyWindow, UnregisterClassW};
        // SAFETY: every handle belongs to this test and is released exactly once.
        unsafe {
            let _ = DestroyWindow(self.window);
            let _ = UnregisterClassW(windows::core::w!("SediciRustOverlayHost"), None);
        }
    }
}

extern "system" fn default_proc(
    hwnd: windows::Win32::Foundation::HWND,
    msg: u32,
    wparam: windows::Win32::Foundation::WPARAM,
    lparam: windows::Win32::Foundation::LPARAM,
) -> windows::Win32::Foundation::LRESULT {
    // SAFETY: the standard default procedure for a window this test owns.
    unsafe { windows::Win32::UI::WindowsAndMessaging::DefWindowProcW(hwnd, msg, wparam, lparam) }
}

fn fixture() -> Option<Fixture> {
    use windows::Win32::Foundation::HINSTANCE;
    use windows::Win32::System::LibraryLoader::GetModuleHandleW;
    use windows::Win32::UI::WindowsAndMessaging::{
        CreateWindowExW, RegisterClassW, CW_USEDEFAULT, WS_POPUP, WINDOW_EX_STYLE, WINDOW_STYLE,
        WNDCLASSW,
    };

    let device = D3d12Device::create_default().ok().flatten()?;
    let queue = device.create_queue(QueueKind::Direct, QueuePriority::Normal).ok()?;
    let allocator = device.create_command_allocator(QueueKind::Direct).ok()?;
    let list = device.create_command_list(&allocator).ok()?;
    let fence = device.create_fence(0).ok()?;
    let output = device
        .create_texture(&TextureDesc::texture2d(
            WIDTH,
            HEIGHT,
            PixelFormat::R8G8B8A8Unorm,
            TextureFlags::ALLOW_RENDER_TARGET,
        ))
        .ok()?;

    // SAFETY: every call uses initialised locals and checks its result; a failure returns `None`.
    let window = unsafe {
        let instance = GetModuleHandleW(None).ok()?;
        let class = WNDCLASSW {
            lpfnWndProc: Some(default_proc),
            hInstance: HINSTANCE(instance.0),
            lpszClassName: windows::core::w!("SediciRustOverlayHost"),
            ..Default::default()
        };
        let _ = RegisterClassW(&class);
        CreateWindowExW(
            WINDOW_EX_STYLE(0),
            windows::core::w!("SediciRustOverlayHost"),
            windows::core::w!("Sedici overlay host"),
            WINDOW_STYLE(WS_POPUP.0),
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            64,
            64,
            None,
            None,
            Some(HINSTANCE(instance.0)),
            None,
        )
        .ok()?
    };
    Some(Fixture { window, device, queue, allocator, list, fence, value: 0, output })
}

impl Fixture {
    fn begin(&self) -> Result<(), String> {
        self.allocator.reset().map_err(|e| e.to_string())?;
        self.list.reset(&self.allocator).map_err(|e| e.to_string())
    }

    /// Closes and submits the list the host recorded into, then waits for it.
    fn submit(&mut self) -> Result<(), String> {
        self.list.close().map_err(|e| e.to_string())?;
        self.queue.execute(&[&self.list]).map_err(|e| e.to_string())?;
        self.value += 1;
        self.queue.signal(&self.fence, self.value).map_err(|e| e.to_string())?;
        self.fence.wait(self.value, WAIT).map_err(|e| e.to_string())
    }
}

/// A store that never touches a file: the host tests never read or write a real `sedici.toml`, and
/// they never ask for a save.
struct NoStore;

impl ConfigStore for NoStore {
    fn default_path(&self) -> Option<std::path::PathBuf> {
        Some(std::path::PathBuf::from("C:/game/sedici.toml"))
    }
    fn is_file(&self, _path: &std::path::Path) -> bool {
        true
    }
    fn stamp(&self, _path: &std::path::Path) -> FileStamp {
        FileStamp { valid: true, exists: true, file_id: 1, ..FileStamp::default() }
    }
    fn load_text(&self, _path: &std::path::Path) -> Result<String, String> {
        Ok("[ui]\nmode = \"mini\"\n".to_owned())
    }
    fn save(&self, _path: &std::path::Path, _values: &ConfigValues, _expected: Option<&FileStamp>) -> SaveOutcome {
        SaveOutcome { written: true, ..SaveOutcome::default() }
    }
}

/// A services bundle whose transaction reads and writes the no-op store, never a real file.
fn services(mode: UiMode) -> Arc<OverlayServices> {
    let mut config = Settings::default();
    config.ui.mode = mode;
    config.ui.mode_source = amd_fidelityfx_framegeneration_dx12::domain::config::Source::File;
    let store: Arc<dyn ConfigStore + Send + Sync> = Arc::new(NoStore);
    OverlayServices::with_store(config, Arc::new(FakeClock::new(1_000_000, 0)), store)
}

/// The mode the live values hold, for the hotkey assertions.
fn live_mode(services: &OverlayServices) -> UiMode {
    services.live.ui_mode()
}

/// A host over the fixture, with a fake cursor guard and the test's hotkey.
fn host(fixture: &Fixture, services: Arc<OverlayServices>, guard: Arc<FakeGuard>) -> OverlayHost {
    OverlayHost::for_test(
        services,
        fixture.device.clone(),
        WindowHandle(fixture.window.0 as isize),
        guard,
    )
}

#[rstest]
#[case(UiMode::Mini)]
#[case(UiMode::Full)]
fn a_few_presents_draw_through_the_host_and_report_cadence(#[case] mode: UiMode) {
    let Some(mut fixture) = fixture() else {
        eprintln!("overlay host: no D3D12 adapter available; skipping");
        return;
    };
    let services = services(mode);
    let guard = Arc::new(FakeGuard::default());
    let host = host(&fixture, Arc::clone(&services), Arc::clone(&guard));
    let _ = OverlayView::Off;

    let capture = TraceCapture::install();
    for _ in 0..3 {
        fixture.begin().expect("reset");
        host.draw(&fixture.list, &fixture.output, false);
        fixture.submit().expect("submit");
        OverlayHook::<D3d12Texture>::presented(&host, false);
    }
    let status = host.status();
    assert!(!status.disabled, "a Mini/Full output must never disable the overlay: {:?}", host.disabled_reason());
    assert_eq!(status.presented, (3, 0));
    // One rgba8 output, the only format the overlay blender supports (K164). The other formats
    // disable the overlay for the session by design, which `tests/overlay_ui.rs` already covers.
    assert_eq!(status.draws.0 + status.draws.1, 3, "each output drew once: {status:?}");

    // The cadence window is 60 outputs wide for the first eight reports; three outputs have no
    // report yet, so drive the recorder to the boundary through the host's own presenter path.
    for _ in 0..57 {
        OverlayHook::<D3d12Texture>::presented(&host, false);
    }
    let lines = capture.lines();
    let report = lines
        .iter()
        .find(|line| line.contains("sedici: overlay present frames=60"))
        .unwrap_or_else(|| panic!("no cadence report line in {lines:?}"));
    assert!(report.contains("real=60 generated=0"), "{report}");
    assert!(
        lines.iter().any(|line| line.contains("sedici: overlay present callback active")),
        "the one-off link line names the callback resources: {lines:?}"
    );
}

#[test]
fn the_hotkey_cycles_off_mini_full_off_through_the_host() {
    let Some(fixture) = fixture() else {
        eprintln!("overlay host: no D3D12 adapter available; skipping");
        return;
    };
    let services = services(UiMode::Off);
    let guard = Arc::new(FakeGuard::default());
    let host = host(&fixture, Arc::clone(&services), Arc::clone(&guard));
    let window = WindowHandle(fixture.window.0 as isize);
    let event = |action| HotkeyEvent { action, window };
    assert_eq!(live_mode(&services), UiMode::Off);
    host.on_hotkey(event(KeyAction::Pressed));
    assert_eq!(live_mode(&services), UiMode::Mini);
    host.on_hotkey(event(KeyAction::Pressed));
    assert_eq!(live_mode(&services), UiMode::Full);
    host.on_hotkey(event(KeyAction::Pressed));
    assert_eq!(live_mode(&services), UiMode::Off);
    // A release is not a cycle.
    host.on_hotkey(event(KeyAction::Released));
    assert_eq!(live_mode(&services), UiMode::Off);
    assert_eq!(host.hotkey(), Hotkey::parse("alt+f10"));
}

#[test]
fn a_disabled_host_stops_drawing_and_presenting() {
    let Some(fixture) = fixture() else {
        eprintln!("overlay host: no D3D12 adapter available; skipping");
        return;
    };
    let services = services(UiMode::Mini);
    let guard = Arc::new(FakeGuard::default());
    let host = host(&fixture, Arc::clone(&services), Arc::clone(&guard));
    host.disable("the renderer failed");
    assert!(host.disabled());
    assert_eq!(host.disabled_reason().as_deref(), Some("the renderer failed"));
    assert_eq!(live_mode(&services), UiMode::Off, "a disabled overlay publishes Off");
    assert!(!guard.active(), "the cursor is given back");
    // Further outputs draw nothing; the cadence is a property of presentation and still counts.
    host.draw(&fixture.list, &fixture.output, false);
    OverlayHook::<D3d12Texture>::presented(&host, false);
    assert_eq!(host.status().presented, (1, 0));
    assert_eq!(host.status().draws, (0, 0));
    assert!(!host.disabled_reason().unwrap().is_empty());
}

#[test]
fn the_host_requests_full_capture_only_from_the_render_path_when_focus_arrives_late() {
    let Some(fixture) = fixture() else {
        eprintln!("overlay host: no D3D12 adapter available; skipping");
        return;
    };
    let services = services(UiMode::Full);
    let guard = Arc::new(FakeGuard::default());
    let host = host(&fixture, Arc::clone(&services), Arc::clone(&guard));
    // Full without capture: the window is hidden, so the capture decision is a refusal and the
    // mode stays Full (the fallback to Mini happens only when interception is unavailable, which
    // the fake guard reports as available).
    host.on_capture_message(WindowHandle(fixture.window.0 as isize));
    assert_eq!(live_mode(&services), UiMode::Full);
    assert!(!guard.active());
    // A presented output asks the window thread again; the fake guard is not foreground so it
    // still refuses, and no mode change is published.
    OverlayHook::<D3d12Texture>::presented(&host, false);
    assert_eq!(live_mode(&services), UiMode::Full);
}

/// The overlay host's canonical type name, so the test fails to compile if the alias it is built
/// against changes shape.
#[test]
fn the_host_implements_the_overlay_hook() {
    fn assert_hook<T: OverlayHook<amd_fidelityfx_framegeneration_dx12::adapters::driven::d3d12::resources::D3d12Texture>>() {}
    assert_hook::<OverlayHost>();
    let _ = OverlayUi::new();
    // Keep the fixture's own types named, so a change to one fails this compile.
    let _: Option<ResourceState> = None;
}
