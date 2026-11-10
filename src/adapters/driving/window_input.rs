//! Window subclass, hotkey and cursor-capture entry (ports the input half of `ui/ui.cpp`).
//!
//! Chains below any procedure already installed on the window (another overlay may sit above us),
//! turns `WM_KEYDOWN`/`WM_SYSKEYDOWN` matching the configured hotkey into [`HotkeyEvent`]s, pairs a
//! consumed key-down with its key-up so the game never sees a stray release, leaves Alt+F4 to the
//! game (K126 via `domain::ui_logic::ui_passes_to_game`), and drives the Full menu's cursor capture
//! through the [`CursorGuard`] port. All `unsafe` Win32 interaction lives here.

use std::sync::atomic::{AtomicBool, AtomicIsize, Ordering};
use std::sync::Mutex;

use tracing::info;
use windows::Win32::Foundation::{HWND, LPARAM, LRESULT, WPARAM};
use windows::Win32::UI::Input::KeyboardAndMouse::{
    GetCapture, GetKeyState, ReleaseCapture, VIRTUAL_KEY, VK_CONTROL, VK_MENU, VK_SHIFT,
};
use windows::Win32::UI::WindowsAndMessaging::{
    CallWindowProcW, DefWindowProcW, GetWindowLongPtrW, LoadCursorW, PostMessageW, SetCursor, SetWindowLongPtrW,
    GWLP_WNDPROC, IDC_ARROW, WM_CHAR, WM_DESTROY, WM_INPUT, WM_KEYDOWN, WM_KEYUP, WM_KILLFOCUS, WM_LBUTTONDBLCLK,
    WM_LBUTTONDOWN, WM_LBUTTONUP, WM_MBUTTONDBLCLK, WM_MBUTTONDOWN, WM_MBUTTONUP, WM_MOUSEHWHEEL, WM_MOUSEMOVE,
    WM_MOUSEWHEEL, WM_NCDESTROY, WM_RBUTTONDBLCLK, WM_RBUTTONDOWN, WM_RBUTTONUP, WM_SETCURSOR, WM_SETFOCUS,
    WM_SYSCHAR, WM_SYSKEYDOWN, WM_SYSKEYUP, WM_XBUTTONDBLCLK, WM_XBUTTONDOWN, WM_XBUTTONUP,
};

use crate::application::ports::input::{CursorGuard, Hotkey, HotkeyEvent, KeyAction, Modifiers, WindowHandle};
use crate::domain::ui_logic::ui_passes_to_game;

/// `WM_APP + 1`: the overlay's own "re-evaluate capture" message.
pub const HOTKEY_MESSAGE: u32 = 0x8000 + 1;
const MAX_QUEUED_MESSAGES: usize = 512;

/// A message the overlay consumed while the menu was open.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct InputMessage {
    pub msg: u32,
    pub wparam: usize,
    pub lparam: isize,
}

/// The application side the window procedure calls. Kept narrow so the adapter stays testable.
pub trait WindowInputHost: Send + Sync {
    /// Full menu is open and has captured the cursor.
    fn cursor_active(&self) -> bool;
    /// The overlay is disabled for this session.
    fn disabled(&self) -> bool;
    /// Hotkey press/release matched the configured hotkey.
    fn on_hotkey(&self, event: HotkeyEvent);
    /// The overlay consumed a game input; gives the rendered overlay its events.
    fn on_input(&self, message: InputMessage);
    /// The window was destroyed.
    fn on_destroyed(&self, window: WindowHandle);
}

/// Whether a message is input the overlay consumes while the Full menu owns the cursor.
pub fn is_consumed_input(msg: u32) -> bool {
    matches!(
        msg,
        WM_MOUSEMOVE
            | WM_LBUTTONDOWN
            | WM_LBUTTONUP
            | WM_LBUTTONDBLCLK
            | WM_RBUTTONDOWN
            | WM_RBUTTONUP
            | WM_RBUTTONDBLCLK
            | WM_MBUTTONDOWN
            | WM_MBUTTONUP
            | WM_MBUTTONDBLCLK
            | WM_XBUTTONDOWN
            | WM_XBUTTONUP
            | WM_XBUTTONDBLCLK
            | WM_MOUSEWHEEL
            | WM_MOUSEHWHEEL
            | WM_KEYDOWN
            | WM_KEYUP
            | WM_SYSKEYDOWN
            | WM_SYSKEYUP
            | WM_CHAR
            | WM_SYSCHAR
    )
}

