//! Interim counting hook (replaced by the egui-backed host in the overlay wiring step).
//!
//! The overlay's frame drawing is the last slice of the port (egui plus a hand-written D3D12
//! renderer). Until it lands, [`Overlay`] is the production [`OverlayHook`] the wrapper installs:
//! it counts the outputs it was shown and draws nothing, which is exactly what the C++ drew before
//! the overlay's first frame. The wrapper's hook contract (draw on the open copy list, restore the
//! frame's state, report each successful Present) is already the one the renderer will implement.
use std::sync::atomic::{AtomicU64, Ordering};

use crate::application::ports::gpu::{GpuCommandList, GpuTexture};
use crate::application::ports::overlay::OverlayHook;

/// The production overlay hook. Safe to call from whichever thread presents.
#[derive(Default)]
pub struct Overlay {
    presented_real: AtomicU64,
    presented_generated: AtomicU64,
}

impl Overlay {
    /// Outputs presented so far, real and generated.
    pub fn counters(&self) -> (u64, u64) {
        (
            self.presented_real.load(Ordering::Relaxed),
            self.presented_generated.load(Ordering::Relaxed),
        )
    }
}

impl<T: GpuTexture> OverlayHook<T> for Overlay {
    fn draw(&self, _list: &dyn GpuCommandList<T>, _back_buffer: &T, _generated: bool) {
        // Nothing is drawn yet; the frame is left in `PRESENT` exactly as the hook contract requires.
    }

    fn presented(&self, generated: bool) {
        if generated {
            self.presented_generated.fetch_add(1, Ordering::Relaxed);
        } else {
            self.presented_real.fetch_add(1, Ordering::Relaxed);
        }
    }
}
