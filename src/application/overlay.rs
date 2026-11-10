//! The overlay use case: output-cadence recording, the early/periodic `overlay present` report
//! window (ports `ui/overlay.cpp`), the Mini/Full menu view-model and its actions (ports the
//! menu half of `ui/ui.cpp`).
//!
//! Nothing here knows a UI toolkit. The menu is described as a plain [`OverlayView`] of values and
//! a set of enums; the adapter (`adapters::driven::overlay_ui`) draws it with egui and turns the
//! user's intent back into [`OverlayAction`]s. That keeps the C++ `draw_full`/`draw_mini` layout
//! the adapter's business and this module's rules testable without a window, a device or a clock.
//!
//! State that outlives one menu frame — the pending target, the edit epoch and the save status —
//! lives in [`OverlayState`], one instance per DLL. The values themselves live in
//! [`crate::application::settings::live`]/[`crate::application::settings::tx`]; the menu only
//! remembers which mode to return to on Cancel and what the last file operation said.
use std::fmt::Write as _;

use crate::application::settings::io::{IoCompletion, IoKind};
use crate::application::settings::live::Live;
use crate::application::settings::tx::{ApplyResult, ApplyStatus, SaveTicket, SettingsTx};
use crate::application::stats::Stats;
use crate::domain::config::{Settings, Source, UiMode};
use crate::domain::timing::{TimingRing, TimingStats};
use crate::domain::ui_logic::{
    nearest_scale_choice, PendingTarget, SCALE_CHOICES, TARGET_MAX_UNITS, TARGET_MIN_UNITS,
};

/// Cadence reports every this many outputs once the settle window is over.
pub const REPORT_EVERY: u64 = 300;
/// After a swapchain create or resize the first reports cover 60 outputs instead of 300, so the
/// slow settle of the real rate (K123) shows up as several lines instead of one.
pub const EARLY_REPORT_EVERY: u64 = 60;
/// How many early reports follow a swapchain change (including the start of the run).
pub const EARLY_REPORTS: u32 = 8;
/// The Mini menu's background opacity, as the C++ `SetNextWindowBgAlpha(0.65f)`.
pub const MINI_BACKGROUND_ALPHA: f32 = 0.65;
/// The Full menu's nominal size at scale 1.0 (`ImVec2(560, 690)`), which egui re-derives as points.
pub const FULL_DEFAULT_SIZE: (f32, f32) = (560.0, 690.0);
/// Left/top margin of the Mini window (`SetNextWindowPos(ImVec2(12, 12))`).
pub const MINI_MARGIN: f32 = 12.0;
/// The graph height in the Full menu (`84.0f * scale` in the C++).
pub const GRAPH_HEIGHT: f32 = 84.0;
/// The graph's fixed vertical scale (`PlotLines(..., 0.0f, 40.0f)`).
pub const GRAPH_MAX_MS: f32 = 40.0;

/// One output present, as the wrapper reports it.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct OutputPresent {
    pub generated: bool,
}
/// One real present, as the wrapper reports it before its own limiter wait.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct RealPresent;

/// What the wrapper hands the overlay for one presented output. Everything a UI needs to know is
/// copied here; the adapter owns the D3D12 resource itself.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct PresentFrame {
    /// The output back buffer's width and height in pixels.
    pub width: u32,
    pub height: u32,
    /// A generated (interpolated) output, not the game's own frame.
    pub generated: bool,
}

/// The link information the C++ printed once, on its first callback (K123: the shape of the line
/// is read by `tools/analyze_log.ps1`). Pointers are formatted by the adapter; only the parts that
/// are values are assembled here.
pub fn active_line(
    device: &str,
    command_list: &str,
    output: &str,
    size: Option<(u32, u32)>,
    format: u32,
    state: u32,
    ui: bool,
) -> String {
    let mut out = String::new();
    let _ = write!(
        out,
        "sedici: overlay present callback active device={device} commandList={command_list} output={output} size="
    );
    match size {
        Some((width, height)) => {
            let _ = write!(out, "{width}x{height}");
        }
        None => out.push_str("n/a"),
    }
    let _ = write!(
        out,
        " format={format} state={state} ui={}",
        if ui { "yes" } else { "no" }
    );
    out
}

/// The periodic `overlay present` line, at every cadence report. `fps` values and the span use the
/// C++ stream precision (two and one decimals after the fps fields, three for the span).
pub fn stats_line(
    frames: u64,
    real: u64,
    generated: u64,
    output_fps: f64,
    real_fps: f64,
    span_ms: f64,
) -> String {
    let mut out = String::new();
    let _ = write!(
        out,
        "sedici: overlay present frames={frames} real={real} generated={generated} output_fps={output_fps:.2} real_fps={real_fps:.2} span_ms={span_ms:.1}"
    );
    out
}

/// The output-cadence window (ports the `Window` in `overlay.cpp`). It is closed over the injected
/// clock: [`record`](Self::record) takes the QPC tick, never reads it.
#[derive(Clone, Debug, Default)]
pub struct OutputCadence {
    calls: u64,
    real: u64,
    generated: u64,
    have_first: bool,
    first: u64,
    last: u64,
    seen_events: u64,
    early_reports: u32,
    /// The C++ `logged_active`: the link line is printed once, on the first output of the session.
    logged_active: bool,
}

/// The line one present produced, if any; the caller emits it at level 1.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct CadenceOutput {
    pub report: Option<String>,
    /// The first output of the session: the caller also emits the one-off
    /// [`active_line`] link report (the C++ `logged_active`).
    pub first_output: bool,
}

impl OutputCadence {
    pub fn new() -> Self {
        Self {
            early_reports: EARLY_REPORTS,
            ..Self::default()
        }
    }

    /// Records one output present. `swapchain_events` is the stats' event count; a change starts a
    /// fresh window and re-arms the early reports, as the C++ does so a report never mixes two
    /// sizes. The first report after a change is the early cadence.
    pub fn record(
        &mut self,
        now: u64,
        generated: bool,
        swapchain_events: u64,
        frequency: u64,
    ) -> CadenceOutput {
        let mut out = CadenceOutput::default();
        if !self.logged_active {
            self.logged_active = true;
            out.first_output = true;
        }
        if !self.have_first {
            self.have_first = true;
            self.first = now;
        }
        self.last = now;

        if swapchain_events != self.seen_events {
            self.seen_events = swapchain_events;
            self.early_reports = EARLY_REPORTS;
            self.calls = 0;
            self.real = 0;
            self.generated = 0;
            self.first = now;
            self.have_first = true;
        }

        self.calls += 1;
        if generated {
            self.generated += 1;
        } else {
            self.real += 1;
        }

        let report_every = if self.early_reports > 0 {
            EARLY_REPORT_EVERY
        } else {
            REPORT_EVERY
        };
        if self.calls >= report_every {
            if self.early_reports > 0 {
                self.early_reports -= 1;
            }
            out.report = Some(self.line(frequency));
            self.calls = 0;
            self.real = 0;
            self.generated = 0;
            self.have_first = false;
            self.first = 0;
            self.last = 0;
        }
        out
    }

