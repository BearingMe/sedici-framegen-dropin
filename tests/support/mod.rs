//! Shared pieces of the Rust offline acceptance tests, ported from `tests/integration/fgcheck_common.h`.
//!
//! Only the scenario scaffolding lives here: the synthetic scene generator and its metrics, and a
//! small D3D12 host (device, queue, allocator, list, fence) that uploads inputs and reads results
//! back. The scenarios themselves stay in the test files that own them, so a failure names its
//! scene and its check.
//!
//! Nothing here is production code and nothing here runs unless a test calls it. A test that needs
//! a GPU checks `Host::new()` and skips with a message when no D3D12 adapter exists.
#![allow(dead_code, unsafe_code)]

use std::time::Duration;

use amd_fidelityfx_framegeneration_dx12::adapters::driven::d3d12::device::D3d12Device;
use amd_fidelityfx_framegeneration_dx12::adapters::driven::d3d12::resources::D3d12CommandList;
use amd_fidelityfx_framegeneration_dx12::application::ports::gpu::{
    GpuBuffer, GpuCommandAllocator, GpuCommandList, GpuDevice, GpuFence, GpuQueue, GpuTexture,
    PixelFormat, QueueKind, QueuePriority, ResourceState, TextureDesc, TextureFlags,
};

pub const WAIT: Duration = Duration::from_secs(20);

/// A synthetic scene: a static gradient background at the far plane with a moving bar in front.
/// The values are `fgcheck_common.h`'s `kScenes`, in the same order.
#[derive(Clone, Copy, Debug)]
pub struct Scene {
    pub name: &'static str,
    pub travel_x: f32,
    pub travel_y: f32,
    pub bar_width: f32,
    pub start_x: f32,
    pub start_y: f32,
}

pub const SCENES: [Scene; 4] = [
    Scene { name: "rightward pan", travel_x: 0.10, travel_y: 0.0, bar_width: 0.12, start_x: 0.35, start_y: 0.50 },
    Scene { name: "leftward pan", travel_x: -0.05, travel_y: 0.0, bar_width: 0.20, start_x: 0.70, start_y: 0.50 },
    Scene { name: "diagonal thin bar", travel_x: 0.06, travel_y: 0.04, bar_width: 0.03, start_x: 0.30, start_y: 0.40 },
    // K120: a quarter of the screen per frame, the disocclusion case at high speed.
    Scene { name: "fast bar", travel_x: 0.25, travel_y: 0.0, bar_width: 0.12, start_x: 0.20, start_y: 0.50 },
];

/// One rendered scene: RGBA colour, one-channel reversed-Z depth and two-channel pixel motion.
pub struct Image {
    pub color: Vec<f32>,
    pub depth: Vec<f32>,
    pub motion: Vec<f32>,
}

pub fn smooth_step(edge0: f32, edge1: f32, value: f32) -> f32 {
    let t = ((value - edge0) / (edge1 - edge0)).clamp(0.0, 1.0);
    t * t * (3.0 - 2.0 * t)
}

