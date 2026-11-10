//! The overlay's D3D12 objects and draw recording (ports `ImGui_ImplDX12_CreateDeviceObjects` and
//! `ImGui_ImplDX12_RenderDrawData`, with the resource plumbing of `ui/ui.cpp`).
//!
//! One graphics pipeline, compiled at first use from [`VS_SOURCE`]/[`PS_SOURCE`] with the system
//! `D3DCompile` (the same call the Stage 1 passes make), one root signature (8 root constants and an
//! SRV table at `t0` with a linear/clamp static sampler), one shader-visible heap with a single SRV
//! slot for egui's atlas, two 64 KiB upload buffers rewritten per draw, and a ring of eight RTVs.
//!
//! # In-flight assumption
//!
//! The C++ backend rotated `NumFramesInFlight = 4` vertex/index buffers and RTVs with **no fence of
//! its own**, which is safe only because draws never overlap on a list. This renderer makes the same
//! assumption explicitly: the swapchain's present waits for each output's own fence before recording
//! the next output, so at most one overlay draw is in flight per upload buffer. A change that lets
//! two draws overlap must add a fence here first. See [`INFLIGHT_NOTE`].
//!
//! # Pure and impure halves
//!
//! [`pack`] turns egui's primitives into the vertex/index byte streams and the scissored batch list
//! with no device involved, so the packing rules are unit-testable. Everything else needs a device
//! and is exercised by the offscreen renderer test (`tests/overlay_ui.rs`).
use std::sync::Mutex;

use egui::epaint::Primitive;
use windows::core::Interface;
use windows::Win32::Foundation::RECT;
use windows::Win32::Graphics::Direct3D::Fxc::{D3DCompile, D3DCOMPILE_OPTIMIZATION_LEVEL3};
use windows::Win32::Graphics::Direct3D::{D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, ID3DBlob};
use windows::Win32::Graphics::Direct3D12::{
    ID3D12CommandAllocator, ID3D12CommandList, ID3D12CommandQueue, ID3D12DescriptorHeap,
    ID3D12Device, ID3D12Fence, ID3D12GraphicsCommandList, ID3D12PipelineState, ID3D12Resource,
    ID3D12RootSignature, D3D12_BLEND_DESC, D3D12_CPU_DESCRIPTOR_HANDLE,
    D3D12_DEPTH_STENCIL_DESC, D3D12_DESCRIPTOR_HEAP_DESC, D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
    D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
    D3D12_DESCRIPTOR_HEAP_TYPE_RTV, D3D12_DESCRIPTOR_RANGE, D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
    D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_GPU_DESCRIPTOR_HANDLE,
    D3D12_GRAPHICS_PIPELINE_STATE_DESC, D3D12_HEAP_FLAG_NONE, D3D12_HEAP_PROPERTIES,
    D3D12_HEAP_TYPE_DEFAULT, D3D12_HEAP_TYPE_UPLOAD, D3D12_INDEX_BUFFER_VIEW,
    D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, D3D12_INPUT_ELEMENT_DESC, D3D12_INPUT_LAYOUT_DESC,
    D3D12_LOGIC_OP_NOOP, D3D12_PLACED_SUBRESOURCE_FOOTPRINT, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE,
    D3D12_RASTERIZER_DESC, D3D12_RENDER_TARGET_BLEND_DESC, D3D12_RENDER_TARGET_VIEW_DESC,
    D3D12_RESOURCE_BARRIER, D3D12_RESOURCE_BARRIER_0, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
    D3D12_RESOURCE_BARRIER_FLAG_NONE, D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
    D3D12_RESOURCE_DESC, D3D12_RESOURCE_DIMENSION_BUFFER, D3D12_RESOURCE_DIMENSION_TEXTURE2D,
    D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_GENERIC_READ,
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET,
    D3D12_RESOURCE_STATES, D3D12_RESOURCE_TRANSITION_BARRIER, D3D12_ROOT_PARAMETER,
    D3D12_ROOT_PARAMETER_0, D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
    D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE, D3D12_ROOT_SIGNATURE_DESC,
    D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT, D3D12_RTV_DIMENSION_TEXTURE2D,
    D3D12_SHADER_BYTECODE, D3D12_SHADER_RESOURCE_VIEW_DESC, D3D12_SHADER_RESOURCE_VIEW_DESC_0,
    D3D12_SHADER_VISIBILITY_ALL, D3D12_SRV_DIMENSION_TEXTURE2D,
    D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK, D3D12_STATIC_SAMPLER_DESC, D3D12_TEX2D_RTV,
    D3D12_TEX2D_SRV, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_COPY_LOCATION,
    D3D12_TEXTURE_COPY_LOCATION_0, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
    D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT,
    D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_VERTEX_BUFFER_VIEW,
    D3D12_VIEWPORT, D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF,
    D3D12_CULL_MODE_NONE, D3D12_FILL_MODE_SOLID, D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_ONE,
    D3D12_BLEND_OP_ADD, D3D12_COLOR_WRITE_ENABLE_ALL, D3D12_COMPARISON_FUNC_ALWAYS,
    D3D_ROOT_SIGNATURE_VERSION_1,
};
use windows::Win32::Graphics::Dxgi::Common::{
    DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_UNKNOWN,
    DXGI_SAMPLE_DESC,
};

use crate::application::ports::ui_renderer::{OutputFormat, OverlayDrawError, RenderTarget};

/// The vertex shader source.
pub const VS_SOURCE: &str = include_str!("hlsl/overlay_vs.hlsl");
/// The pixel shader source.
pub const PS_SOURCE: &str = include_str!("hlsl/overlay_ps.hlsl");
/// Repeats the in-flight assumption from the module doc where the buffers are bound.
pub const INFLIGHT_NOTE: &str =
    "at most one overlay draw in flight per upload buffer (P2.8); a fence is required first";

/// Root constants: `screen_size.xy`, `inv_screen_size.xy`, then four zero words for alignment.
pub const ROOT_CONSTANTS: u32 = 8;
/// The vertex stride in bytes: `float2 position`, `float2 uv`, `uint4 colour`.
pub const VERTEX_STRIDE: u32 = 20;
/// The most bytes a single draw's vertex or index channel may use (the ImGui backend's 64 KiB).
pub const MAX_UPLOAD_BYTES: usize = 64 * 1024;
/// The RTVs rotated per draw (the C++ `kRtvDescriptors`).
pub const RTV_SLOTS: u32 = 8;
/// `DXGI_FORMAT_R8G8B8A8_UNORM`, the only output format this renderer blends into.
pub const OUTPUT_R8G8B8A8_UNORM: u32 = 28;

/// One egui vertex, exactly the shader's input layout.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct OverlayVertex {
    pub pos: [f32; 2],
    pub uv: [f32; 2],
    /// sRGBA, premultiplied (epaint's `Color32`).
    pub color: [u8; 4],
}

/// A clip rectangle in physical pixels. A plain struct so batching stays testable without Win32.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ClipRect {
    pub left: i32,
    pub top: i32,
    pub right: i32,
    pub bottom: i32,
}

