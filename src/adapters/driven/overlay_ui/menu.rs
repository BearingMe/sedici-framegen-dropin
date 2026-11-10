//! The Mini and Full menus, drawn with egui (ports `draw_mini`/`draw_full` of `ui/ui.cpp`).
//!
//! Every egui call for the overlay lives here. The application supplies an
//! [`OverlayView`](crate::application::overlay::OverlayView) of plain values and this module turns
//! each interaction into one [`OverlayAction`] for the caller to apply. That keeps the layout in
//! one readable place without letting egui leak into the application layer.
//!
//! # Layout parity with the C++
//!
//! - Mini: top-left at 12 points, 65 % background, one line `real / out / p99`, and no input
//!   (`NoInputs` then; an interactable `false` area now).
//! - Full: a "Sedici" window 560x690 points at scale 1.0, not collapsible, with the same groups in
//!   the same order: Status, Frame times (two graphs), Target FPS (mode combo, half-fps slider,
//!   Apply, provenance note, limiter line), Logging, Menu (scale combo and effective scale), then
//!   Save / Cancel / Close and the save status.
//!
//! Scale: the C++ scaled every style size by `effective_scale`. egui has one zoom factor for the
//! whole context, which the adapter sets from the same value, so this module never scales itself.
use egui::{Align2, Color32, Order, RichText, Sense, Stroke, Vec2};

use crate::application::overlay::{
    FullView, MiniView, OverlayAction, OverlayView, ScaleChoice, TargetMode, FULL_DEFAULT_SIZE,
    GRAPH_HEIGHT, GRAPH_MAX_MS, MINI_BACKGROUND_ALPHA, MINI_MARGIN,
};

/// The Mini window's egui id. Stable so egui remembers its position between frames.
const MINI_ID: &str = "sedici-mini";
/// The Full window's title, as the C++ `ImGui::Begin("Sedici")`.
const FULL_TITLE: &str = "Sedici";

/// Draws one view and returns the actions the user asked for, in click order.
pub fn show(ctx: &egui::Context, view: &OverlayView) -> Vec<OverlayAction> {
    let mut actions = Vec::new();
    match view {
        OverlayView::Off => {}
        OverlayView::Mini(mini) => show_mini(ctx, mini),
        OverlayView::Full(full) => show_full(ctx, full, &mut actions),
    }
    actions
}

/// The Mini menu: a fixed top-left panel that never takes input, so the game keeps the mouse.
fn show_mini(ctx: &egui::Context, mini: &MiniView) {
    let fill = Color32::from_black_alpha((MINI_BACKGROUND_ALPHA * 255.0).round() as u8);
    egui::Area::new(egui::Id::new(MINI_ID))
        .order(Order::Foreground)
        .fixed_pos(egui::pos2(MINI_MARGIN, MINI_MARGIN))
        .interactable(false)
        .show(ctx, |ui| {
            // egui's own panel frame would be opaque; draw the C++ 65 % black instead.
            egui::Frame::NONE
                .fill(fill)
                .inner_margin(egui::Margin::same(2))
                .show(ui, |ui| {
                    ui.label(RichText::new(mini.line()).color(Color32::WHITE));
                });
        });
}

/// The Full menu. Every control is disabled while an edit baseline is missing or a save is running;
/// Close stays available, as in the C++ where it sat outside `BeginDisabled`.
fn show_full(ctx: &egui::Context, full: &FullView, actions: &mut Vec<OverlayAction>) {
    let scale = if full.scale > 0.0 { full.scale } else { 1.0 };
    let default_size = Vec2::new(FULL_DEFAULT_SIZE.0, FULL_DEFAULT_SIZE.1) * scale;
    egui::Window::new(FULL_TITLE)
        .id(egui::Id::new("sedici-full"))
        .anchor(Align2::LEFT_TOP, Vec2::new(MINI_MARGIN, MINI_MARGIN))
        .default_size(default_size)
        .resizable(true)
        .collapsible(false)
        .show(ctx, |ui| {
            ui.set_max_width(default_size.x.max(320.0));
            status_group(ui, full);
            frame_time_group(ui, full, scale);
            let mut save = None;
            let mut cancel = None;
            let mut close = None;
            ui.add_enabled_ui(full.edits_enabled, |ui| {
                target_group(ui, full, actions);
                logging_group(ui, full, actions);
                menu_group(ui, full, actions);
                ui.separator();
                ui.horizontal(|ui| {
                    if ui.button("Save").clicked() {
                        save = Some(());
                    }
                    if ui.button("Cancel").clicked() {
                        cancel = Some(());
                    }
                    // Close is outside the disabled group: closing the menu is always possible.
                    if ui.button("Close").clicked() {
                        close = Some(());
                    }
                });
            });
            if save.is_some() {
                actions.push(OverlayAction::Save);
            }
            if cancel.is_some() {
                actions.push(OverlayAction::Cancel);
            }
            if close.is_some() {
                actions.push(OverlayAction::Close);
            }
            if !full.save_status.is_empty() {
                ui.label(&full.save_status);
            }
        });
}