/// Renders the scene at time `t` in [0, 1] (0 = previous frame, 1 = current).
pub fn make_scene(scene: &Scene, t: f32, width: u32, height: u32) -> Image {
    let width_f = width as f32;
    let height_f = height as f32;
    let centre_x = (scene.start_x + scene.travel_x * t) * width_f;
    let centre_y = (scene.start_y + scene.travel_y * t) * height_f;
    let half_width = 0.5 * scene.bar_width * width_f;
    let travel_pixels_x = scene.travel_x * width_f;
    let travel_pixels_y = scene.travel_y * height_f;
    let feather = 1.25;
    let half_height = if scene.travel_y != 0.0 { 0.15 * height_f } else { 0.5 * height_f };

    let mut image = Image {
        color: vec![0.0; (width * height * 4) as usize],
        depth: vec![0.0; (width * height) as usize],
        motion: vec![0.0; (width * height * 2) as usize],
    };

    for y in 0..height {
        let v = (y as f32 + 0.5) / height_f;
        for x in 0..width {
            let u = (x as f32 + 0.5) / width_f;
            let pixel_x = x as f32 + 0.5;
            let pixel_y = y as f32 + 0.5;
            let horizontal = smooth_step(half_width, half_width - feather, (pixel_x - centre_x).abs());
            let vertical = smooth_step(half_height, half_height - feather, (pixel_y - centre_y).abs());
            let inside = horizontal * vertical;
            let index = (y * width + x) as usize;

            let background_r = 0.10 + 0.25 * v;
            let background_g = 0.14 + 0.30 * v;
            let background_b = 0.22 + 0.20 * u;
            let (bar_r, bar_g, bar_b) = (0.90, 0.55, 0.15);

            image.color[index * 4] = background_r + (bar_r - background_r) * inside;
            image.color[index * 4 + 1] = background_g + (bar_g - background_g) * inside;
            image.color[index * 4 + 2] = background_b + (bar_b - background_b) * inside;
            image.color[index * 4 + 3] = 1.0;
            image.depth[index] = if inside > 0.5 { 1.0 } else { 0.0 };
            image.motion[index * 2] = if inside > 0.5 { travel_pixels_x } else { 0.0 };
            image.motion[index * 2 + 1] = if inside > 0.5 { travel_pixels_y } else { 0.0 };
        }
    }
    image
}

/// The scene at `t`, quantised to RGBA8 exactly as the C++ `quantise` did.
pub fn quantise(values: &[f32]) -> Vec<u8> {
    values
        .iter()
        .enumerate()
        .map(|(index, value)| {
            if index % 4 == 3 {
                255
            } else {
                (value.clamp(0.0, 1.0) * 255.0).round() as u8
            }
        })
        .collect()
}

/// FFX motion: current-to-previous UV vectors in an RGBA16F texture. `prepare` multiplies by the
/// motion scale, so the stored half values are `-pixels / size`.
pub fn pack_ffx_motion(values: &[f32], width: u32, height: u32) -> Vec<u8> {
    let mut bytes = vec![0u8; values.len() * 4];
    for i in 0..values.len() / 2 {
        let half_x = half_bits(-values[i * 2] / width as f32);
        let half_y = half_bits(-values[i * 2 + 1] / height as f32);
        bytes[i * 8..i * 8 + 2].copy_from_slice(&half_x.to_le_bytes());
        bytes[i * 8 + 2..i * 8 + 4].copy_from_slice(&half_y.to_le_bytes());
    }
    bytes
}

/// f32 -> IEEE half, round to nearest even, as `XMConvertFloatToHalf` does.
pub fn half_bits(value: f32) -> u16 {
    let bits = value.to_bits();
    let sign = ((bits >> 16) & 0x8000) as u16;
    let exponent = ((bits >> 23) & 0xFF) as i32 - 127 + 15;
    let mantissa = bits & 0x7F_FFFF;
    if exponent <= 0 {
        if exponent < -10 {
            return sign;
        }
        let mantissa = (mantissa | 0x80_0000) >> (1 - exponent);
        let rounded = (mantissa + 0x1000) >> 13;
        return sign | rounded as u16;
    }
    if exponent >= 31 {
        return sign | 0x7C00;
    }
    let rounded = (mantissa + 0x1000) >> 13;
    let exponent = exponent as u16;
    sign | (exponent << 10) | rounded as u16
}

pub fn psnr(reference: &[u8], candidate: &[u8]) -> f64 {
    if reference.len() != candidate.len() || reference.is_empty() {
        return 0.0;
    }
    let mut sum = 0.0;
    let mut samples = 0usize;
    for (index, (a, b)) in reference.iter().zip(candidate).enumerate() {
        if index % 4 == 3 {
            continue;
        }
        let difference = *a as f64 - *b as f64;
        sum += difference * difference;
        samples += 1;
    }
    if samples == 0 {
        return 0.0;
    }
    let mse = sum / samples as f64;
    if mse <= 0.0 {
        99.0
    } else {
        10.0 * (255.0 * 255.0 / mse).log10()
    }
}