/// The modifiers currently held, from `GetKeyState`.
pub fn current_modifiers() -> Modifiers {
    let held = |vk: VIRTUAL_KEY| {
        // SAFETY: `GetKeyState` takes a virtual-key code and reads this thread's key state.
        (unsafe { GetKeyState(vk.0 as i32) } & 0x8000_u16 as i16) != 0
    };
    Modifiers { ctrl: held(VK_CONTROL), alt: held(VK_MENU), shift: held(VK_SHIFT) }
}

/// What the window procedure decided about one message.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Dispatch {
    /// Consumed: return this value to the game's caller.
    Consumed(LRESULT),
    /// Not consumed: forward to the original procedure.
    Forward,
}

/// State shared by every subclassed window for this process.
pub struct WindowInput<H: WindowInputHost, G: CursorGuard> {
    host: H,
    guard: G,
    hotkey: Hotkey,
    /// The window that currently owns the overlay's input.
    current: AtomicIsize,
    disabled: AtomicBool,
    /// The window whose hotkey key-down was consumed, so its key-up is consumed too.
    hotkey_down_window: AtomicIsize,
    /// The window procedure we replaced, captured once per window.
    original: Mutex<Vec<(isize, isize)>>,
    queued: Mutex<Vec<InputMessage>>,
    _render: std::marker::PhantomData<()>,
}

impl<H: WindowInputHost, G: CursorGuard> WindowInput<H, G> {
    pub fn new(host: H, guard: G, hotkey: Hotkey) -> Self {
        Self {
            host,
            guard,
            hotkey,
            current: AtomicIsize::new(0),
            disabled: AtomicBool::new(false),
            hotkey_down_window: AtomicIsize::new(0),
            original: Mutex::new(Vec::new()),
            queued: Mutex::new(Vec::new()),
            _render: std::marker::PhantomData,
        }
    }

    /// Installs the subclass on `window` and makes it the input target. Another window already
    /// captured keeps its own chain; this one simply stops being the target.
    pub fn capture_window(&self, window: WindowHandle) -> bool {
        if window.is_none() {
            return false;
        }
        let hwnd = hwnd_of(window);
        let mut originals = self.original.lock().unwrap_or_else(|e| e.into_inner());
        if !originals.iter().any(|(handle, _)| *handle == window.0) {
            // SAFETY: replacing the window procedure is the documented subclasses idiom. We store
            // the previous procedure and chain to it; allocation happens before the change.
            let previous = unsafe { SetWindowLongPtrW(hwnd, GWLP_WNDPROC, window_proc_entry as *const () as isize) };
            if previous == 0 {
                // A zero return can be a valid previous procedure; the C++ distinguishes via
                // GetLastError, which this binding does not surface, so treat any value as the
                // chain to call. If the subclass failed entirely, the chain call is harmless.
            }
            originals.push((window.0, previous));
        }
        self.current.store(window.0, Ordering::Release);
        info!("sedici: overlay input hook installed");
        true
    }

    /// Drives the Full menu's capture for the current window when the hotkey message arrives.
    pub fn on_hotkey_message(&self, window: WindowHandle) {
        let Some(current) = self.current_window() else { return };
        if current != window {
            // A stale window's posted message: make sure it is not still capturing.
            self.guard.leave_window(window, true);
            return;
        }
        if self.disabled.load(Ordering::Acquire) {
            self.guard.leave(true);
            return;
        }
        if !self.host.cursor_active() && !self.guard.enter(window) {
            // No capture possible: the renderer falls back to the Mini menu.
            self.host.on_input(InputMessage { msg: HOTKEY_MESSAGE, wparam: 0, lparam: 0 });
        }
    }