impl ClipRect {
    fn to_win32(self) -> RECT {
        RECT { left: self.left, top: self.top, right: self.right, bottom: self.bottom }
    }
}

/// One scissored draw into the merged vertex/index stream.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DrawBatch {
    pub index_offset: u32,
    pub index_count: u32,
    pub vertex_offset: u32,
    pub clip: ClipRect,
}

/// One egui primitive list packed into the vertex and index channels plus its draw list.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct PackedDraws {
    pub vertices: Vec<u8>,
    pub indices: Vec<u8>,
    pub batches: Vec<DrawBatch>,
    pub vertex_count: u32,
}

/// Clamps an egui clip rectangle to the output, in physical pixels.
pub fn clip_rect(rect: &egui::Rect, width: u32, height: u32, pixels_per_point: f32) -> ClipRect {
    let scale = if pixels_per_point > 0.0 { pixels_per_point } else { 1.0 };
    let left = (rect.left() * scale).floor().clamp(0.0, width as f32) as i32;
    let top = (rect.top() * scale).floor().clamp(0.0, height as f32) as i32;
    let right = (rect.right() * scale).ceil().clamp(0.0, width as f32) as i32;
    let bottom = (rect.bottom() * scale).ceil().clamp(0.0, height as f32) as i32;
    ClipRect { left, top, right: right.max(left), bottom: bottom.max(top) }
}

/// Packs egui's primitives into one vertex channel, one index channel and a batch list.
///
/// `PaintCallback` primitives are skipped (the overlay never registers one) and so are empty meshes.
/// A stream over [`MAX_UPLOAD_BYTES`] is refused rather than truncated.
pub fn pack(
    primitives: &[egui::ClippedPrimitive],
    width: u32,
    height: u32,
    pixels_per_point: f32,
) -> Result<PackedDraws, OverlayDrawError> {
    let mut packed = PackedDraws::default();
    for primitive in primitives {
        let Primitive::Mesh(mesh) = &primitive.primitive else {
            continue;
        };
        if mesh.indices.is_empty() || mesh.vertices.is_empty() {
            continue;
        }
        let vertex_offset = packed.vertex_count;
        let index_offset = u32::try_from(packed.indices.len() / 4)
            .map_err(|_| OverlayDrawError::Failed("the overlay ran out of index space".to_owned()))?;
        for vertex in &mesh.vertices {
            for value in [vertex.pos.x, vertex.pos.y, vertex.uv.x, vertex.uv.y] {
                packed.vertices.extend_from_slice(&value.to_le_bytes());
            }
            packed.vertices.extend_from_slice(&vertex.color.to_array());
            packed.vertex_count += 1;
        }
        for index in &mesh.indices {
            packed.indices.extend_from_slice(&index.to_le_bytes());
        }
        packed.batches.push(DrawBatch {
            index_offset,
            index_count: mesh.indices.len() as u32,
            vertex_offset,
            clip: clip_rect(&primitive.clip_rect, width, height, pixels_per_point),
        });
        if packed.vertices.len() > MAX_UPLOAD_BYTES || packed.indices.len() > MAX_UPLOAD_BYTES {
            return Err(OverlayDrawError::Failed(
                "the overlay's vertex or index buffer exceeded 64 KiB".to_owned(),
            ));
        }
    }
    Ok(packed)
}

/// The screen size and its reciprocal, then four zero words.
pub fn root_constants(width: f32, height: f32) -> [u32; ROOT_CONSTANTS as usize] {
    let width = width.max(1.0);
    let height = height.max(1.0);
    [
        width.to_bits(),
        height.to_bits(),
        (1.0 / width).to_bits(),
        (1.0 / height).to_bits(),
        0,
        0,
        0,
        0,
    ]
}

/// The staging bytes for a `width`x`height` RGBA upload, rows aligned to 256 **bytes**.
pub fn staging_size(width: u32, height: u32) -> u64 {
    let pitch = aligned_pitch(width);
    u64::from(pitch) * u64::from(height)
}

/// A row's byte pitch: `width * 4` rounded up to the 256-byte texture copy alignment.
fn aligned_pitch(width: u32) -> u32 {
    let row = width.max(1).saturating_mul(4);
    row.div_ceil(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT) * D3D12_TEXTURE_DATA_PITCH_ALIGNMENT
}

struct DeviceObjects {
    device: ID3D12Device,
    root_signature: ID3D12RootSignature,
    pipeline: ID3D12PipelineState,
    srv_heap: ID3D12DescriptorHeap,
    srv_increment: u32,
    vertex_upload: ID3D12Resource,
    index_upload: ID3D12Resource,
}

struct RtvRing {
    heap: ID3D12DescriptorHeap,
    increment: u32,
    next: u32,
}

/// The overlay D3D12 renderer. One per DLL. The state it shares with the status query and the
/// present hook is behind one mutex.
pub struct EguiD3d12Renderer {
    objects: Option<DeviceObjects>,
    rtv: Option<RtvRing>,
    format: Option<OutputFormat>,
    device_identity: usize,
    atlas: Option<egui::TextureId>,
    /// The atlas texture the SRV slot points at. It must outlive the descriptor: dropping it would
    /// destroy the resource while the shader-visible heap still references it.
    atlas_texture: Option<ID3D12Resource>,
    state: Mutex<RendererState>,
}

#[derive(Default)]
struct RendererState {
    disabled_reason: Option<String>,
    real_draws: u64,
    generated_draws: u64,
}

impl std::fmt::Debug for EguiD3d12Renderer {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("EguiD3d12Renderer")
            .field("ready", &self.objects.is_some())
            .field("format", &self.format)
            .finish()
    }
}

impl Default for EguiD3d12Renderer {
    fn default() -> Self {
        Self::new()
    }
}

impl EguiD3d12Renderer {
    pub fn new() -> Self {
        Self {
            objects: None,
            rtv: None,
            format: None,
            device_identity: 0,
            atlas: None,
            atlas_texture: None,
            state: Mutex::new(RendererState::default()),
        }
    }