pub fn ssim(reference: &[u8], candidate: &[u8], width: u32, height: u32) -> f64 {
    const C1: f64 = 6.5025;
    const C2: f64 = 58.5225;
    const WINDOW: u32 = 8;
    const STRIDE: u32 = 4;

    let luma = |pixels: &[u8], x: u32, y: u32| -> f64 {
        let index = ((y * width + x) * 4) as usize;
        0.299 * pixels[index] as f64
            + 0.587 * pixels[index + 1] as f64
            + 0.114 * pixels[index + 2] as f64
    };

    let mut total = 0.0;
    let mut windows = 0u32;
    let mut y = 0;
    while y + WINDOW <= height {
        let mut x = 0;
        while x + WINDOW <= width {
            let (mut sum_a, mut sum_b, mut sum_aa, mut sum_bb, mut sum_ab) =
                (0.0, 0.0, 0.0, 0.0, 0.0);
            for dy in 0..WINDOW {
                for dx in 0..WINDOW {
                    let a = luma(reference, x + dx, y + dy);
                    let b = luma(candidate, x + dx, y + dy);
                    sum_a += a;
                    sum_b += b;
                    sum_aa += a * a;
                    sum_bb += b * b;
                    sum_ab += a * b;
                }
            }
            let n = (WINDOW * WINDOW) as f64;
            let mean_a = sum_a / n;
            let mean_b = sum_b / n;
            let var_a = sum_aa / n - mean_a * mean_a;
            let var_b = sum_bb / n - mean_b * mean_b;
            let covariance = sum_ab / n - mean_a * mean_b;
            let numerator = (2.0 * mean_a * mean_b + C1) * (2.0 * covariance + C2);
            let denominator = (mean_a * mean_a + mean_b * mean_b + C1) * (var_a + var_b + C2);
            total += if denominator != 0.0 { numerator / denominator } else { 0.0 };
            windows += 1;
            x += STRIDE;
        }
        y += STRIDE;
    }
    if windows == 0 {
        0.0
    } else {
        total / windows as f64
    }
}

/// One device, queue, allocator, list and fence, as `fgcheck_common.h`'s `Device` had.
pub struct Host {
    pub device: D3d12Device,
    pub queue: <D3d12Device as GpuDevice>::Queue,
    pub allocator: <D3d12Device as GpuDevice>::CommandAllocator,
    pub list: D3d12CommandList,
    pub fence: <D3d12Device as GpuDevice>::Fence,
    pub fence_value: u64,
}

impl Host {
    /// `None` when there is no D3D12 adapter (the caller skips with a message).
    pub fn new() -> Option<Self> {
        let device = D3d12Device::create_default().ok().flatten()?;
        let queue = device.create_queue(QueueKind::Direct, QueuePriority::Normal).ok()?;
        let allocator = device.create_command_allocator(QueueKind::Direct).ok()?;
        let list = device.create_command_list(&allocator).ok()?;
        let fence = device.create_fence(0).ok()?;
        Some(Self { device, queue, allocator, list, fence, fence_value: 0 })
    }

    /// Resets the list for a fresh recording.
    pub fn begin(&self) -> Result<(), String> {
        self.allocator.reset().map_err(|error| error.to_string())?;
        self.list.reset(&self.allocator).map_err(|error| error.to_string())
    }

    /// Closes, executes and waits for the recording.
    pub fn submit(&mut self) -> Result<(), String> {
        self.list.close().map_err(|error| error.to_string())?;
        self.queue.execute(&[&self.list]).map_err(|error| error.to_string())?;
        self.fence_value += 1;
        self.queue.signal(&self.fence, self.fence_value).map_err(|error| error.to_string())?;
        self.fence.wait(self.fence_value, WAIT).map_err(|error| error.to_string())
    }