    fn line(&self, frequency: u64) -> String {
        let intervals = self.calls.saturating_sub(1);
        let span_ms = if self.have_first {
            if frequency == 0 {
                0.0
            } else {
                self.last.saturating_sub(self.first) as f64 * 1000.0 / frequency as f64
            }
        } else {
            0.0
        };
        let seconds = span_ms / 1000.0;
        let output_fps = if seconds > 0.0 {
            intervals as f64 / seconds
        } else {
            0.0
        };
        let real_fps = if seconds > 0.0 {
            self.real as f64 / seconds
        } else {
            0.0
        };
        stats_line(
            self.calls,
            self.real,
            self.generated,
            output_fps,
            real_fps,
            span_ms,
        )
    }

    /// The early reports still to come in this window.
    pub fn early_reports_left(&self) -> u32 {
        self.early_reports
    }

    pub fn calls(&self) -> u64 {
        self.calls
    }
}

// ---------------------------------------------------------------------------
// Menu view-model
// ---------------------------------------------------------------------------

/// The Mini menu's one line: `real %.1f  |  out %.1f  |  p99 %.1f ms`.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct MiniView {
    pub real_fps: f32,
    pub output_fps: f32,
    pub output_p99_ms: f32,
}

impl MiniView {
    /// The exact text the C++ `ImGui::Text` laid out, so the adapter and the tests agree.
    pub fn line(&self) -> String {
        format!(
            "real {:.1}  |  out {:.1}  |  p99 {:.1} ms",
            self.real_fps, self.output_fps, self.output_p99_ms
        )
    }
}

/// Where each Full-menu group's values come from. `Live` for choices bound to the live settings,
/// `Env` when an environment override owns the value, in which case the control is disabled and the
/// menu says which variable overrides it.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Provenance {
    pub target_env: bool,
    pub log_env: bool,
}

/// The Full menu's values. Every field is a plain number or text the adapter renders; nothing here
/// is a widget. The adapter decides how each is drawn and which [`OverlayAction`] a click sends.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct FullView {
    pub scale: f32,
    /// `route_any_generation_enabled()`: "Frame generation: on/off".
    pub frame_generation_on: bool,
    /// `stats_render_size()`; `(0, 0)` means the menu shows the output size alone.
    pub render_size: (u32, u32),
    pub output_size: (u32, u32),
    pub real_fps: f32,
    pub output_fps: f32,
    pub real_dropped: u64,
    pub stall_count: u64,
    pub last_stall_ms: f32,
    pub output_stats: TimingStats,
    pub real_stats: TimingStats,
    /// Chronological interval samples, oldest first, for the two graphs.
    pub output_samples: Vec<f32>,
    pub real_samples: Vec<f32>,

    /// Controls are disabled until the edit baseline exists and no save is running.
    pub edits_enabled: bool,
    pub save_status: String,

    pub provenance: Provenance,
    /// 0 = Uncapped, 1 = Auto, 2 = Fixed.
    pub target_mode: TargetMode,
    /// The pending slider value, in half-fps units (FR-01, K118).
    pub pending_units: i32,
    pub pending_dirty: bool,
    pub pending_fps: f64,
    /// The real-frame cap the limiter is enforcing now, and the output refresh rate.
    pub limiter_fps: f64,
    pub output_refresh_hz: f64,

    pub log_level: i32,
    /// 0 = Auto, 1 = 0.80, 2 = 0.90, 3 = 1.00 (FR-04).
    pub scale_choice: ScaleChoice,
    pub applied_scale: f32,
}

/// The Full menu's target control, as the C++ combo box presents it.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum TargetMode {
    #[default]
    Uncapped,
    Auto,
    Fixed,
}

impl TargetMode {
    pub const ITEMS: [&'static str; 3] = ["Uncapped", "Auto (match the monitor)", "Fixed"];
}

/// The menu scale combo, as index 0 = Auto and 1..=3 = [`SCALE_CHOICES`] (FR-04).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ScaleChoice(pub usize);

impl ScaleChoice {
    pub const ITEMS: [&'static str; 4] = ["Auto", "0.80", "0.90", "1.00"];

    /// The choice for a live value: Auto in Auto mode, else the nearest fixed choice plus one.
    pub fn from_live(mode: crate::domain::config::UiScaleMode, value: f32) -> Self {
        match mode {
            crate::domain::config::UiScaleMode::Auto => Self(0),
            crate::domain::config::UiScaleMode::Fixed => Self(nearest_scale_choice(value) + 1),
        }
    }
}

/// One thing the user asked the menu to do. The adapter turns each click into one of these and
/// [`OverlayState::apply`] performs it, so the menu layout stays in the adapter and the rules stay
/// here.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum OverlayAction {
    /// The target combo moved: 0 uncapped, 1 auto, 2 fixed.
    SetTargetMode(u8),
    /// The slider moved to whole half-fps units.
    SlideTarget(i32),
    /// The Apply button: set the live cap to the pending value (FR-02).
    ApplyTarget,
    /// The log-level combo moved (0..=2).
    SetLogLevel(i32),
    /// The scale combo moved (0 Auto, 1..=3 [`SCALE_CHOICES`]).
    SetScaleChoice(usize),
    /// Save the edit to `sedici.toml`.
    Save,
    /// Cancel the edit: restore the values from when Full was opened.
    Cancel,
    /// Close the menu (`live_set_ui_mode(Off)`).
    Close,
}

/// What one action did. `status` is the message the menu should show; `None` means "leave the
/// status alone", and `Some("")` clears it.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct ActionOutcome {
    pub status: Option<String>,
    /// For a rejected or conflicting change, the reason, already prefixed as the C++ `report` does.
    pub rejected: bool,
    /// The UI mode changed as a result (Cancel returns to the mode Full was opened from).
    pub mode: Option<UiMode>,
}

/// A complete view of the menu for one frame plus the state an action may change.
///
/// `FullView` is much larger than `MiniView`; the enum is built once per output on the present
/// thread, so boxing it would cost an allocation per frame for no benefit. `Clone`/`Debug` exist
/// for the frame output a test inspects, not for the per-frame path.
#[derive(Clone, Debug, PartialEq)]
#[allow(clippy::large_enum_variant)]
#[derive(Default)]
pub enum OverlayView {
    #[default]
    Off,
    Mini(MiniView),
    Full(FullView),
}

