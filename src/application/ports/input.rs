//! Cursor ownership and hotkey events (ports `ui/ui_input.*` and the input half of `ui/ui.cpp`).
//!
//! Plain types only: no `windows` handles, no message constants. The driving adapter
//! (`adapters/driving/window_input.rs`) translates window messages into [`HotkeyEvent`]s and the
//! driven adapter (`adapters/driven/win32/iat.rs`) implements [`CursorGuard`].

/// A window identity, opaque to the application. `0` is "no window".
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub struct WindowHandle(pub isize);

impl WindowHandle {
    pub const NONE: Self = Self(0);
    pub const fn is_none(self) -> bool {
        self.0 == 0
    }
}

/// Keyboard modifiers held when a key message arrived.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Modifiers {
    pub ctrl: bool,
    pub alt: bool,
    pub shift: bool,
}

/// A parsed `ui.hotkey` value: modifiers plus one virtual-key code.
///
/// The default is `alt+f10`, matching the C++ `Hotkey` default and the shipped `sedici.toml`.
/// Parsing is case-insensitive on modifier names; unknown parts are ignored; a hotkey with no
/// modifier gains Alt, as the C++ does.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Hotkey {
    pub ctrl: bool,
    pub alt: bool,
    pub shift: bool,
    pub vk: u32,
}

/// `VK_F10`, the default key.
pub const VK_F10: u32 = 0x79;

impl Default for Hotkey {
    fn default() -> Self {
        Self {
            ctrl: false,
            alt: true,
            shift: false,
            vk: VK_F10,
        }
    }
}

fn eq_ignore_ascii_case(a: &str, b: &str) -> bool {
    a.len() == b.len()
        && a.bytes()
            .zip(b.bytes())
            .all(|(x, y)| x.eq_ignore_ascii_case(&y))
}

impl Hotkey {
    /// Parses the user-facing text (`"alt+f10"`, `"ctrl+shift+a"`). Worst case: the default.
    pub fn parse(text: &str) -> Self {
        let mut hotkey = Self {
            ctrl: false,
            alt: false,
            shift: false,
            vk: VK_F10,
        };
        for part in text.split('+') {
            if eq_ignore_ascii_case(part, "alt") {
                hotkey.alt = true;
            } else if eq_ignore_ascii_case(part, "ctrl") {
                hotkey.ctrl = true;
            } else if eq_ignore_ascii_case(part, "shift") {
                hotkey.shift = true;
            } else if part.len() >= 2 && (part.starts_with('f') || part.starts_with('F')) {
                let number: u32 = part[1..]
                    .bytes()
                    .filter(u8::is_ascii_digit)
                    .fold(0, |acc, d| acc * 10 + u32::from(d - b'0'));
                if (1..=12).contains(&number) {
                    hotkey.vk = 0x70 + number - 1; // VK_F1 .. VK_F12
                }
            } else if part.len() == 1 {
                let byte = part.as_bytes()[0];
                if byte.is_ascii_lowercase() {
                    hotkey.vk = u32::from(byte - b'a') + b'A' as u32;
                } else if byte.is_ascii_digit() {
                    hotkey.vk = u32::from(byte - b'0') + b'0' as u32;
                }
            }
        }
        if !hotkey.ctrl && !hotkey.alt && !hotkey.shift {
            hotkey.alt = true;
        }
        hotkey
    }

    /// True when the held modifiers are exactly the configured ones.
    pub fn matches(&self, modifiers: Modifiers) -> bool {
        modifiers.ctrl == self.ctrl && modifiers.alt == self.alt && modifiers.shift == self.shift
    }
}

/// Press or release.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum KeyAction {
    Pressed,
    Released,
}

/// One hotkey key message for the current window, already matched against the configured hotkey.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct HotkeyEvent {
    pub action: KeyAction,
    pub window: WindowHandle,
}

