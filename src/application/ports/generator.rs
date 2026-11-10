//! The frame generator (Stage 1 today, Stage 2 later).
//!
//! Shaped so a residual network can slot in behind the same trait: [`FrameGenerator::prepare`]
//! snapshots and normalizes the game's inputs, [`FrameGenerator::generate`] produces one or more
//! phases into caller-owned outputs, and the analytic/residual split happens inside `generate` —
//! a Stage 2 runs only on the synthesis path, never on a reset seed (K53).
//!
//! Resource ownership stays with the caller: the trait never takes a texture by value and never
//! destroys one. A generator is a single in-flight slot, not a concurrent recorder.
use std::time::Duration;

use thiserror::Error;

use crate::domain::error::Rejection;
use crate::domain::frame::FrameParams;

use super::gpu::{GpuDevice, GpuError, GpuTexture, PixelFormat, ResourceState};

/// Most generated frames recorded for one real frame. Two covers every output rate up to twice the
/// real rate, including an adaptive target whose output slots do not line up with real frames.
pub const MAX_PHASES: u32 = 2;

/// One real frame's inputs, as the game supplies them. Colour is display resolution; depth and
/// motion are render resolution.
#[derive(Clone, Debug)]
pub struct InputFrame<'a, T: GpuTexture> {
    pub color: Option<&'a T>,
    pub depth: Option<&'a T>,
    pub motion: Option<&'a T>,
    pub color_state: ResourceState,
    pub depth_state: ResourceState,
    pub motion_state: ResourceState,
    /// Typed SRV formats compatible with the game's resources, not necessarily their allocation
    /// formats (a `D32_FLOAT` depth is viewed as `R32_FLOAT`). The prepare pass converts these
    /// views to the pipeline's internal layout.
    pub depth_format: PixelFormat,
    pub motion_format: PixelFormat,
}

impl<'a, T: GpuTexture> InputFrame<'a, T> {
    pub fn new(color: &'a T) -> Self {
        Self {
            color: Some(color),
            depth: None,
            motion: None,
            color_state: ResourceState::COPY_SOURCE,
            depth_state: ResourceState::NON_PIXEL_SHADER_RESOURCE,
            motion_state: ResourceState::NON_PIXEL_SHADER_RESOURCE,
            depth_format: PixelFormat::R32Float,
            motion_format: PixelFormat::R16G16B16A16Float,
        }
    }
}

/// Optional HUD source for a generated frame: the game's current backbuffer, which still carries
/// the HUD. The generator already holds the HUD-less copy it made for the same frame, so the
/// difference is the HUD layer.
#[derive(Debug)]
pub struct HudSource<'a, T: GpuTexture> {
    pub real: &'a T,
    pub real_state: ResourceState,
}

// A HUD source is a borrowed resource plus a copyable state; it is safe to copy regardless of what
// the texture type's own derives say.
impl<T: GpuTexture> Clone for HudSource<'_, T> {
    fn clone(&self) -> Self {
        *self
    }
}

impl<T: GpuTexture> Copy for HudSource<'_, T> {}

/// One generated frame. `alpha` in (0, 1) is its time between the previous real frame (0) and the
/// current one (1). The output is returned to `output_state`.
#[derive(Debug)]
pub struct GeneratePhase<'a, T: GpuTexture> {
    pub output: &'a T,
    pub output_state: ResourceState,
    pub alpha: f32,
}

// A phase is a borrowed resource plus copyable values; copying it cannot alias anything unsafely.
impl<T: GpuTexture> Clone for GeneratePhase<'_, T> {
    fn clone(&self) -> Self {
        *self
    }
}

impl<T: GpuTexture> Copy for GeneratePhase<'_, T> {}

/// The profiling hook: called right after each compute pass is recorded so a test host can write a
/// GPU timestamp there. Production never sets it.
pub type PassMark<'a> = &'a mut dyn FnMut(Stage1Pass);

/// The Stage 1 passes, in recording order.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Stage1Pass {
    Prepare,
    Clear,
    Reproject,
    Fill,
    Warp,
    Composite,
}

impl Stage1Pass {
    pub const fn name(self) -> &'static str {
        match self {
            Self::Prepare => "prepare",
            Self::Clear => "clear",
            Self::Reproject => "reproject",
            Self::Fill => "fill",
            Self::Warp => "warp",
            Self::Composite => "composite",
        }
    }
}