    fn lock(&self) -> std::sync::MutexGuard<'_, RendererState> {
        self.state.lock().unwrap_or_else(|poisoned| poisoned.into_inner())
    }

    /// The output format the objects were built for, if any.
    pub fn format(&self) -> Option<OutputFormat> {
        self.format
    }

    /// True once the GPU objects exist.
    pub fn ready(&self) -> bool {
        self.objects.is_some()
    }

    /// The device identity the objects were built for.
    pub fn device_identity(&self) -> usize {
        self.device_identity
    }

    /// Creates the GPU objects for `device` and `format`. A repeat call for the same pair is a no-op;
    /// a different device or format is a defined refusal (the C++ disabled the overlay for both).
    pub fn initialize(
        &mut self,
        device: &ID3D12Device,
        format: OutputFormat,
    ) -> Result<(), OverlayDrawError> {
        if self.objects.is_some() {
            if self.format != Some(format) {
                return Err(OverlayDrawError::UnsupportedFormat(format));
            }
            if self.device_identity != identity(device) {
                return Err(OverlayDrawError::DeviceChanged);
            }
            return Ok(());
        }
        if format.0 != OUTPUT_R8G8B8A8_UNORM {
            return Err(OverlayDrawError::UnsupportedFormat(format));
        }
        let root_signature = create_root_signature(device)?;
        let pipeline = create_pipeline(device, &root_signature)?;
        let (srv_heap, srv_increment) = create_srv_heap(device)?;
        let rtv = create_rtv_ring(device)?;
        let vertex_upload = create_upload(device, MAX_UPLOAD_BYTES as u64)?;
        let index_upload = create_upload(device, MAX_UPLOAD_BYTES as u64)?;
        self.objects = Some(DeviceObjects {
            device: device.clone(),
            root_signature,
            pipeline,
            srv_heap,
            srv_increment,
            vertex_upload,
            index_upload,
        });
        self.rtv = Some(rtv);
        self.format = Some(format);
        self.device_identity = identity(device);
        Ok(())
    }

    /// Records a whole-image atlas update into a fresh texture and points the SRV slot at it.
    ///
    /// The caller supplies the upload queue, allocator, list and fence and the value to signal; the
    /// upload is one record-submit-wait on that pair, as the C++ `create_device_objects` did.
    #[allow(clippy::too_many_arguments)]
    pub fn upload_atlas(
        &mut self,
        queue: &ID3D12CommandQueue,
        allocator: &ID3D12CommandAllocator,
        list: &ID3D12GraphicsCommandList,
        fence: &ID3D12Fence,
        value: u64,
        id: egui::TextureId,
        delta: &egui::epaint::ImageDelta,
        wait: std::time::Duration,
    ) -> Result<(), OverlayDrawError> {
        let Some(objects) = self.objects.as_ref() else {
            return Err(OverlayDrawError::NoDevice);
        };
        let (width, height, pixels) = match &delta.image {
            egui::epaint::ImageData::Color(image) => {
                let bytes: Vec<u8> = image.pixels.iter().flat_map(|color| color.to_array()).collect();
                (image.size[0] as u32, image.size[1] as u32, bytes)
            }
        };
        if width == 0 || height == 0 {
            return Ok(());
        }
        if delta.pos.is_some() {
            // epaint sends whole-image deltas for its atlases (including when one grows); a partial
            // delta would need a box copy, which this renderer does not implement. Refusing it is
            // honest and keeps the atlas from showing the wrong glyphs.
            return Err(OverlayDrawError::Failed(
                "the overlay renderer only supports whole-image texture updates".to_owned(),
            ));
        }
        let texture = create_atlas_texture(&objects.device, width, height)?;
        let staging = create_upload(&objects.device, staging_size(width, height))?;
        // SAFETY: the caller owns the allocator, list and fence and guarantees previous work retired.
        unsafe {
            allocator.Reset().map_err(|error| {
                OverlayDrawError::Failed(format!("overlay atlas allocator reset ({:#010x})", error.code().0))
            })?;
            list.Reset(allocator, None).map_err(|error| {
                OverlayDrawError::Failed(format!("overlay atlas list reset ({:#010x})", error.code().0))
            })?;
        }
        upload_pixels(
            &objects.device,
            &objects.srv_heap,
            objects.srv_increment,
            &staging,
            &texture,
            &pixels,
            width,
            height,
            list,
        )?;
        submit_and_wait(queue, list, fence, value, wait)?;
        self.atlas = Some(id);
        self.atlas_texture = Some(texture);
        Ok(())
    }

    /// The texture id the SRV slot currently holds.
    pub fn atlas_texture(&self) -> Option<egui::TextureId> {
        self.atlas
    }

    /// The atlas resource (test seam: lets a probe read back what was uploaded).
    pub fn atlas_resource(&self) -> Option<&ID3D12Resource> {
        self.atlas_texture.as_ref()
    }

    /// The texture id of the atlas the SRV slot points at (test seam).
    pub fn atlas_id(&self) -> Option<egui::TextureId> {
        self.atlas
    }

    /// Records the packed draws onto the caller's open list, transitioning `target` into and back out
    /// of `RENDER_TARGET`. `output` is the caller's own reference to the back buffer, so no pointer
    /// is reconstructed from an identity here.
    pub fn record(
        &mut self,
        list: &ID3D12GraphicsCommandList,
        output: &ID3D12Resource,
        target: &RenderTarget,
        packed: &PackedDraws,
        generated: bool,
    ) -> Result<(), OverlayDrawError> {
        if let Some(reason) = self.lock().disabled_reason.clone() {
            return Err(OverlayDrawError::Disabled(reason));
        }
        let Some(_objects) = self.objects.as_ref() else {
            return Err(OverlayDrawError::NoDevice);
        };
        if target.device != self.device_identity {
            return Err(OverlayDrawError::DeviceChanged);
        }
        if self.format != Some(target.format) {
            return Err(OverlayDrawError::UnsupportedFormat(target.format));
        }
        if packed.batches.is_empty() {
            return Ok(());
        }
        if packed.vertices.len() > MAX_UPLOAD_BYTES || packed.indices.len() > MAX_UPLOAD_BYTES {
            return Err(OverlayDrawError::Failed(
                "the overlay's vertex or index buffer exceeded 64 KiB".to_owned(),
            ));
        }

        let before = D3D12_RESOURCE_STATES(target.state.0 as i32);
        let transition = before != D3D12_RESOURCE_STATE_RENDER_TARGET;
        if transition {
            let barrier = transition_barrier(output, before, D3D12_RESOURCE_STATE_RENDER_TARGET);
            // SAFETY: the barrier names the live output resource in `before`.
            unsafe { list.ResourceBarrier(&[barrier]) };
        }

        let result = self.record_draws(list, output, target, packed);

        if transition {
            let barrier = transition_barrier(output, D3D12_RESOURCE_STATE_RENDER_TARGET, before);
            // SAFETY: the same live output resource, back in its original state.
            unsafe { list.ResourceBarrier(&[barrier]) };
        }
        result?;
        let mut state = self.lock();
        if generated {
            state.generated_draws += 1;
        } else {
            state.real_draws += 1;
        }
        Ok(())
    }

    fn record_draws(
        &mut self,
        list: &ID3D12GraphicsCommandList,
        output: &ID3D12Resource,
        target: &RenderTarget,
        packed: &PackedDraws,
    ) -> Result<(), OverlayDrawError> {
        let Some(ring) = self.rtv.as_mut() else {
            return Err(OverlayDrawError::NoDevice);
        };
        let slot = ring.next % RTV_SLOTS;
        ring.next = ring.next.wrapping_add(1);
        let rtv = cpu_handle_base(&ring.heap, ring.increment, slot);
        let objects = self.objects.as_ref().ok_or(OverlayDrawError::NoDevice)?;

        write_mapped(&objects.vertex_upload, &packed.vertices)?;
        write_mapped(&objects.index_upload, &packed.indices)?;

        let srv = gpu_handle(&objects.srv_heap, objects.srv_increment, 0);
        let constants = root_constants(target.width as f32, target.height as f32);
        let rtv_desc = D3D12_RENDER_TARGET_VIEW_DESC {
            Format: DXGI_FORMAT_R8G8B8A8_UNORM,
            ViewDimension: D3D12_RTV_DIMENSION_TEXTURE2D,
            Anonymous: windows::Win32::Graphics::Direct3D12::D3D12_RENDER_TARGET_VIEW_DESC_0 {
                Texture2D: D3D12_TEX2D_RTV::default(),
            },
        };
        let vertex_view = D3D12_VERTEX_BUFFER_VIEW {
            BufferLocation: gpu_address(&objects.vertex_upload),
            SizeInBytes: packed.vertices.len() as u32,
            StrideInBytes: VERTEX_STRIDE,
        };
        let index_view = D3D12_INDEX_BUFFER_VIEW {
            BufferLocation: gpu_address(&objects.index_upload),
            SizeInBytes: packed.indices.len() as u32,
            Format: DXGI_FORMAT_R32_UINT,
        };
        let viewport = D3D12_VIEWPORT {
            TopLeftX: 0.0,
            TopLeftY: 0.0,
            Width: target.width as f32,
            Height: target.height as f32,
            MinDepth: 0.0,
            MaxDepth: 1.0,
        };
        // SAFETY: the list is open and recording; every object bound here is live for the recording
        // and the SRV was written before the first draw that samples it.
        unsafe {
            objects.device.CreateRenderTargetView(output, Some(&rtv_desc), rtv);
            list.SetDescriptorHeaps(&[Some(objects.srv_heap.clone())]);
            list.SetGraphicsRootSignature(&objects.root_signature);
            list.SetPipelineState(&objects.pipeline);
            list.SetGraphicsRoot32BitConstants(0, ROOT_CONSTANTS, constants.as_ptr().cast(), 0);
            // Parameter 0 is the root-constant block, parameter 1 the SRV table (see
            // `create_root_signature`).
            list.SetGraphicsRootDescriptorTable(1, srv);
            list.OMSetRenderTargets(1, Some(&rtv), false, None);
            // A rasterizer stage needs a viewport; without one every triangle is clipped away.
            list.RSSetViewports(&[viewport]);
            // The scissor test is off only when the scissor rectangle is left at its default; it
            // must be set per draw, so each batch below sets its own and the loop resets it.
            list.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list.IASetVertexBuffers(0, Some(&[vertex_view]));
            list.IASetIndexBuffer(Some(&index_view));
            for batch in &packed.batches {
                list.RSSetScissorRects(&[batch.clip.to_win32()]);
                list.DrawIndexedInstanced(batch.index_count, 1, batch.index_offset, 0, batch.vertex_offset);
            }
            // Leave the whole output scissored so whatever records after us is unaffected.
            let full = RECT {
                left: 0,
                top: 0,
                right: target.width as i32,
                bottom: target.height as i32,
            };
            list.RSSetScissorRects(&[full]);
        }
        Ok(())
    }

    /// Marks the renderer disabled, permanently for this session.
    pub fn disable(&mut self, reason: &str) {
        let mut state = self.lock();
        if state.disabled_reason.is_none() {
            state.disabled_reason = Some(reason.to_owned());
        }
    }

    pub fn disabled_reason(&self) -> Option<String> {
        self.lock().disabled_reason.clone()
    }

    pub fn draw_counts(&self) -> (u64, u64) {
        let state = self.lock();
        (state.real_draws, state.generated_draws)
    }
}

