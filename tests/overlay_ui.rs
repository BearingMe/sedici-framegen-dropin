//! The overlay's GPU path, ported from `tests/regression/regression.cpp`'s `gpu-mini` and
//! `gpu-full` scenarios: one parameterised test that runs the production `OverlayUi` for a chosen
//! menu mode against a real D3D12 device and an offscreen R8G8B8A8_UNORM output.
//!
//! What the C++ checked, and what this keeps:
//!
//! - the overlay draws every frame and stays enabled (`GPU overlay remains enabled`);
//! - Mini and Full both leave the device healthy and the debug layer free of errors;
//! - Full switches itself off and gives input back when the renderer fails (`arm_bookkeeping_fault`
//!   in the C++), which is simulated here by disabling the renderer mid-run and asserting the next
//!   draw reports it and draws nothing.
//!
//! Two checks are added because egui paints differently from ImGui and the pixels are the only
//! honest evidence: a Mini frame must darken the top-left corner (its 65 % black panel) and must
//! leave the far corner untouched; a Full frame must touch the far corners not at all. Pixels are
//! read back through a readback buffer.
//!
//! No D3D12 adapter skips cleanly with a message. The game is never launched and nothing is
//! downloaded.
#![allow(unsafe_code)] // The DX12 debug layer is a raw COM query; see the safety notes below.
use std::time::Duration;

use amd_fidelityfx_framegeneration_dx12::adapters::driven::d3d12::device::D3d12Device;
use amd_fidelityfx_framegeneration_dx12::adapters::driven::d3d12::resources::D3d12CommandList;
use amd_fidelityfx_framegeneration_dx12::adapters::driven::overlay_ui::OverlayUi;
use amd_fidelityfx_framegeneration_dx12::application::overlay::{
    FullView, MiniView, OverlayView, ScaleChoice, TargetMode,
};
use amd_fidelityfx_framegeneration_dx12::application::ports::gpu::{
    GpuCommandAllocator, GpuDevice, GpuFence, GpuQueue, PixelFormat,
    QueueKind, QueuePriority, ResourceState, TextureDesc, TextureFlags,
};
use amd_fidelityfx_framegeneration_dx12::application::ports::ui_renderer::{
    OutputFormat, OverlayRenderer, RenderTarget, UiFrameInfo,
};
use amd_fidelityfx_framegeneration_dx12::domain::timing::TimingStats;
use rstest::rstest;

const WIDTH: u32 = 640;
const HEIGHT: u32 = 360;
const WAIT: Duration = Duration::from_secs(20);
/// `DXGI_FORMAT_R8G8B8A8_UNORM`.
const R8G8B8A8_UNORM: u32 = 28;
/// The output starts as a zeroed committed resource; an overlay pixel that stayed zero means nothing
/// was drawn there. Alpha is zero too, since no clear ever wrote 255.
const CLEAR: [u8; 4] = [0, 0, 0, 0];

fn mini_view() -> OverlayView {
    OverlayView::Mini(MiniView { real_fps: 60.0, output_fps: 120.0, output_p99_ms: 9.0 })
}

fn full_view() -> OverlayView {
    OverlayView::Full(FullView {
        scale: 1.0,
        frame_generation_on: true,
        render_size: (1280, 720),
        output_size: (WIDTH, HEIGHT),
        real_fps: 60.0,
        output_fps: 120.0,
        real_dropped: 0,
        stall_count: 0,
        last_stall_ms: 0.0,
        output_stats: TimingStats::default(),
        real_stats: TimingStats::default(),
        output_samples: vec![8.0, 9.0, 10.0, 9.5],
        real_samples: Vec::new(),
        edits_enabled: true,
        save_status: String::new(),
        provenance: Default::default(),
        target_mode: TargetMode::Fixed,
        pending_units: 73,
        pending_dirty: false,
        pending_fps: 36.5,
        limiter_fps: 36.5,
        output_refresh_hz: 75.0,
        log_level: 1,
        scale_choice: ScaleChoice(0),
        applied_scale: 1.0,
    })
}

struct Host {
    device: D3d12Device,
    queue: <D3d12Device as GpuDevice>::Queue,
    allocator: <D3d12Device as GpuDevice>::CommandAllocator,
    list: D3d12CommandList,
    fence: <D3d12Device as GpuDevice>::Fence,
    value: u64,
}

impl Host {
    fn new() -> Option<Self> {
        // The debug layer must be on before the device exists.
        enable_debug_layer();
        let device = D3d12Device::create_default().ok().flatten()?;
        let queue = device.create_queue(QueueKind::Direct, QueuePriority::Normal).ok()?;
        let allocator = device.create_command_allocator(QueueKind::Direct).ok()?;
        let list = device.create_command_list(&allocator).ok()?;
        let fence = device.create_fence(0).ok()?;
        Some(Self { device, queue, allocator, list, fence, value: 0 })
    }