/// Why a generator call was refused. Every variant is a local rejection the caller may retry with
/// different inputs; none of them is a game-facing FFX error on its own.
#[derive(Debug, Clone, Error)]
pub enum GeneratorError {
    #[error("frame generator is not initialized")]
    NotInitialized,
    #[error("frame generator is not configured")]
    NotConfigured,
    #[error("frame parameters are invalid: {0}")]
    Params(#[from] crate::domain::frame::FrameParamsError),
    #[error("frame generator rejected the prepare snapshot")]
    PrepareRejected,
    #[error("frame generator rejected the frame: {0}")]
    FrameRejected(&'static str),
    #[error("frame generator refused configuration while a prepare snapshot is unconsumed")]
    SnapshotOutstanding,
    #[error(transparent)]
    Gpu(#[from] GpuError),
    #[error(transparent)]
    Rejection(#[from] Rejection),
}

/// Creates and drives the generator. `D` is the device type the adapter's [`GpuDevice`] provides.
pub trait FrameGenerator<D: GpuDevice> {
    /// The command list type this generator records into.
    type List;

    /// Creates the pipelines. The device must outlive the generator.
    fn create(device: &D) -> Result<Self, GeneratorError>
    where
        Self: Sized;

    /// Applies the frame geometry. Recreates the intermediate targets when the sizes change; safe
    /// to call every frame. Refused while a prepare snapshot is unconsumed.
    fn configure(&mut self, params: &FrameParams) -> Result<(), GeneratorError>;

    /// Records depth/motion normalization and dilation on the game's prepare command list, before
    /// the application can recycle its input textures. Generation must execute after this command
    /// list on the same queue, or with an explicit queue dependency. One outstanding pair per
    /// generator.
    fn prepare(
        &mut self,
        list: &Self::List,
        frame: &InputFrame<'_, D::Texture>,
    ) -> Result<(), GeneratorError>;

    /// Only after all recorded work has retired. Keeps resources and state tracking, but abandons
    /// the snapshot and forces the next generated frame to seed history.
    fn discard_prepare(&mut self);

    /// Produces `count` (1..=[`MAX_PHASES`]) generated frames between the same pair of real
    /// frames, each at its own alpha into its own output. The history advances once. On a reset
    /// every output receives the real frame (K53).
    fn generate(
        &mut self,
        list: &Self::List,
        frame: &InputFrame<'_, D::Texture>,
        phases: &[GeneratePhase<'_, D::Texture>],
        reset_history: bool,
        hud: Option<HudSource<'_, D::Texture>>,
        pass_mark: Option<PassMark<'_>>,
    ) -> Result<(), GeneratorError>;

    /// The configured geometry.
    fn params(&self) -> &FrameParams;

    /// Whether the pipelines and device are ready to record.
    fn ready(&self) -> bool;

    /// Releases every intermediate target. Keeps the device and pipelines.
    fn release_targets(&mut self);

    /// True while a prepare snapshot is recorded and not yet consumed.
    fn pending(&self) -> bool;
}

/// A bounded retirement wait, expressed once so both the context and the swapchain use the same
/// rule. Returns `Ok(true)` when the work has retired, `Ok(false)` on a timeout, and an error only
/// for a removed device.
///
/// `device_removed` reports the device's removal reason. A removed device executes nothing more, so
/// work recorded or submitted on it can never be "in flight" and counts as retired (K38), but the
/// reason is still reported so a caller that needs the error can see it.
pub fn wait_retired<F: super::gpu::GpuFence>(
    fence: &F,
    value: u64,
    timeout: Duration,
    device_removed: impl FnOnce() -> Option<u32>,
) -> Result<bool, GpuError> {
    use crate::domain::gpu_policy::{classify_fence, FenceProgress};
    match classify_fence(fence.completed(), value) {
        FenceProgress::Complete => return Ok(true),
        FenceProgress::DeviceRemoved => {
            return Err(GpuError::DeviceRemoved {
                reason: crate::domain::gpu_policy::removed_device_error(
                    device_removed().unwrap_or(crate::domain::gpu_policy::DEVICE_REMOVED),
                ),
            })
        }
        FenceProgress::Pending => {}
    }
    match fence.wait(value, timeout) {
        Ok(()) => Ok(true),
        // The event can fire for a device removal that the fence reports as a non-sentinel value.
        Err(GpuError::Timeout) => Ok(device_removed().is_some()),
        Err(GpuError::DeviceRemoved { reason }) => Err(GpuError::DeviceRemoved { reason }),
        Err(other) => Err(other),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::domain::gpu_policy::DEVICE_REMOVED;
    use std::cell::Cell;

    struct FakeFence {
        completed: Cell<u64>,
        waits: Cell<u32>,
    }

    impl FakeFence {
        fn new(value: u64) -> Self {
            Self {
                completed: Cell::new(value),
                waits: Cell::new(0),
            }
        }
    }

    impl super::super::gpu::GpuFence for FakeFence {
        fn completed(&self) -> u64 {
            self.completed.get()
        }

        fn wait(&self, _value: u64, _timeout: Duration) -> Result<(), GpuError> {
            self.waits.set(self.waits.get() + 1);
            Err(GpuError::Timeout)
        }
    }

    #[test]
    fn retired_wait_skips_a_completed_fence() {
        let fence = FakeFence::new(7);
        assert_eq!(
            wait_retired(&fence, 5, Duration::from_millis(1), || None),
            Ok(true)
        );
        assert_eq!(fence.waits.get(), 0);
    }

    #[test]
    fn retired_wait_reports_timeout_without_failing() {
        let fence = FakeFence::new(3);
        assert_eq!(
            wait_retired(&fence, 5, Duration::from_millis(1), || None),
            Ok(false)
        );
        assert_eq!(fence.waits.get(), 1);
    }

    #[test]
    fn retired_wait_never_treats_removal_as_success() {
        let fence = FakeFence::new(u64::MAX);
        assert_eq!(
            wait_retired(&fence, 5, Duration::from_millis(1), || None),
            Err(GpuError::DeviceRemoved {
                reason: DEVICE_REMOVED
            })
        );
        assert_eq!(fence.waits.get(), 0);
    }

    #[test]
    fn retired_wait_counts_work_on_a_removed_device_as_retired() {
        // A timed-out wait on a removed device is not a live slot: the GPU will never signal.
        let fence = FakeFence::new(3);
        assert_eq!(
            wait_retired(&fence, 5, Duration::from_millis(1), || Some(0x887A_0005)),
            Ok(true)
        );
    }

    #[test]
    fn stage1_pass_names_match_the_cpp_hook() {
        let names: Vec<_> = [
            Stage1Pass::Prepare,
            Stage1Pass::Clear,
            Stage1Pass::Reproject,
            Stage1Pass::Fill,
            Stage1Pass::Warp,
            Stage1Pass::Composite,
        ]
        .iter()
        .map(|pass| pass.name())
        .collect();
        assert_eq!(
            names,
            ["prepare", "clear", "reproject", "fill", "warp", "composite"]
        );
    }
}