// ---------------------------------------------------------------------------
// Device objects
// ---------------------------------------------------------------------------

fn identity<T: Interface>(object: &T) -> usize {
    object.as_raw() as usize
}

fn create_srv_heap(device: &ID3D12Device) -> Result<(ID3D12DescriptorHeap, u32), OverlayDrawError> {
    let desc = D3D12_DESCRIPTOR_HEAP_DESC {
        Type: D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        NumDescriptors: 1,
        Flags: D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
        NodeMask: 0,
    };
    // SAFETY: `desc` is a fully initialized local that outlives the call.
    let heap = unsafe { device.CreateDescriptorHeap::<ID3D12DescriptorHeap>(&desc) }.map_err(|error| {
        OverlayDrawError::Failed(format!("overlay SRV heap creation ({:#010x})", error.code().0))
    })?;
    // SAFETY: a pure query on a live device.
    let increment =
        unsafe { device.GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) };
    Ok((heap, increment))
}

fn create_rtv_ring(device: &ID3D12Device) -> Result<RtvRing, OverlayDrawError> {
    let desc = D3D12_DESCRIPTOR_HEAP_DESC {
        Type: D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
        NumDescriptors: RTV_SLOTS,
        Flags: D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
        NodeMask: 0,
    };
    // SAFETY: `desc` is a fully initialized local that outlives the call.
    let heap = unsafe { device.CreateDescriptorHeap::<ID3D12DescriptorHeap>(&desc) }.map_err(|error| {
        OverlayDrawError::Failed(format!("overlay RTV heap creation ({:#010x})", error.code().0))
    })?;
    // SAFETY: a pure query on a live device.
    let increment = unsafe { device.GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV) };
    Ok(RtvRing { heap, increment, next: 0 })
}

fn create_root_signature(device: &ID3D12Device) -> Result<ID3D12RootSignature, OverlayDrawError> {
    let srv_range = D3D12_DESCRIPTOR_RANGE {
        RangeType: D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
        NumDescriptors: 1,
        BaseShaderRegister: 0,
        RegisterSpace: 0,
        OffsetInDescriptorsFromTableStart: 0,
    };
    let mut parameters = [D3D12_ROOT_PARAMETER::default(); 2];
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Anonymous = D3D12_ROOT_PARAMETER_0 {
        Constants: windows::Win32::Graphics::Direct3D12::D3D12_ROOT_CONSTANTS {
            ShaderRegister: 0,
            RegisterSpace: 0,
            Num32BitValues: ROOT_CONSTANTS,
        },
    };
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].Anonymous = D3D12_ROOT_PARAMETER_0 {
        DescriptorTable: windows::Win32::Graphics::Direct3D12::D3D12_ROOT_DESCRIPTOR_TABLE {
            NumDescriptorRanges: 1,
            pDescriptorRanges: &srv_range,
        },
    };
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // Linear filtering with clamp addressing: epaint rasterises glyphs at their own size, so the
    // filter only smooths a fractional position, matching the ImGui backend's sampler.
    let sampler = D3D12_STATIC_SAMPLER_DESC {
        Filter: D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        AddressU: D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        AddressV: D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        AddressW: D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        MipLODBias: 0.0,
        MaxAnisotropy: 1,
        ComparisonFunc: D3D12_COMPARISON_FUNC_ALWAYS,
        BorderColor: D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK,
        MinLOD: 0.0,
        MaxLOD: f32::MAX,
        ShaderRegister: 0,
        RegisterSpace: 0,
        ShaderVisibility: D3D12_SHADER_VISIBILITY_ALL,
    };

    let description = D3D12_ROOT_SIGNATURE_DESC {
        NumParameters: parameters.len() as u32,
        pParameters: parameters.as_ptr(),
        NumStaticSamplers: 1,
        pStaticSamplers: &sampler,
        Flags: D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT,
    };
    let mut serialized: Option<ID3DBlob> = None;
    let mut errors: Option<ID3DBlob> = None;
    // SAFETY: the description and its arrays outlive the call, which copies them.
    let result = unsafe {
        windows::Win32::Graphics::Direct3D12::D3D12SerializeRootSignature(
            &description,
            D3D_ROOT_SIGNATURE_VERSION_1,
            &mut serialized,
            Some(&mut errors),
        )
    };
    let blob = match (result, serialized) {
        (Ok(()), Some(blob)) => blob,
        (Err(error), _) => {
            return Err(OverlayDrawError::Failed(format!(
                "overlay root signature serialization ({:#010x})",
                error.code().0
            )))
        }
        (Ok(()), None) => {
            return Err(OverlayDrawError::Failed("overlay root signature serialization".to_owned()))
        }
    };
    // SAFETY: the blob owns the serialized bytes; the slice is read while it is alive.
    let bytes =
        unsafe { std::slice::from_raw_parts(blob.GetBufferPointer().cast::<u8>(), blob.GetBufferSize()) };
    // SAFETY: `bytes` is a valid serialized root signature for the call, which copies it.
    unsafe { device.CreateRootSignature::<ID3D12RootSignature>(0, bytes) }.map_err(|error| {
        OverlayDrawError::Failed(format!("overlay root signature creation ({:#010x})", error.code().0))
    })
}

