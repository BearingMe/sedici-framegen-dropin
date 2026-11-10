//! Overlay composition onto a presented frame (ports the draw half of `presentation/overlay_compose.*`
//! and the `OutputHooks` the wrapper calls per output).
//!
//! The wrapper and the presenter call [`OverlayHook::draw`] while the presentation command list is
//! still open, after the frame has been copied into the native back buffer and before it is closed
//! and submitted. `presented` follows each successful native Present.
//!
//! The list arrives as `&dyn GpuCommandList<T>`, so one hook serves both the wrapper's copy list and
//! the presenter's, and the application never names a D3D12 type. Both methods are called from
//! whichever thread presents, which may be the "vrr" presenter thread, so implementations must be
//! thread-safe.

use crate::application::ports::gpu::{GpuCommandList, GpuTexture};

/// A hook the presentation path calls for each output it shows.
pub trait OverlayHook<T: GpuTexture>: Send + Sync {
    /// Draw the overlay onto the frame just copied into the native back buffer. The frame is in
    /// `PRESENT`; the implementation must leave it in `PRESENT`, because the copy is submitted
    /// right after this returns.
    fn draw(&self, list: &dyn GpuCommandList<T>, back_buffer: &T, generated: bool);

    /// The frame was presented by the native chain. A failed Present is not reported.
    fn presented(&self, generated: bool);
}

/// No overlay: the wrapper's default and what a cleared hook falls back to.
pub struct NoOverlay;

impl<T: GpuTexture> OverlayHook<T> for NoOverlay {
    fn draw(&self, _list: &dyn GpuCommandList<T>, _back_buffer: &T, _generated: bool) {}
    fn presented(&self, _generated: bool) {}
}

/// Counts a real frame the presenter dropped because a later output was already due (K125).
pub trait RealDropSink: Send + Sync {
    fn on_real_dropped(&self);
}

/// Accepts and discards the event.
pub struct NoRealDropSink;

impl RealDropSink for NoRealDropSink {
    fn on_real_dropped(&self) {}
}