fn status_group(ui: &mut egui::Ui, full: &FullView) {
    ui.separator();
    ui.strong("Status");
    ui.label(format!(
        "Frame generation: {}",
        if full.frame_generation_on { "on" } else { "off" }
    ));
    let (rw, rh) = full.render_size;
    let (ow, oh) = full.output_size;
    if rw != 0 && rh != 0 {
        ui.label(format!("Render {rw}x{rh}  ->  output {ow}x{oh}"));
    } else {
        ui.label(format!("Output {ow}x{oh}"));
    }
    ui.label(format!(
        "Real shown {:.1} fps  |  Output {:.1} fps",
        full.real_fps, full.output_fps
    ));
    ui.label(format!(
        "Real frames dropped: {} (not the game's count; the game renders shown + dropped)",
        full.real_dropped
    ));
    ui.label(format!(
        "Stalls: {}  |  last {:.1} ms",
        full.stall_count, full.last_stall_ms
    ));
}

fn frame_time_group(ui: &mut egui::Ui, full: &FullView, scale: f32) {
    ui.separator();
    ui.strong("Frame times");
    ui.label(format!(
        "Output  p50 {:.2}  p99 {:.2}  max {:.2} ms",
        full.output_stats.p50_ms, full.output_stats.p99_ms, full.output_stats.max_ms
    ));
    graph(ui, "##output_graph", &full.output_samples, scale);
    ui.label(format!(
        "Real    p50 {:.2}  p99 {:.2}  max {:.2} ms",
        full.real_stats.p50_ms, full.real_stats.p99_ms, full.real_stats.max_ms
    ));
    graph(ui, "##real_graph", &full.real_samples, scale);
}

/// One interval graph. `PlotLines` in the C++ drew the newest `count` samples across the width with
/// a fixed 0..=40 ms scale; egui has no plot widget here, so the same points are drawn as a polyline
/// in an allocated, non-interactive strip. Empty rings say `waiting for samples`, as the C++ did.
fn graph(ui: &mut egui::Ui, label: &str, samples: &[f32], scale: f32) {
    if samples.is_empty() {
        ui.weak(format!("{label}: waiting for samples"));
        return;
    }
    let height = GRAPH_HEIGHT * scale;
    let width = ui.available_width().max(1.0);
    let (response, painter) = ui.allocate_painter(Vec2::new(width, height), Sense::hover());
    let rect = response.rect;
    let frame = Color32::from_gray(48);
    painter.rect_filled(rect, 0.0, Color32::from_gray(24));
    painter.rect_stroke(
        rect,
        0.0,
        Stroke::new(1.0_f32, frame),
        egui::StrokeKind::Inside,
    );
    // A 40 ms vertical span over the strip; values above it clip to the top.
    let to_point = |index: usize, ms: f32| {
        let x = if samples.len() > 1 {
            index as f32 / (samples.len() - 1) as f32
        } else {
            0.0
        };
        let y = (ms / GRAPH_MAX_MS).clamp(0.0, 1.0);
        egui::pos2(rect.left() + x * rect.width(), rect.bottom() - y * rect.height())
    };
    let points: Vec<egui::Pos2> = samples.iter().copied().enumerate().map(|(i, ms)| to_point(i, ms)).collect();
    if points.len() > 1 {
        painter.add(egui::Shape::line(points, Stroke::new(1.0_f32, Color32::from_rgb(120, 190, 255))));
    } else if let Some(&first) = points.first() {
        painter.circle_filled(first, 1.0, Color32::from_rgb(120, 190, 255));
    }
}