/// Compiles one shader to `entry`, `target` at optimisation level 3 with the system `D3DCompile`.
pub fn compile_shader(source: &str, entry: &str, target: &str) -> Result<Vec<u8>, OverlayDrawError> {
    let entry_name = std::ffi::CString::new(entry)
        .map_err(|_| OverlayDrawError::Failed("overlay shader entry name".to_owned()))?;
    let target_name = std::ffi::CString::new(target)
        .map_err(|_| OverlayDrawError::Failed("overlay shader target name".to_owned()))?;
    let mut bytecode: Option<ID3DBlob> = None;
    let mut errors: Option<ID3DBlob> = None;
    // SAFETY: the source pointer and length describe a live UTF-8 string; the C strings outlive the
    // call; the out pointers are valid locals; D3DCompile copies what it needs.
    let result = unsafe {
        D3DCompile(
            source.as_ptr().cast(),
            source.len(),
            windows::core::PCSTR::from_raw(c"overlay.hlsl".as_ptr().cast()),
            None,
            None,
            windows::core::PCSTR::from_raw(entry_name.as_ptr().cast()),
            windows::core::PCSTR::from_raw(target_name.as_ptr().cast()),
            D3DCOMPILE_OPTIMIZATION_LEVEL3,
            0,
            &mut bytecode,
            Some(&mut errors),
        )
    };
    let blob = match (result, bytecode) {
        (Ok(()), Some(blob)) => blob,
        (Err(error), _) => {
            let message = errors
                .as_ref()
                .map(|blob| {
                    // SAFETY: a compiler error blob is a null-terminated ASCII message.
                    let bytes = unsafe {
                        std::slice::from_raw_parts(blob.GetBufferPointer().cast::<u8>(), blob.GetBufferSize())
                    };
                    String::from_utf8_lossy(bytes).into_owned()
                })
                .unwrap_or_else(|| "no message".to_owned());
            tracing::error!("sedici: overlay shader {entry} failed: {message}");
            return Err(OverlayDrawError::Failed(format!(
                "overlay shader compilation {entry} ({:#010x})",
                error.code().0
            )));
        }
        (Ok(()), None) => {
            return Err(OverlayDrawError::Failed("overlay shader compilation".to_owned()))
        }
    };
    // SAFETY: the blob owns the bytecode; the slice is read while the blob is alive.
    let bytes =
        unsafe { std::slice::from_raw_parts(blob.GetBufferPointer().cast::<u8>(), blob.GetBufferSize()) };
    Ok(bytes.to_vec())
}

fn create_pipeline(
    device: &ID3D12Device,
    root_signature: &ID3D12RootSignature,
) -> Result<ID3D12PipelineState, OverlayDrawError> {
    let vs = compile_shader(VS_SOURCE, "VSMain", "vs_5_1")?;
    let ps = compile_shader(PS_SOURCE, "PSMain", "ps_5_1")?;

    let input_elements = [
        D3D12_INPUT_ELEMENT_DESC {
            SemanticName: windows::core::PCSTR::from_raw(c"POSITION".as_ptr().cast()),
            SemanticIndex: 0,
            Format: DXGI_FORMAT_R32G32_FLOAT,
            InputSlot: 0,
            AlignedByteOffset: 0,
            InputSlotClass: D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
            InstanceDataStepRate: 0,
        },
        D3D12_INPUT_ELEMENT_DESC {
            SemanticName: windows::core::PCSTR::from_raw(c"TEXCOORD".as_ptr().cast()),
            SemanticIndex: 0,
            Format: DXGI_FORMAT_R32G32_FLOAT,
            InputSlot: 0,
            AlignedByteOffset: 8,
            InputSlotClass: D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
            InstanceDataStepRate: 0,
        },
        D3D12_INPUT_ELEMENT_DESC {
            SemanticName: windows::core::PCSTR::from_raw(c"COLOR".as_ptr().cast()),
            SemanticIndex: 0,
            Format: DXGI_FORMAT_R8G8B8A8_UNORM,
            InputSlot: 0,
            AlignedByteOffset: 16,
            InputSlotClass: D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
            InstanceDataStepRate: 0,
        },
    ];

    // Premultiplied alpha: epaint's `Color32` is premultiplied, the pixel shader multiplies it by the
    // (straight-alpha) atlas, and the target composites with `One, INV_SRC_ALPHA` in both channels.
    // The ImGui backend used `SRC_ALPHA` because its own shader did the multiply by alpha; egui does
    // not, so `One` is the factor that matches.
    let mut targets = [D3D12_RENDER_TARGET_BLEND_DESC::default(); 8];
    targets[0] = D3D12_RENDER_TARGET_BLEND_DESC {
        BlendEnable: true.into(),
        LogicOpEnable: false.into(),
        SrcBlend: D3D12_BLEND_ONE,
        DestBlend: D3D12_BLEND_INV_SRC_ALPHA,
        BlendOp: D3D12_BLEND_OP_ADD,
        SrcBlendAlpha: D3D12_BLEND_ONE,
        DestBlendAlpha: D3D12_BLEND_INV_SRC_ALPHA,
        BlendOpAlpha: D3D12_BLEND_OP_ADD,
        LogicOp: D3D12_LOGIC_OP_NOOP,
        RenderTargetWriteMask: D3D12_COLOR_WRITE_ENABLE_ALL.0 as u8,
    };
    let blend = D3D12_BLEND_DESC {
        AlphaToCoverageEnable: false.into(),
        IndependentBlendEnable: false.into(),
        RenderTarget: targets,
    };
    let rasterizer = D3D12_RASTERIZER_DESC {
        FillMode: D3D12_FILL_MODE_SOLID,
        // egui is not consistent about triangle winding, so culling is off.
        CullMode: D3D12_CULL_MODE_NONE,
        FrontCounterClockwise: false.into(),
        DepthBias: 0,
        DepthBiasClamp: 0.0,
        SlopeScaledDepthBias: 0.0,
        DepthClipEnable: true.into(),
        MultisampleEnable: false.into(),
        AntialiasedLineEnable: false.into(),
        ForcedSampleCount: 0,
        ConservativeRaster: D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF,
    };

    let depth = D3D12_DEPTH_STENCIL_DESC {
        DepthEnable: false.into(),
        StencilEnable: false.into(),
        ..D3D12_DEPTH_STENCIL_DESC::default()
    };

    let desc = D3D12_GRAPHICS_PIPELINE_STATE_DESC {
        pRootSignature: std::mem::ManuallyDrop::new(Some(root_signature.clone())),
        VS: D3D12_SHADER_BYTECODE { pShaderBytecode: vs.as_ptr().cast(), BytecodeLength: vs.len() },
        PS: D3D12_SHADER_BYTECODE { pShaderBytecode: ps.as_ptr().cast(), BytecodeLength: ps.len() },
        BlendState: blend,
        RasterizerState: rasterizer,
        DepthStencilState: depth,
        InputLayout: D3D12_INPUT_LAYOUT_DESC {
            pInputElementDescs: input_elements.as_ptr(),
            NumElements: input_elements.len() as u32,
        },
        PrimitiveTopologyType: D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE,
        NumRenderTargets: 1,
        RTVFormats: {
            let mut formats = [DXGI_FORMAT_UNKNOWN; 8];
            formats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
            formats
        },
        SampleDesc: DXGI_SAMPLE_DESC { Count: 1, Quality: 0 },
        // K149: the desc's default `SampleMask` is 0, which means "write no samples at all" — the
        // pipeline compiled, every draw executed without an error, and the output stayed untouched.
        // All samples must be enabled.
        SampleMask: u32::MAX,
        ..D3D12_GRAPHICS_PIPELINE_STATE_DESC::default()
    };
    // SAFETY: every pointer in `desc` names a live object for the call, which copies the description.
    unsafe { device.CreateGraphicsPipelineState::<ID3D12PipelineState>(&desc) }.map_err(|error| {
        OverlayDrawError::Failed(format!("overlay pipeline creation ({:#010x})", error.code().0))
    })
}

