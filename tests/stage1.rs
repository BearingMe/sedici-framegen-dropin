//! Stage 1 GPU acceptance test: one parameterised scenario per synthetic scene, ported from
//! `tests/integration/fgcheck_quality.cpp`.
//!
//! The C++ host ran four scenes through the production pipeline and compared the generated midpoint
//! against the scene evaluated at the midpoint, plus the phases-in-one-list, HUD-preservation and
//! reset-seed checks. This port keeps the same scenarios and the same thresholds:
//!
//! * the midpoint must beat the nearer real frame on PSNR (the C++ phase check);
//! * the HUD block must survive compositing (more than 95% of the block is white);
//! * a reset seed must be the game's own frame with a HUD source and the HUD-less colour without;
//! * the two phases recorded into one list must each beat their nearest real frame.
//!
//! The PSNR/SSIM figures are reported, not asserted: MILESTONES.md P1.4 says there is no invented
//! universal PSNR/SSIM threshold. The one hard floor is the C++ session check's 35 dB for a
//! midpoint, which is a regression guard, not a quality target.
//!
//! No D3D12 adapter, or a D3D12 adapter without compute support, skips cleanly with a message. The
//! game is never launched; this is an offline device only.
use std::time::Duration;

use amd_fidelityfx_framegeneration_dx12::adapters::driven::d3d12::device::D3d12Device;
use amd_fidelityfx_framegeneration_dx12::adapters::driven::d3d12::resources::D3d12CommandList;
use amd_fidelityfx_framegeneration_dx12::adapters::driven::stage1::Stage1;
use amd_fidelityfx_framegeneration_dx12::application::ports::generator::{
    FrameGenerator, GeneratePhase, HudSource, InputFrame,
};
use amd_fidelityfx_framegeneration_dx12::application::ports::gpu::{
    GpuCommandAllocator, GpuDevice, GpuFence, GpuQueue, GpuTexture, PixelFormat, QueueKind,
    QueuePriority, ResourceState, TextureDesc, TextureFlags,
};
use amd_fidelityfx_framegeneration_dx12::domain::frame::{
    ColorFormat, FrameParams, MIDPOINT_ALPHA,
};


const DISPLAY_WIDTH: u32 = 480;
const DISPLAY_HEIGHT: u32 = 270;
const RENDER_WIDTH: u32 = 320;
const RENDER_HEIGHT: u32 = 180;
const WAIT: Duration = Duration::from_secs(20);

/// A synthetic scene: a static gradient background at the far plane with a moving bar in front.
#[derive(Clone, Copy, Debug)]
struct Scene {
    name: &'static str,
    travel_x: f32,
    travel_y: f32,
    bar_width: f32,
    start_x: f32,
    start_y: f32,
}

const SCENES: [Scene; 4] = [
    Scene { name: "rightward pan", travel_x: 0.10, travel_y: 0.0, bar_width: 0.12, start_x: 0.35, start_y: 0.50 },
    Scene { name: "leftward pan", travel_x: -0.05, travel_y: 0.0, bar_width: 0.20, start_x: 0.70, start_y: 0.50 },
    Scene { name: "diagonal thin bar", travel_x: 0.06, travel_y: 0.04, bar_width: 0.03, start_x: 0.30, start_y: 0.40 },
    Scene { name: "fast bar", travel_x: 0.25, travel_y: 0.0, bar_width: 0.12, start_x: 0.20, start_y: 0.50 },
];

struct Image {
    color: Vec<f32>,
    depth: Vec<f32>,
    motion: Vec<f32>,
}

fn smooth_step(edge0: f32, edge1: f32, value: f32) -> f32 {
    let t = ((value - edge0) / (edge1 - edge0)).clamp(0.0, 1.0);
    t * t * (3.0 - 2.0 * t)
}

