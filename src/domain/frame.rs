//! FrameParams, packing and colour-format rules (ports generator/params, packing).
//!
//! Pure rules only: no GPU types, no FFX types. The adapter converts its DXGI/FFX values into these
//! and back, so the shader-facing rules (geometry validity, the constant block layout, the
//! reprojection packing) can be checked without a device.
use thiserror::Error;

/// The interpolation phase for one generated frame between two real frames.
pub const MIDPOINT_ALPHA: f32 = 0.5;

/// Upstream defaults (Mob-FGSR `src/offscreen_renderer.h`). The depth threshold is not directly
/// comparable to a reversed-Z buffer; see `docs/findings/007-fg-baseline.md`.
pub const DEFAULT_DEPTH_THRESHOLD: f32 = 0.004;
pub const DEFAULT_COLOR_THRESHOLD: f32 = 0.01;

/// Smallest accepted motion-vector scale, matching `params.cpp`.
const MIN_SCALE: f32 = 1.0 / 65536.0;

/// The back-buffer / HUD-less colour format Sedici can generate into.
///
/// Both are 32-bit-per-pixel UNORM formats the shaders read and write as `float4`; only the resource
/// format differs. `R10G10B10A2` was added for Alan Wake 2 (P4.8, K129); the HDR create flag does
/// not change the passes, because Stage 1 warps, fills and blends in the encoded space it is given.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub enum ColorFormat {
    #[default]
    R8G8B8A8Unorm,
    R10G10B10A2Unorm,
}

impl ColorFormat {
    /// Bytes per pixel; both supported formats are 32 bits.
    pub const fn bytes_per_pixel(self) -> u32 {
        4
    }

    /// The FFX `FfxApiSurfaceFormat` discriminant this format maps to, or `None` when unsupported.
    /// Values are the `FFX_API_SURFACE_FORMAT_*` enumerators from `third_party/ffx`.
    pub const fn from_ffx_surface_format(value: u32) -> Option<Self> {
        match value {
            // FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM = 10
            10 => Some(Self::R8G8B8A8Unorm),
            // FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM = 17
            17 => Some(Self::R10G10B10A2Unorm),
            _ => None,
        }
    }

    /// The FFX `FfxApiSurfaceFormat` discriminant for this format.
    pub const fn to_ffx_surface_format(self) -> u32 {
        match self {
            Self::R8G8B8A8Unorm => 10,
            Self::R10G10B10A2Unorm => 17,
        }
    }
}

/// Everything one generated frame needs to know about the pair of real frames it sits between.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct FrameParams {
    pub render_width: u32,
    pub render_height: u32,
    pub display_width: u32,
    pub display_height: u32,
    /// Back-buffer / HUD-less colour format.
    pub color_format: ColorFormat,
    /// Reverse-Z depth: near = 1, far = 0 (`FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED`).
    pub depth_inverted: bool,
    pub alpha: f32,
    pub depth_threshold: f32,
    pub color_threshold: f32,
    /// FFX scale: `pixelMotion = inputMotion * motion_vector_scale`. Use render size for UV inputs,
    /// (1, 1) for pixel inputs. Signed scales are legal; direction is converted at the FFX boundary.
    pub motion_vector_scale_x: f32,
    pub motion_vector_scale_y: f32,
}

impl Default for FrameParams {
    fn default() -> Self {
        Self {
            render_width: 0,
            render_height: 0,
            display_width: 0,
            display_height: 0,
            color_format: ColorFormat::default(),
            depth_inverted: true,
            alpha: MIDPOINT_ALPHA,
            depth_threshold: DEFAULT_DEPTH_THRESHOLD,
            color_threshold: DEFAULT_COLOR_THRESHOLD,
            motion_vector_scale_x: 1.0,
            motion_vector_scale_y: 1.0,
        }
    }
}

/// Why a set of frame parameters cannot describe a frame. Mirrors `fg::valid()` exactly; the reason
/// text is what the C++ logs.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Error)]
pub enum FrameParamsError {
    #[error("frame parameters have a zero size")]
    EmptySize,
    #[error("frame parameters have an alpha outside (0, 1)")]
    AlphaOutOfRange,
    #[error("frame parameters have a non-finite or too-small motion vector scale")]
    MotionScale,
    #[error("frame parameters have an invalid depth or colour threshold")]
    Threshold,
    #[error("frame parameters have a render size larger than the display size")]
    RenderLargerThanDisplay,
}

