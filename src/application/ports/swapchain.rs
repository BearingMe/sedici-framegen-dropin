//! The real native swapchain behind the wrapper (ports the DXGI half of `presentation/swapchain.*`).
//!
//! The native chain is the one the game created. The wrapper never replaces it in "vsync"/"game"
//! mode: it copies its own private images into the chain's back buffers and forwards every call it
//! does not intercept here. In "vrr" mode the adapter may hand back a chain recreated on Sedici's
//! present queue (`Presenter::adopt` in the C++), but the port shape is the same.
//!
//! Plain types only: no `windows` handles, no COM pointers. Flags stay in the DXGI bit encoding
//! the game already speaks, because the wrapper's job is to forward them unchanged.
use std::time::Duration;

use crate::application::ports::gpu::{GpuError, GpuTexture, PixelFormat};

/// A back-buffer present flag, in DXGI's bit encoding.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct PresentFlags(pub u32);

impl PresentFlags {
    pub const NONE: Self = Self(0);
    /// `DXGI_PRESENT_TEST`: answer whether Present would succeed, present nothing.
    pub const TEST: Self = Self(1);
    /// `DXGI_PRESENT_DO_NOT_WAIT`: never block.
    pub const DO_NOT_WAIT: Self = Self(8);
    /// `DXGI_PRESENT_ALLOW_TEARING`: the chain was created tearing-capable.
    pub const ALLOW_TEARING: Self = Self(512);

    pub const fn contains(self, other: Self) -> bool {
        self.0 & other.0 == other.0
    }

    pub const fn without(self, other: Self) -> Self {
        Self(self.0 & !other.0)
    }
}

/// `RECT`, in client pixels.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Rect {
    pub left: i32,
    pub top: i32,
    pub right: i32,
    pub bottom: i32,
}

/// `POINT`, in client pixels.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Offset {
    pub x: i32,
    pub y: i32,
}

/// The parts of `DXGI_PRESENT_PARAMETERS` the wrapper must own across an asynchronous Present.
///
/// The game's pointers are only valid for the duration of its call; a "vrr" job outlives it, so
/// the wrapper copies these (the owner-approved Present1 metadata exception: a failed copy is a
/// local error before any schedule or GPU work, K76).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct PresentParams {
    pub dirty_rects: Vec<Rect>,
    pub scroll_rect: Option<Rect>,
    pub scroll_offset: Option<Offset>,
}

impl PresentParams {
    /// True when Present1 was called with no metadata at all: an empty `Present1` is not the same
    /// call as `Present`.
    pub fn is_empty(&self) -> bool {
        self.dirty_rects.is_empty() && self.scroll_rect.is_none() && self.scroll_offset.is_none()
    }
}

/// The chain's creation description, as `GetDesc1` reports it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SwapChainDesc {
    pub width: u32,
    pub height: u32,
    pub format: PixelFormat,
    pub buffer_count: u32,
    pub sample_count: u32,
    pub stereo: bool,
    pub flags: u32,
}

impl Default for SwapChainDesc {
    fn default() -> Self {
        Self {
            width: 0,
            height: 0,
            format: PixelFormat::Unknown,
            buffer_count: 0,
            sample_count: 0,
            stereo: false,
            flags: 0,
        }
    }
}

impl SwapChainDesc {
    /// The two colour formats the generator supports; anything else is refused with a defined
    /// error rather than guessed at (`supported_color_format` in the C++).
    pub fn supported_format(&self) -> bool {
        matches!(
            self.format,
            PixelFormat::R8G8B8A8Unorm | PixelFormat::R10G10B10A2Unorm
        )
    }

    /// A chain the wrapper can present through.
    pub fn is_supported(&self) -> bool {
        self.sample_count == 1 && self.buffer_count != 0 && !self.stereo && self.supported_format()
    }

    /// `DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING`, kept as raw bits.
    pub fn allow_tearing(&self) -> bool {
        self.flags & 0x800 != 0
    }

    /// `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT`.
    pub fn waitable(&self) -> bool {
        self.flags & 0x40 != 0
    }
}

/// One `DXGI_FRAME_STATISTICS` sample; the time fields are QPC ticks.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct FrameStatistics {
    pub present_count: u32,
    pub present_refresh_count: u32,
    pub sync_refresh_count: u32,
    pub sync_qpc_ticks: i64,
}

/// What the containing output reports, for the refresh rate and the tear guard.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct OutputDesc {
    /// The GDI device name (`DEVMODE.dmDeviceName`) as UTF-16 units, terminator stripped.
    pub device_name: Vec<u16>,
    pub width: i64,
    pub height: i64,
    pub rotated_quarter_turn: bool,
    /// The current mode's refresh rate; 0 when it cannot be read.
    pub refresh_hz: f64,
}