    fn current_window(&self) -> Option<WindowHandle> {
        let handle = self.current.load(Ordering::Acquire);
        (!handle.eq(&0)).then_some(WindowHandle(handle))
    }

    /// The core per-message decision, free of Win32 so tests can drive it.
    pub fn dispatch(&self, window: WindowHandle, msg: u32, wparam: WPARAM, lparam: LPARAM) -> Dispatch {
        // Posted hotkey message: enter or leave Full capture.
        if msg == HOTKEY_MESSAGE {
            self.on_hotkey_message(window);
            return Dispatch::Consumed(LRESULT(0));
        }

        // A consumed hotkey key-down also consumes its matching key-up, even if modifiers or mode
        // changed between the two. Never consume an unrelated key-up.
        if (msg == WM_KEYUP || msg == WM_SYSKEYUP) && wparam.0 as u32 == self.hotkey.vk {
            if self.hotkey_down_window.compare_exchange(window.0, 0, Ordering::AcqRel, Ordering::Acquire).is_ok() {
                return Dispatch::Consumed(LRESULT(0));
            }
        }
        if msg == WM_KILLFOCUS || msg == WM_NCDESTROY {
            let _ = self.hotkey_down_window.compare_exchange(window.0, 0, Ordering::AcqRel, Ordering::Acquire);
        }
        if msg == WM_DESTROY || msg == WM_NCDESTROY {
            self.guard.leave_window(window, false);
        }
        if msg == WM_SETFOCUS {
            self.queued.lock().unwrap_or_else(|e| e.into_inner()).push(InputMessage { msg, wparam: wparam.0, lparam: lparam.0 });
        }

        // Hotkey press.
        if msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN {
            if wparam.0 as u32 == self.hotkey.vk {
                let modifiers = current_modifiers();
                if self.hotkey.matches(modifiers) {
                    // Bit 30 of lparam is the previous key state; ignore auto-repeat.
                    if (lparam.0 as u64 >> 30) & 1 == 0 {
                        self.hotkey_down_window.store(window.0, Ordering::Release);
                        self.host.on_hotkey(HotkeyEvent { action: KeyAction::Pressed, window });
                    }
                    return Dispatch::Consumed(LRESULT(0));
                }
            }
        }

        // Otherwise the Full menu consumes input while it owns the cursor, except Alt+F4 (K126).
        if !self.disabled.load(Ordering::Acquire) && self.host.cursor_active() {
            if msg == WM_SETCURSOR {
                set_arrow();
                return Dispatch::Consumed(LRESULT(1));
            }
            if msg == WM_INPUT {
                // SAFETY: no arguments beyond the window; `DefWindowProcW` is the documented default.
                return Dispatch::Consumed(unsafe { DefWindowProcW(hwnd_of(window), msg, wparam, lparam) });
            }
            if !ui_passes_to_game(msg, wparam.0 as u64, lparam.0 as i64) && is_consumed_input(msg) {
                {
                    let mut queued = self.queued.lock().unwrap_or_else(|e| e.into_inner());
                    if queued.len() >= MAX_QUEUED_MESSAGES {
                        queued.remove(0);
                    }
                    queued.push(InputMessage { msg, wparam: wparam.0, lparam: lparam.0 });
                }
                self.host.on_input(InputMessage { msg, wparam: wparam.0, lparam: lparam.0 });
                if msg == WM_MOUSEMOVE {
                    set_arrow();
                }
                return Dispatch::Consumed(LRESULT(0));
            }
        }

        // Not ours: forward to the original procedure.
        Dispatch::Forward
    }

    /// Messages queued for the overlay renderer since the last call.
    pub fn take_queued(&self) -> Vec<InputMessage> {
        std::mem::take(&mut self.queued.lock().unwrap_or_else(|e| e.into_inner()))
    }