impl FrameParams {
    /// Returns `Err` when the sizes or parameters cannot describe a frame.
    ///
    /// Order matches `fg::valid()` so the first reported reason is the same.
    pub fn validate(&self) -> Result<(), FrameParamsError> {
        if self.render_width == 0 || self.render_height == 0 {
            return Err(FrameParamsError::EmptySize);
        }
        if self.display_width == 0 || self.display_height == 0 {
            return Err(FrameParamsError::EmptySize);
        }
        if !(self.alpha > 0.0 && self.alpha < 1.0) {
            return Err(FrameParamsError::AlphaOutOfRange);
        }
        if !self.motion_vector_scale_x.is_finite() || !self.motion_vector_scale_y.is_finite() {
            return Err(FrameParamsError::MotionScale);
        }
        if self.motion_vector_scale_x.abs() < MIN_SCALE
            || self.motion_vector_scale_y.abs() < MIN_SCALE
        {
            return Err(FrameParamsError::MotionScale);
        }
        if !self.depth_threshold.is_finite() || self.depth_threshold <= 0.0 {
            return Err(FrameParamsError::Threshold);
        }
        if !self.color_threshold.is_finite() || self.color_threshold < 0.0 {
            return Err(FrameParamsError::Threshold);
        }
        if self.display_width < self.render_width || self.display_height < self.render_height {
            return Err(FrameParamsError::RenderLargerThanDisplay);
        }
        Ok(())
    }

    /// The 96-byte root-constant block uploaded to every Stage 1 pass.
    ///
    /// The first 80 bytes mirror upstream Mob-FGSR's `Uniforms.glsl` block term by term; the
    /// trailing values are Sedici's extension for FFX inputs. `composite_weight` is the value the
    /// dispatch path overwrites into `padding[0]`; `make_constants` leaves it at zero.
    pub fn constants(&self) -> Constants {
        let rw = self.render_width as f32;
        let rh = self.render_height as f32;
        let dw = self.display_width as f32;
        let dh = self.display_height as f32;

        let a = self.alpha;

        Constants {
            render_size: [rw, rh, 1.0 / rw, 1.0 / rh],
            presentation_size: [dw, dh, 1.0 / dw, 1.0 / dh],
            delta: [a, a * 0.5, a * 1.5, a * a * 0.5],
            jitter: [0.0, 0.0],
            depth_threshold: self.depth_threshold,
            color_threshold: self.color_threshold,
            reserved: 0.0,
            // The shader stores "smaller is nearer". Reversed-Z arrives the other way round, so
            // flip it instead of branching per pixel.
            depth_scale: if self.depth_inverted { -1.0 } else { 1.0 },
            depth_bias: if self.depth_inverted { 1.0 } else { 0.0 },
            render_scale: dw / rw,
            motion_vector_scale: [
                self.motion_vector_scale_x / rw,
                self.motion_vector_scale_y / rh,
            ],
            padding: [0.0, 0.0],
        }
    }
}

/// The 96-byte constant block. Field order and padding must match the HLSL `cbuffer Constants`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Constants {
    /// width, height, 1/width, 1/height
    pub render_size: [f32; 4],
    /// width, height, 1/width, 1/height
    pub presentation_size: [f32; 4],
    /// alpha, alpha/2, 3*alpha/2, alpha^2/2
    pub delta: [f32; 4],
    /// reserved, upstream parity
    pub jitter: [f32; 2],
    /// upstream `depth_diff_threshold_fg`
    pub depth_threshold: f32,
    /// upstream `color_diff_threshold_fg`
    pub color_threshold: f32,
    /// upstream `depth_diff_threshold_sr`
    pub reserved: f32,
    pub depth_scale: f32,
    pub depth_bias: f32,
    pub render_scale: f32,
    pub motion_vector_scale: [f32; 2],
    /// `[composite HUD weight, unused]`; `dispatch` writes the HUD weight into `padding[0]`.
    pub padding: [f32; 2],
}

const _: () = assert!(std::mem::size_of::<Constants>() == 96);

impl Constants {
    /// The block as root constants: 24 `u32` values.
    pub fn as_u32_words(&self) -> [u32; 24] {
        let mut words = [0u32; 24];
        for (word, value) in words.iter_mut().zip(self.floats()) {
            *word = value.to_bits();
        }
        words
    }

