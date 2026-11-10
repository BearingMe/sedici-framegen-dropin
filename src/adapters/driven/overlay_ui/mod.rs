//! The egui overlay: one egui pass per output, the Mini/Full menus and a hand-written D3D12 renderer.
//!
//! Ports `ui/ui.cpp` and `ui/overlay.cpp` together with the Dear ImGui DX12 backend they used:
//!
//! - [`device`] builds the D3D12 objects (root signature, pipeline, heaps, upload buffers), compiles
//!   the two HLSL shaders with the system `D3DCompile` and records the draws;
//! - [`menu`] is the only place egui widgets appear: it turns an
//!   [`OverlayView`](crate::application::overlay::OverlayView) into `OverlayAction`s;
//! - [`input`] turns the window messages the Full menu consumed into egui events, with the
//!   client-to-output mapping and the Alt+F4 pass-through (K126);
//! - this module owns the [`egui::Context`], the font-atlas texture lifecycle and the
//!   [`OverlayRenderer`] implementation the swapchain slice calls.
//!
//! # Lifetime
//!
//! The context is created on the first draw in Mini or Full and lives for the session, because egui's
//! memory holds the window position and hover state. The D3D12 objects are created on the first draw
//! too, since only then is the output format known; a device or format change disables the overlay,
//! as the C++ `ensure_imgui` did.
//!
//! # Atlas upload
//!
//! epaint allocates a font atlas on the first pass and grows it by whole-image deltas when new glyphs
//! appear. The renderer keeps one SRV slot and replaces the texture on each whole-image delta; a
//! partial delta is refused because there is no box-copy path here (epaint does not send one for its
//! atlases). The upload is one record-submit-wait on a private queue, allocator and fence, as the
//! C++ `create_device_objects` did.
use std::sync::Mutex;
use std::time::Duration;

use egui::ClippedPrimitive;
use windows::core::Interface;
use windows::Win32::Graphics::Direct3D12::{ID3D12GraphicsCommandList, ID3D12Resource};

use crate::adapters::driven::d3d12::device::D3d12Device;
use crate::adapters::driven::d3d12::fence::D3d12Fence;
use crate::adapters::driven::d3d12::queue::D3d12Queue;
use crate::adapters::driven::d3d12::resources::{D3d12CommandAllocator, D3d12CommandList};
use crate::application::overlay::{OverlayAction, OverlayView};
use crate::application::ports::gpu::{
    GpuDevice, GpuFence, QueueKind, QueuePriority,
};
use crate::application::ports::ui_renderer::{
    OverlayDrawError, OverlayRenderer, OutputFormat, RenderTarget, UiFrameInfo, UiInput,
};

pub mod device;
pub mod input;
pub mod menu;

pub use device::EguiD3d12Renderer;

/// The waiting bound for one atlas upload.
const ATLAS_UPLOAD_WAIT: Duration = Duration::from_secs(5);

/// The overlay renderer: an egui context plus the D3D12 renderer and its atlas upload queue.
///
/// Not `Sync`: the C++ serialized every overlay call behind one render mutex, and the swapchain's
/// present hook owns this value. The counters and the disable flag, which the status query reads
/// from another thread, have their own mutexes.
pub struct OverlayUi {
    context: Option<egui::Context>,
    renderer: EguiD3d12Renderer,
    /// Created on the first initialize; the atlas upload record-submit-waits on it.
    upload: Option<Upload>,
    owner: Mutex<Option<Owner>>,
    disabled: Mutex<Option<String>>,
    real_draws: Mutex<u64>,
    generated_draws: Mutex<u64>,
}

/// The private upload queue for the atlas.
struct Upload {
    queue: D3d12Queue,
    allocator: D3d12CommandAllocator,
    list: D3d12CommandList,
    fence: D3d12Fence,
    value: u64,
}


/// The game device and output format the renderer was created for. The format lives on the renderer
/// (it is what the pipeline was compiled for); this keeps the owning device alive so the overlay's
/// COM references outlive the swapchain's own.
struct Owner {
    device: D3d12Device,
}

