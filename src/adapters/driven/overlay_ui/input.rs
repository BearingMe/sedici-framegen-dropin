//! Window messages to egui events (ports `drain_messages` in `ui/ui.cpp`).
//!
//! The Full menu owns the cursor while it is open, so the driving adapter consumed the messages and
//! hands them here. Coordinates arrive in **client** pixels; the overlay draws at the **output**
//! resolution, so each point is mapped through `application::ports::input::map_point` first (the C++
//! `ui_input_map_point`). Alt+F4 is delivered to the game instead (K126): the driving adapter never
//! hands us one, and this module refuses one defensively.
//!
//! Buttons carry a position, but egui reacts with the last known pointer, so a button message is
//! preceded by a `PointerMoved` to the same point: the C++ did exactly that with an extra
//! `WM_MOUSEMOVE` before every button message.
use egui::{Event, Key, Modifiers, PointerButton, Pos2, RawInput, Vec2};

use crate::application::ports::input::map_point;
use crate::application::ports::ui_renderer::UiInput;

const WM_MOUSEMOVE: u32 = 0x0200;
const WM_LBUTTONDOWN: u32 = 0x0201;
const WM_LBUTTONUP: u32 = 0x0202;
const WM_LBUTTONDBLCLK: u32 = 0x0203;
const WM_RBUTTONDOWN: u32 = 0x0204;
const WM_RBUTTONUP: u32 = 0x0205;
const WM_MBUTTONDOWN: u32 = 0x0207;
const WM_MBUTTONUP: u32 = 0x0208;
const WM_MOUSEWHEEL: u32 = 0x020A;
const WM_MOUSEHWHEEL: u32 = 0x020E;
const WM_KEYDOWN: u32 = 0x0100;
const WM_KEYUP: u32 = 0x0101;
const WM_SYSKEYDOWN: u32 = 0x0104;
const WM_SYSKEYUP: u32 = 0x0105;
const WM_CHAR: u32 = 0x0102;
const WM_SYSCHAR: u32 = 0x0106;

/// Builds one pass's [`RawInput`] from the consumed messages.
#[allow(clippy::too_many_arguments)]
pub fn raw_input(
    input: &[UiInput],
    screen_rect: egui::Rect,
    client_width: i32,
    client_height: i32,
    output_width: u32,
    output_height: u32,
    delta_seconds: Option<f32>,
) -> RawInput {
    let mut events = Vec::new();
    for message in input {
        if crate::domain::ui_logic::ui_passes_to_game(message.msg, message.wparam, message.lparam) {
            continue;
        }
        translate(
            message,
            client_width,
            client_height,
            output_width,
            output_height,
            &mut events,
        );
    }
    RawInput {
        screen_rect: Some(screen_rect),
        time: Some(0.0),
        predicted_dt: delta_seconds.unwrap_or(1.0 / 60.0).clamp(0.001, 0.1),
        events,
        focused: true,
        ..RawInput::default()
    }
}

