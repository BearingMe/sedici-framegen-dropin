//! Drawing the overlay onto the game's back buffer.
//!
//! The swapchain slice calls [`OverlayRenderer::draw`] once per presented output, after generation
//! and before Present, with the command list that frame is already being recorded on. The
//! application hands over a plain [`OverlayView`] and gets back the [`OverlayAction`]s the user asked
//! for; no UI-toolkit type appears in this port, and the adapter owns the egui context, the input
//! translation and the D3D12 renderer.
//!
//! # Failures are never game failures
//!
//! A renderer failure disables the overlay for the session (the C++ `ui_disable`) and is reported to
//! the caller as [`OverlayDrawError`]. The caller logs it, gives input back and keeps presenting;
//! AGENTS.md forbids failing a game call because of Sedici's own bookkeeping.
//!
//! # What is passed in
//!
//! - [`RenderTarget`]: the native back buffer identity, size, format and current state, plus the
//!   command list and device identities. All plain values; the adapter downcasts them back to COM
//!   pointers it created itself and refuses a pair it does not own rather than guessing.
//! - [`OverlayView`]: what to draw (`Off`, `Mini`, or `Full`), built by
//!   [`crate::application::overlay::OverlayState`].
//! - [`UiInput`]: the window messages the Full menu consumed, already in client pixels. The adapter
//!   maps them to output pixels, feeds egui and lets Alt+F4 through (K126).
use crate::application::overlay::{OverlayAction, OverlayView};

/// The D3D12 `DXGI_FORMAT` value of the output. Numeric so this module imports no `windows` type.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct OutputFormat(pub u32);

/// A native back buffer and the list to record onto.
#[derive(Clone, Copy, Debug)]
pub struct RenderTarget {
    /// A `ID3D12GraphicsCommandList*` identity, recorded by the adapter.
    pub command_list: usize,
    /// A `ID3D12Resource*` identity of the output back buffer.
    pub resource: usize,
    /// A `ID3D12Device*` identity, so the renderer can refuse a device it was not created for.
    pub device: usize,
    pub width: u32,
    pub height: u32,
    pub format: OutputFormat,
    /// The buffer's state before the draw, in the D3D12 `D3D12_RESOURCE_STATES` bits that
    /// [`crate::application::ports::gpu::ResourceState`] already carries.
    pub state: crate::application::ports::gpu::ResourceState,
    /// A generated (interpolated) output rather than the game's own frame.
    pub generated: bool,
}

/// One input message the Full menu consumed, in client coordinates.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct UiInput {
    /// The Win32 message id (`WM_MOUSEMOVE`, `WM_LBUTTONDOWN`, ...).
    pub msg: u32,
    pub wparam: u64,
    pub lparam: i64,
}

/// The output size and the window client size at the time of the frame. Mouse coordinates arrive in
/// client pixels and egui draws at the output resolution, so the adapter maps between the two.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct UiFrameInfo {
    pub output_width: u32,
    pub output_height: u32,
    pub client_width: i32,
    pub client_height: i32,
    /// Seconds since the previous overlay frame, for egui's animation clock. `None` lets egui assume
    /// 1/60 s. Clamped by the caller to the same 1..=100 ms window the C++ used.
    pub delta_seconds: Option<f32>,
}

/// A renderer failure. Non-fatal to the game; the overlay switches itself off for the session.
#[derive(Clone, Debug, PartialEq, Eq, thiserror::Error)]
pub enum OverlayDrawError {
    #[error("{0}")]
    Failed(String),
    /// No D3D12 device, or the device was removed.
    #[error("the D3D12 device is unavailable")]
    NoDevice,
    /// The device or command list differs from the one the renderer was created for.
    #[error("the D3D12 device changed")]
    DeviceChanged,
    /// The output format is one this renderer cannot blend into.
    #[error("the output format changed")]
    UnsupportedFormat(OutputFormat),
    /// The renderer is disabled; nothing was drawn.
    #[error("the overlay renderer is disabled: {0}")]
    Disabled(String),
}

impl OverlayDrawError {
    /// Whether the failure permanently disables the overlay, as the C++ `ui_disable` did. A changed
    /// format or device is also permanent for the session: the C++ disabled the overlay for either.
    pub fn is_permanent(&self) -> bool {
        true
    }

    /// The one-line reason the caller logs after `sedici: overlay disabled`.
    pub fn reason(&self) -> String {
        self.to_string()
    }
}

/// One overlay renderer per DLL.
pub trait OverlayRenderer: Send {
    /// Runs one egui pass for `view`, feeds it `input`, records it onto `target`'s command list and
    /// returns the actions the user asked for.
    ///
    /// The overlay is drawn after generation and outside the limiter, so it is never interpolated.
    /// A view of [`OverlayView::Off`] records nothing and returns no actions.
    fn draw(
        &mut self,
        target: &RenderTarget,
        view: &OverlayView,
        input: &[UiInput],
        frame: UiFrameInfo,
    ) -> Result<Vec<OverlayAction>, OverlayDrawError>;

    /// True after a failed draw or an explicit [`disable`](Self::disable); the caller stops calling
    /// [`draw`](Self::draw) and gives input back.
    fn disabled(&self) -> bool;

    /// The reason the renderer disabled itself, for the `sedici: overlay disabled` line pair.
    fn disabled_reason(&self) -> Option<String>;

    /// Permanently disables the renderer (ports `ui_disable`).
    fn disable(&mut self, reason: &str);

    /// Draws recorded so far: `(real, generated)` (ports `ui_render_status`).
    fn draw_counts(&self) -> (u64, u64);
}