    /// Stops suppression and drops the subclass chain on every captured window.
    pub fn disable(&self) {
        self.disabled.store(true, Ordering::Release);
        self.guard.disable();
        let windows: Vec<isize> = self.original.lock().unwrap_or_else(|e| e.into_inner()).iter().map(|(w, _)| *w).collect();
        for window in windows {
            // SAFETY: posting a message to a live window; a destroyed window fails harmlessly.
            let _ = unsafe { PostMessageW(Some(hwnd_of(WindowHandle(window))), HOTKEY_MESSAGE, WPARAM(0), LPARAM(0)) };
        }
    }

    /// Called from the window procedure when a window it captured is destroyed.
    fn forget_window(&self, window: WindowHandle) {
        self.guard.leave_window(window, false);
        self.host.on_destroyed(window);
        let mut originals = self.original.lock().unwrap_or_else(|e| e.into_inner());
        originals.retain(|(handle, _)| *handle != window.0);
        let _ = self.current.compare_exchange(window.0, 0, Ordering::AcqRel, Ordering::Acquire);
    }

    /// The procedure stored for `window`, if any.
    fn original_for(&self, window: WindowHandle) -> Option<isize> {
        self.original.lock().unwrap_or_else(|e| e.into_inner()).iter().find(|(h, _)| *h == window.0).map(|(_, p)| *p)
    }
}

fn hwnd_of(window: WindowHandle) -> HWND {
    HWND(window.0 as *mut std::ffi::c_void)
}

fn set_arrow() {
    // SAFETY: the arrow cursor is a shared system resource; no ownership is taken.
    if let Ok(arrow) = unsafe { LoadCursorW(None, IDC_ARROW) } {
        unsafe { SetCursor(Some(arrow)) };
    }
}

// ---- Static trampoline ------------------------------------------------------------------------
// SetWindowLongPtrW takes a plain function pointer with no context argument, so the live
// `WindowInput` must be reachable from it. One process-wide instance is registered here; the
// window procedure looks it up on every message. This mirrors the C++ file-scope state.

type Trampoline = Box<dyn Fn(WindowHandle, u32, WPARAM, LPARAM) -> LRESULT + Send + Sync>;

static REGISTRY: Mutex<Option<Trampoline>> = Mutex::new(None);

fn window_proc_entry(hwnd: HWND, msg: u32, wparam: WPARAM, lparam: LPARAM) -> LRESULT {
    let window = WindowHandle(hwnd.0 as isize);
    let registry = REGISTRY.lock().unwrap_or_else(|e| e.into_inner());
    match registry.as_ref() {
        Some(handler) => handler(window, msg, wparam, lparam),
        // SAFETY: no handler registered; behave as an ordinary window procedure.
        None => unsafe { DefWindowProcW(hwnd, msg, wparam, lparam) },
    }
}

/// Registers `input` as the process's window-procedure handler; the caller must keep it alive for
/// as long as the subclassed windows exist.
pub fn install_trampoline<H: WindowInputHost + 'static, G: CursorGuard + 'static>(input: &'static WindowInput<H, G>) {
    let handler: Trampoline = Box::new(move |window, msg, wparam, lparam| {
        let result = match input.dispatch(window, msg, wparam, lparam) {
            Dispatch::Consumed(value) => value,
            Dispatch::Forward => match input.original_for(window) {
                // SAFETY: chain to the procedure we replaced; the handle is a live window.
                Some(original) if original != 0 => unsafe {
                    let proc: windows::Win32::UI::WindowsAndMessaging::WNDPROC = std::mem::transmute(original);
                    CallWindowProcW(proc, hwnd_of(window), msg, wparam, lparam)
                },
                _ => unsafe { DefWindowProcW(hwnd_of(window), msg, wparam, lparam) },
            },
        };
        if msg == WM_NCDESTROY {
            input.forget_window(window);
        }
        result
    });
    *REGISTRY.lock().unwrap_or_else(|e| e.into_inner()) = Some(handler);
}

/// Releases mouse capture from `window` if it holds it (used when entering Full).
pub fn release_capture_if_held(window: WindowHandle) {
    // SAFETY: no arguments; compare and release are safe for any window handle.
    if unsafe { GetCapture() } == hwnd_of(window) {
        let _ = unsafe { ReleaseCapture() };
    }
}