    fn floats(&self) -> [f32; 24] {
        [
            self.render_size[0],
            self.render_size[1],
            self.render_size[2],
            self.render_size[3],
            self.presentation_size[0],
            self.presentation_size[1],
            self.presentation_size[2],
            self.presentation_size[3],
            self.delta[0],
            self.delta[1],
            self.delta[2],
            self.delta[3],
            self.jitter[0],
            self.jitter[1],
            self.depth_threshold,
            self.color_threshold,
            self.reserved,
            self.depth_scale,
            self.depth_bias,
            self.render_scale,
            self.motion_vector_scale[0],
            self.motion_vector_scale[1],
            self.padding[0],
            self.padding[1],
        ]
    }
}

// ---------------------------------------------------------------------------------------------
// Reprojection packing (ports generator/packing.h)
// ---------------------------------------------------------------------------------------------

/// An empty reprojection entry.
pub const INVALID_REPROJECTION: u32 = 0xFFFF_FFFF;
pub const DEPTH_BITS: u32 = 11;
pub const X_BITS: u32 = 11;
pub const Y_BITS: u32 = 10;
pub const MAX_DEPTH: u32 = (1 << DEPTH_BITS) - 1;
pub const MIN_X: i32 = -(1 << (X_BITS - 1));
pub const MIN_Y: i32 = -(1 << (Y_BITS - 1));
pub const MAX_X: i32 = (1 << (X_BITS - 1)) - 1;
pub const MAX_Y: i32 = (1 << (Y_BITS - 1)) - 1;

/// One unpacked reprojection entry.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Reprojection {
    pub depth: f32,
    pub source_x: i32,
    pub source_y: i32,
}

/// `depth` is the remapped, nearer-is-smaller value in [0, 1].
pub fn pack_reprojection(
    depth: f32,
    source_x: i32,
    source_y: i32,
    target_x: i32,
    target_y: i32,
) -> u32 {
    // `depth > 0.0` is false for NaN, so the clamp matches the shader's `saturate` path.
    #[allow(clippy::neg_cmp_op_on_partial_ord)]
    let depth = if !(depth > 0.0) {
        0.0
    } else if depth > 1.0 {
        1.0
    } else {
        depth
    };

    let packed_depth = (MAX_DEPTH as f32 * depth) as u32;
    let relative_x = (target_x - source_x).clamp(MIN_X, MAX_X);
    let relative_y = (target_y - source_y).clamp(MIN_Y, MAX_Y);
    let unsigned_x = (relative_x - MIN_X) as u32;
    let unsigned_y = (relative_y - MIN_Y) as u32;

    (packed_depth << (32 - DEPTH_BITS)) | (unsigned_x << Y_BITS) | unsigned_y
}

pub fn unpack_depth(value: u32) -> f32 {
    (value >> (32 - DEPTH_BITS)) as f32 / MAX_DEPTH as f32
}