impl OverlayUi {
    pub fn new() -> Self {
        Self {
            context: None,
            renderer: EguiD3d12Renderer::new(),
            upload: None,
            owner: Mutex::new(None),
            disabled: Mutex::new(None),
            real_draws: Mutex::new(0),
            generated_draws: Mutex::new(0),
        }
    }

    /// Prepares the D3D12 objects and the upload queue for `device` and `format`. A repeat call for
    /// the same pair is a no-op; a different device or format is a defined refusal.
    pub fn initialize(&mut self, device: &D3d12Device, format: OutputFormat) -> Result<(), OverlayDrawError> {
        if self.upload.is_none() {
            let queue = device
                .create_queue(QueueKind::Direct, QueuePriority::Normal)
                .map_err(|error| OverlayDrawError::Failed(error.to_string()))?;
            let allocator = device
                .create_command_allocator(QueueKind::Direct)
                .map_err(|error| OverlayDrawError::Failed(error.to_string()))?;
            let list = device
                .create_command_list(&allocator)
                .map_err(|error| OverlayDrawError::Failed(error.to_string()))?;
            let fence = device
                .create_fence(0)
                .map_err(|error| OverlayDrawError::Failed(error.to_string()))?;
            self.upload = Some(Upload { queue, allocator, list, fence, value: 0 });
        }
        self.renderer.initialize(device.raw(), format)?;
        *self.owner.lock().unwrap_or_else(|poisoned| poisoned.into_inner()) =
            Some(Owner { device: device.clone() });
        Ok(())
    }

    fn owner_device(&self) -> Option<D3d12Device> {
        self.owner
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .as_ref()
            .map(|owner| owner.device.clone())
    }