/// The menu's own state. One instance per DLL, behind the render lock.
pub struct OverlayState {
    last_mode: UiMode,
    snapshot_previous_mode: UiMode,
    snapshot_valid: bool,
    edit_epoch: u64,
    edit_generation: u64,
    begin_requested: bool,
    save_status: String,
    pending_target: PendingTarget,
    applied_scale: f32,
}

impl Default for OverlayState {
    fn default() -> Self {
        Self::new()
    }
}

impl OverlayState {
    pub fn new() -> Self {
        Self {
            last_mode: UiMode::Off,
            snapshot_previous_mode: UiMode::Off,
            snapshot_valid: false,
            edit_epoch: 0,
            edit_generation: 0,
            begin_requested: false,
            save_status: String::new(),
            pending_target: PendingTarget::default(),
            applied_scale: 0.0,
        }
    }

    pub fn save_status(&self) -> &str {
        &self.save_status
    }

    pub fn snapshot_valid(&self) -> bool {
        self.snapshot_valid
    }

    /// The edit epoch an asynchronous completion must carry to be accepted.
    pub fn edit_epoch(&self) -> u64 {
        self.edit_epoch
    }

    /// A Save could not be started: the menu says so (the C++ `Cannot start settings save`).
    pub fn note_save_started(&mut self, started: bool) {
        self.save_status = if started {
            "Saving...".to_owned()
        } else {
            "Cannot start settings save".to_owned()
        };
    }

    /// The scale the menu was last drawn at, so the menu can show `Effective %.2fx`.
    pub fn applied_scale(&self) -> f32 {
        self.applied_scale
    }

    /// The menu's render height at which the scale is derived (`effective_scale`). Auto divides by
    /// 1080 and clamps to 0.5..=2.0.
    pub fn effective_scale(&self, height: u32, live: &Live) -> f32 {
        let values = live.snapshot();
        if values.scale_mode == crate::domain::config::UiScaleMode::Auto {
            let scale = if height > 0 {
                height as f32 / 1080.0
            } else {
                1.0
            };
            scale.clamp(0.5, 2.0)
        } else {
            values.ui_scale.clamp(0.5, 2.0)
        }
    }

    /// Records the scale used for a frame; the menu reads it back as "Effective".
    pub fn set_applied_scale(&mut self, scale: f32) {
        self.applied_scale = scale;
    }

    pub fn last_mode(&self) -> UiMode {
        self.last_mode
    }

    /// Full was entered: begin an edit. In-memory only; the file baseline is a worker job the
    /// caller starts with the returned generation.
    pub fn enter_full(&mut self, tx: &SettingsTx) -> u64 {
        self.edit_epoch = self.edit_epoch.wrapping_add(1);
        self.snapshot_previous_mode = self.last_mode;
        self.snapshot_valid = false;
        self.edit_generation = tx.start_edit();
        self.begin_requested = self.edit_generation != 0;
        self.save_status = if self.begin_requested {
            "Loading settings...".to_owned()
        } else {
            "Cannot start settings edit".to_owned()
        };
        self.edit_generation
    }

    /// The generation the caller must hand `SettingsIo::start_begin`, or `None` when nothing is
    /// waiting to start.
    pub fn take_begin_request(&mut self) -> Option<(u64, u64)> {
        if !self.begin_requested {
            return None;
        }
        self.begin_requested = false;
        Some((self.edit_generation, self.edit_epoch))
    }

    pub fn mark_begin_failed(&mut self) {
        self.begin_requested = false;
        self.save_status = "Cannot start settings I/O".to_owned();
    }

    /// Applies a mode change, including leaving Full (which invalidates outstanding I/O). Returns
    /// true when the mode actually changed.
    pub fn apply_mode_transition(&mut self, mode: UiMode, tx: &SettingsTx) -> bool {
        if mode == self.last_mode {
            return false;
        }
        if self.last_mode == UiMode::Full && mode != UiMode::Full {
            self.edit_epoch = self.edit_epoch.wrapping_add(1);
            self.snapshot_valid = false;
            self.begin_requested = false;
            tx.invalidate_edit();
        }
        if mode == UiMode::Full {
            self.enter_full(tx);
        }
        self.last_mode = mode;
        true
    }

    /// One settings-I/O completion. Results from an earlier Full-menu instance are discarded.
    pub fn on_io_completion(&mut self, mode: UiMode, completion: &IoCompletion) {
        if mode != UiMode::Full || completion.view_epoch != self.edit_epoch {
            return;
        }
        if completion.stalled {
            if completion.kind == IoKind::BeginEdit {
                self.snapshot_valid = false;
            }
            self.save_status = completion.save.message.clone();
            return;
        }
        match completion.kind {
            IoKind::BeginEdit => {
                self.snapshot_valid = completion.begin_ok;
                self.save_status = if completion.begin_ok {
                    String::new()
                } else {
                    "Cannot edit: file state or settings changed".to_owned()
                };
            }
            IoKind::Save => {
                self.save_status = if completion.save.message.is_empty() {
                    "Save failed".to_owned()
                } else {
                    completion.save.message.clone()
                };
            }
        }
    }

    /// Builds the Mini view.
    pub fn mini_view(real: TimingStats, output: TimingStats) -> MiniView {
        MiniView {
            real_fps: real.fps,
            output_fps: output.fps,
            output_p99_ms: output.p99_ms,
        }
    }

    /// The edit gate the C++ used: controls are enabled only once the baseline exists and no save is
    /// in flight. Closing the menu stays available regardless.
    pub fn edits_enabled(&self, io_busy: bool) -> bool {
        self.snapshot_valid && !io_busy
    }

    /// Builds the Full view from the live values, the acknowledged transaction and the injected
    /// timing samples. Nothing here touches a clock, a file or a GPU.
    #[allow(clippy::too_many_arguments)]
    pub fn full_view(
        &mut self,
        live: &Live,
        config: &Settings,
        real: TimingStats,
        output: TimingStats,
        output_samples: Vec<f32>,
        real_samples: Vec<f32>,
        stats: &Stats,
        frame_generation_on: bool,
        output_size: (u32, u32),
        io_busy: bool,
    ) -> FullView {
        let values = live.snapshot();
        let target_mode = if values.target_auto {
            TargetMode::Auto
        } else if values.target_fps > 0.0 {
            TargetMode::Fixed
        } else {
            TargetMode::Uncapped
        };
        // The untouched slider follows the live value; a touched one is never re-read (K118).
        self.pending_target.follow_live(values.target_fps);

        FullView {
            scale: self.applied_scale.max(f32::EPSILON),
            frame_generation_on,
            render_size: stats.render_size(),
            output_size,
            real_fps: real.fps,
            output_fps: output.fps,
            real_dropped: stats.real_dropped(),
            stall_count: stats.stall_count(),
            last_stall_ms: stats.last_stall_ms() as f32,
            output_stats: output,
            real_stats: real,
            output_samples,
            real_samples,
            edits_enabled: self.edits_enabled(io_busy),
            save_status: self.save_status.clone(),
            provenance: Provenance {
                target_env: config.fg.target_fps_source == Source::Env,
                log_env: config.log.level_source == Source::Env,
            },
            target_mode,
            pending_units: self.pending_target.units,
            pending_dirty: self.pending_target.dirty,
            pending_fps: crate::domain::ui_logic::target_fps_from_units(self.pending_target.units),
            limiter_fps: live.limiter_fps(),
            output_refresh_hz: live.output_refresh_hz(),
            log_level: values.log_level,
            scale_choice: ScaleChoice::from_live(values.scale_mode, values.ui_scale),
            applied_scale: self.applied_scale,
        }
    }