    pub fn texture(
        &self,
        format: PixelFormat,
        width: u32,
        height: u32,
        flags: TextureFlags,
    ) -> Result<<D3d12Device as GpuDevice>::Texture, String> {
        let desc = TextureDesc::texture2d(width, height, format, flags);
        self.device.create_texture(&desc).map_err(|error| error.to_string())
    }

    /// Uploads rows through a staging buffer and leaves the texture in `final_state`.
    pub fn upload_into(
        &self,
        texture: &<D3d12Device as GpuDevice>::Texture,
        staging: &<D3d12Device as GpuDevice>::Buffer,
        data: &[u8],
        width: u32,
        height: u32,
        bytes_per_pixel: u32,
        final_state: ResourceState,
    ) -> Result<(), String> {
        self.list
            .upload_texture(
                texture,
                staging,
                data,
                width,
                height,
                bytes_per_pixel,
                ResourceState::COMMON,
                final_state,
            )
            .map_err(|error| error.to_string())
    }

    /// Uploads rows through a staging buffer and leaves the texture in COMMON.
    pub fn upload(
        &self,
        texture: &<D3d12Device as GpuDevice>::Texture,
        data: &[u8],
        width: u32,
        height: u32,
        bytes_per_pixel: u32,
    ) -> Result<(), String> {
        let pitch = D3d12CommandList::row_pitch(width, bytes_per_pixel);
        let staging = self
            .device
            .create_upload_buffer(u64::from(pitch) * u64::from(height))
            .map_err(|error| error.to_string())?;
        self.upload_into(texture, &staging, data, width, height, bytes_per_pixel, ResourceState::COMMON)
    }

    /// Copies a texture into a readback buffer, executes and returns tightly packed RGBA8 rows.
    pub fn readback(
        &mut self,
        texture: &<D3d12Device as GpuDevice>::Texture,
        from_state: ResourceState,
    ) -> Result<Vec<u8>, String> {
        let info = GpuTexture::info(texture);
        let pitch = D3d12CommandList::row_pitch(info.width, 4);
        let staging = self
            .device
            .create_readback_buffer(u64::from(pitch) * u64::from(info.height))
            .map_err(|error| error.to_string())?;
        self.begin()?;
        self.list
            .readback_texture(texture, &staging, from_state, from_state)
            .map_err(|error| error.to_string())?;
        self.submit()?;
        let bytes = self.device.read_buffer(&staging).map_err(|error| error.to_string())?;
        let row_bytes = (info.width * 4) as usize;
        let mut packed = Vec::with_capacity(row_bytes * info.height as usize);
        for y in 0..info.height as usize {
            let start = y * pitch as usize;
            packed.extend_from_slice(&bytes[start..start + row_bytes]);
        }
        Ok(packed)
    }

    /// Leaves the device removed reason, which a healthy run never sets.
    pub fn removed_reason(&self) -> Option<u32> {
        self.device.removed_reason()
    }
}

/// The raw resource pointer, as the FFX-facing handle the context resolves.
pub fn resource_handle(texture: &<D3d12Device as GpuDevice>::Texture) -> usize {
    use windows::core::Interface;
    Interface::as_raw(texture.raw()) as usize
}

/// The raw command-list pointer, as the FFX-facing handle the context resolves.
pub fn list_handle(list: &D3d12CommandList) -> usize {
    use windows::core::Interface;
    Interface::as_raw(list.raw()) as usize
}

/// Readback of a texture whose bytes are not RGBA8 (a 10-bit packed texture is still 4 bytes per
/// pixel, so `Host::readback` works; this is only here to make the intent explicit at call sites).
pub fn bpp_of(format: PixelFormat) -> u32 {
    let _ = format;
    4
}