    fn disabled_slot(&self) -> std::sync::MutexGuard<'_, Option<String>> {
        self.disabled.lock().unwrap_or_else(|poisoned| poisoned.into_inner())
    }

    /// Uploads the whole-image texture deltas of one egui pass, oldest first.
    fn apply_textures(&mut self, delta: &egui::TexturesDelta) -> Result<(), OverlayDrawError> {
        if delta.set.is_empty() {
            return Ok(());
        }
        let Some(upload) = self.upload.as_mut() else {
            return Err(OverlayDrawError::NoDevice);
        };
        for (id, image) in &delta.set {
            upload.value += 1;
            // The allocator and list are the adapter's own types; their COM objects are visible
            // within the crate, so the renderer takes the raw interfaces without an accessor.
            self.renderer.upload_atlas(
                upload.queue.raw(),
                &upload.allocator.allocator,
                upload.list.raw(),
                upload.fence.raw(),
                upload.value,
                *id,
                image,
                ATLAS_UPLOAD_WAIT,
            )?;
            if let Err(error) = upload.fence.wait(upload.value, ATLAS_UPLOAD_WAIT) {
                return Err(OverlayDrawError::Failed(error.to_string()));
            }
        }
        Ok(())
    }

    /// Runs one egui frame for `view` and returns the actions it produced. Does not draw; the caller
    /// packs and records the returned primitives, which keeps the egui state machine separate from
    /// the D3D12 recording for the offscreen test.
    ///
    /// # Two passes on the first frame (K148)
    ///
    /// egui reserves a `Shape::Noop` placeholder for every area and window background, then fills
    /// the shape in at the end of the pass once the content's size is known. On the very first pass
    /// of a fresh [`egui::Context`] that placeholder is what remains in `FullOutput::shapes`, because
    /// the size is still unmeasured, and `tessellate` then yields no triangles at all. The C++ Dear
    /// ImGui backend hid this by always having sized the window from `SetNextWindowSize`; egui only
    /// knows the size after one frame. So when a pass produced no mesh primitives, the frame runs a
    /// second pass with the now-known sizes, which is what egui's own backend does for a resize.
    pub fn run_pass(
        &mut self,
        view: &OverlayView,
        input: &[UiInput],
        frame: UiFrameInfo,
    ) -> Result<(Vec<OverlayAction>, PassOutput), OverlayDrawError> {
        let scale = match view {
            OverlayView::Full(full) => full.scale.clamp(0.5, 2.0),
            _ => 1.0,
        };
        let context = self.context.get_or_insert_with(|| {
            let context = egui::Context::default();
            context.set_visuals(egui::Visuals::dark());
            context
        });
        context.set_zoom_factor(scale);
        let pixels_per_point = context.pixels_per_point().max(0.25);
        let screen_rect = egui::Rect::from_min_size(
            egui::Pos2::ZERO,
            egui::vec2(
                frame.output_width as f32 / pixels_per_point,
                frame.output_height as f32 / pixels_per_point,
            ),
        );

        let mut actions = Vec::new();
        let mut textures = egui::TexturesDelta::default();
        let mut primitives = Vec::new();
        // At most two passes: the second only when the first produced nothing to draw. Input is
        // consumed by the first pass; replaying it into a second would apply a click twice, so the
        // retry carries no events.
        for attempt in 0..2 {
            let events: &[UiInput] = if attempt == 0 { input } else { &[] };
            let raw = input::raw_input(
                events,
                screen_rect,
                frame.client_width,
                frame.client_height,
                frame.output_width,
                frame.output_height,
                frame.delta_seconds,
            );
            let mut ran = Vec::new();
            let output = context.run(raw, |ctx| {
                ran = menu::show(ctx, view);
            });
            textures.append(output.textures_delta);
            primitives = context.tessellate(output.shapes, pixels_per_point);
            if attempt == 0 {
                actions = ran;
            }
            if primitives
                .iter()
                .any(|p| matches!(p.primitive, egui::epaint::Primitive::Mesh(_)))
            {
                break;
            }
        }
        self.apply_textures(&textures)?;
        Ok((actions, PassOutput { primitives, pixels_per_point }))
    }

    /// Draws one frame onto the caller's own list and output texture. The adapter uses this when it
    /// owns both objects as COM interfaces; the port's [`OverlayRenderer::draw`] reaches the same
    /// code through pointer identities. Returns whether a draw was recorded.
    pub fn draw_with_raw(
        &mut self,
        list: &D3d12CommandList,
        output: &crate::adapters::driven::d3d12::resources::D3d12Texture,
        target: &RenderTarget,
        view: &OverlayView,
        input: &[UiInput],
        frame: UiFrameInfo,
    ) -> Result<bool, OverlayDrawError> {
        if let Some(reason) = self.disabled_slot().clone() {
            return Err(OverlayDrawError::Disabled(reason));
        }
        if matches!(view, OverlayView::Off) {
            return Ok(false);
        }
        if target.device != device_identity_of(&self.owner_device().ok_or(OverlayDrawError::NoDevice)?) {
            return Err(OverlayDrawError::DeviceChanged);
        }
        if self.renderer.format() != Some(target.format) {
            return Err(OverlayDrawError::UnsupportedFormat(target.format));
        }
        let (_, pass) = self.run_pass(view, input, frame)?;
        if pass.primitives.is_empty() {
            return Ok(false);
        }
        let packed = device::pack(&pass.primitives, target.width, target.height, pass.pixels_per_point)?;
        self.renderer.record(list.raw(), output.raw(), target, &packed, target.generated)?;
        self.count_draw(target.generated);
        Ok(true)
    }

    fn count_draw(&self, generated: bool) {
        let counter = if generated { &self.generated_draws } else { &self.real_draws };
        *counter.lock().unwrap_or_else(|poisoned| poisoned.into_inner()) += 1;
    }

    /// Records `packed` through the caller-owned list and output named by `target`'s identities.
    fn record_borrowed(
        &mut self,
        target: &RenderTarget,
        packed: &device::PackedDraws,
    ) -> Result<(), OverlayDrawError> {
        if target.command_list == 0 || target.resource == 0 {
            return Err(OverlayDrawError::NoDevice);
        }
        let raw_list = target.command_list as *mut core::ffi::c_void;
        let raw_output = target.resource as *mut core::ffi::c_void;
        // SAFETY: both identities are the caller's live command list and output resource, passed as
        // plain values at the port boundary and valid for the duration of this call.
        let (list, output) = unsafe {
            (
                ID3D12GraphicsCommandList::from_raw_borrowed(&raw_list),
                ID3D12Resource::from_raw_borrowed(&raw_output),
            )
        };
        let (Some(list), Some(output)) = (list, output) else {
            return Err(OverlayDrawError::NoDevice);
        };
        let renderer = &mut self.renderer;
        renderer.record(list, output, target, packed, target.generated)
    }
}

