//! The 10-bit back buffer with the HDR create flag, ported from `tests/integration/fgcheck_tenbit.cpp`.
//!
//! Alan Wake 2 creates its frame generation context with `R10G10B10A2_UNORM` colour and the HDR
//! flag (K14, K129). Stage 1 samples and writes `float4`, so the format changes only the storage.
//! The C++ proved that on a GPU by running one scene through an 8-bit and a 10-bit context, lifting
//! the 8-bit pictures to 10 bits exactly (`v << 2 | v >> 6`) and reading the 10-bit result back
//! with rounding. This port keeps the same scenarios and the same thresholds:
//!
//! * `between > 42.0` dB — the two results are not identical (the storage rounds differently
//!   inside the warp's float filter) but they must agree closely;
//! * `tenScore > eightScore - 0.5` — the 10-bit result must not score worse.
//!
//! The C++ ran `kScenes[0]` and `kScenes[2]`; this is one rstest case per scene. No D3D12 adapter
//! skips cleanly with a message. The game is never launched.
//!
//! The `Context` half of the same shape — the HDR-flagged 10-bit create reaching the generator as
//! `ColorFormat::R10G10B10A2Unorm`, and the game's own generation dispatch being acknowledged — is
//! pinned without a GPU by `tests/routing.rs` (`an_hdr_flagged_ten_bit_session_is_accepted` and
//! `a_dispatch_of_an_unknown_type_is_refused_and_the_game_own_dispatch_is_acknowledged`).
#![allow(unsafe_code)]

mod support;

use rstest::rstest;

use amd_fidelityfx_framegeneration_dx12::adapters::driven::stage1::Stage1;
use amd_fidelityfx_framegeneration_dx12::application::ports::generator::{
    FrameGenerator, GeneratePhase, InputFrame,
};
use amd_fidelityfx_framegeneration_dx12::application::ports::gpu::{
    PixelFormat, ResourceState, TextureFlags,
};
use amd_fidelityfx_framegeneration_dx12::domain::frame::{
    ColorFormat, FrameParams, MIDPOINT_ALPHA,
};

use support::{make_scene, pack_ffx_motion, psnr, quantise, Host, Scene, SCENES};

const DISPLAY_WIDTH: u32 = 480;
const DISPLAY_HEIGHT: u32 = 270;
const RENDER_WIDTH: u32 = 320;
const RENDER_HEIGHT: u32 = 180;

/// `v << 2 | v >> 6`, exactly the C++ `lift`.
fn lift(value: u8) -> u32 {
    (u32::from(value) << 2) | (u32::from(value) >> 6)
}

/// RGBA8 packed into R10G10B10A2 with the C++ `lift`, four bytes per pixel.
fn to_ten_bit(rgba8: &[u8]) -> Vec<u8> {
    let mut out = vec![0u8; rgba8.len()];
    for i in (0..rgba8.len().saturating_sub(3)).step_by(4) {
        let packed = lift(rgba8[i])
            | (lift(rgba8[i + 1]) << 10)
            | (lift(rgba8[i + 2]) << 20)
            | (u32::from(rgba8[i + 3] >> 6) << 30);
        out[i..i + 4].copy_from_slice(&packed.to_le_bytes());
    }
    out
}

/// `(v10 * 255 + 511) / 1023`, exactly the C++ `reduce`.
fn reduce(value: u32) -> u8 {
    ((value * 255 + 511) / 1023) as u8
}

/// R10G10B10A2 packed back to RGBA8 with the C++ `from_ten_bit`.
fn from_ten_bit(packed: &[u8]) -> Vec<u8> {
    let mut out = vec![0u8; packed.len()];
    for i in (0..packed.len().saturating_sub(3)).step_by(4) {
        let value = u32::from_le_bytes([packed[i], packed[i + 1], packed[i + 2], packed[i + 3]]);
        out[i] = reduce(value & 1023);
        out[i + 1] = reduce((value >> 10) & 1023);
        out[i + 2] = reduce((value >> 20) & 1023);
        out[i + 3] = 255;
    }
    out
}

fn params(ten_bit: bool) -> FrameParams {
    FrameParams {
        render_width: RENDER_WIDTH,
        render_height: RENDER_HEIGHT,
        display_width: DISPLAY_WIDTH,
        display_height: DISPLAY_HEIGHT,
        color_format: if ten_bit {
            ColorFormat::R10G10B10A2Unorm
        } else {
            ColorFormat::R8G8B8A8Unorm
        },
        depth_inverted: true,
        // `Context::prepare` negates the FFX scale before Stage 1 sees it; the sign is load-bearing.
        motion_vector_scale_x: -(RENDER_WIDTH as f32),
        motion_vector_scale_y: -(RENDER_HEIGHT as f32),
        ..FrameParams::default()
    }
}