/// Adds the events for one message. Exposed for tests.
pub fn translate(
    message: &UiInput,
    client_width: i32,
    client_height: i32,
    output_width: u32,
    output_height: u32,
    events: &mut Vec<Event>,
) {
    let point = |lparam: i64| -> Pos2 {
        let x = (lparam & 0xFFFF) as u16 as i16 as i32;
        let y = ((lparam >> 16) & 0xFFFF) as u16 as i16 as i32;
        let (px, py) = map_point(
            x,
            y,
            client_width,
            client_height,
            output_width as i32,
            output_height as i32,
        );
        Pos2::new(px as f32, py as f32)
    };

    match message.msg {
        WM_MOUSEMOVE => events.push(Event::PointerMoved(point(message.lparam))),
        WM_LBUTTONDOWN | WM_LBUTTONDBLCLK => {
            let position = point(message.lparam);
            // Feed the position first so a click lands where the cursor visibly is, as the C++ did.
            events.push(Event::PointerMoved(position));
            events.push(pointer_button(position, PointerButton::Primary, true));
        }
        WM_LBUTTONUP => {
            let position = point(message.lparam);
            events.push(Event::PointerMoved(position));
            events.push(pointer_button(position, PointerButton::Primary, false));
        }
        WM_RBUTTONDOWN => {
            let position = point(message.lparam);
            events.push(Event::PointerMoved(position));
            events.push(pointer_button(position, PointerButton::Secondary, true));
        }
        WM_RBUTTONUP => {
            let position = point(message.lparam);
            events.push(Event::PointerMoved(position));
            events.push(pointer_button(position, PointerButton::Secondary, false));
        }
        WM_MBUTTONDOWN => {
            let position = point(message.lparam);
            events.push(Event::PointerMoved(position));
            events.push(pointer_button(position, PointerButton::Middle, true));
        }
        WM_MBUTTONUP => {
            let position = point(message.lparam);
            events.push(Event::PointerMoved(position));
            events.push(pointer_button(position, PointerButton::Middle, false));
        }
        WM_MOUSEWHEEL | WM_MOUSEHWHEEL => {
            // The delta is in the high word of wparam, signed, in wheel units of 120. egui wants
            // points; one wheel notch scrolls the same as about 50 points.
            let raw = ((message.wparam >> 16) & 0xFFFF) as u16 as i16 as i32;
            let lines = f32::from(raw as i16) / 120.0;
            let points = if message.msg == WM_MOUSEWHEEL {
                Vec2::new(0.0, -lines * 50.0)
            } else {
                Vec2::new(lines * 50.0, 0.0)
            };
            events.push(Event::MouseWheel {
                unit: egui::MouseWheelUnit::Point,
                delta: points,
                modifiers: Modifiers::default(),
            });
        }
        WM_KEYDOWN | WM_SYSKEYDOWN | WM_KEYUP | WM_SYSKEYUP => {
            let vk = message.wparam as u32;
            let pressed = message.msg == WM_KEYDOWN || message.msg == WM_SYSKEYDOWN;
            if let Some(key) = key_for(vk) {
                events.push(Event::Key {
                    key,
                    physical_key: Some(key),
                    pressed,
                    repeat: false,
                    modifiers: Modifiers::default(),
                });
                if pressed && key == Key::Enter {
                    events.push(Event::Text("\n".to_owned()));
                }
            }
        }
        WM_CHAR | WM_SYSCHAR => {
            if let Some(ch) = char::from_u32(message.wparam as u32) {
                if !ch.is_control() {
                    events.push(Event::Text(ch.to_string()));
                }
            }
        }
        _ => {}
    }
}

fn pointer_button(pos: Pos2, button: PointerButton, pressed: bool) -> Event {
    Event::PointerButton { pos, button, pressed, modifiers: Modifiers::default() }
}

/// The egui key for a Win32 virtual-key code. Only the keys a menu needs; anything else is ignored
/// so the game's own binding never reaches egui.
pub fn key_for(vk: u32) -> Option<Key> {
    let key = match vk {
        0x0D => Key::Enter,
        0x1B => Key::Escape,
        0x20 => Key::Space,
        0x09 => Key::Tab,
        0x25 => Key::ArrowLeft,
        0x26 => Key::ArrowUp,
        0x27 => Key::ArrowRight,
        0x28 => Key::ArrowDown,
        0x08 => Key::Backspace,
        0x2E => Key::Delete,
        0x24 => Key::Home,
        0x23 => Key::End,
        0x21 => Key::PageUp,
        0x22 => Key::PageDown,
        0x41..=0x5A => letter_key(vk),
        0x30..=0x39 => digit_key(vk),
        _ => return None,
    };
    Some(key)
}

fn letter_key(vk: u32) -> Key {
    // The `Key` enum exposes letters as `Key::A` .. `Key::Z`.
    const LETTERS: [Key; 26] = [
        Key::A, Key::B, Key::C, Key::D, Key::E, Key::F, Key::G, Key::H, Key::I, Key::J, Key::K,
        Key::L, Key::M, Key::N, Key::O, Key::P, Key::Q, Key::R, Key::S, Key::T, Key::U, Key::V,
        Key::W, Key::X, Key::Y, Key::Z,
    ];
    LETTERS[(vk - 0x41) as usize]
}

