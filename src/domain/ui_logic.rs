//! Logic behind the Full menu's controls, free of any UI toolkit.

/// FR-04: menu scale choices (besides Auto).
pub const SCALE_CHOICES: [f32; 3] = [0.8, 0.9, 1.0];

/// Index of the choice closest to `value`; an older file's 0.5 or 1.75 snaps to the nearest.
pub fn nearest_scale_choice(value: f32) -> usize {
    let mut best = 0;
    let mut best_distance = 1e9_f32;
    for (i, choice) in SCALE_CHOICES.iter().enumerate() {
        let distance = (value - choice).abs();
        if distance < best_distance {
            best_distance = distance;
            best = i;
        }
    }
    best
}

/// FR-01: the target slider works on whole steps of half a frame per second (K118).
pub const TARGET_MIN_UNITS: i32 = 40; // 20 fps
pub const TARGET_MAX_UNITS: i32 = 480; // 240 fps

pub fn target_units_from_fps(fps: f64) -> i32 {
    if !fps.is_finite() {
        return TARGET_MIN_UNITS;
    }
    ((fps * 2.0).round() as i64).clamp(i64::from(TARGET_MIN_UNITS), i64::from(TARGET_MAX_UNITS)) as i32
}

pub fn target_fps_from_units(units: i32) -> f64 {
    f64::from(units.clamp(TARGET_MIN_UNITS, TARGET_MAX_UNITS)) / 2.0
}

/// The Full menu edits a pending value and applies it with one button (FR-02). `units` follows the
/// live value until the user touches the slider.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PendingTarget {
    pub units: i32,
    pub dirty: bool,
}

impl Default for PendingTarget {
    fn default() -> Self {
        Self { units: 73, dirty: false } // 36.5 fps
    }
}

impl PendingTarget {
    pub fn follow_live(&mut self, live_fps: f64) {
        if !self.dirty && live_fps > 0.0 {
            self.units = target_units_from_fps(live_fps);
        }
    }
    pub fn edit(&mut self, units: i32) {
        self.units = units.clamp(TARGET_MIN_UNITS, TARGET_MAX_UNITS);
        self.dirty = true;
    }
    /// The value to apply; clears the pending state.
    pub fn apply(&mut self) -> f64 {
        self.dirty = false;
        target_fps_from_units(self.units)
    }
}

/// K126: Alt+F4 must reach the game in every menu mode. `lparam` bit 29 is set while Alt is held.
pub fn ui_passes_to_game(msg: u32, wparam: u64, lparam: i64) -> bool {
    const WM_SYSKEYDOWN: u32 = 0x0104;
    const WM_SYSKEYUP: u32 = 0x0105;
    const VK_F4: u64 = 0x73;
    (msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP) && wparam == VK_F4 && ((lparam as u64) >> 29) & 1 != 0
}

#[cfg(test)]
mod tests {
    use super::*;
    use rstest::rstest;

    #[rstest]
    #[case(0.5, 0)]
    #[case(0.86, 1)]
    #[case(1.75, 2)]
    fn snaps_scale(#[case] value: f32, #[case] index: usize) {
        assert_eq!(nearest_scale_choice(value), index);
    }

    #[rstest]
    #[case(36.5, 73)]
    #[case(1.0, TARGET_MIN_UNITS)]
    #[case(999.0, TARGET_MAX_UNITS)]
    #[case(f64::NAN, TARGET_MIN_UNITS)]
    fn target_units(#[case] fps: f64, #[case] units: i32) {
        assert_eq!(target_units_from_fps(fps), units);
    }

    #[test]
    fn pending_follows_live_until_edited() {
        let mut p = PendingTarget::default();
        p.follow_live(60.0);
        assert_eq!(p.units, 120);
        p.edit(100);
        p.follow_live(30.0);
        assert_eq!(p.apply(), 50.0);
        assert!(!p.dirty);
    }

    #[test]
    fn alt_f4_passes_through() {
        assert!(ui_passes_to_game(0x0104, 0x73, 1 << 29));
        assert!(!ui_passes_to_game(0x0100, 0x73, 1 << 29));
        assert!(!ui_passes_to_game(0x0104, 0x73, 0));
    }
}