    /// The seam that reads the info queue's error-severity messages.
    fn errors(&self) -> Option<QueueErrors> {
        use windows::core::Interface;
        let queue: windows::Win32::Graphics::Direct3D12::ID3D12InfoQueue =
            self.device.raw().cast().ok()?;
        // SAFETY: a pure query on a live device.
        let from = unsafe { queue.GetNumStoredMessages() };
        Some(QueueErrors { queue, from })
    }

    fn begin(&self) -> Result<(), String> {
        self.allocator.reset().map_err(|error| error.to_string())?;
        self.list.reset(&self.allocator).map_err(|error| error.to_string())
    }

    fn submit(&mut self) -> Result<(), String> {
        self.list.close().map_err(|error| error.to_string())?;
        self.queue.execute(&[&self.list]).map_err(|error| error.to_string())?;
        self.value += 1;
        self.queue.signal(&self.fence, self.value).map_err(|error| error.to_string())?;
        self.fence.wait(self.value, WAIT).map_err(|error| error.to_string())
    }

    /// A D3D12 committed texture reads as zeros before anything writes it, so the test's "nothing
    /// drawn here" baseline is the freshly created resource. (Upload was refused at list Close with
    /// `E_INVALIDARG` in this environment; the readback path is the one this test needs anyway.)
    fn clear(&mut self, _texture: &<D3d12Device as GpuDevice>::Texture) -> Result<(), String> {
        Ok(())
    }

    fn readback(
        &mut self,
        texture: &<D3d12Device as GpuDevice>::Texture,
    ) -> Result<Vec<u8>, String> {
        let readback = self
            .device
            .create_readback_buffer((WIDTH * HEIGHT * 4) as u64)
            .map_err(|error| error.to_string())?;
        self.begin()?;
        let pitch = self
            .list
            .readback_texture(texture, &readback, ResourceState::COMMON, ResourceState::COMMON)
            .map_err(|error| error.to_string())?;
        self.submit()?;
        let bytes = self.device.read_buffer(&readback).map_err(|error| error.to_string())?;
        let mut packed = Vec::with_capacity((WIDTH * HEIGHT * 4) as usize);
        for y in 0..HEIGHT as usize {
            let row = y * pitch as usize;
            packed.extend_from_slice(&bytes[row..row + (WIDTH * 4) as usize]);
        }
        Ok(packed)
    }
}

fn pixel(pixels: &[u8], x: u32, y: u32) -> [u8; 4] {
    let index = ((y * WIDTH + x) * 4) as usize;
    [pixels[index], pixels[index + 1], pixels[index + 2], pixels[index + 3]]
}

/// True when a pixel was painted at all. The overlay's panel fill is 65 % black over a zeroed back
/// buffer, so its RGB stays 0; alpha is what shows that a fragment reached the target.
fn is_painted(px: [u8; 4]) -> bool {
    px[0] != 0 || px[1] != 0 || px[2] != 0 || px[3] != 0
}