fn target_group(ui: &mut egui::Ui, full: &FullView, actions: &mut Vec<OverlayAction>) {
    ui.separator();
    ui.strong("Target FPS");
    // A target overridden from the environment cannot be edited (K92); the combo is disabled and the
    // menu names the variable that owns it.
    ui.add_enabled_ui(!full.provenance.target_env, |ui| {
        let mut mode = full.target_mode;
        egui::ComboBox::from_id_salt("sedici_target_mode")
            .selected_text(TargetMode::ITEMS[mode_index(mode)])
            .show_ui(ui, |ui| {
                for (index, label) in TargetMode::ITEMS.iter().enumerate() {
                    let value = mode_from_index(index);
                    if ui.selectable_value(&mut mode, value, *label).clicked() {
                        actions.push(OverlayAction::SetTargetMode(index as u8));
                    }
                }
            });
        if full.target_mode == TargetMode::Fixed {
            // Whole half-fps steps (FR-01, K118): an integer slider that cannot land between steps.
            let (min, max) = crate::application::overlay::target_units_range();
            let mut units = full.pending_units;
            let slider = egui::Slider::new(&mut units, min..=max)
                .show_value(false)
                .integer();
            if ui.add(slider).changed() {
                actions.push(OverlayAction::SlideTarget(units));
            }
            ui.horizontal(|ui| {
                let text = if full.pending_dirty {
                    format!("{:.1} fps (not applied)", full.pending_fps)
                } else {
                    format!("{:.1} fps", full.pending_fps)
                };
                ui.label(text);
                let apply = ui.add_enabled(full.pending_dirty, egui::Button::new("Apply"));
                if apply.clicked() {
                    actions.push(OverlayAction::ApplyTarget);
                }
            });
        }
    });
    if full.provenance.target_env {
        ui.weak("overridden by SEDICI_FG_TARGET_FPS or SEDICI_LIMIT_FPS");
    }
    if full.limiter_fps > 0.0 {
        ui.weak(format!(
            "Real frames capped at {:.1} fps (display {:.0} Hz)",
            full.limiter_fps, full.output_refresh_hz
        ));
    } else {
        ui.weak("Uncapped");
    }
}

fn logging_group(ui: &mut egui::Ui, full: &FullView, actions: &mut Vec<OverlayAction>) {
    ui.separator();
    ui.strong("Logging");
    const LEVELS: [&str; 3] = ["errors", "normal", "verbose"];
    ui.add_enabled_ui(!full.provenance.log_env, |ui| {
        let mut level = full.log_level;
        egui::ComboBox::from_id_salt("sedici_log_level")
            .selected_text(LEVELS[level.clamp(0, 2) as usize])
            .show_ui(ui, |ui| {
                for (index, label) in LEVELS.iter().enumerate() {
                    if ui
                        .selectable_value(&mut level, index as i32, *label)
                        .clicked()
                    {
                        actions.push(OverlayAction::SetLogLevel(index as i32));
                    }
                }
            });
    });
    if full.provenance.log_env {
        ui.weak("overridden by MFSRFG_LOG");
    }
}

fn menu_group(ui: &mut egui::Ui, full: &FullView, actions: &mut Vec<OverlayAction>) {
    ui.separator();
    ui.strong("Menu");
    let mut choice = full.scale_choice.0.min(ScaleChoice::ITEMS.len() - 1);
    egui::ComboBox::from_id_salt("sedici_menu_scale")
        .selected_text(ScaleChoice::ITEMS[choice])
        .show_ui(ui, |ui| {
            for (index, label) in ScaleChoice::ITEMS.iter().enumerate() {
                if ui.selectable_value(&mut choice, index, *label).clicked() {
                    actions.push(OverlayAction::SetScaleChoice(index));
                }
            }
        });
    ui.label(format!("Effective {:.2}x", full.applied_scale));
}

fn mode_index(mode: TargetMode) -> usize {
    match mode {
        TargetMode::Uncapped => 0,
        TargetMode::Auto => 1,
        TargetMode::Fixed => 2,
    }
}

fn mode_from_index(index: usize) -> TargetMode {
    match index {
        1 => TargetMode::Auto,
        2 => TargetMode::Fixed,
        _ => TargetMode::Uncapped,
    }
}

/// The key the Full menu must let through to the game: Alt+F4 is the game's close shortcut, never a
/// menu one (K126). The adapter checks `domain::ui_logic::ui_passes_to_game` before feeding a key to
/// egui; this helper exists so the rule has one home next to the menu that must honour it.
pub fn delivers_to_game(msg: u32, wparam: u64, lparam: i64) -> bool {
    crate::domain::ui_logic::ui_passes_to_game(msg, wparam, lparam)
}

/// The Full window's inner rectangle at the current pixels-per-point, for the adapter's hit-testing
/// of the input mapping. Falls back to the whole output while the window has not been shown yet.
pub fn full_bounds(ctx: &egui::Context) -> egui::Rect {
    ctx.memory(|memory| memory.area_rect(egui::Id::new("sedici-full")))
        .unwrap_or_else(|| ctx.content_rect())
}