/// The native chain. Every call forwards the game's own request; nothing here is invented.
///
/// The methods take `&self` so one chain handle serves the game thread and the "vrr" presenter
/// thread: DXGI's chain is free-threaded for the calls the wrapper forwards.
pub trait NativeSwapchain: Send + Sync {
    type Buffer: GpuTexture;

    /// One of the chain's back buffers, as a texture the wrapper can copy into and restore.
    fn back_buffer(&self, index: u32) -> Result<Self::Buffer, GpuError>;

    /// How many references the application still holds on a back buffer. Resize must fail while the
    /// game owns one, just as native DXGI does; `None` when the adapter cannot count them.
    fn buffer_reference_count(&self, index: u32) -> Option<u32>;

    fn desc(&self) -> Result<SwapChainDesc, GpuError>;

    fn current_back_buffer_index(&self) -> u32;

    fn present(&self, sync: u32, flags: PresentFlags) -> Result<(), GpuError>;

    /// Present1. `params` is always given: an empty one is itself a distinct call.
    fn present1(
        &self,
        sync: u32,
        flags: PresentFlags,
        params: &PresentParams,
    ) -> Result<(), GpuError>;

    fn resize_buffers(
        &self,
        count: u32,
        width: u32,
        height: u32,
        format: PixelFormat,
        flags: u32,
    ) -> Result<(), GpuError>;

    /// ResizeBuffers1 with one node mask per buffer. Only one direct queue and one node are
    /// supported; the adapter checks the queue identity it was created with, the wrapper checks
    /// that every entry names that same queue.
    fn resize_buffers1(
        &self,
        count: u32,
        width: u32,
        height: u32,
        format: PixelFormat,
        flags: u32,
        node_masks: &[u32],
    ) -> Result<(), GpuError>;

    fn frame_statistics(&self) -> Option<FrameStatistics>;

    fn last_present_count(&self) -> Option<u32>;

    fn maximum_frame_latency(&self) -> Option<u32>;

    fn set_maximum_frame_latency(&self, latency: u32) -> Result<(), GpuError>;

    fn has_waitable_object(&self) -> bool;

    fn containing_output(&self) -> Option<OutputDesc>;

    fn set_fullscreen_state(&self, fullscreen: bool) -> Result<(), GpuError>;

    fn fullscreen(&self) -> bool;

    fn is_temporary_mono_supported(&self) -> bool;

    /// Signals the adapter's own fence once everything queued on the chain's queue has retired and
    /// waits for it with a bound. Used by teardown; the wrapper's own drain lives in the backend.
    fn wait_for_retirement(&self, timeout: Duration) -> Result<(), GpuError>;
}

/// Where the panel's scan is, as D3DKMT reports it; the tear guard's input.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Scanline {
    pub in_vertical_blank: bool,
    pub line: u32,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_the_generator_formats_are_presentable() {
        let mut desc = SwapChainDesc {
            width: 1280,
            height: 720,
            format: PixelFormat::R8G8B8A8Unorm,
            buffer_count: 3,
            sample_count: 1,
            stereo: false,
            flags: 0,
        };
        assert!(desc.is_supported());
        desc.format = PixelFormat::R16G16B16A16Float;
        assert!(!desc.is_supported());
        assert!(!desc.supported_format());
        desc.format = PixelFormat::R10G10B10A2Unorm;
        assert!(desc.is_supported());
        desc.sample_count = 2;
        assert!(!desc.is_supported());
        desc.sample_count = 1;
        desc.buffer_count = 0;
        assert!(!desc.is_supported());
        desc.buffer_count = 3;
        desc.stereo = true;
        assert!(!desc.is_supported());
    }

    #[test]
    fn chain_flags_are_read_from_the_raw_bits() {
        let desc = SwapChainDesc {
            width: 1,
            height: 1,
            format: PixelFormat::R8G8B8A8Unorm,
            buffer_count: 2,
            sample_count: 1,
            stereo: false,
            flags: 0x840,
        };
        assert!(desc.allow_tearing());
        assert!(desc.waitable());
    }

    #[test]
    fn present_flags_forward_unchanged() {
        let flags = PresentFlags(PresentFlags::DO_NOT_WAIT.0 | PresentFlags::ALLOW_TEARING.0);
        assert!(flags.contains(PresentFlags::DO_NOT_WAIT));
        assert!(flags.contains(PresentFlags::ALLOW_TEARING));
        assert!(!flags.contains(PresentFlags::TEST));
        let stripped = flags.without(PresentFlags::DO_NOT_WAIT);
        assert_eq!(stripped, PresentFlags::ALLOW_TEARING);
    }

    #[test]
    fn empty_metadata_is_not_the_same_as_absent_metadata() {
        assert!(PresentParams::default().is_empty());
        let with_rect = PresentParams {
            dirty_rects: vec![Rect {
                left: 0,
                top: 0,
                right: 4,
                bottom: 4,
            }],
            ..PresentParams::default()
        };
        assert!(!with_rect.is_empty());
    }
}