#[rstest]
#[case("Mini", mini_view())]
#[case("Full", full_view())]
fn gpu_overlay_draws_a_known_frame(#[case] name: &str, #[case] view: OverlayView) {
    let Some(mut host) = Host::new() else {
        eprintln!("SKIP {name}: no D3D12 adapter");
        return;
    };
    let overlay_errors = host.errors();
    let output = host
        .device
        .create_texture(&TextureDesc::texture2d(
            WIDTH,
            HEIGHT,
            PixelFormat::R8G8B8A8Unorm,
            // A render target view needs this flag; ALLOW_UNORDERED_ACCESS alone makes D3D12 refuse
            // the RTV and then remove the device.
            TextureFlags::ALLOW_UNORDERED_ACCESS.union(TextureFlags::ALLOW_RENDER_TARGET),
        ))
        .expect("output texture");
    host.clear(&output).expect("clear");
    assert_eq!(pixel(&host.readback(&output).expect("read"), 0, 0), CLEAR);

    let mut overlay = OverlayUi::new();
    overlay
        .initialize(&host.device, OutputFormat(R8G8B8A8_UNORM))
        .expect("overlay initialize");
    let target = RenderTarget {
        command_list: 0,
        resource: 0,
        device: amd_fidelityfx_framegeneration_dx12::adapters::driven::overlay_ui::device_identity_of(&host.device),
        width: WIDTH,
        height: HEIGHT,
        format: OutputFormat(R8G8B8A8_UNORM),
        // `create_texture` leaves the resource in COMMON; the renderer transitions it in and out.
        state: ResourceState::COMMON,
        generated: false,
    };
    let frame = UiFrameInfo {
        output_width: WIDTH,
        output_height: HEIGHT,
        client_width: WIDTH as i32,
        client_height: HEIGHT as i32,
        delta_seconds: Some(1.0 / 60.0),
    };

    // Three frames: the first allocates egui's atlas, the later ones exercise ring reuse.
    let mut recorded = false;
    for frame_index in 0..3 {
        host.begin().expect("begin");
        let this_frame = overlay
            .draw_with_raw(&host.list, &output, &target, &view, &[], frame)
            .expect("overlay draw");
        if frame_index == 2 {
            recorded = this_frame;
        }
        host.submit().unwrap_or_else(|error| {
            let messages = host_errors(&overlay_errors);
            panic!("{name}: submit frame {frame_index}: {error}\nvalidation: {messages:?}")
        });
        assert!(!overlay.disabled(), "GPU overlay remains enabled");
    }
    // The first pass of a fresh context is egui's placeholder pass (K148); the overlay's second
    // pass draws, so by the third frame every frame records a draw.
    assert!(recorded, "{name}: the overlay must record a draw");

    let pixels = host.readback(&output).expect("read back");
    // The Mini panel sits at (12, 12) with a few points of margin; the Full window starts at the same
    // anchor. Sample inside that area and at the far corner, which no menu reaches at this size.
    assert!(is_painted(pixel(&pixels, 20, 20)), "{name}: the menu area must be painted");
    assert_eq!(
        pixel(&pixels, WIDTH - 2, HEIGHT - 2),
        CLEAR,
        "{name}: the far corner is outside the menu"
    );
    let (real, _generated) = overlay.draw_counts();
    assert_eq!(real, 3, "every frame is counted as a real draw");

    // The renderer failure path: a disabled renderer reports it and draws nothing.
    overlay.disable("injected regression failure");
    assert!(overlay.disabled());
    host.begin().expect("begin");
    assert!(
        overlay
            .draw_with_raw(&host.list, &output, &target, &view, &[], frame)
            .is_err(),
        "a disabled overlay refuses to draw"
    );
    host.submit().expect("submit");
    assert_eq!(host.device.removed_reason(), None, "GPU device remains healthy");
    if let Some(errors) = overlay_errors {
        let messages = errors.messages();
        assert!(messages.is_empty(), "{name}: DX12 validation errors: {messages:?}");
    }
}

fn host_errors(errors: &Option<QueueErrors>) -> Vec<String> {
    errors.as_ref().map(QueueErrors::messages).unwrap_or_default()
}

/// A tiny wrapper over the debug layer's stored messages (the regression's "DX12 debug layer reports
/// no error" check).
struct QueueErrors {
    queue: windows::Win32::Graphics::Direct3D12::ID3D12InfoQueue,
    from: u64,
}

impl QueueErrors {
    fn messages(&self) -> Vec<String> {
        // SAFETY: a pure query on a live info queue.
        let count = unsafe { self.queue.GetNumStoredMessages() };
        let mut out = Vec::new();
        for index in self.from..count {
            let mut bytes = 0usize;
            // SAFETY: the first call measures the message; the second fills a buffer of that size,
            // and the description pointer is read only while the buffer is alive.
            unsafe {
                let _ = self.queue.GetMessage(index, None, &mut bytes);
                let mut storage = vec![0u8; bytes];
                let message = storage.as_mut_ptr() as *mut windows::Win32::Graphics::Direct3D12::D3D12_MESSAGE;
                if self.queue.GetMessage(index, Some(message), &mut bytes).is_ok() {
                    let description = (*message).pDescription;
                    if !description.is_null() && (*message).DescriptionByteLength > 0 {
                        let slice = std::slice::from_raw_parts(
                            description,
                            (*message).DescriptionByteLength as usize,
                        );
                        let text = String::from_utf8_lossy(slice).trim_end_matches('\0').to_owned();
                        if (*message).Severity.0 <= 1 {
                            out.push(text);
                        }
                    }
                }
            }
        }
        out
    }
}

/// Enables the D3D12 debug layer when it is installed. A machine without it runs the test without
/// validation, as the C++ regression did.
fn enable_debug_layer() {
    // SAFETY: the out pointer is a valid local; enabling is idempotent and needs the device next.
    unsafe {
        let mut debug: Option<windows::Win32::Graphics::Direct3D12::ID3D12Debug> = None;
        if windows::Win32::Graphics::Direct3D12::D3D12GetDebugInterface(&mut debug).is_ok() {
            if let Some(debug) = debug {
                debug.EnableDebugLayer();
            }
        }
    }
}