/// Renders the scene at time `t` in [0, 1] (0 = previous frame, 1 = current).
fn make_scene(scene: &Scene, t: f32, width: u32, height: u32) -> Image {
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

/// The scene at the midpoint, quantised to RGBA8 exactly as the C++ `quantise` did.
fn quantise(values: &[f32]) -> Vec<u8> {
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

/// FFX motion: current-to-previous UV vectors in an RGBA16F texture. The `prepare` pass multiplies
/// by the motion scale, so the stored half values are `-pixels / size`.
fn pack_ffx_motion(values: &[f32]) -> Vec<u8> {
    let mut bytes = vec![0u8; values.len() * 4];
    for i in 0..values.len() / 2 {
        let half_x = half_bits(-values[i * 2] / RENDER_WIDTH as f32);
        let half_y = half_bits(-values[i * 2 + 1] / RENDER_HEIGHT as f32);
        bytes[i * 8..i * 8 + 2].copy_from_slice(&half_x.to_le_bytes());
        bytes[i * 8 + 2..i * 8 + 4].copy_from_slice(&half_y.to_le_bytes());
    }
    bytes
}

/// f32 -> IEEE half, round to nearest even, as `XMConvertFloatToHalf` does.
fn half_bits(value: f32) -> u16 {
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
    let result = sign | (exponent << 10) | rounded as u16;
    // A mantissa overflow carries into the exponent, which is exactly what half does.
    result
}

fn psnr(reference: &[u8], candidate: &[u8]) -> f64 {
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

fn ssim(reference: &[u8], candidate: &[u8], width: u32, height: u32) -> f64 {
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


/// The host: one device, queue, allocator, list and fence, as `fgcheck_common.h` had.
struct Host {
    device: D3d12Device,
    queue: <D3d12Device as GpuDevice>::Queue,
    allocator: <D3d12Device as GpuDevice>::CommandAllocator,
    list: D3d12CommandList,
    fence: <D3d12Device as GpuDevice>::Fence,
    fence_value: u64,
}

impl Host {
    fn new() -> Option<Self> {
        let device = D3d12Device::create_default().ok().flatten()?;
        let queue = device.create_queue(QueueKind::Direct, QueuePriority::Normal).ok()?;
        let allocator = device.create_command_allocator(QueueKind::Direct).ok()?;
        let list = device.create_command_list(&allocator).ok()?;
        let fence = device.create_fence(0).ok()?;
        Some(Self { device, queue, allocator, list, fence, fence_value: 0 })
    }

    /// Resets the list for a fresh recording.
    fn begin(&self) -> Result<(), String> {
        self.allocator.reset().map_err(|error| error.to_string())?;
        self.list.reset(&self.allocator).map_err(|error| error.to_string())
    }

    /// Closes, executes and waits for the recording.
    fn submit(&mut self) -> Result<(), String> {
        self.list.close().map_err(|error| error.to_string())?;
        self.queue.execute(&[&self.list]).map_err(|error| error.to_string())?;
        self.fence_value += 1;
        self.queue.signal(&self.fence, self.fence_value).map_err(|error| error.to_string())?;
        self.fence.wait(self.fence_value, WAIT).map_err(|error| error.to_string())
    }

    fn texture(
        &self,
        format: PixelFormat,
        width: u32,
        height: u32,
        flags: TextureFlags,
    ) -> Result<<D3d12Device as GpuDevice>::Texture, String> {
        let desc = TextureDesc::texture2d(width, height, format, flags);
        self.device.create_texture(&desc).map_err(|error| error.to_string())
    }

    /// Uploads rows through a staging buffer and leaves the texture in COMMON.
    fn upload(
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
        self.list
            .upload_texture(
                texture,
                &staging,
                data,
                width,
                height,
                bytes_per_pixel,
                ResourceState::COMMON,
                ResourceState::COMMON,
            )
            .map_err(|error| error.to_string())
    }

    /// Copies a texture into a readback buffer, executes and returns tightly packed RGBA8 rows.
    fn readback(
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
}

/// The generator's inputs for one real frame.
struct Pair {
    color: Vec<u8>,
    depth: Vec<u8>,
    motion: Vec<u8>,
}

/// Builds the real frame at time `t`: colour at display resolution, depth and motion at render
/// resolution. Motion follows the shader convention (displacement over one frame, in pixels), and
/// FFX stores it current-to-previous in UV, so the sign is flipped and divided by the render size.
fn build_frame(scene: &Scene, t: f32) -> Pair {
    let display = make_scene(scene, t, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    let render = make_scene(scene, t, RENDER_WIDTH, RENDER_HEIGHT);
    let depth: Vec<u8> = render.depth.iter().flat_map(|value| value.to_le_bytes()).collect();
    Pair {
        color: quantise(&display.color),
        depth,
        motion: pack_ffx_motion(&render.motion),
    }
}

fn params() -> FrameParams {
    FrameParams {
        render_width: RENDER_WIDTH,
        render_height: RENDER_HEIGHT,
        display_width: DISPLAY_WIDTH,
        display_height: DISPLAY_HEIGHT,
        color_format: ColorFormat::R8G8B8A8Unorm,
        depth_inverted: true,
        // FFX reports motion current-to-previous; `Context::prepare` negates its scale before the
        // shaders see it (`params.motionVectorScaleX = -m_motion_scale_x`). The test does the same.
        motion_vector_scale_x: -(RENDER_WIDTH as f32),
        motion_vector_scale_y: -(RENDER_HEIGHT as f32),
        ..FrameParams::default()
    }
}

/// A pair of real frames the generator keeps history for, plus their outputs.
struct Frames {
    colors: Vec<<D3d12Device as GpuDevice>::Texture>,
    depths: Vec<<D3d12Device as GpuDevice>::Texture>,
    motions: Vec<<D3d12Device as GpuDevice>::Texture>,
}

impl Frames {
    fn upload(
        host: &mut Host,
        scene: &Scene,
        time: f32,
    ) -> Result<Self, String> {
        let pair = build_frame(scene, time);
        let color =
            host.texture(PixelFormat::R8G8B8A8Unorm, DISPLAY_WIDTH, DISPLAY_HEIGHT, TextureFlags::NONE)?;
        let depth =
            host.texture(PixelFormat::R32Float, RENDER_WIDTH, RENDER_HEIGHT, TextureFlags::NONE)?;
        let motion = host.texture(
            PixelFormat::R16G16B16A16Float,
            RENDER_WIDTH,
            RENDER_HEIGHT,
            TextureFlags::NONE,
        )?;
        host.begin()?;
        host.upload(&color, &pair.color, DISPLAY_WIDTH, DISPLAY_HEIGHT, 4)?;
        host.upload(&depth, &pair.depth, RENDER_WIDTH, RENDER_HEIGHT, 4)?;
        host.upload(&motion, &pair.motion, RENDER_WIDTH, RENDER_HEIGHT, 8)?;
        host.submit()?;
        Ok(Self { colors: vec![color], depths: vec![depth], motions: vec![motion] })
    }
}

/// Runs one scene through the production Stage 1 and checks the C++ thresholds.
fn run_scene(scene: &Scene) -> Result<(), String> {
    let Some(mut host) = Host::new() else {
        return Err("no D3D12 adapter".to_string());
    };
    let mut generator = Stage1::create(&host.device).map_err(|error| error.to_string())?;
    generator.configure(&params()).map_err(|error| error.to_string())?;

    // Consecutive real frames: the first seeds history, the second synthesizes the midpoint
    // between them. The C++ loop did the same with `source = iteration % 2` and a reset on the
    // first frame of each pair.
    let previous = Frames::upload(&mut host, scene, 0.0)?;
    let current = Frames::upload(&mut host, scene, 1.0)?;
    let colors = [&previous.colors[0], &current.colors[0]];
    let previous_color = quantise(&make_scene(scene, 0.0, DISPLAY_WIDTH, DISPLAY_HEIGHT).color);
    let current_color = quantise(&make_scene(scene, 1.0, DISPLAY_WIDTH, DISPLAY_HEIGHT).color);

    let output = host.texture(
        PixelFormat::R8G8B8A8Unorm,
        DISPLAY_WIDTH,
        DISPLAY_HEIGHT,
        TextureFlags::ALLOW_UNORDERED_ACCESS,
    )?;
    let second = host.texture(
        PixelFormat::R8G8B8A8Unorm,
        DISPLAY_WIDTH,
        DISPLAY_HEIGHT,
        TextureFlags::ALLOW_UNORDERED_ACCESS,
    )?;

    let mut midpoint: Option<Vec<u8>> = None;
    for (source, color) in colors.iter().enumerate() {
        let mut frame = InputFrame::new(*color);
        frame.depth = Some(if source == 0 { &previous.depths[0] } else { &current.depths[0] });
        frame.motion = Some(if source == 0 { &previous.motions[0] } else { &current.motions[0] });
        frame.depth_format = PixelFormat::R32Float;
        frame.motion_format = PixelFormat::R16G16B16A16Float;
        let phase = GeneratePhase {
            output: &output,
            output_state: ResourceState::COMMON,
            alpha: MIDPOINT_ALPHA,
        };
        host.begin()?;
        generator
            .generate(&host.list, &frame, &[phase], source == 0, None, None)
            .map_err(|error| error.to_string())?;
        host.submit()?;
        if source == 1 {
            midpoint = Some(host.readback(&output, ResourceState::COMMON)?);
        }
    }

    let generated = midpoint.ok_or("no generated midpoint")?;
    let reference = quantise(&make_scene(scene, MIDPOINT_ALPHA, DISPLAY_WIDTH, DISPLAY_HEIGHT).color);
    let score = psnr(&reference, &generated);
    let structural = ssim(&reference, &generated, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    let nearest = psnr(&reference, &previous_color);
    eprintln!(
        "{}: midpoint psnr {score:.2} dB, ssim {structural:.4}, nearest real {nearest:.2} dB",
        scene.name
    );

    // The C++ phase check: the generated midpoint must be closer to the true scene than simply
    // showing the nearer real frame. The 35 dB floor is the C++ session check's midpoint bound.
    if score <= nearest {
        return Err(format!(
            "{}: midpoint {score:.2} dB does not beat the nearest real frame {nearest:.2} dB",
            scene.name
        ));
    }
    if score < 35.0 {
        return Err(format!("{}: midpoint {score:.2} dB is below the 35 dB floor", scene.name));
    }

    // Phases in one list: two frames from one prepare, at 1/3 and 2/3, into two outputs recorded in
    // one command list. Each must be closer to the true scene at its time than the nearer real
    // frame, which a wrong or shared alpha would not be. The first call seeds history.
    let third =
        GeneratePhase { output: &output, output_state: ResourceState::COMMON, alpha: 1.0 / 3.0 };
    let two_thirds =
        GeneratePhase { output: &second, output_state: ResourceState::COMMON, alpha: 2.0 / 3.0 };
    for (source, color) in colors.iter().enumerate() {
        let mut frame = InputFrame::new(*color);
        frame.depth = Some(if source == 0 { &previous.depths[0] } else { &current.depths[0] });
        frame.motion = Some(if source == 0 { &previous.motions[0] } else { &current.motions[0] });
        frame.depth_format = PixelFormat::R32Float;
        frame.motion_format = PixelFormat::R16G16B16A16Float;
        host.begin()?;
        generator
            .generate(&host.list, &frame, &[third, two_thirds], source == 0, None, None)
            .map_err(|error| error.to_string())?;
        host.submit()?;
    }

    let third_pixels = host.readback(&output, ResourceState::COMMON)?;
    let two_thirds_pixels = host.readback(&second, ResourceState::COMMON)?;
    let truth_third = quantise(&make_scene(scene, 1.0 / 3.0, DISPLAY_WIDTH, DISPLAY_HEIGHT).color);
    let truth_two_thirds =
        quantise(&make_scene(scene, 2.0 / 3.0, DISPLAY_WIDTH, DISPLAY_HEIGHT).color);
    let generated_third = psnr(&truth_third, &third_pixels);
    let generated_two_thirds = psnr(&truth_two_thirds, &two_thirds_pixels);
    let nearest_third = psnr(&truth_third, &previous_color);
    let nearest_two_thirds = psnr(&truth_two_thirds, &current_color);
    eprintln!(
        "{}: phases in one list 1/3 psnr {generated_third:.2} (nearest real {nearest_third:.2}), \
         2/3 psnr {generated_two_thirds:.2} (nearest real {nearest_two_thirds:.2})",
        scene.name
    );
    if !(generated_third > nearest_third && generated_two_thirds > nearest_two_thirds) {
        return Err(format!("{}: phases do not beat the nearest real frame", scene.name));
    }

    // HUD preservation: the game's backbuffer carries a HUD the HUD-less input does not. A known
    // white block over a background-only corner must survive the composite.
    let (x0, y0, x1, y1) = (4u32, 4u32, 60u32, 40u32);
    let mut hud_color = make_scene(scene, 1.0, DISPLAY_WIDTH, DISPLAY_HEIGHT).color;
    for y in y0..y1 {
        for x in x0..x1 {
            let index = ((y * DISPLAY_WIDTH + x) * 4) as usize;
            hud_color[index] = 1.0;
            hud_color[index + 1] = 1.0;
            hud_color[index + 2] = 1.0;
        }
    }
    let hud_bytes = quantise(&hud_color);
    let hud_texture =
        host.texture(PixelFormat::R8G8B8A8Unorm, DISPLAY_WIDTH, DISPLAY_HEIGHT, TextureFlags::NONE)?;
    host.begin()?;
    host.upload(&hud_texture, &hud_bytes, DISPLAY_WIDTH, DISPLAY_HEIGHT, 4)?;
    host.submit()?;

    let mut frame = InputFrame::new(colors[1]);
    frame.depth = Some(&current.depths[0]);
    frame.motion = Some(&current.motions[0]);
    frame.depth_format = PixelFormat::R32Float;
    frame.motion_format = PixelFormat::R16G16B16A16Float;
    let hud = HudSource { real: &hud_texture, real_state: ResourceState::COMMON };
    let phase = GeneratePhase {
        output: &output,
        output_state: ResourceState::COPY_SOURCE,
        alpha: MIDPOINT_ALPHA,
    };
    host.begin()?;
    generator
        .generate(&host.list, &frame, &[phase], false, Some(hud), None)
        .map_err(|error| error.to_string())?;
    host.submit()?;
    let composite = host.readback(&output, ResourceState::COPY_SOURCE)?;

    let mut inside = 0usize;
    let mut inside_white = 0usize;
    for y in y0 + 1..y1 - 1 {
        for x in x0 + 1..x1 - 1 {
            let index = ((y * DISPLAY_WIDTH + x) * 4) as usize;
            inside += 1;
            if composite[index] > 216 && composite[index + 1] > 216 && composite[index + 2] > 216 {
                inside_white += 1;
            }
        }
    }
    let preserved = if inside == 0 { 0.0 } else { inside_white as f64 / inside as f64 };
    eprintln!("{}: hud preservation {:.1}%", scene.name, preserved * 100.0);
    if preserved <= 0.95 {
        return Err(format!("{}: HUD layer did not survive compositing", scene.name));
    }

    // K53: a reset seed emits the real frame, not a HUD-less phase. With a HUD source the output is
    // the game's own frame; without one, the HUD-less colour.
    let mut seed = |hud: Option<HudSource<'_, _>>| -> Result<Vec<u8>, String> {
        let phase = GeneratePhase {
            output: &output,
            output_state: ResourceState::COPY_SOURCE,
            alpha: MIDPOINT_ALPHA,
        };
        host.begin()?;
        generator
            .generate(&host.list, &frame, &[phase], true, hud, None)
            .map_err(|error| error.to_string())?;
        host.submit()?;
        host.readback(&output, ResourceState::COPY_SOURCE)
    };
    if seed(Some(hud))? != hud_bytes {
        return Err(format!("{}: a HUD seed is not the game's own frame", scene.name));
    }
    if seed(None)? != current_color {
        return Err(format!("{}: a HUD-less seed is not the HUD-less colour", scene.name));
    }
    Ok(())
}

#[test]
fn synthetic_scenes_meet_the_ffx_quality_thresholds() {
    let Some(host) = Host::new() else {
        eprintln!("SKIP stage 1 quality: no D3D12 adapter on this machine");
        return;
    };
    if let Err(error) = Stage1::create(&host.device) {
        eprintln!("SKIP stage 1 quality: the adapter cannot create the Stage 1 pipelines: {error:?}");
        return;
    }
    drop(host);

    // One scenario per synthetic scene; each fails on its own so a broken scene is named.
    for scene in SCENES {
        if let Err(reason) = run_scene(&scene) {
            if reason == "no D3D12 adapter" {
                eprintln!("SKIP stage 1 quality: no D3D12 adapter on this machine");
                return;
            }
            panic!("{}: {reason}", scene.name);
        }
    }
}