fn buffer_desc(size: u64) -> D3D12_RESOURCE_DESC {
    D3D12_RESOURCE_DESC {
        Dimension: D3D12_RESOURCE_DIMENSION_BUFFER,
        Alignment: 0,
        Width: size.max(1),
        Height: 1,
        DepthOrArraySize: 1,
        MipLevels: 1,
        Format: DXGI_FORMAT_UNKNOWN,
        // D3D12 insists on a sample count of 1 even for buffers (K147).
        SampleDesc: DXGI_SAMPLE_DESC { Count: 1, Quality: 0 },
        Layout: D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
        Flags: D3D12_RESOURCE_FLAG_NONE,
    }
}

fn create_upload(device: &ID3D12Device, size: u64) -> Result<ID3D12Resource, OverlayDrawError> {
    let heap = D3D12_HEAP_PROPERTIES { Type: D3D12_HEAP_TYPE_UPLOAD, ..Default::default() };
    let desc = buffer_desc(size);
    create_committed(device, &heap, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, "overlay upload buffer")
}

fn create_atlas_texture(
    device: &ID3D12Device,
    width: u32,
    height: u32,
) -> Result<ID3D12Resource, OverlayDrawError> {
    let heap = D3D12_HEAP_PROPERTIES { Type: D3D12_HEAP_TYPE_DEFAULT, ..Default::default() };
    let desc = D3D12_RESOURCE_DESC {
        Dimension: D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        Alignment: 0,
        Width: u64::from(width),
        Height: height,
        DepthOrArraySize: 1,
        MipLevels: 1,
        Format: DXGI_FORMAT_R8G8B8A8_UNORM,
        SampleDesc: DXGI_SAMPLE_DESC { Count: 1, Quality: 0 },
        Layout: D3D12_TEXTURE_LAYOUT_UNKNOWN,
        Flags: D3D12_RESOURCE_FLAG_NONE,
    };
    create_committed(device, &heap, &desc, D3D12_RESOURCE_STATE_COPY_DEST, "overlay atlas")
}

fn create_committed(
    device: &ID3D12Device,
    heap: &D3D12_HEAP_PROPERTIES,
    desc: &D3D12_RESOURCE_DESC,
    state: D3D12_RESOURCE_STATES,
    what: &str,
) -> Result<ID3D12Resource, OverlayDrawError> {
    let mut resource: Option<ID3D12Resource> = None;
    // SAFETY: both descs are fully initialized locals that outlive the call.
    unsafe {
        device.CreateCommittedResource(heap, D3D12_HEAP_FLAG_NONE, desc, state, None, &mut resource)
    }
    .map_err(|error| OverlayDrawError::Failed(format!("{what} creation ({:#010x})", error.code().0)))?;
    resource.ok_or_else(|| OverlayDrawError::Failed(format!("{what} creation")))
}