/// What one egui pass produced, ready to pack and record.
pub struct PassOutput {
    pub primitives: Vec<ClippedPrimitive>,
    pub pixels_per_point: f32,
}

impl Default for OverlayUi {
    fn default() -> Self {
        Self::new()
    }
}

impl OverlayRenderer for OverlayUi {
    fn draw(
        &mut self,
        target: &RenderTarget,
        view: &OverlayView,
        input: &[UiInput],
        frame: UiFrameInfo,
    ) -> Result<Vec<OverlayAction>, OverlayDrawError> {
        if let Some(reason) = self.disabled_slot().clone() {
            return Err(OverlayDrawError::Disabled(reason));
        }
        // Off records nothing, which keeps an Off output's overlay cost at zero.
        if matches!(view, OverlayView::Off) {
            return Ok(Vec::new());
        }
        let Some(owner) = self.owner_device() else {
            return Err(OverlayDrawError::NoDevice);
        };
        if target.device != owner.raw().as_raw() as usize {
            return Err(OverlayDrawError::DeviceChanged);
        }
        if self.renderer.format() != Some(target.format) {
            return Err(OverlayDrawError::UnsupportedFormat(target.format));
        }
        let (actions, pass) = self.run_pass(view, input, frame)?;
        if pass.primitives.is_empty() {
            return Ok(actions);
        }
        let packed = device::pack(
            &pass.primitives,
            target.width,
            target.height,
            pass.pixels_per_point,
        )?;
        self.record_borrowed(target, &packed)?;
        self.count_draw(target.generated);
        Ok(actions)
    }

    fn disabled(&self) -> bool {
        self.renderer.disabled_reason().is_some() || self.disabled_slot().is_some()
    }

    fn disabled_reason(&self) -> Option<String> {
        self.renderer.disabled_reason().or_else(|| self.disabled_slot().clone())
    }

    fn disable(&mut self, reason: &str) {
        let mut slot = self.disabled_slot();
        if slot.is_none() {
            *slot = Some(reason.to_owned());
        }
        drop(slot);
        self.renderer.disable(reason);
    }

    fn draw_counts(&self) -> (u64, u64) {
        let real = *self.real_draws.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        let generated = *self
            .generated_draws
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        (real, generated)
    }
}

/// Borrows a caller-owned identity for one closure. `from_raw_borrowed` reads the pointer without
/// taking ownership, so the caller's reference count is untouched; callers use this for the two
/// objects a record needs at once.
#[allow(dead_code)]
fn with_borrowed_list<T>(identity: usize, f: impl FnOnce(&ID3D12GraphicsCommandList) -> T) -> Result<T, OverlayDrawError> {
    if identity == 0 {
        return Err(OverlayDrawError::NoDevice);
    }
    let raw = identity as *mut core::ffi::c_void;
    // SAFETY: the identity is the caller's live `ID3D12GraphicsCommandList`, passed as a plain value
    // at the port boundary and valid for the duration of this call.
    let list = unsafe { ID3D12GraphicsCommandList::from_raw_borrowed(&raw) }
        .ok_or(OverlayDrawError::NoDevice)?;
    Ok(f(list))
}

/// As [`with_borrowed_list`], for the output back buffer. Kept beside its sibling so the port's two
/// identities are read the same way.
#[allow(dead_code)]
fn with_borrowed_resource<T>(identity: usize, f: impl FnOnce(&ID3D12Resource) -> T) -> Result<T, OverlayDrawError> {
    if identity == 0 {
        return Err(OverlayDrawError::NoDevice);
    }
    let raw = identity as *mut core::ffi::c_void;
    // SAFETY: the identity is the caller's live output resource, valid for this call.
    let resource = unsafe { ID3D12Resource::from_raw_borrowed(&raw) }
        .ok_or(OverlayDrawError::NoDevice)?;
    Ok(f(resource))
}

/// The raw pointer identity of a device, so the port's numeric target can be compared without
/// exposing the COM type.
pub fn device_identity_of(device: &D3d12Device) -> usize {
    device.raw().as_raw() as usize
}