fn digit_key(vk: u32) -> Key {
    const DIGITS: [Key; 10] = [
        Key::Num0, Key::Num1, Key::Num2, Key::Num3, Key::Num4, Key::Num5, Key::Num6, Key::Num7,
        Key::Num8, Key::Num9,
    ];
    DIGITS[(vk - 0x30) as usize]
}

#[cfg(test)]
mod tests {
    use super::*;
    use rstest::rstest;

    #[test]
    fn a_mouse_move_maps_client_to_output_pixels() {
        let mut events = Vec::new();
        // Client point (320, 180) in a 640x360 client on a 1280x720 output.
        let lparam = ((180i64) << 16) | 320;
        translate(&UiInput { msg: WM_MOUSEMOVE, wparam: 0, lparam }, 640, 360, 1280, 720, &mut events);
        assert_eq!(events, vec![Event::PointerMoved(Pos2::new(640.0, 360.0))]);
    }

    #[test]
    fn a_click_feeds_the_position_first() {
        let mut events = Vec::new();
        let lparam = ((10i64) << 16) | 20;
        translate(&UiInput { msg: WM_LBUTTONDOWN, wparam: 0, lparam }, 640, 360, 1280, 720, &mut events);
        assert_eq!(events.len(), 2, "{events:?}");
        assert!(matches!(events[0], Event::PointerMoved(_)));
        assert!(matches!(
            events[1],
            Event::PointerButton { pressed: true, button: PointerButton::Primary, .. }
        ));
    }

    #[test]
    fn alt_f4_never_reaches_egui() {
        let mut events = Vec::new();
        // WM_SYSKEYDOWN with VK_F4 and lparam bit 29 set (Alt held).
        translate(&UiInput { msg: WM_SYSKEYDOWN, wparam: 0x73, lparam: 1 << 29 }, 640, 360, 1280, 720, &mut events);
        // `translate` itself translates; the pass-through filter sits in `raw_input`. A direct call
        // still maps F4 to nothing, because `key_for` only covers the menu's keys.
        assert!(events.is_empty(), "{events:?}");
    }

    #[test]
    fn raw_input_filters_the_passthrough_key() {
        let raw = raw_input(
            &[UiInput { msg: WM_SYSKEYDOWN, wparam: 0x73, lparam: 1 << 29 }],
            egui::Rect::from_min_size(egui::Pos2::ZERO, egui::vec2(1280.0, 720.0)),
            640,
            360,
            1280,
            720,
            Some(0.016),
        );
        assert!(raw.events.is_empty());
    }

    #[test]
    fn typing_a_character_becomes_text_but_a_control_code_does_not() {
        let mut events = Vec::new();
        translate(&UiInput { msg: WM_CHAR, wparam: u64::from(b'a'), lparam: 0 }, 0, 0, 0, 0, &mut events);
        assert_eq!(events, vec![Event::Text("a".to_owned())]);
        events.clear();
        translate(&UiInput { msg: WM_CHAR, wparam: 13, lparam: 0 }, 0, 0, 0, 0, &mut events);
        assert!(events.is_empty());
    }

    #[rstest]
    #[case(0x0D, Some(Key::Enter))]
    #[case(0x09, Some(Key::Tab))]
    #[case(0x41, Some(Key::A))]
    #[case(0x5A, Some(Key::Z))]
    #[case(0x30, Some(Key::Num0))]
    #[case(0x73, None)] // F4 is not a menu key
    #[case(0x0F, None)] // Tab is 0x09; 0x0F is not mapped
    fn keys_map_to_the_menu_only(#[case] vk: u32, #[case] want: Option<Key>) {
        assert_eq!(key_for(vk), want);
    }

    #[test]
    fn wheel_units_become_points() {
        let mut events = Vec::new();
        // One notch down: high word 120 (0xFF88 would be up).
        let wparam = 120u64 << 16;
        translate(&UiInput { msg: WM_MOUSEWHEEL, wparam, lparam: 0 }, 0, 0, 0, 0, &mut events);
        match &events[0] {
            Event::MouseWheel { unit, delta, .. } => {
                assert_eq!(*unit, egui::MouseWheelUnit::Point);
                assert!((delta.y + 50.0).abs() < 1e-3, "{delta:?}");
            }
            other => panic!("{other:?}"),
        }
    }
}