/// What the last interception scan found, for the log and tests (K132).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct InterceptionSummary {
    pub modules_scanned: u32,
    pub set_cursor_pos_slots: u32,
    pub clip_cursor_slots: u32,
    /// An import slot that held something other than the real call.
    pub skipped_slots: u32,
}

/// Full-menu cursor ownership: clip save/restore and suppression of the game's cursor calls.
///
/// Every method is non-failing: the C++ enters capture only when it can, and a failed enter is a
/// `false`, never a game error.
pub trait CursorGuard: Send + Sync {
    /// Takes ownership for `window`: saves the clip, hides it, clamps the cursor visibility.
    /// Returns `false` when the window is not foreground, not on this thread, or interception is
    /// unavailable.
    fn enter(&self, window: WindowHandle) -> bool;

    /// Releases ownership of the window that owns it.
    fn leave(&self, restore_clip: bool);

    /// Releases ownership only if `window` is the owner.
    fn leave_window(&self, window: WindowHandle, restore_clip: bool);

    /// Permanently stops suppression (overlay disabled).
    fn disable(&self);

    /// True while capture is held on the foreground window in Full mode.
    fn active(&self) -> bool;

    /// True when at least one cursor import was intercepted; rescans at most every two seconds.
    fn available(&self) -> bool;

    /// What the last scan found.
    fn summary(&self) -> InterceptionSummary;
}

/// Maps a client-area point to output pixels. The overlay draws at the output resolution while
/// Windows reports input in client coordinates, so the two only match when the window is not scaled.
/// Negative coordinates clamp to the near edge; points past the far edge clamp to the last pixel.
pub fn map_point(
    x: i32,
    y: i32,
    client_width: i32,
    client_height: i32,
    output_width: i32,
    output_height: i32,
) -> (i32, i32) {
    if client_width <= 0 || client_height <= 0 || output_width <= 0 || output_height <= 0 {
        return (x, y);
    }
    let map = |value: i32, client: i32, output: i32| {
        ((i64::from(value) * i64::from(output)) / i64::from(client)) as i32
    };
    let mut px = map(x, client_width, output_width);
    let mut py = map(y, client_height, output_height);
    px = px.clamp(0, output_width - 1);
    py = py.clamp(0, output_height - 1);
    (px, py)
}

#[cfg(test)]
mod tests {
    use super::*;
    use rstest::rstest;

    #[rstest]
    #[case("alt+f10", Hotkey { ctrl: false, alt: true, shift: false, vk: VK_F10 })]
    #[case("CTRL+SHIFT+a", Hotkey { ctrl: true, alt: false, shift: true, vk: b'A' as u32 })]
    #[case("CTRL+A", Hotkey { ctrl: true, alt: false, shift: false, vk: VK_F10 })] // uppercase key ignored, as in C++
    #[case("f2", Hotkey { ctrl: false, alt: true, shift: false, vk: 0x71 })]
    #[case("ctrl+9", Hotkey { ctrl: true, alt: false, shift: false, vk: b'9' as u32 })]
    #[case("", Hotkey::default())]
    #[case("nonsense", Hotkey::default())]
    fn parses_hotkey_text(#[case] text: &str, #[case] want: Hotkey) {
        assert_eq!(Hotkey::parse(text), want);
    }

    #[test]
    fn modifiers_must_match_exactly() {
        let hotkey = Hotkey::parse("alt+f10");
        assert!(hotkey.matches(Modifiers {
            alt: true,
            ..Modifiers::default()
        }));
        assert!(!hotkey.matches(Modifiers {
            alt: true,
            ctrl: true,
            ..Modifiers::default()
        }));
    }

    #[test]
    fn maps_and_clamps_points() {
        assert_eq!(map_point(-10, -20, 640, 360, 1280, 720), (0, 0));
        assert_eq!(map_point(320, 180, 640, 360, 1280, 720), (640, 360));
        assert_eq!(map_point(9999, 9999, 640, 360, 1280, 720), (1279, 719));
        assert_eq!(map_point(5, 6, 0, 360, 1280, 720), (5, 6));
    }
}