/// Maps `staging`, copies the RGBA rows into it, records the copy into `texture` and the transition
/// to `PIXEL_SHADER_RESOURCE`, then writes the SRV. The caller resets and submits the list.
#[allow(clippy::too_many_arguments)]
fn upload_pixels(
    device: &ID3D12Device,
    srv_heap: &ID3D12DescriptorHeap,
    srv_increment: u32,
    staging: &ID3D12Resource,
    texture: &ID3D12Resource,
    pixels: &[u8],
    width: u32,
    height: u32,
    list: &ID3D12GraphicsCommandList,
) -> Result<(), OverlayDrawError> {
    let row_pitch = aligned_pitch(width);
    let row_bytes = (width as usize) * 4;
    let mut mapped: *mut core::ffi::c_void = std::ptr::null_mut();
    // SAFETY: an upload-heap buffer is mappable; the range says the CPU will not read it.
    unsafe { staging.Map(0, None, Some(&mut mapped)) }
        .map_err(|error| OverlayDrawError::Failed(format!("overlay atlas map ({:#010x})", error.code().0)))?;
    for y in 0..height as usize {
        let source = y * row_bytes;
        if source + row_bytes > pixels.len() {
            // SAFETY: unmapping a buffer this call mapped.
            unsafe { staging.Unmap(0, None) };
            return Err(OverlayDrawError::Failed(
                "overlay atlas pixels are shorter than its size".to_owned(),
            ));
        }
        // SAFETY: the mapped buffer is `row_pitch * height` bytes and the source row fits in
        // `pixels`, so each destination row is in range.
        unsafe {
            std::ptr::copy_nonoverlapping(
                pixels.as_ptr().add(source),
                mapped.cast::<u8>().add(y * row_pitch as usize),
                row_bytes,
            );
        }
    }
    // SAFETY: unmapping a buffer this call mapped; no range is reported as written.
    unsafe { staging.Unmap(0, None) };

    let from = D3D12_TEXTURE_COPY_LOCATION {
        pResource: std::mem::ManuallyDrop::new(Some(staging.clone())),
        Type: D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
        Anonymous: D3D12_TEXTURE_COPY_LOCATION_0 {
            PlacedFootprint: D3D12_PLACED_SUBRESOURCE_FOOTPRINT {
                Offset: 0,
                Footprint: windows::Win32::Graphics::Direct3D12::D3D12_SUBRESOURCE_FOOTPRINT {
                    Format: DXGI_FORMAT_R8G8B8A8_UNORM,
                    Width: width,
                    Height: height,
                    Depth: 1,
                    RowPitch: row_pitch,
                },
            },
        },
    };
    let to = D3D12_TEXTURE_COPY_LOCATION {
        pResource: std::mem::ManuallyDrop::new(Some(texture.clone())),
        Type: D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
        Anonymous: D3D12_TEXTURE_COPY_LOCATION_0 { SubresourceIndex: 0 },
    };
    // SAFETY: the source is a live staging buffer and the destination a live texture in COPY_DEST;
    // the footprint matches the uploaded rows.
    unsafe { list.CopyTextureRegion(&to, 0, 0, 0, &from, None) };
    let barrier = transition_barrier(
        texture,
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
    );
    // SAFETY: the copy above wrote the whole texture; the barrier names the same live resource.
    unsafe { list.ResourceBarrier(&[barrier]) };

    let desc = D3D12_SHADER_RESOURCE_VIEW_DESC {
        Format: DXGI_FORMAT_R8G8B8A8_UNORM,
        ViewDimension: D3D12_SRV_DIMENSION_TEXTURE2D,
        Shader4ComponentMapping:
            windows::Win32::Graphics::Direct3D12::D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
        Anonymous: D3D12_SHADER_RESOURCE_VIEW_DESC_0 {
            Texture2D: D3D12_TEX2D_SRV { MipLevels: 1, ..Default::default() },
        },
    };
    let handle = cpu_handle_base(srv_heap, srv_increment, 0);
    // SAFETY: the texture is live and the heap has one SRV descriptor.
    unsafe { device.CreateShaderResourceView(texture, Some(&desc), handle) };
    Ok(())
}

/// Closes, executes and waits for the recorded upload. The wait is bounded; a timeout disables the
/// overlay rather than blocking the game's present thread forever.
fn submit_and_wait(
    queue: &ID3D12CommandQueue,
    list: &ID3D12GraphicsCommandList,
    fence: &ID3D12Fence,
    value: u64,
    wait: std::time::Duration,
) -> Result<(), OverlayDrawError> {
    // SAFETY: the list is open and fully recorded.
    unsafe { list.Close() }.map_err(|error| {
        OverlayDrawError::Failed(format!("overlay atlas list close ({:#010x})", error.code().0))
    })?;
    // SAFETY: the list is the same object as a base command list.
    let base: ID3D12CommandList = list.cast().map_err(|error| {
        OverlayDrawError::Failed(format!("overlay list cast ({:#010x})", error.code().0))
    })?;
    // SAFETY: the list is closed and recorded and stays alive until the wait below.
    unsafe { queue.ExecuteCommandLists(&[Some(base)]) };
    // SAFETY: the fence belongs to the same device as the queue.
    unsafe { queue.Signal(fence, value) }.map_err(|error| {
        OverlayDrawError::Failed(format!("overlay atlas signal ({:#010x})", error.code().0))
    })?;
    wait_fence(fence, value, wait)
}

fn wait_fence(
    fence: &ID3D12Fence,
    value: u64,
    wait: std::time::Duration,
) -> Result<(), OverlayDrawError> {
    // SAFETY: a pure query on a live fence.
    if unsafe { fence.GetCompletedValue() } >= value {
        return Ok(());
    }
    // SAFETY: no attributes, not manual reset, initially unsignalled, no name.
    let event = unsafe { windows::Win32::System::Threading::CreateEventW(None, false, false, None) }
        .map_err(|error| {
            OverlayDrawError::Failed(format!("overlay wait event ({:#010x})", error.code().0))
        })?;
    // SAFETY: the event outlives its registration below.
    let registered = unsafe { fence.SetEventOnCompletion(value, event) };
    if let Err(error) = registered {
        // SAFETY: the event was created above and is closed exactly once.
        let _ = unsafe { windows::Win32::Foundation::CloseHandle(event) };
        return Err(OverlayDrawError::Failed(format!(
            "overlay fence registration ({:#010x})",
            error.code().0
        )));
    }
    let millis = u32::try_from(wait.as_millis()).unwrap_or(u32::MAX);
    // SAFETY: waiting on an owned event handle with a finite timeout.
    let result = unsafe { windows::Win32::System::Threading::WaitForSingleObject(event, millis) };
    // SAFETY: the event was created above and is closed exactly once.
    let _ = unsafe { windows::Win32::Foundation::CloseHandle(event) };
    if result != windows::Win32::Foundation::WAIT_OBJECT_0 {
        return Err(OverlayDrawError::Failed("overlay atlas upload timed out".to_owned()));
    }
    Ok(())
}

fn write_mapped(resource: &ID3D12Resource, data: &[u8]) -> Result<(), OverlayDrawError> {
    if data.is_empty() {
        return Ok(());
    }
    let mut mapped: *mut core::ffi::c_void = std::ptr::null_mut();
    // SAFETY: an upload-heap buffer is mappable; the range says the CPU will not read it.
    unsafe { resource.Map(0, None, Some(&mut mapped)) }
        .map_err(|error| OverlayDrawError::Failed(format!("overlay upload map ({:#010x})", error.code().0)))?;
    // SAFETY: `mapped` addresses `MAX_UPLOAD_BYTES` writable bytes and `data` was checked against it.
    unsafe { std::ptr::copy_nonoverlapping(data.as_ptr(), mapped.cast::<u8>(), data.len()) };
    // SAFETY: unmapping a buffer this call mapped; no range is reported as written.
    unsafe { resource.Unmap(0, None) };
    Ok(())
}

fn cpu_handle_base(
    heap: &ID3D12DescriptorHeap,
    increment: u32,
    index: u32,
) -> D3D12_CPU_DESCRIPTOR_HANDLE {
    // SAFETY: a pure query on a live heap; `index` is below the heap's descriptor count.
    let base = unsafe { heap.GetCPUDescriptorHandleForHeapStart() };
    D3D12_CPU_DESCRIPTOR_HANDLE { ptr: base.ptr + index as usize * increment as usize }
}

fn gpu_handle(
    heap: &ID3D12DescriptorHeap,
    increment: u32,
    index: u32,
) -> D3D12_GPU_DESCRIPTOR_HANDLE {
    // SAFETY: a pure query on a live heap; `index` is below the heap's descriptor count.
    let base = unsafe { heap.GetGPUDescriptorHandleForHeapStart() };
    D3D12_GPU_DESCRIPTOR_HANDLE { ptr: base.ptr + u64::from(index) * u64::from(increment) }
}