/// One scene through one context. Returns the generated midpoint as RGBA8 (a 10-bit result is
/// reduced), or an error string when a step failed.
fn run_format(host: &mut Host, scene: &Scene, ten_bit: bool) -> Result<Vec<u8>, String> {
    let format = if ten_bit {
        PixelFormat::R10G10B10A2Unorm
    } else {
        PixelFormat::R8G8B8A8Unorm
    };
    let mut generator = Stage1::create(&host.device).map_err(|error| error.to_string())?;
    generator
        .configure(&params(ten_bit))
        .map_err(|error| error.to_string())?;

    let previous_render = make_scene(scene, 0.0, RENDER_WIDTH, RENDER_HEIGHT);
    let current_render = make_scene(scene, 1.0, RENDER_WIDTH, RENDER_HEIGHT);
    let previous_display = make_scene(scene, 0.0, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    let current_display = make_scene(scene, 1.0, DISPLAY_WIDTH, DISPLAY_HEIGHT);

    let mut colors = [
        quantise(&previous_display.color),
        quantise(&current_display.color),
    ];
    if ten_bit {
        for color in &mut colors {
            *color = to_ten_bit(color);
        }
    }
    let motions = [
        pack_ffx_motion(&previous_render.motion, RENDER_WIDTH, RENDER_HEIGHT),
        pack_ffx_motion(&current_render.motion, RENDER_WIDTH, RENDER_HEIGHT),
    ];
    let renders = [&previous_render, &current_render];

    let mut color_textures = Vec::new();
    let mut depth_textures = Vec::new();
    let mut motion_textures = Vec::new();
    for index in 0..2 {
        let color = host.texture(format, DISPLAY_WIDTH, DISPLAY_HEIGHT, TextureFlags::NONE)?;
        let depth = host.texture(
            PixelFormat::R32Float,
            RENDER_WIDTH,
            RENDER_HEIGHT,
            TextureFlags::NONE,
        )?;
        let motion = host.texture(
            PixelFormat::R16G16B16A16Float,
            RENDER_WIDTH,
            RENDER_HEIGHT,
            TextureFlags::NONE,
        )?;
        let depth_bytes: Vec<u8> = renders[index]
            .depth
            .iter()
            .flat_map(|value| value.to_le_bytes())
            .collect();
        host.begin()?;
        host.upload(&color, &colors[index], DISPLAY_WIDTH, DISPLAY_HEIGHT, 4)?;
        host.upload(&depth, &depth_bytes, RENDER_WIDTH, RENDER_HEIGHT, 4)?;
        host.upload(&motion, &motions[index], RENDER_WIDTH, RENDER_HEIGHT, 8)?;
        host.submit()?;
        color_textures.push(color);
        depth_textures.push(depth);
        motion_textures.push(motion);
    }
    let output = host.texture(
        format,
        DISPLAY_WIDTH,
        DISPLAY_HEIGHT,
        TextureFlags::ALLOW_UNORDERED_ACCESS,
    )?;

    // Four iterations with `source = iteration % 2` and a reset on every even source, as the C++.
    let mut midpoint: Option<Vec<u8>> = None;
    for iteration in 0..4u32 {
        let source = (iteration % 2) as usize;
        let mut frame = InputFrame::new(&color_textures[source]);
        frame.depth = Some(&depth_textures[source]);
        frame.motion = Some(&motion_textures[source]);
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
        if iteration == 3 {
            midpoint = Some(host.readback(&output, ResourceState::COMMON)?);
        }
    }
    let midpoint = midpoint.ok_or("no generated midpoint")?;
    Ok(if ten_bit {
        from_ten_bit(&midpoint)
    } else {
        midpoint
    })
}

/// One scene through both formats. The two results must agree within the storage quantisation and
/// the 10-bit result must not score worse.
#[rstest]
#[case(&SCENES[0])]
#[case(&SCENES[2])]
fn ten_bit_matches_eight_bit_within_the_storage_quantisation(#[case] scene: &Scene) {
    let Some(mut host) = Host::new() else {
        eprintln!(
            "SKIP 10-bit {}: no D3D12 adapter on this machine",
            scene.name
        );
        return;
    };
    if let Err(error) = Stage1::create(&host.device) {
        eprintln!(
            "SKIP 10-bit {}: the adapter cannot create the Stage 1 pipelines: {error:?}",
            scene.name
        );
        return;
    }

    let eight = match run_format(&mut host, scene, false) {
        Ok(pixels) => pixels,
        Err(reason) => panic!("10-bit {}: the 8-bit run failed: {reason}", scene.name),
    };
    let ten = match run_format(&mut host, scene, true) {
        Ok(pixels) => pixels,
        Err(reason) => panic!("10-bit {}: the 10-bit run failed: {reason}", scene.name),
    };
    assert_eq!(eight.len(), ten.len(), "the two runs produce the same size");

    let truth = quantise(&make_scene(scene, MIDPOINT_ALPHA, DISPLAY_WIDTH, DISPLAY_HEIGHT).color);
    let eight_score = psnr(&truth, &eight);
    let ten_score = psnr(&truth, &ten);
    let between = psnr(&eight, &ten);
    eprintln!(
        "{}: 10-bit psnr 8-bit {eight_score:.2} dB, 10-bit {ten_score:.2} dB, between them {between:.2} dB",
        scene.name
    );

    // The C++ thresholds, unchanged: the two results are not identical because the storage rounds
    // differently inside the warp's float filter, so they must agree closely, and the 10-bit one
    // must not score worse.
    assert!(
        between > 42.0,
        "{}: the two formats agree to {between:.2} dB (want > 42.0)",
        scene.name
    );
    assert!(
        ten_score > eight_score - 0.5,
        "{}: the 10-bit score {ten_score:.2} dB is worse than the 8-bit {eight_score:.2} dB by more than 0.5",
        scene.name
    );
    assert_eq!(
        host.removed_reason(),
        None,
        "{}: the device stays healthy",
        scene.name
    );
}