    /// Performs one action. Every settings change goes through the transaction, so a rejected or
    /// conflicting request leaves the acknowledged value in place and says why.
    pub fn apply(&mut self, action: OverlayAction, tx: &SettingsTx) -> ActionOutcome {
        let mut outcome = ActionOutcome::default();
        match action {
            OverlayAction::SetTargetMode(mode) => {
                let target = match mode {
                    0 => TargetMode::Uncapped,
                    1 => TargetMode::Auto,
                    _ => TargetMode::Fixed,
                };
                let pending = self.pending_target;
                let result = tx.change_edit(|values| {
                    values.target_auto = target == TargetMode::Auto;
                    values.target_fps = if target == TargetMode::Fixed {
                        crate::domain::ui_logic::target_fps_from_units(pending.units)
                    } else {
                        0.0
                    };
                });
                if target == TargetMode::Fixed {
                    self.pending_target.dirty = false;
                }
                outcome.status = report(&result);
                outcome.rejected = is_refusal(&result);
            }
            OverlayAction::SlideTarget(units) => {
                self.pending_target.edit(units);
            }
            OverlayAction::ApplyTarget => {
                let next = self.pending_target.apply();
                let result = tx.change_edit(|values| values.target_fps = next);
                outcome.status = report(&result);
                outcome.rejected = is_refusal(&result);
            }
            OverlayAction::SetLogLevel(level) => {
                let level = level.clamp(0, 2);
                let result = tx.change_edit(|values| values.log_level = level);
                outcome.status = report(&result);
                outcome.rejected = is_refusal(&result);
            }
            OverlayAction::SetScaleChoice(index) => {
                let index = index.min(SCALE_CHOICES.len());
                let result = tx.change_edit(|values| {
                    if index == 0 {
                        values.scale_mode = crate::domain::config::UiScaleMode::Auto;
                    } else {
                        values.scale_mode = crate::domain::config::UiScaleMode::Fixed;
                        values.ui_scale = SCALE_CHOICES[index - 1];
                    }
                });
                outcome.status = report(&result);
                outcome.rejected = is_refusal(&result);
            }
            OverlayAction::Save => {
                let ticket = tx.capture_save_ticket();
                if !ticket.valid {
                    self.save_status = "Settings changed; reopen the menu before saving".to_owned();
                    outcome.status = Some(self.save_status.clone());
                    outcome.rejected = true;
                } else {
                    outcome.status = Some(String::new());
                    // The caller starts the file job with this ticket (see `save_ticket`).
                    self.save_status = String::new();
                }
            }
            OverlayAction::Cancel => {
                if !self.snapshot_valid {
                    return outcome;
                }
                let result = tx.cancel_edit();
                if is_refusal(&result) {
                    self.save_status = format!("Not applied: {}", result.reason);
                    outcome.status = Some(self.save_status.clone());
                    outcome.rejected = true;
                    return outcome;
                }
                outcome.mode = Some(self.snapshot_previous_mode);
                self.save_status = "Cancelled".to_owned();
                outcome.status = Some(self.save_status.clone());
            }
            OverlayAction::Close => {
                outcome.mode = Some(UiMode::Off);
            }
        }
        outcome
    }

    /// Whether a baseline job is waiting to start (the menu shows "Loading settings...").
    pub fn begin_requested(&self) -> bool {
        self.begin_requested
    }

    /// The ticket a Save action needs. Valid only immediately after [`apply`](Self::apply) ran
    /// `Save` on a transaction whose baseline is current.
    pub fn save_ticket(tx: &SettingsTx) -> SaveTicket {
        tx.capture_save_ticket()
    }
}

/// `report()` in the C++: a rejected or conflicting request leaves the acknowledged values in
/// place and says so, instead of pretending the control took effect.
fn report(result: &ApplyResult) -> Option<String> {
    is_refusal(result).then(|| format!("Not applied: {}", result.reason))
}

fn is_refusal(result: &ApplyResult) -> bool {
    matches!(result.status, ApplyStatus::Rejected | ApplyStatus::Conflict)
}

/// Number of half-fps units in the slider range, for the adapter's slider bounds.
pub fn target_units_range() -> (i32, i32) {
    (TARGET_MIN_UNITS, TARGET_MAX_UNITS)
}

/// The overlay's timing rings as the settings side feeds them. Kept next to the menu because the
/// menu reads them; the wrapper feeds them from its present hooks.
#[derive(Debug, Default)]
pub struct OverlayTiming {
    pub real: TimingRing,
    pub output: TimingRing,
}

impl OverlayTiming {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn on_real_present(&mut self, qpc: u64, frequency: u64) {
        self.real.push(qpc, frequency);
    }

    pub fn on_output_present(&mut self, qpc: u64, frequency: u64) {
        self.output.push(qpc, frequency);
    }

    pub fn real_stats(&self) -> TimingStats {
        self.real.stats()
    }

    pub fn output_stats(&self) -> TimingStats {
        self.output.stats()
    }

    pub fn real_samples(&self) -> Vec<f32> {
        self.real.chronological()
    }

    pub fn output_samples(&self) -> Vec<f32> {
        self.output.chronological()
    }
}

// ---------------------------------------------------------------------------
// Per-output menu pass (ports `ui_on_present`)
// ---------------------------------------------------------------------------

/// What one overlay frame needs from the swapchain wrapper and the settings side. Plain values and
/// references only; the adapter owns the D3D12 objects and the window.
pub struct OverlayFrame<'a> {
    /// The menu mode the live settings hold, before any transition this frame performs.
    pub mode: UiMode,
    /// The game window the overlay input is bound to, or `None` when no window was captured.
    pub window: crate::application::ports::input::WindowHandle,
    /// The output's size in pixels, as the frame being drawn at.
    pub output_size: (u32, u32),
    /// The window's client size in pixels; `(0, 0)` when unknown.
    pub client_size: (i32, i32),
    /// Seconds since the previous overlay frame, already clamped to 1..=100 ms by the clock owner.
    pub delta_seconds: Option<f32>,
    /// `route_any_generation_enabled()`: what the Full menu's first line reports.
    pub frame_generation_on: bool,
    pub live: &'a Live,
    pub tx: &'a SettingsTx,
    pub stats: &'a Stats,
    pub timing: &'a OverlayTiming,
    /// The settings I/O scheduler, for the begin-edit request and the completions it reported.
    pub io: &'a crate::application::settings::io::SettingsIo,
}