pub fn unpack_reprojection(value: u32, target_x: i32, target_y: i32) -> Reprojection {
    let relative_x = (value >> Y_BITS) & ((1u32 << X_BITS) - 1);
    let relative_y = value & ((1u32 << Y_BITS) - 1);
    Reprojection {
        depth: unpack_depth(value),
        source_x: target_x - (relative_x as i32 + MIN_X),
        source_y: target_y - (relative_y as i32 + MIN_Y),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use rstest::rstest;

    fn valid_params() -> FrameParams {
        FrameParams {
            render_width: 320,
            render_height: 180,
            display_width: 480,
            display_height: 270,
            ..FrameParams::default()
        }
    }

    #[test]
    fn constants_match_the_shader_layout() {
        let params = valid_params();
        let c = params.constants();
        assert_eq!(c.render_size, [320.0, 180.0, 1.0 / 320.0, 1.0 / 180.0]);
        assert_eq!(
            c.presentation_size,
            [480.0, 270.0, 1.0 / 480.0, 1.0 / 270.0]
        );
        assert_eq!(c.delta, [0.5, 0.25, 0.75, 0.125]);
        assert_eq!(c.depth_scale, -1.0);
        assert_eq!(c.depth_bias, 1.0);
        assert_eq!(c.render_scale, 1.5);
        assert_eq!(c.motion_vector_scale, [1.0 / 320.0, 1.0 / 180.0]);
        assert_eq!(c.padding, [0.0, 0.0]);
        assert_eq!(c.as_u32_words().len(), 24);
        // Every word round-trips as the f32 it was built from.
        for (word, value) in c.as_u32_words().iter().zip(c.floats()) {
            assert_eq!(f32::from_bits(*word), value);
        }
    }

    #[test]
    fn non_inverted_depth_uses_identity_remap() {
        let params = FrameParams {
            depth_inverted: false,
            ..valid_params()
        };
        let c = params.constants();
        assert_eq!(c.depth_scale, 1.0);
        assert_eq!(c.depth_bias, 0.0);
    }

    #[rstest]
    #[case::valid(valid_params(), Ok(()))]
    #[case::zero_render(FrameParams { render_width: 0, ..valid_params() }, Err(FrameParamsError::EmptySize))]
    #[case::zero_display(FrameParams { display_height: 0, ..valid_params() }, Err(FrameParamsError::EmptySize))]
    #[case::alpha_zero(FrameParams { alpha: 0.0, ..valid_params() }, Err(FrameParamsError::AlphaOutOfRange))]
    #[case::alpha_one(FrameParams { alpha: 1.0, ..valid_params() }, Err(FrameParamsError::AlphaOutOfRange))]
    #[case::scale_nan(FrameParams { motion_vector_scale_x: f32::NAN, ..valid_params() }, Err(FrameParamsError::MotionScale))]
    #[case::scale_zero(FrameParams { motion_vector_scale_y: 0.0, ..valid_params() }, Err(FrameParamsError::MotionScale))]
    #[case::depth_zero(FrameParams { depth_threshold: 0.0, ..valid_params() }, Err(FrameParamsError::Threshold))]
    #[case::colour_negative(FrameParams { color_threshold: -0.1, ..valid_params() }, Err(FrameParamsError::Threshold))]
    #[case::render_larger(FrameParams { render_width: 481, ..valid_params() }, Err(FrameParamsError::RenderLargerThanDisplay))]
    fn validates_like_the_cpp(
        #[case] params: FrameParams,
        #[case] expected: Result<(), FrameParamsError>,
    ) {
        assert_eq!(params.validate(), expected);
    }

    #[rstest]
    #[case::r8(10, Some(ColorFormat::R8G8B8A8Unorm))]
    #[case::r10(17, Some(ColorFormat::R10G10B10A2Unorm))]
    #[case::unknown(0, None)]
    #[case::srgb(12, None)]
    fn colour_formats_map_to_ffx(#[case] ffx: u32, #[case] expected: Option<ColorFormat>) {
        assert_eq!(ColorFormat::from_ffx_surface_format(ffx), expected);
        if let Some(format) = expected {
            assert_eq!(format.to_ffx_surface_format(), ffx);
            assert_eq!(format.bytes_per_pixel(), 4);
        }
    }

    #[test]
    fn packing_round_trips() {
        let packed = pack_reprojection(0.75, 10, 20, 13, 18);
        let unpacked = unpack_reprojection(packed, 13, 18);
        assert_eq!(unpacked.source_x, 10);
        assert_eq!(unpacked.source_y, 20);
        assert!((unpacked.depth - 0.75).abs() < 1.0 / MAX_DEPTH as f32);
    }

    #[test]
    fn packing_saturates_depth_and_clamps_offsets() {
        // Out-of-range depth saturates; the offset clamps to the field width.
        let packed = pack_reprojection(2.0, 0, 0, 100_000, -100_000);
        assert!((unpack_depth(packed) - 1.0).abs() < 1e-6);
        let unpacked = unpack_reprojection(packed, 100_000, -100_000);
        assert_eq!(unpacked.source_x, 100_000 - MAX_X);
        assert_eq!(unpacked.source_y, -100_000 - MIN_Y);
        // NaN depth packs as zero, as `!(depth > 0.0)` in the shader.
        assert_eq!(unpack_depth(pack_reprojection(f32::NAN, 0, 0, 0, 0)), 0.0);
        // depth 0.5 -> 1023 in 11 bits; a zero offset is MIN_X/MIN_Y, which is 1024 in x and 512
        // in the 10-bit y field.
        let expected = (1023u32 << (32 - DEPTH_BITS)) | (1024u32 << Y_BITS) | 512;
        assert_eq!(pack_reprojection(0.5, 0, 0, 0, 0), expected);
        assert_ne!(pack_reprojection(0.5, 0, 0, 0, 0), INVALID_REPROJECTION);
    }
}