fn gpu_address(resource: &ID3D12Resource) -> u64 {
    // SAFETY: a pure query on a live resource.
    unsafe { resource.GetGPUVirtualAddress() }
}

fn transition_barrier(
    resource: &ID3D12Resource,
    before: D3D12_RESOURCE_STATES,
    after: D3D12_RESOURCE_STATES,
) -> D3D12_RESOURCE_BARRIER {
    D3D12_RESOURCE_BARRIER {
        Type: D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        Flags: D3D12_RESOURCE_BARRIER_FLAG_NONE,
        Anonymous: D3D12_RESOURCE_BARRIER_0 {
            Transition: std::mem::ManuallyDrop::new(D3D12_RESOURCE_TRANSITION_BARRIER {
                pResource: std::mem::ManuallyDrop::new(Some(resource.clone())),
                Subresource: D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                StateBefore: before,
                StateAfter: after,
            }),
        },
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use rstest::rstest;

    fn triangle(rect: egui::Rect) -> egui::ClippedPrimitive {
        let mesh = egui::Mesh {
            indices: vec![0, 1, 2],
            vertices: vec![
                egui::epaint::Vertex {
                    pos: egui::pos2(0.0, 0.0),
                    uv: egui::Pos2::new(0.0, 0.0),
                    color: egui::Color32::from_rgba_premultiplied(255, 0, 0, 255),
                },
                egui::epaint::Vertex {
                    pos: egui::pos2(10.0, 0.0),
                    uv: egui::Pos2::new(0.0, 0.0),
                    color: egui::Color32::from_rgba_premultiplied(0, 255, 0, 255),
                },
                egui::epaint::Vertex {
                    pos: egui::pos2(0.0, 10.0),
                    uv: egui::Pos2::new(0.0, 0.0),
                    color: egui::Color32::from_rgba_premultiplied(0, 0, 255, 255),
                },
            ],
            texture_id: egui::TextureId::default(),
        };
        egui::ClippedPrimitive { clip_rect: rect, primitive: egui::epaint::Primitive::Mesh(mesh) }
    }

    #[test]
    fn packing_writes_twenty_bytes_per_vertex_and_four_per_index() {
        let primitives = [triangle(egui::Rect::from_min_size(egui::Pos2::ZERO, egui::vec2(64.0, 64.0)))];
        let packed = pack(&primitives, 64, 64, 1.0).expect("pack");
        assert_eq!(packed.vertices.len(), 3 * 20);
        assert_eq!(packed.indices.len(), 3 * 4);
        assert_eq!(packed.vertex_count, 3);
        assert_eq!(packed.batches.len(), 1);
        let batch = &packed.batches[0];
        assert_eq!(batch.index_count, 3);
        assert_eq!(batch.vertex_offset, 0);
        assert_eq!(&packed.vertices[0..4], &0.0f32.to_le_bytes());
        assert_eq!(&packed.vertices[16..20], &[255, 0, 0, 255]);
    }

    #[test]
    fn packing_concatenates_primitives_with_offsets() {
        let primitives = [
            triangle(egui::Rect::from_min_size(egui::Pos2::ZERO, egui::vec2(64.0, 64.0))),
            triangle(egui::Rect::from_min_size(egui::Pos2::ZERO, egui::vec2(64.0, 64.0))),
        ];
        let packed = pack(&primitives, 64, 64, 1.0).expect("pack");
        assert_eq!(packed.batches.len(), 2);
        assert_eq!(packed.batches[1].vertex_offset, 3);
        assert_eq!(packed.batches[1].index_offset, 3);
    }

    #[test]
    fn packing_skips_empty_meshes() {
        let empty = egui::ClippedPrimitive {
            clip_rect: egui::Rect::from_min_size(egui::Pos2::ZERO, egui::vec2(1.0, 1.0)),
            primitive: egui::epaint::Primitive::Mesh(egui::Mesh::default()),
        };
        let packed = pack(&[empty], 64, 64, 1.0).expect("pack");
        assert!(packed.batches.is_empty());
    }

    #[rstest]
    #[case(1.0, ClipRect { left: 0, top: 0, right: 64, bottom: 64 })]
    #[case(2.0, ClipRect { left: 0, top: 0, right: 64, bottom: 64 })]
    #[case(0.5, ClipRect { left: 0, top: 0, right: 32, bottom: 32 })]
    fn clip_rects_scale_to_physical_pixels(#[case] scale: f32, #[case] want: ClipRect) {
        let rect = egui::Rect::from_min_size(egui::Pos2::ZERO, egui::vec2(64.0, 64.0));
        assert_eq!(clip_rect(&rect, 64, 64, scale), want);
    }

    #[test]
    fn a_clip_outside_the_output_is_clamped() {
        let rect = egui::Rect::from_min_size(egui::pos2(-10.0, -10.0), egui::vec2(200.0, 200.0));
        assert_eq!(clip_rect(&rect, 64, 32, 1.0), ClipRect { left: 0, top: 0, right: 64, bottom: 32 });
    }

    #[test]
    fn a_vertex_stream_over_64_kib_is_refused() {
        let mut mesh = egui::Mesh::default();
        for i in 0..4000 {
            mesh.vertices.push(egui::epaint::Vertex {
                pos: egui::pos2(i as f32, 0.0),
                uv: egui::Pos2::new(0.0, 0.0),
                color: egui::Color32::WHITE,
            });
        }
        mesh.indices = vec![0, 1, 2];
        let primitive = egui::ClippedPrimitive {
            clip_rect: egui::Rect::from_min_size(egui::Pos2::ZERO, egui::vec2(64.0, 64.0)),
            primitive: egui::epaint::Primitive::Mesh(mesh),
        };
        assert!(pack(&[primitive], 64, 64, 1.0).is_err());
    }

    #[test]
    fn root_constants_carry_the_screen_size_and_its_reciprocal() {
        let words = root_constants(1920.0, 1080.0);
        assert_eq!(f32::from_bits(words[0]), 1920.0);
        assert_eq!(f32::from_bits(words[1]), 1080.0);
        assert!((f32::from_bits(words[2]) - 1.0 / 1920.0).abs() < 1e-9);
        assert_eq!(words[4..], [0, 0, 0, 0]);
        let words = root_constants(0.0, 0.0);
        assert_eq!(f32::from_bits(words[2]), 1.0);
    }

    #[test]
    fn staging_rows_are_aligned_to_256_bytes() {
        assert_eq!(staging_size(64, 10), 256 * 10); // 64 * 4 = 256 bytes exactly
        assert_eq!(staging_size(100, 10), 512 * 10); // 400 bytes rounds up to 512
        assert_eq!(staging_size(257, 10), 1280 * 10); // 1028 bytes rounds up to 1280
    }

    #[test]
    fn the_shaders_are_embedded() {
        assert!(VS_SOURCE.contains("VSMain"));
        assert!(PS_SOURCE.contains("PSMain"));
        assert!(VS_SOURCE.contains("register(b0)"));
        assert!(PS_SOURCE.contains("register(t0)"));
    }
}