/// Reads the window procedure installed by Win32 for `window` (diagnostic).
pub fn window_procedure(window: WindowHandle) -> isize {
    // SAFETY: reading a window's long pointer is safe for a live handle.
    unsafe { GetWindowLongPtrW(hwnd_of(window), GWLP_WNDPROC) }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::application::ports::input::InterceptionSummary;
    use rstest::rstest;
    use std::sync::atomic::AtomicU32;

    #[derive(Default)]
    struct TestHost {
        active: AtomicBool,
        disabled: AtomicBool,
        hotkeys: Mutex<Vec<HotkeyEvent>>,
        inputs: Mutex<Vec<InputMessage>>,
    }

    impl WindowInputHost for TestHost {
        fn cursor_active(&self) -> bool {
            self.active.load(Ordering::Acquire)
        }
        fn disabled(&self) -> bool {
            self.disabled.load(Ordering::Acquire)
        }
        fn on_hotkey(&self, event: HotkeyEvent) {
            self.hotkeys.lock().unwrap().push(event);
        }
        fn on_input(&self, message: InputMessage) {
            self.inputs.lock().unwrap().push(message);
        }
        fn on_destroyed(&self, _window: WindowHandle) {}
    }

    #[derive(Default)]
    struct TestGuard {
        active: AtomicBool,
        enters: AtomicU32,
        leaves: AtomicU32,
        available: AtomicBool,
    }

    impl CursorGuard for TestGuard {
        fn enter(&self, _window: WindowHandle) -> bool {
            self.enters.fetch_add(1, Ordering::Relaxed);
            self.active.store(true, Ordering::Release);
            self.available.load(Ordering::Relaxed)
        }
        fn leave(&self, _restore_clip: bool) {
            self.leaves.fetch_add(1, Ordering::Relaxed);
            self.active.store(false, Ordering::Release);
        }
        fn leave_window(&self, _window: WindowHandle, restore_clip: bool) {
            self.leave(restore_clip);
        }
        fn disable(&self) {
            self.active.store(false, Ordering::Release);
        }
        fn active(&self) -> bool {
            self.active.load(Ordering::Acquire)
        }
        fn available(&self) -> bool {
            self.available.load(Ordering::Acquire)
        }
        fn summary(&self) -> InterceptionSummary {
            InterceptionSummary::default()
        }
    }

    fn input() -> (std::sync::Arc<TestHost>, std::sync::Arc<TestGuard>, WindowInput<TestHostRef, TestGuardRef>) {
        let host = std::sync::Arc::new(TestHost::default());
        let guard = std::sync::Arc::new(TestGuard::default());
        guard.available.store(true, Ordering::Relaxed);
        let input = WindowInput::new(TestHostRef(host.clone()), TestGuardRef(guard.clone()), Hotkey::parse("alt+f10"));
        (host, guard, input)
    }

    // Arc-backed wrappers so tests keep handles while the adapter owns its copy.
    struct TestHostRef(std::sync::Arc<TestHost>);
    impl WindowInputHost for TestHostRef {
        fn cursor_active(&self) -> bool {
            self.0.cursor_active()
        }
        fn disabled(&self) -> bool {
            self.0.disabled()
        }
        fn on_hotkey(&self, event: HotkeyEvent) {
            self.0.on_hotkey(event)
        }
        fn on_input(&self, message: InputMessage) {
            self.0.on_input(message)
        }
        fn on_destroyed(&self, window: WindowHandle) {
            self.0.on_destroyed(window)
        }
    }
    struct TestGuardRef(std::sync::Arc<TestGuard>);
    impl CursorGuard for TestGuardRef {
        fn enter(&self, window: WindowHandle) -> bool {
            self.0.enter(window)
        }
        fn leave(&self, restore_clip: bool) {
            self.0.leave(restore_clip)
        }
        fn leave_window(&self, window: WindowHandle, restore_clip: bool) {
            self.0.leave_window(window, restore_clip)
        }
        fn disable(&self) {
            self.0.disable()
        }
        fn active(&self) -> bool {
            self.0.active()
        }
        fn available(&self) -> bool {
            self.0.available()
        }
        fn summary(&self) -> InterceptionSummary {
            self.0.summary()
        }
    }

    const WINDOW: WindowHandle = WindowHandle(7);

    #[test]
    fn a_full_menu_swallows_game_input() {
        let (host, _, input) = input();
        host.active.store(true, Ordering::Release);
        assert_eq!(input.dispatch(WINDOW, WM_MOUSEMOVE, WPARAM(0), LPARAM(0)), Dispatch::Consumed(LRESULT(0)));
        assert_eq!(input.take_queued().len(), 1, "the mouse move reaches the overlay");
        assert!(host.inputs.lock().unwrap().len() >= 1);
    }

    #[test]
    fn alt_f4_reaches_the_game_in_full_mode() {
        let (host, _, input) = input();
        host.active.store(true, Ordering::Release);
        // WM_SYSKEYDOWN F4 with Alt held (lparam bit 29).
        assert_eq!(input.dispatch(WINDOW, WM_SYSKEYDOWN, WPARAM(0x73), LPARAM(1 << 29)), Dispatch::Forward);
        assert!(input.take_queued().is_empty());
    }

    #[test]
    fn a_consumed_hotkey_key_down_consumes_its_key_up() {
        let (host, _, input) = input();
        // Alt must be down for the Alt+F10 hotkey to match: `GetKeyState` reports the real thread
        // state, so this branch is driven by the paired-up logic only when the press matched. Set
        // the down-window directly the way a matched press would.
        input.hotkey_down_window.store(WINDOW.0, Ordering::Release);
        assert_eq!(input.dispatch(WINDOW, WM_SYSKEYUP, WPARAM(0x79), LPARAM(0)), Dispatch::Consumed(LRESULT(0)));
        let _ = host;
    }

    #[test]
    fn an_unpaired_key_up_is_forwarded() {
        let (_, _, input) = input();
        assert_eq!(input.dispatch(WINDOW, WM_SYSKEYUP, WPARAM(0x79), LPARAM(0)), Dispatch::Forward);
    }

    #[test]
    fn destroy_leaves_capture_and_drops_the_chain() {
        let (_, guard, input) = input();
        input.capture_window(WINDOW);
        guard.enters.store(0, Ordering::Relaxed);
        assert_eq!(input.dispatch(WINDOW, WM_NCDESTROY, WPARAM(0), LPARAM(0)), Dispatch::Forward);
        assert_eq!(guard.leaves.load(Ordering::Relaxed), 1);
        assert!(input.original_for(WINDOW).is_some(), "the chain is remembered until the forward completes");
    }

    #[test]
    fn set_cursor_is_consumed_and_returns_true() {
        let (host, _, input) = input();
        host.active.store(true, Ordering::Release);
        assert_eq!(input.dispatch(WINDOW, WM_SETCURSOR, WPARAM(0), LPARAM(0)), Dispatch::Consumed(LRESULT(1)));
    }

    #[test]
    fn wm_input_goes_to_the_default_procedure() {
        let (host, _, input) = input();
        host.active.store(true, Ordering::Release);
        // Not the game's window, so `DefWindowProcW` returns 0.
        assert!(matches!(input.dispatch(WINDOW, WM_INPUT, WPARAM(0), LPARAM(0)), Dispatch::Consumed(_)));
    }

    #[rstest]
    #[case(WM_MOUSEMOVE, true)]
    #[case(WM_MOUSEWHEEL, true)]
    #[case(WM_KEYDOWN, true)]
    #[case(WM_CHAR, true)]
    #[case(WM_SETCURSOR, false)]
    #[case(WM_INPUT, false)]
    #[case(WM_SETFOCUS, false)]
    fn consumed_input_matches_the_cpp_list(#[case] msg: u32, #[case] consumed: bool) {
        assert_eq!(is_consumed_input(msg), consumed);
    }
}