/// What one overlay frame decided. The hook applies the mode change and starts any settings job.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct OverlayFrameOutput {
    /// The mode to publish after this frame (`apply_mode_transition`/`live_set_ui_mode`), when it
    /// differs from the mode the frame was given.
    pub mode: Option<UiMode>,
    /// A settings baseline job the caller must start with `SettingsIo::start_begin`.
    pub begin: Option<(u64, u64)>,
    /// A Save was requested with a ticket the caller must start with `SettingsIo::start_save`.
    pub save: Option<SaveTicket>,
    /// The view to draw; `OverlayView::Off` means the overlay records nothing this output.
    pub view: OverlayView,
    /// The GUP/one-line status the caller may log (the `save_status` text when it changed).
    pub status: Option<String>,
}

impl OverlayState {
    /// Runs one overlay frame: the C++ `ui_on_present`, split so every decision is a value.
    ///
    /// It processes the settings I/O completions first, applies the mode transition, starts a
    /// pending begin-edit when the scheduler is idle, and builds the view for the mode. The caller
    /// draws the view and then applies the returned mode change and starts the returned jobs; all
    /// of that is non-fatal to the game and none of it touches the GPU.
    pub fn frame(&mut self, frame: OverlayFrame<'_>) -> OverlayFrameOutput {
        let mut out = OverlayFrameOutput::default();
        // Results from an earlier Full-menu instance are discarded; a stalled job says so once.
        while let Some(completion) = frame.io.poll() {
            let before = self.save_status.clone();
            self.on_io_completion(frame.mode, &completion);
            if self.save_status != before {
                out.status = Some(self.save_status.clone());
            }
        }
        let mode = frame.mode;
        if self.apply_mode_transition(mode, frame.tx) {
            out.mode = Some(mode);
        }
        if mode == UiMode::Full && !frame.io.busy() {
            if let Some(request) = self.take_begin_request() {
                out.begin = Some(request);
            }
        }
        // Full with a busy scheduler keeps the request pending: it starts on the first quiet frame.
        // A stalled worker is reported by the scheduler itself (K83), so this is not an error.
        let io_busy = frame.io.busy();
        match mode {
            UiMode::Off => {
                out.view = OverlayView::Off;
            }
            UiMode::Mini => {
                out.view = OverlayView::Mini(Self::mini_view(
                    frame.timing.real_stats(),
                    frame.timing.output_stats(),
                ));
            }
            UiMode::Full => {
                self.set_applied_scale(self.effective_scale(frame.output_size.1, frame.live));
                let view = self.full_view(
                    frame.live,
                    frame.tx.config(),
                    frame.timing.real_stats(),
                    frame.timing.output_stats(),
                    frame.timing.output_samples(),
                    frame.timing.real_samples(),
                    frame.stats,
                    frame.frame_generation_on,
                    frame.output_size,
                    io_busy,
                );
                self.set_applied_scale(view.scale);
                out.view = OverlayView::Full(view);
            }
        }
        out
    }

    /// Performs one action the menu produced, then hands the caller the jobs to start. `mode` is the
    /// mode the frame was drawn in; Cancel and Close change it through the returned value.
    pub fn act(&mut self, action: OverlayAction, mode: UiMode, tx: &SettingsTx) -> ActionPlan {
        let outcome = self.apply(action, tx);
        // A Cancel or Close must make the same mode transition the frame loop makes, so leaving
        // Full invalidates the outstanding edit before the caller publishes the mode.
        if let Some(mode) = outcome.mode {
            self.apply_mode_transition(mode, tx);
        }
        let mut plan = ActionPlan {
            status: outcome.status.clone(),
            mode: outcome.mode,
            ..ActionPlan::default()
        };
        if action == OverlayAction::Save && !outcome.rejected {
            plan.save = Some(Self::save_ticket(tx));
        }
        if outcome.mode.is_some() {
            plan.transition_from = Some(mode);
        }
        plan
    }
}

/// What [`OverlayState::act`] asks the caller to do after an action.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct ActionPlan {
    /// The text to log/show, already the menu's own status.
    pub status: Option<String>,
    /// The mode the action changes to (Cancel remembers the mode Full was opened from).
    pub mode: Option<UiMode>,
    /// The mode the action was performed in, so the caller can run the same transition the frame
    /// would have made next time (leaving Full invalidates the outstanding I/O).
    pub transition_from: Option<UiMode>,
    /// A Save to start on the settings worker.
    pub save: Option<SaveTicket>,
}

