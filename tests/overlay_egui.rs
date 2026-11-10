//! Isolated checks that the egui context built the way the overlay builds it produces triangles,
//! for both menus. No D3D12 and no window: if these fail, the wiring is wrong, not the renderer.
use amd_fidelityfx_framegeneration_dx12::adapters::driven::overlay_ui::{input, menu};
use amd_fidelityfx_framegeneration_dx12::application::overlay::{
    FullView, MiniView, OverlayView, ScaleChoice, TargetMode,
};
use amd_fidelityfx_framegeneration_dx12::domain::timing::TimingStats;

fn scaffold() -> (egui::Context, egui::Rect) {
    let context = egui::Context::default();
    context.set_visuals(egui::Visuals::dark());
    let screen = egui::Rect::from_min_size(egui::Pos2::ZERO, egui::vec2(640.0, 360.0));
    (context, screen)
}

fn full_view() -> FullView {
    FullView {
        scale: 1.0,
        frame_generation_on: true,
        render_size: (1280, 720),
        output_size: (640, 360),
        real_fps: 60.0,
        output_fps: 120.0,
        real_dropped: 0,
        stall_count: 0,
        last_stall_ms: 0.0,
        output_stats: TimingStats::default(),
        real_stats: TimingStats::default(),
        output_samples: vec![8.0, 9.0],
        real_samples: Vec::new(),
        edits_enabled: true,
        save_status: String::new(),
        provenance: Default::default(),
        target_mode: TargetMode::Fixed,
        pending_units: 73,
        pending_dirty: false,
        pending_fps: 36.5,
        limiter_fps: 36.5,
        output_refresh_hz: 75.0,
        log_level: 1,
        scale_choice: ScaleChoice(0),
        applied_scale: 1.0,
    }
}

/// egui emits a pass of `Noop` placeholders on the very first frame (area and window backgrounds are
/// reserved before their size is known and filled in at the end of the pass); from the second pass
/// on it produces real geometry. The overlay therefore never draws the warm-up pass's empty
/// primitive list, and this test asserts that a later pass holds triangles.
fn triangles_after_warmup(view: &OverlayView) -> usize {
    let (context, screen) = scaffold();
    let mut last = 0;
    for _ in 0..3 {
        let raw = input::raw_input(&[], screen, 640, 360, 640, 360, Some(1.0 / 60.0));
        context.set_zoom_factor(1.0);
        let output = context.run(raw, |ctx| {
            let _ = menu::show(ctx, view);
        });
        let primitives = context.tessellate(output.shapes, 1.0);
        last = primitives
            .iter()
            .filter(|p| matches!(p.primitive, egui::epaint::Primitive::Mesh(_)))
            .count();
    }
    last
}

#[test]
fn the_mini_menu_tessellates_after_the_warmup_pass() {
    let view = OverlayView::Mini(MiniView {
        real_fps: 60.0,
        output_fps: 120.0,
        output_p99_ms: 9.0,
    });
    assert!(
        triangles_after_warmup(&view) > 0,
        "a Mini menu must produce triangles"
    );
}

#[test]
fn the_full_menu_tessellates_after_the_warmup_pass() {
    let view = OverlayView::Full(full_view());
    assert!(
        triangles_after_warmup(&view) > 0,
        "a Full menu must produce triangles"
    );
}