/// The hotkey cycle: Off -> Mini -> Full -> Off (the C++ `cycle_mode`). A disabled overlay never
/// cycles.
pub fn cycle_mode(mode: UiMode) -> UiMode {
    match mode {
        UiMode::Off => UiMode::Mini,
        UiMode::Mini => UiMode::Full,
        UiMode::Full => UiMode::Off,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::application::ports::config_store::memory_store::MemoryStore;
    use crate::application::settings::io::IoCompletion;
    use crate::domain::timing::TimingStats;
    use rstest::rstest;
    use std::sync::Arc;

    fn tx(text: &str) -> (Arc<SettingsTx>, Arc<MemoryStore>, Arc<Live>) {
        let live = Arc::new(Live::new());
        live.initialize(&Settings::default());
        live.set_target_fps(36.5);
        let store = Arc::new(MemoryStore::new("C:/game/sedici.toml", text));
        let tx = Arc::new(SettingsTx::new(
            Arc::clone(&live),
            Settings::default(),
            store.clone(),
        ));
        (tx, store, live)
    }

    #[test]
    fn cadence_reports_60_eight_times_then_300() {
        let mut cadence = OutputCadence::new();
        assert_eq!(cadence.early_reports_left(), EARLY_REPORTS);
        let mut reports = Vec::new();
        for i in 0..=(60 * 8 + 300) {
            let out = cadence.record(1000 * i, i % 2 == 0, 0, 1_000_000);
            if let Some(line) = out.report {
                reports.push((i, line));
            }
        }
        // Eight early reports at 60..=480, then the 300 report at 780.
        assert_eq!(reports.len(), 9, "{reports:?}");
        assert_eq!(reports[0].0, 59);
        assert_eq!(reports[7].0, 479);
        assert_eq!(reports[8].0, 779);
        assert!(
            reports[0]
                .1
                .starts_with("sedici: overlay present frames=60 real="),
            "{}",
            reports[0].1
        );
        assert!(
            reports[8]
                .1
                .starts_with("sedici: overlay present frames=300"),
            "{}",
            reports[8].1
        );
        assert_eq!(cadence.early_reports_left(), 0);
    }

    #[test]
    fn a_swapchain_change_restarts_the_window_and_rearms_early_reports() {
        let mut cadence = OutputCadence::new();
        for i in 0..20 {
            cadence.record(i * 1000, false, 0, 1_000_000);
        }
        assert_eq!(cadence.calls(), 20);
        // The swapchain event invalidates the counts but not this present: it starts a fresh window.
        let out = cadence.record(20_000, true, 1, 1_000_000);
        assert_eq!(out, CadenceOutput::default());
        assert_eq!(cadence.calls(), 1);
        assert_eq!(cadence.early_reports_left(), EARLY_REPORTS);
    }

    #[rstest]
    #[case(0, 0.0)]
    #[case(2, 0.0)]
    fn zero_frequency_reports_a_zero_span(#[case] events: u64, #[case] _ignore: f64) {
        let mut cadence = OutputCadence::new();
        let mut report = None;
        for i in 0..60 {
            report = cadence.record(i, false, events, 0).report.or(report);
        }
        let line = report.expect("report");
        assert!(line.contains("span_ms=0.0"), "{line}");
        assert!(line.contains("output_fps=0.00"), "{line}");
    }

    #[test]
    fn the_active_line_names_the_callback_resources() {
        let line = active_line("0x1", "0x2", "0x3", Some((1920, 1080)), 28, 128, false);
        assert_eq!(
            line,
            "sedici: overlay present callback active device=0x1 commandList=0x2 output=0x3 size=1920x1080 format=28 state=128 ui=no"
        );
        let line = active_line("n/a", "n/a", "n/a", None, 0, 0, true);
        assert!(line.ends_with("size=n/a format=0 state=0 ui=yes"), "{line}");
    }

    #[test]
    fn mini_line_matches_the_cpp_format() {
        let mini = MiniView {
            real_fps: 60.0,
            output_fps: 119.94,
            output_p99_ms: 9.06,
        };
        assert_eq!(mini.line(), "real 60.0  |  out 119.9  |  p99 9.1 ms");
    }

    #[test]
    fn entering_full_starts_an_edit_and_remembers_the_previous_mode() {
        let (tx, _, _) = tx("[fg]\ntarget_fps = 30\n");
        let mut state = OverlayState::new();
        state.apply_mode_transition(UiMode::Mini, &tx);
        assert!(state.apply_mode_transition(UiMode::Full, &tx));
        assert!(!state.snapshot_valid());
        assert_eq!(state.save_status(), "Loading settings...");
        let request = state.take_begin_request().expect("begin request");
        assert_ne!(request.0, 0, "the edit generation is non-zero");
        assert!(
            state.take_begin_request().is_none(),
            "the request is taken once"
        );
    }

    #[test]
    fn an_io_completion_for_an_older_epoch_is_discarded() {
        let (tx, _, _) = tx("");
        let mut state = OverlayState::new();
        state.apply_mode_transition(UiMode::Full, &tx);
        let (_, epoch) = state.take_begin_request().unwrap();
        // A completion from an earlier Full-menu instance.
        state.on_io_completion(
            UiMode::Full,
            &IoCompletion {
                kind: IoKind::BeginEdit,
                view_epoch: epoch.wrapping_sub(1),
                begin_ok: true,
                ..IoCompletion::default()
            },
        );
        assert!(!state.snapshot_valid());
        state.on_io_completion(
            UiMode::Full,
            &IoCompletion {
                kind: IoKind::BeginEdit,
                view_epoch: epoch,
                begin_ok: true,
                ..IoCompletion::default()
            },
        );
        assert!(state.snapshot_valid());
        assert_eq!(state.save_status(), "");
    }

    #[test]
    fn leaving_full_invalidates_the_edit() {
        let (tx, _, _) = tx("");
        let mut state = OverlayState::new();
        state.apply_mode_transition(UiMode::Full, &tx);
        let (generation, _) = state.take_begin_request().unwrap();
        state.apply_mode_transition(UiMode::Off, &tx);
        // The generation the worker would still be using is now stale.
        assert!(!tx.begin_edit_for(generation));
    }

    #[rstest]
    #[case(75.0, 2.0)]
    #[case(60.0, 2.0)]
    fn apply_button_sets_the_live_cap(#[case] target: f64, #[case] ratio: f64) {
        let (tx, _, live) = tx("");
        live.set_output_context(ratio, 75.0);
        assert!(tx.begin_edit());
        live.set_target_auto(false);
        live.set_target_fps(target);
        let mut state = OverlayState::new();
        state.snapshot_valid = true;
        let units = crate::domain::ui_logic::target_units_from_fps(70.0);
        let outcome = state.apply(OverlayAction::SlideTarget(units), &tx);
        assert!(
            outcome.status.is_none(),
            "sliding only edits the pending value"
        );
        let outcome = state.apply(OverlayAction::ApplyTarget, &tx);
        assert_eq!(outcome.status, None, "an applied change reports nothing");
        assert_eq!(live.snapshot().target_fps, 70.0);
        assert!(!state.pending_target.dirty);
    }

    #[test]
    fn a_slow_upward_drag_is_never_undone_by_the_live_value() {
        let (tx, _, live) = tx("");
        assert!(tx.begin_edit());
        live.set_target_fps(40.0);
        let mut state = OverlayState::new();
        state.snapshot_valid = true;
        let mut previous = crate::domain::ui_logic::target_units_from_fps(40.0);
        for _ in 1..=20 {
            state.apply(OverlayAction::SlideTarget(previous + 1), &tx);
            state.pending_target.follow_live(live.snapshot().target_fps);
            previous = state.pending_target.units;
        }
        assert_eq!(
            state.pending_target.units, 100,
            "K118: the drag is not rounded back"
        );
        assert!(state.pending_target.dirty);
    }

    #[test]
    fn a_rejected_change_says_why_and_keeps_the_live_value() {
        let live = Arc::new(Live::new());
        live.initialize(&Settings::default());
        live.set_target_fps(37.0);
        let store = Arc::new(MemoryStore::new("C:/game/sedici.toml", ""));
        let mut config = Settings::default();
        config.log.level_source = Source::Env;
        let tx = SettingsTx::new(Arc::clone(&live), config, store);
        assert!(tx.begin_edit());
        let mut state = OverlayState::new();
        state.snapshot_valid = true;
        let outcome = state.apply(OverlayAction::SetLogLevel(2), &tx);
        assert!(outcome.rejected);
        let status = outcome.status.expect("status");
        assert!(status.starts_with("Not applied: "), "{status}");
        assert_eq!(live.log_level(), 1);
    }

    #[test]
    fn cancel_returns_to_the_mode_full_was_opened_from() {
        let (tx, _, live) = tx("");
        let mut state = OverlayState::new();
        state.apply_mode_transition(UiMode::Mini, &tx);
        state.apply_mode_transition(UiMode::Full, &tx);
        let (generation, epoch) = state.take_begin_request().unwrap();
        // The I/O worker captures the baseline; the completion then reports it ready.
        assert!(tx.begin_edit_for(generation));
        state.on_io_completion(
            UiMode::Full,
            &IoCompletion {
                kind: IoKind::BeginEdit,
                view_epoch: epoch,
                begin_ok: true,
                ..IoCompletion::default()
            },
        );
        state.apply(OverlayAction::SlideTarget(120), &tx);
        assert_eq!(state.apply(OverlayAction::ApplyTarget, &tx).status, None);
        assert_eq!(live.snapshot().target_fps, 60.0);
        let outcome = state.apply(OverlayAction::Cancel, &tx);
        assert_eq!(outcome.mode, Some(UiMode::Mini));
        assert_eq!(outcome.status.as_deref(), Some("Cancelled"));
        assert_eq!(
            live.snapshot().target_fps,
            36.5,
            "cancel restores the baseline"
        );
    }

    #[test]
    fn cancel_without_a_baseline_does_nothing() {
        let (tx, _, _) = tx("");
        let mut state = OverlayState::new();
        assert_eq!(state.apply(OverlayAction::Cancel, &tx).mode, None);
    }

    #[test]
    fn save_without_a_valid_ticket_asks_for_a_reopen() {
        let (tx, _, _) = tx("");
        let mut state = OverlayState::new();
        let outcome = state.apply(OverlayAction::Save, &tx);
        assert!(outcome.rejected);
        assert_eq!(
            state.save_status(),
            "Settings changed; reopen the menu before saving"
        );
    }

    #[rstest]
    #[case(0, 0.0, TargetMode::Uncapped)]
    #[case(1, 0.0, TargetMode::Auto)]
    #[case(2, 60.0, TargetMode::Fixed)]
    fn full_view_reports_the_target_mode(
        #[case] mode: u8,
        #[case] fps: f64,
        #[case] want: TargetMode,
    ) {
        let (tx, _, live) = tx("");
        live.set_target_auto(mode == 1);
        live.set_target_fps(fps);
        let mut state = OverlayState::new();
        state.snapshot_valid = true;
        let stats = Stats::new(1_000_000);
        let view = state.full_view(
            &live,
            tx.config(),
            TimingStats::default(),
            TimingStats::default(),
            Vec::new(),
            Vec::new(),
            &stats,
            true,
            (1920, 1080),
            false,
        );
        assert_eq!(view.target_mode, want);
        assert!(view.edits_enabled);
        assert!(view.frame_generation_on);
        assert_eq!(view.output_size, (1920, 1080));
        assert_eq!(view.scale_choice, ScaleChoice(0));
    }

    #[test]
    fn the_untouched_slider_follows_the_live_value() {
        let (tx, _, live) = tx("");
        live.set_target_auto(false);
        live.set_target_fps(36.5);
        let mut state = OverlayState::new();
        state.snapshot_valid = true;
        let stats = Stats::new(1_000_000);
        let view = state.full_view(
            &live,
            tx.config(),
            TimingStats::default(),
            TimingStats::default(),
            Vec::new(),
            Vec::new(),
            &stats,
            true,
            (1280, 720),
            false,
        );
        assert_eq!(view.pending_units, 73);
        assert!(!view.pending_dirty);
        assert_eq!(view.pending_fps, 36.5);
    }

    #[rstest]
    #[case(UiMode::Off, 0)]
    #[case(UiMode::Mini, 0)]
    fn effective_scale_is_auto_or_clamped(#[case] _mode: UiMode, #[case] _unused: u8) {
        let (tx, _, live) = tx("");
        let state = OverlayState::new();
        // Auto at 2160p clamps to 2.0, at 720p to 0.66.
        assert_eq!(state.effective_scale(2160, &live), 2.0);
        assert!((state.effective_scale(720, &live) - 720.0 / 1080.0).abs() < 1e-6);
        assert_eq!(state.effective_scale(0, &live), 1.0);
        // Fixed follows the live value, clamped.
        live.set_ui_scale_mode(crate::domain::config::UiScaleMode::Fixed);
        live.set_ui_scale(0.9);
        assert!((state.effective_scale(2160, &live) - 0.9).abs() < 1e-6);
        let _ = tx;
    }

    #[test]
    fn the_scale_choice_snaps_an_older_value() {
        use crate::domain::config::UiScaleMode;
        assert_eq!(
            ScaleChoice::from_live(UiScaleMode::Auto, 1.75),
            ScaleChoice(0)
        );
        assert_eq!(
            ScaleChoice::from_live(UiScaleMode::Fixed, 1.75),
            ScaleChoice(3)
        );
        assert_eq!(
            ScaleChoice::from_live(UiScaleMode::Fixed, 0.86),
            ScaleChoice(2)
        );
        assert_eq!(
            ScaleChoice::from_live(UiScaleMode::Fixed, 0.5),
            ScaleChoice(1)
        );
    }

    #[test]
    fn choosing_a_scale_updates_the_live_value() {
        let (tx, _, live) = tx("");
        assert!(tx.begin_edit());
        let mut state = OverlayState::new();
        state.snapshot_valid = true;
        assert_eq!(
            state.apply(OverlayAction::SetScaleChoice(2), &tx).status,
            None
        );
        assert!((live.ui_scale() - 0.9).abs() < 1e-6);
        assert_eq!(
            live.ui_scale_mode(),
            crate::domain::config::UiScaleMode::Fixed
        );
        assert_eq!(
            state.apply(OverlayAction::SetScaleChoice(0), &tx).status,
            None
        );
        assert_eq!(
            live.ui_scale_mode(),
            crate::domain::config::UiScaleMode::Auto
        );
    }

    #[test]
    fn edits_are_gated_on_the_baseline_and_the_worker() {
        let (tx, _, _) = tx("");
        let mut state = OverlayState::new();
        assert!(!state.edits_enabled(false));
        state.snapshot_valid = true;
        assert!(state.edits_enabled(false));
        assert!(!state.edits_enabled(true));
        let _ = tx;
    }

    #[test]
    fn the_timing_rings_feed_the_samples() {
        let mut timing = OverlayTiming::new();
        timing.on_output_present(0, 1_000_000);
        timing.on_output_present(8_000, 1_000_000);
        timing.on_output_present(16_000, 1_000_000);
        let samples = timing.output_samples();
        assert_eq!(samples.len(), 2);
        assert!((timing.output_stats().fps - 125.0).abs() < 1e-3);
        assert!(timing.real_samples().is_empty());
    }

    /// One frame's inputs over the test transaction, live values, stats and an idle I/O scheduler.
    fn frame_inputs<'a>(
        mode: UiMode,
        live: &'a Arc<Live>,
        tx: &'a Arc<SettingsTx>,
        stats: &'a Stats,
        timing: &'a OverlayTiming,
        io: &'a crate::application::settings::io::SettingsIo,
    ) -> OverlayFrame<'a> {
        OverlayFrame {
            mode,
            window: crate::application::ports::input::WindowHandle::NONE,
            output_size: (1920, 1080),
            client_size: (1920, 1080),
            delta_seconds: Some(0.016),
            frame_generation_on: true,
            live,
            tx,
            stats,
            timing,
            io,
        }
    }

    /// An I/O scheduler over the test transaction whose jobs never start (a refused spawn), so the
    /// frame machine sees an idle scheduler with no completion.
    fn test_io(
        tx: &Arc<SettingsTx>,
        store: &Arc<MemoryStore>,
    ) -> crate::application::settings::io::SettingsIo {
        let _ = store;
        struct NoWorker;
        impl crate::application::settings::io::WorkerHost for NoWorker {
            fn spawn(&self, _job: Box<dyn FnOnce() + Send>) -> bool {
                false
            }
            fn thread_id(&self) -> u32 {
                0
            }
            fn cancel(&self) {}
        }
        let port: Arc<dyn crate::application::settings::io::SettingsIoPort> =
            Arc::clone(tx) as Arc<dyn crate::application::settings::io::SettingsIoPort>;
        crate::application::settings::io::SettingsIo::new(port, Arc::new(NoWorker))
    }

    #[rstest]
    #[case(UiMode::Off)]
    #[case(UiMode::Mini)]
    #[case(UiMode::Full)]
    fn a_frame_builds_the_view_for_its_mode(#[case] mode: UiMode) {
        let (tx, store, live) = tx("");
        live.set_ui_mode(mode);
        let io = test_io(&tx, &store);
        let stats = Stats::new(1_000_000);
        let timing = OverlayTiming::new();
        let mut state = OverlayState::new();
        let out = state.frame(frame_inputs(mode, &live, &tx, &stats, &timing, &io));
        match mode {
            UiMode::Off => assert!(matches!(out.view, OverlayView::Off)),
            UiMode::Mini => assert!(matches!(out.view, OverlayView::Mini(_))),
            UiMode::Full => {
                assert!(matches!(out.view, OverlayView::Full(_)));
                // Entering Full starts an edit and asks the caller for the baseline job.
                let request = out.begin.expect("a begin-edit request");
                assert!(request.0 != 0 && request.1 != 0);
                assert_eq!(state.save_status(), "Loading settings...");
            }
        }
    }

    #[test]
    fn leaving_full_invalidates_the_edit_through_the_frame() {
        let (tx, store, live) = tx("");
        live.set_ui_mode(UiMode::Full);
        let io = test_io(&tx, &store);
        let stats = Stats::new(1_000_000);
        let timing = OverlayTiming::new();
        let mut state = OverlayState::new();
        let out = state.frame(frame_inputs(UiMode::Full, &live, &tx, &stats, &timing, &io));
        let (generation, _) = out.begin.expect("begin request");
        live.set_ui_mode(UiMode::Mini);
        state.frame(frame_inputs(UiMode::Mini, &live, &tx, &stats, &timing, &io));
        assert!(
            !tx.begin_edit_for(generation),
            "the outstanding edit is stale"
        );
    }

    #[test]
    fn the_hotkey_cycles_off_mini_full_off() {
        let mut mode = UiMode::Off;
        mode = cycle_mode(mode);
        assert_eq!(mode, UiMode::Mini);
        mode = cycle_mode(mode);
        assert_eq!(mode, UiMode::Full);
        mode = cycle_mode(mode);
        assert_eq!(mode, UiMode::Off);
    }

    #[test]
    fn a_cancel_action_returns_to_the_mode_full_was_opened_from() {
        let (tx, store, live) = tx("");
        live.set_ui_mode(UiMode::Mini);
        let io = test_io(&tx, &store);
        let stats = Stats::new(1_000_000);
        let timing = OverlayTiming::new();
        let mut state = OverlayState::new();
        state.frame(frame_inputs(UiMode::Mini, &live, &tx, &stats, &timing, &io));
        live.set_ui_mode(UiMode::Full);
        let out = state.frame(frame_inputs(UiMode::Full, &live, &tx, &stats, &timing, &io));
        assert!(out.begin.is_some());
        // The worker captures the baseline; the completion then marks the snapshot valid.
        assert!(tx.begin_edit());
        state.snapshot_valid = true;
        let plan = state.act(OverlayAction::Cancel, UiMode::Full, &tx);
        assert_eq!(plan.mode, Some(UiMode::Mini));
        assert_eq!(plan.transition_from, Some(UiMode::Full));
    }

    #[test]
    fn a_save_action_hands_out_a_ticket_only_with_a_baseline() {
        let (tx, store, live) = tx("");
        live.set_ui_mode(UiMode::Full);
        let io = test_io(&tx, &store);
        let stats = Stats::new(1_000_000);
        let timing = OverlayTiming::new();
        let mut state = OverlayState::new();
        state.frame(frame_inputs(UiMode::Full, &live, &tx, &stats, &timing, &io));
        // No baseline captured yet: the menu asks for a reopen, and no job starts.
        let plan = state.act(OverlayAction::Save, UiMode::Full, &tx);
        assert!(plan.save.is_none());
        assert!(state.save_status().contains("reopen"));
    }

    #[test]
    fn a_drawn_action_changes_the_live_value() {
        let (tx, _, live) = tx("");
        assert!(tx.begin_edit());
        let mut state = OverlayState::new();
        state.snapshot_valid = true;
        let units = crate::domain::ui_logic::target_units_from_fps(70.0);
        state.act(OverlayAction::SetTargetMode(2), UiMode::Full, &tx);
        state.act(OverlayAction::SlideTarget(units), UiMode::Full, &tx);
        let plan = state.act(OverlayAction::ApplyTarget, UiMode::Full, &tx);
        assert!(plan.status.is_none(), "an applied change reports nothing");
        assert_eq!(live.snapshot().target_fps, 70.0);
    }

    #[test]
    fn the_cadence_line_arrives_once_and_the_active_line_on_the_first_output() {
        let mut cadence = OutputCadence::new();
        let mut firsts = 0;
        for i in 0..=60 {
            if cadence.record(i * 1000, false, 0, 1_000_000).first_output {
                firsts += 1;
            }
        }
        assert_eq!(firsts, 1, "the link line is reported exactly once");
    }
}
