//! IAT and delay-import patching of `SetCursorPos`/`ClipCursor` (ports `ui/ui_input.cpp`, K132).
//!
//! The overlay's Full menu needs to own the cursor. Games reach the two calls through their own
//! imports, sometimes from an engine DLL rather than the executable, so this scans the executable
//! and every loaded non-system module, patches a slot only when it holds the real function,
//! rescans at most every two seconds to catch modules loaded or delay-load slots resolved since,
//! and restores every slot it changed on drop. The unsafe PE parsing and `VirtualProtect` calls
//! live here; [`CursorInterception`] is the safe API and implements the [`CursorGuard`] port.

use std::sync::atomic::{AtomicBool, AtomicIsize, AtomicU32, Ordering};
use std::sync::{Mutex, MutexGuard, OnceLock};

use tracing::info;
use windows::core::{s, w, PCSTR, PCWSTR};
use windows::Win32::Foundation::{FARPROC, FALSE, HWND, RECT, TRUE};
use windows::core::BOOL;
use windows::Win32::System::Diagnostics::Debug::{
    IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT, IMAGE_DIRECTORY_ENTRY_IMPORT, IMAGE_NT_HEADERS64,
};
use windows::Win32::System::LibraryLoader::{
    GetModuleHandleExW, GetModuleHandleW, GetProcAddress, GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
};
use windows::Win32::System::Memory::{VirtualProtect, PAGE_READWRITE};
use windows::Win32::System::SystemInformation::GetTickCount64;
use windows::Win32::System::SystemServices::{
    IMAGE_DOS_HEADER, IMAGE_DOS_SIGNATURE, IMAGE_IMPORT_DESCRIPTOR, IMAGE_NT_SIGNATURE, IMAGE_ORDINAL_FLAG64,
};
use windows::Win32::System::Threading::GetCurrentThreadId;
use windows::Win32::System::WindowsProgramming::{IMAGE_DELAYLOAD_DESCRIPTOR, IMAGE_THUNK_DATA64};
use windows::Win32::UI::Input::KeyboardAndMouse::{GetCapture, ReleaseCapture};
use windows::Win32::UI::WindowsAndMessaging::{
    ClipCursor, GetClipCursor, GetForegroundWindow, GetWindowThreadProcessId, LoadCursorW, SetCursor, SetCursorPos,
    ShowCursor, IDC_ARROW,
};

use crate::adapters::driven::win32::modules::{executable, is_system_module, loaded_modules, Module};
use crate::application::ports::input::{CursorGuard, InterceptionSummary, WindowHandle};

type SetCursorPosFn = unsafe extern "system" fn(i32, i32) -> BOOL;
type ClipCursorFn = unsafe extern "system" fn(*const RECT) -> BOOL;

const RESCAN_MS: u64 = 2000;
/// When nothing was patched, a failed scan retries sooner (an import may resolve at any moment).
const RETRY_MS: u64 = 250;

static REAL_SET_CURSOR_POS: OnceLock<usize> = OnceLock::new();
static REAL_CLIP_CURSOR: OnceLock<usize> = OnceLock::new();

/// The address the import slots hold when they have not been hooked: the user32 export itself,
/// falling back to the import address when user32 cannot be queried.
fn resolve_real(name: PCSTR, cell: &'static OnceLock<usize>, fallback: usize) -> usize {
    *cell.get_or_init(|| {
        // SAFETY: user32 is loaded in any process with a window; the export names are documented.
        match unsafe { GetModuleHandleW(w!("user32.dll")) } {
            Ok(module) => {
                let proc: FARPROC = unsafe { GetProcAddress(module, name) };
                proc.map(|p| p as usize).unwrap_or(fallback)
            }
            Err(_) => fallback,
        }
    })
}

pub fn real_set_cursor_pos() -> usize {
    // SAFETY: taking an imported function's address; no call is made.
    resolve_real(s!("SetCursorPos"), &REAL_SET_CURSOR_POS, SetCursorPos as *const () as usize)
}

pub fn real_clip_cursor() -> usize {
    // SAFETY: taking an imported function's address; no call is made.
    resolve_real(s!("ClipCursor"), &REAL_CLIP_CURSOR, ClipCursor as *const () as usize)
}

fn call_real_set_cursor_pos(x: i32, y: i32) -> BOOL {
    // SAFETY: the cached address is the real user32 export with the documented signature.
    unsafe { std::mem::transmute::<usize, SetCursorPosFn>(real_set_cursor_pos())(x, y) }
}

fn call_real_clip_cursor(rect: *const RECT) -> BOOL {
    // SAFETY: as above; `rect` is null or a live RECT for the duration of the call.
    unsafe { std::mem::transmute::<usize, ClipCursorFn>(real_clip_cursor())(rect) }
}

// Capture state. The hooks run on arbitrary threads, so it is atomics plus one mutex for the clip
// rectangle and single-flight ownership transitions.
static ACTIVE: AtomicBool = AtomicBool::new(false);
static DISABLED: AtomicBool = AtomicBool::new(false);
static FULL_MODE: AtomicBool = AtomicBool::new(false);
static WINDOW: AtomicIsize = AtomicIsize::new(0);
static SUPPRESSED_POS: AtomicU32 = AtomicU32::new(0);
static SUPPRESSED_CLIP: AtomicU32 = AtomicU32::new(0);

#[derive(Default)]
struct Ownership {
    clip_rect: RECT,
    clip_set: bool,
    show_adjustments: i32,
}

static OWNERSHIP: Mutex<Ownership> =
    Mutex::new(Ownership { clip_rect: RECT { left: 0, top: 0, right: 0, bottom: 0 }, clip_set: false, show_adjustments: 0 });

fn lock_ownership() -> MutexGuard<'static, Ownership> {
    OWNERSHIP.lock().unwrap_or_else(|poison| poison.into_inner())
}

/// The driving adapter mirrors the live UI mode here; only Full capture suppresses cursor calls.
pub fn set_full_mode(enabled: bool) {
    FULL_MODE.store(enabled, Ordering::Release);
}

/// Mirrors `capturing()` in the C++: inert except during foreground Full capture.
fn capturing() -> bool {
    let window = WINDOW.load(Ordering::Acquire);
    if window == 0 || DISABLED.load(Ordering::Acquire) || !FULL_MODE.load(Ordering::Acquire) || !ACTIVE.load(Ordering::Acquire) {
        return false;
    }
    // SAFETY: no arguments; returns the foreground window or null.
    unsafe { GetForegroundWindow() == HWND(window as *mut std::ffi::c_void) }
}

extern "system" fn set_pos_hook(x: i32, y: i32) -> BOOL {
    if capturing() {
        SUPPRESSED_POS.fetch_add(1, Ordering::Relaxed);
        return TRUE;
    }
    call_real_set_cursor_pos(x, y)
}

extern "system" fn clip_hook(rect: *const RECT) -> BOOL {
    let mut ownership = lock_ownership();
    if !capturing() {
        return call_real_clip_cursor(rect);
    }
    SUPPRESSED_CLIP.fetch_add(1, Ordering::Relaxed);
    ownership.clip_set = !rect.is_null();
    if !rect.is_null() {
        // SAFETY: Win32 passes a live RECT; copy it before returning.
        ownership.clip_rect = unsafe { *rect };
    }
    TRUE
}

/// One patched import slot, remembered so a rescan never patches it twice and `Drop` can restore it.
struct PatchedSlot {
    slot: *mut usize,
    original: usize,
}

struct Patcher {
    patched: Vec<PatchedSlot>,
    summary: InterceptionSummary,
    last_scan: u64,
    reported: bool,
}

// SAFETY: the table is only touched under `PATCHER`; the slot pointers point into loaded module
// memory that stays mapped while the DSP is loaded.
unsafe impl Send for Patcher {}

static PATCHER: Mutex<Option<Patcher>> = Mutex::new(None);

fn lock_patcher() -> MutexGuard<'static, Option<Patcher>> {
    PATCHER.lock().unwrap_or_else(|poison| poison.into_inner())
}

fn empty_patcher() -> Patcher {
    Patcher { patched: Vec::new(), summary: InterceptionSummary::default(), last_scan: 0, reported: false }
}

/// Replaces the pointer in an import slot only if it still holds `expected`.
fn replace_slot(slot: *mut usize, expected: usize, replacement: usize) -> bool {
    let mut old = PAGE_READWRITE;
    // SAFETY: the slot is a live word inside a loaded image; the protection change covers one
    // pointer and `old` is a local out-parameter.
    if unsafe { VirtualProtect(slot.cast(), std::mem::size_of::<usize>(), PAGE_READWRITE, &mut old) }.is_err() {
        return false;
    }
    // SAFETY: aligned pointer-sized access to module memory holding an import address. The compare
    // happens before the store so we never chain through a foreign hook.
    let previous = unsafe { std::ptr::read_volatile(slot) };
    if previous == expected {
        unsafe { std::ptr::write_volatile(slot, replacement) };
    }
    let mut ignored = PAGE_READWRITE;
    // SAFETY: restores the original protection; best effort, as in the C++.
    let _ = unsafe { VirtualProtect(slot.cast(), std::mem::size_of::<usize>(), old, &mut ignored) };
    previous == expected
}

fn patch_if_cursor_import(patcher: &mut Patcher, api: &str, slot: *mut usize, saw_position: &mut bool, saw_clip: &mut bool) {
    let position = match api {
        "SetCursorPos" => true,
        "ClipCursor" => false,
        _ => return,
    };
    if patcher.patched.iter().any(|done| done.slot == slot) {
        return;
    }
    let (expected, replacement) = if position {
        (real_set_cursor_pos(), set_pos_hook as *const () as usize)
    } else {
        (real_clip_cursor(), clip_hook as *const () as usize)
    };
    if position {
        *saw_position = true;
    } else {
        *saw_clip = true;
    }
    // SAFETY: the slot is an import address-table entry; read it to decide.
    let current = unsafe { std::ptr::read_volatile(slot) };
    if current != expected {
        // Another hook, or a delay-load stub that has not resolved yet. Chaining through a foreign
        // hook is not safe, so leave it; a later scan picks up a resolved delay-load slot.
        patcher.summary.skipped_slots += 1;
        return;
    }
    if !replace_slot(slot, expected, replacement) {
        patcher.summary.skipped_slots += 1;
        return;
    }
    patcher.patched.push(PatchedSlot { slot, original: expected });
    if position {
        patcher.summary.set_cursor_pos_slots += 1;
    } else {
        patcher.summary.clip_cursor_slots += 1;
    }
}

fn is_ordinal(thunk: u64) -> bool {
    thunk & IMAGE_ORDINAL_FLAG64 != 0
}

/// Scans one loaded image. `base` must be the module's mapped image base.
fn scan_module(patcher: &mut Patcher, base: *const u8, saw_position: &mut bool, saw_clip: &mut bool) {
    if base.is_null() {
        return;
    }
    // SAFETY: `base` is a loaded module base. The PE headers are mapped and readable; the C++ makes
    // the same assumption without bounds checks.
    let dos = unsafe { std::ptr::read_unaligned(base.cast::<IMAGE_DOS_HEADER>()) };
    if dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 {
        return;
    }
    // SAFETY: `e_lfanew` is the RVA of the NT headers inside the same image.
    let nt = unsafe { std::ptr::read_unaligned(base.add(dos.e_lfanew as usize).cast::<IMAGE_NT_HEADERS64>()) };
    if nt.Signature != IMAGE_NT_SIGNATURE {
        return;
    }

    let imports = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT.0 as usize];
    if imports.VirtualAddress != 0 {
        // SAFETY: import descriptors sit at the image RVA; the list is null-terminated.
        let mut desc = unsafe { base.add(imports.VirtualAddress as usize).cast::<IMAGE_IMPORT_DESCRIPTOR>() };
        loop {
            let entry = unsafe { std::ptr::read_unaligned(desc) };
            if entry.Name == 0 {
                break;
            }
            let original_thunk = unsafe { entry.Anonymous.OriginalFirstThunk };
            if original_thunk != 0 {
                // SAFETY: the thunk arrays are at their RVAs and run to a null entry.
                let mut names = unsafe { base.add(original_thunk as usize).cast::<IMAGE_THUNK_DATA64>() };
                let mut slots = unsafe { base.add(entry.FirstThunk as usize).cast::<IMAGE_THUNK_DATA64>().cast_mut() };
                loop {
                    let address_of_data = unsafe { (*names).u1.AddressOfData };
                    if address_of_data == 0 {
                        break;
                    }
                    if !is_ordinal(address_of_data) {
                        let api = unsafe { read_api_name(base.add(address_of_data as usize)) };
                        let slot = unsafe { std::ptr::addr_of_mut!((*slots).u1).cast::<usize>() };
                        patch_if_cursor_import(patcher, &api, slot, saw_position, saw_clip);
                    }
                    names = unsafe { names.add(1) };
                    slots = unsafe { slots.add(1) };
                }
            }
            desc = unsafe { desc.add(1) };
        }
    }

    let delayed = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT.0 as usize];
    if delayed.VirtualAddress != 0 {
        // SAFETY: delay descriptors sit at the image RVA; the list is null-terminated.
        let mut desc = unsafe { base.add(delayed.VirtualAddress as usize).cast::<IMAGE_DELAYLOAD_DESCRIPTOR>() };
        loop {
            let entry = unsafe { std::ptr::read_unaligned(desc) };
            if entry.DllNameRVA == 0 {
                break;
            }
            const RVA_BASED: u32 = 1;
            let attributes = unsafe { entry.Attributes.AllAttributes };
            if attributes & RVA_BASED != 0 && entry.ImportNameTableRVA != 0 && entry.ImportAddressTableRVA != 0 {
                let mut names = unsafe { base.add(entry.ImportNameTableRVA as usize).cast::<IMAGE_THUNK_DATA64>() };
                let mut slots = unsafe { base.add(entry.ImportAddressTableRVA as usize).cast::<IMAGE_THUNK_DATA64>().cast_mut() };
                loop {
                    let address_of_data = unsafe { (*names).u1.AddressOfData };
                    if address_of_data == 0 {
                        break;
                    }
                    if !is_ordinal(address_of_data) {
                        let api = unsafe { read_api_name(base.add(address_of_data as usize)) };
                        let slot = unsafe { std::ptr::addr_of_mut!((*slots).u1).cast::<usize>() };
                        patch_if_cursor_import(patcher, &api, slot, saw_position, saw_clip);
                    }
                    names = unsafe { names.add(1) };
                    slots = unsafe { slots.add(1) };
                }
            }
            desc = unsafe { desc.add(1) };
        }
    }
}

/// Reads the NUL-terminated name after the 2-byte hint of an `IMAGE_IMPORT_BY_NAME`.
unsafe fn read_api_name(name: *const u8) -> String {
    // SAFETY: the caller guarantees the pointer is inside the mapped image; read at most 256 bytes
    // so a malformed image cannot run away.
    let mut bytes = Vec::new();
    for index in 0..256 {
        let byte = unsafe { *name.add(2 + index) };
        if byte == 0 {
            break;
        }
        bytes.push(byte);
    }
    String::from_utf8_lossy(&bytes).into_owned()
}

/// An address inside this module, used to find our own handle.
fn hook_anchor() {}

/// Patches every eligible module, honouring the two-second rescan window.
fn install(patcher: &mut Patcher) -> bool {
    // SAFETY: no arguments; a monotonic millisecond count.
    let now = unsafe { GetTickCount64() };
    if !patcher.patched.is_empty() && now.wrapping_sub(patcher.last_scan) < RESCAN_MS {
        return true;
    }
    if patcher.patched.is_empty() && patcher.last_scan != 0 && now.wrapping_sub(patcher.last_scan) < RETRY_MS {
        return false;
    }
    patcher.last_scan = now;

    let mut self_module = windows::Win32::Foundation::HMODULE::default();
    // SAFETY: the address is inside this module; the flag asks for its handle without a reference.
    let _ = unsafe {
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            PCWSTR(hook_anchor as *const () as *const u16),
            &mut self_module,
        )
    };
    let self_module = Module(self_module);
    let executable = executable();
    let modules = loaded_modules();

    let before = (patcher.summary.set_cursor_pos_slots, patcher.summary.clip_cursor_slots);
    let first_scan = patcher.summary.modules_scanned == 0;
    let mut saw_position = false;
    let mut saw_clip = false;
    let mut scanned = 0_u32;
    for module in modules {
        // Sedici's own imports stay alone unless Sedici is the executable (the offline test host).
        if module.0.is_invalid() || (!self_module.0.is_invalid() && module == self_module && Some(module) != executable) {
            continue;
        }
        if Some(module) != executable && is_system_module(module) {
            continue;
        }
        scanned += 1;
        scan_module(patcher, module.0 .0 as *const u8, &mut saw_position, &mut saw_clip);
    }
    patcher.summary.modules_scanned = scanned;

    let changed = (patcher.summary.set_cursor_pos_slots, patcher.summary.clip_cursor_slots) != before;
    if changed || first_scan {
        patcher.reported = true;
        info!(
            "sedici: overlay cursor interception {}: modules={} slots{{SetCursorPos={} ClipCursor={}}} skipped={} seen{{SetCursorPos={} ClipCursor={}}}",
            if patcher.patched.is_empty() { "unavailable" } else { "installed" },
            scanned,
            patcher.summary.set_cursor_pos_slots,
            patcher.summary.clip_cursor_slots,
            patcher.summary.skipped_slots,
            if saw_position { "yes" } else { "no" },
            if saw_clip { "yes" } else { "no" }
        );
    }
    // One of the two is enough to be useful: the game then loses only the call it does not make
    // through an import we can see. The summary records which.
    !patcher.patched.is_empty()
}

/// Safe cursor-ownership facade over the hooks. Implements [`CursorGuard`].
pub struct CursorInterception;

impl CursorInterception {
    pub fn new() -> Self {
        let mut guard = lock_patcher();
        if guard.is_none() {
            *guard = Some(empty_patcher());
            ACTIVE.store(false, Ordering::Release);
            WINDOW.store(0, Ordering::Release);
        }
        Self
    }
}

impl Default for CursorInterception {
    fn default() -> Self {
        Self::new()
    }
}

fn hwnd_of(window: WindowHandle) -> HWND {
    HWND(window.0 as *mut std::ffi::c_void)
}

impl CursorGuard for CursorInterception {
    fn enter(&self, window: WindowHandle) -> bool {
        let hwnd = hwnd_of(window);
        if hwnd.is_invalid() || DISABLED.load(Ordering::Acquire) {
            return false;
        }
        // SAFETY: no arguments.
        if unsafe { GetForegroundWindow() } != hwnd || !self.install_hooks() {
            return false;
        }

        let mut ownership = lock_ownership();
        // Entry/exit and ShowCursor accounting belong to the owning window thread.
        // SAFETY: the handle is a window the caller owns; a null process-id out-parameter is allowed.
        if unsafe { GetWindowThreadProcessId(hwnd, None) } != unsafe { GetCurrentThreadId() } {
            return false;
        }
        if ACTIVE.load(Ordering::Acquire) {
            return WINDOW.load(Ordering::Acquire) == window.0;
        }
        let mut clip = RECT::default();
        // SAFETY: `clip` is a local out-parameter.
        if unsafe { GetClipCursor(&mut clip) }.is_err() {
            return false;
        }
        ownership.clip_rect = clip;
        ownership.clip_set = true;
        WINDOW.store(window.0, Ordering::Release);
        ACTIVE.store(true, Ordering::Release);
        if call_real_clip_cursor(std::ptr::null()) == FALSE {
            ACTIVE.store(false, Ordering::Release);
            return false;
        }

        ownership.show_adjustments = 0;
        // Raise this input thread's ShowCursor counter to >= 0 (visible) and record only the calls
        // needed, so leaving restores the original count.
        // SAFETY: thread-local cursor counter.
        let mut count = unsafe { ShowCursor(true) };
        if count > 0 {
            let _ = unsafe { ShowCursor(false) };
        } else {
            ownership.show_adjustments = 1;
            while count < 0 && ownership.show_adjustments < 32 {
                count = unsafe { ShowCursor(true) };
                ownership.show_adjustments += 1;
            }
        }

        // Publish all bookkeeping before releasing capture: ReleaseCapture sends
        // WM_CAPTURECHANGED synchronously, and the game's handler may call the clip hook or leave
        // Full. Neither may run under this mutex.
        drop(ownership);
        // SAFETY: no arguments; `GetCapture` returns the capturing window or null.
        if unsafe { GetCapture() } == hwnd {
            let _ = unsafe { ReleaseCapture() };
        }
        if !capturing() {
            return false;
        }
        // SAFETY: the arrow cursor is a shared system resource; no ownership is taken.
        if let Ok(arrow) = unsafe { LoadCursorW(None, IDC_ARROW) } {
            unsafe { SetCursor(Some(arrow)) };
        }
        true
    }

    fn leave(&self, restore_clip: bool) {
        let window = WINDOW.load(Ordering::Acquire);
        self.leave_window(WindowHandle(window), restore_clip);
    }

    fn leave_window(&self, window: WindowHandle, restore_clip: bool) {
        let mut ownership = lock_ownership();
        let hwnd = hwnd_of(window);
        if hwnd.is_invalid() || WINDOW.load(Ordering::Acquire) != window.0 || !ACTIVE.load(Ordering::Acquire) {
            return;
        }
        // SAFETY: as in `enter`; the caller holds the window.
        if unsafe { GetWindowThreadProcessId(hwnd, None) } != unsafe { GetCurrentThreadId() } {
            return;
        }
        ACTIVE.store(false, Ordering::Release);
        WINDOW.store(0, Ordering::Release);
        while ownership.show_adjustments > 0 {
            // SAFETY: thread-local cursor counter.
            unsafe { ShowCursor(false) };
            ownership.show_adjustments -= 1;
        }
        // SAFETY: no arguments.
        if restore_clip && unsafe { GetForegroundWindow() } == hwnd {
            let ptr = if ownership.clip_set { &ownership.clip_rect as *const RECT } else { std::ptr::null() };
            let _ = call_real_clip_cursor(ptr);
        }
        let positions = SUPPRESSED_POS.swap(0, Ordering::Relaxed);
        let clips = SUPPRESSED_CLIP.swap(0, Ordering::Relaxed);
        if positions > 0 || clips > 0 {
            info!("sedici: overlay cursor interception released suppressed{{sets={positions} clips={clips}}}");
        }
    }

    fn disable(&self) {
        DISABLED.store(true, Ordering::Release);
    }

    fn active(&self) -> bool {
        capturing()
    }

    fn available(&self) -> bool {
        self.install_hooks()
    }

    fn summary(&self) -> InterceptionSummary {
        lock_patcher().as_ref().map(|patcher| patcher.summary).unwrap_or_default()
    }
}

impl CursorInterception {
    fn install_hooks(&self) -> bool {
        let mut guard = lock_patcher();
        let patcher = guard.get_or_insert_with(empty_patcher);
        install(patcher)
    }
}

impl Drop for CursorInterception {
    fn drop(&mut self) {
        // The C++ keeps its hooks for the process lifetime; restoring them here keeps the Rust
        // adapter honest and makes tests independent.
        let mut guard = lock_patcher();
        let Some(patcher) = guard.as_mut() else { return };
        for slot in patcher.patched.drain(..) {
            let mut old = PAGE_READWRITE;
            // SAFETY: the slot was patched by us and still belongs to a mapped image.
            if unsafe { VirtualProtect(slot.slot.cast(), std::mem::size_of::<usize>(), PAGE_READWRITE, &mut old) }.is_err() {
                continue;
            }
            unsafe { std::ptr::write_volatile(slot.slot, slot.original) };
            let mut ignored = PAGE_READWRITE;
            let _ = unsafe { VirtualProtect(slot.slot.cast(), std::mem::size_of::<usize>(), old, &mut ignored) };
        }
        ACTIVE.store(false, Ordering::Release);
        WINDOW.store(0, Ordering::Release);
    }
}

// The interception is process-global, exactly like the C++ statics it replaces.
static SHARED: Mutex<Option<&'static CursorInterception>> = Mutex::new(None);

/// The process-wide interception, created once. The entries below are what `entry.rs` wires in.
pub fn shared() -> &'static CursorInterception {
    let mut guard = SHARED.lock().unwrap_or_else(|e| e.into_inner());
    guard.get_or_insert_with(|| Box::leak(Box::new(CursorInterception::new())))
}

#[cfg(test)]
mod tests {
    use super::*;
    use rstest::rstest;

    /// A synthetic in-memory PE64 image with one normal import DLL and one delay-load DLL, each
    /// importing `SetCursorPos` and `ClipCursor` the way a game module would.
    struct FakeImage {
        words: Vec<u64>,
        normal_slots: [*mut usize; 2],
        delay_slots: [*mut usize; 2],
    }

    const IMAGE_WORDS: usize = 256; // 2 KiB covers every RVA below

    impl FakeImage {
        fn new(set_pos_value: usize, clip_value: usize) -> Self {
            let mut words = vec![0u64; IMAGE_WORDS];
            let base = words.as_mut_ptr().cast::<u8>();
            unsafe {
                let dos = base.cast::<IMAGE_DOS_HEADER>();
                (*dos).e_magic = IMAGE_DOS_SIGNATURE;
                (*dos).e_lfanew = 0x40;
            }
            let import_rva = 0x100_u32;
            let delay_rva = 0x400_u32;
            unsafe {
                let nt = base.add(0x40).cast::<IMAGE_NT_HEADERS64>();
                (*nt).Signature = IMAGE_NT_SIGNATURE;
                (*nt).OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT.0 as usize].VirtualAddress = import_rva;
                (*nt).OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT.0 as usize].VirtualAddress = delay_rva;
            }

            let slot_rva = 0x180_u32;
            unsafe {
                let desc = base.add(import_rva as usize).cast::<IMAGE_IMPORT_DESCRIPTOR>();
                (*desc).Anonymous.OriginalFirstThunk = 0x140;
                (*desc).FirstThunk = slot_rva;
                (*desc).Name = 0x3E0;
            }
            unsafe {
                let names = base.add(0x140).cast::<IMAGE_THUNK_DATA64>();
                (*names).u1.AddressOfData = 0x360;
                (*names.add(1)).u1.AddressOfData = 0x380;
                let slots = base.add(slot_rva as usize).cast::<IMAGE_THUNK_DATA64>();
                (*slots).u1.Function = set_pos_value as u64;
                (*slots.add(1)).u1.Function = clip_value as u64;
            }
            write_import_name(base, 0x360, b"SetCursorPos");
            write_import_name(base, 0x380, b"ClipCursor");
            write_import_name(base, 0x3E0, b"fake.dll");

            let delay_slot_rva = 0x480_u32;
            unsafe {
                let desc = base.add(delay_rva as usize).cast::<IMAGE_DELAYLOAD_DESCRIPTOR>();
                (*desc).Attributes.AllAttributes = 1; // RvaBased
                (*desc).DllNameRVA = 0x3C0;
                (*desc).ImportNameTableRVA = 0x440;
                (*desc).ImportAddressTableRVA = delay_slot_rva;
            }
            unsafe {
                let names = base.add(0x440).cast::<IMAGE_THUNK_DATA64>();
                (*names).u1.AddressOfData = 0x360;
                (*names.add(1)).u1.AddressOfData = 0x380;
                let slots = base.add(delay_slot_rva as usize).cast::<IMAGE_THUNK_DATA64>();
                (*slots).u1.Function = set_pos_value as u64;
                (*slots.add(1)).u1.Function = clip_value as u64;
            }
            write_import_name(base, 0x3C0, b"fake-delayed.dll");

            let normal_slots = unsafe {
                [
                    std::ptr::addr_of_mut!((*(base.add(slot_rva as usize).cast::<IMAGE_THUNK_DATA64>())).u1).cast::<usize>(),
                    std::ptr::addr_of_mut!((*(base.add(slot_rva as usize + 8).cast::<IMAGE_THUNK_DATA64>())).u1).cast::<usize>(),
                ]
            };
            let delay_slots = unsafe {
                [
                    std::ptr::addr_of_mut!((*(base.add(delay_slot_rva as usize).cast::<IMAGE_THUNK_DATA64>())).u1).cast::<usize>(),
                    std::ptr::addr_of_mut!((*(base.add(delay_slot_rva as usize + 8).cast::<IMAGE_THUNK_DATA64>())).u1).cast::<usize>(),
                ]
            };
            Self { words, normal_slots, delay_slots }
        }

        fn base(&self) -> *const u8 {
            self.words.as_ptr().cast()
        }

        fn scan(&self, patcher: &mut Patcher) -> (bool, bool) {
            let mut saw_position = false;
            let mut saw_clip = false;
            scan_module(patcher, self.base(), &mut saw_position, &mut saw_clip);
            (saw_position, saw_clip)
        }

        fn normal_value(&self, index: usize) -> usize {
            unsafe { std::ptr::read_volatile(self.normal_slots[index]) }
        }

        fn delay_value(&self, index: usize) -> usize {
            unsafe { std::ptr::read_volatile(self.delay_slots[index]) }
        }
    }

    fn write_import_name(base: *mut u8, offset: usize, name: &[u8]) {
        // SAFETY: the caller passes an offset inside the locally allocated image.
        let target = unsafe { base.add(offset) };
        unsafe { std::ptr::write_bytes(target, 0, 2) }; // hint
        unsafe { std::ptr::copy_nonoverlapping(name.as_ptr(), target.add(2), name.len()) };
    }

    #[test]
    fn patches_import_and_delay_import_slots_that_hold_the_real_functions() {
        let image = FakeImage::new(real_set_cursor_pos(), real_clip_cursor());
        let mut patcher = empty_patcher();
        let (saw_position, saw_clip) = image.scan(&mut patcher);
        assert!(saw_position && saw_clip, "both cursor names were seen");
        assert_eq!(patcher.summary.set_cursor_pos_slots, 2, "normal and delay SetCursorPos slots patched");
        assert_eq!(patcher.summary.clip_cursor_slots, 2, "normal and delay ClipCursor slots patched");
        assert_eq!(patcher.summary.skipped_slots, 0);
        assert_eq!(image.normal_value(0), set_pos_hook as *const () as usize);
        assert_eq!(image.normal_value(1), clip_hook as *const () as usize);
        assert_eq!(image.delay_value(0), set_pos_hook as *const () as usize);
        assert_eq!(image.delay_value(1), clip_hook as *const () as usize);
    }

    #[test]
    fn a_slot_holding_something_else_is_skipped_not_chained() {
        let foreign = hook_anchor as *const () as usize; // stands in for another hook or an unresolved stub
        let image = FakeImage::new(foreign, real_clip_cursor());
        let mut patcher = empty_patcher();
        image.scan(&mut patcher);
        assert_eq!(patcher.summary.set_cursor_pos_slots, 0);
        assert_eq!(patcher.summary.clip_cursor_slots, 2);
        assert_eq!(patcher.summary.skipped_slots, 2, "both SetCursorPos slots held a foreign value");
        assert_eq!(image.normal_value(0), foreign, "the foreign slot is left untouched");
    }

    #[test]
    fn a_rescan_never_patches_a_slot_twice() {
        let image = FakeImage::new(real_set_cursor_pos(), real_clip_cursor());
        let mut patcher = empty_patcher();
        image.scan(&mut patcher);
        let counts = (patcher.summary.set_cursor_pos_slots, patcher.summary.clip_cursor_slots);
        image.scan(&mut patcher);
        assert_eq!((patcher.summary.set_cursor_pos_slots, patcher.summary.clip_cursor_slots), counts);
        assert_eq!(patcher.patched.len(), 4);
    }

    #[test]
    fn restore_puts_the_original_pointers_back() {
        let image = FakeImage::new(real_set_cursor_pos(), real_clip_cursor());
        let mut patcher = empty_patcher();
        image.scan(&mut patcher);
        for slot in patcher.patched.drain(..) {
            let mut old = PAGE_READWRITE;
            unsafe { VirtualProtect(slot.slot.cast(), std::mem::size_of::<usize>(), PAGE_READWRITE, &mut old) }.unwrap();
            unsafe { std::ptr::write_volatile(slot.slot, slot.original) };
        }
        assert_eq!(image.normal_value(0), real_set_cursor_pos());
        assert_eq!(image.normal_value(1), real_clip_cursor());
    }

    #[test]
    fn an_ordinal_entry_is_not_patched() {
        let image = FakeImage::new(real_set_cursor_pos(), real_clip_cursor());
        unsafe {
            let names = image.base().add(0x140).cast::<IMAGE_THUNK_DATA64>().cast_mut();
            (*names).u1.Ordinal |= IMAGE_ORDINAL_FLAG64;
        }
        let mut patcher = empty_patcher();
        image.scan(&mut patcher);
        assert_eq!(patcher.summary.set_cursor_pos_slots, 1, "only the delay slot was patched");
        assert_eq!(image.normal_value(0), real_set_cursor_pos(), "the ordinal slot is untouched");
    }

    #[test]
    fn module_enumeration_includes_the_executable() {
        let modules = loaded_modules();
        let exe = executable().expect("the test executable is loaded");
        assert!(modules.contains(&exe));
        assert!(!is_system_module(exe));
    }

    /// Serialises tests that touch the process-wide capture state.
    static CAPTURE: std::sync::Mutex<()> = std::sync::Mutex::new(());

    /// A throwaway window the process owns. No game is involved.
    struct TestWindow(HWND);

    impl TestWindow {
        fn create() -> Option<Self> {
            use windows::Win32::System::LibraryLoader::GetModuleHandleW;
            use windows::Win32::Foundation::{HINSTANCE, LPARAM, LRESULT, WPARAM};
            use windows::Win32::UI::WindowsAndMessaging::{
                CreateWindowExW, DefWindowProcW, RegisterClassW, CW_USEDEFAULT, WINDOW_EX_STYLE, WNDCLASSW,
                WS_OVERLAPPEDWINDOW, WINDOW_STYLE,
            };
            extern "system" fn default_proc(hwnd: HWND, msg: u32, wparam: WPARAM, lparam: LPARAM) -> LRESULT {
                // SAFETY: the standard default window procedure.
                unsafe { DefWindowProcW(hwnd, msg, wparam, lparam) }
            }
            // SAFETY: a private window class with the default procedure; the handle is released in Drop.
            unsafe {
                let instance = GetModuleHandleW(None).ok()?;
                let class = WNDCLASSW {
                    lpfnWndProc: Some(default_proc),
                    hInstance: HINSTANCE(instance.0),
                    lpszClassName: w!("SediciRustCursorTest"),
                    ..Default::default()
                };
                RegisterClassW(&class);
                let hwnd = CreateWindowExW(
                    WINDOW_EX_STYLE(0),
                    w!("SediciRustCursorTest"),
                    w!("Sedici"),
                    WINDOW_STYLE(WS_OVERLAPPEDWINDOW.0),
                    CW_USEDEFAULT,
                    CW_USEDEFAULT,
                    320,
                    240,
                    None,
                    None,
                    Some(HINSTANCE(instance.0)),
                    None,
                )
                .ok()?;
                Some(Self(hwnd))
            }
        }

        fn handle(&self) -> WindowHandle {
            WindowHandle(self.0 .0 as isize)
        }
    }

    impl Drop for TestWindow {
        fn drop(&mut self) {
            // SAFETY: the window belongs to this test.
            let _ = unsafe { windows::Win32::UI::WindowsAndMessaging::DestroyWindow(self.0) };
        }
    }

    /// Attaches to the foreground thread's input queue so the process may take the foreground
    /// (K137). Returns whether the window is now foreground.
    fn take_foreground(window: &TestWindow) -> bool {
        use windows::Win32::System::Threading::{AttachThreadInput, GetCurrentThreadId};
        use windows::Win32::UI::WindowsAndMessaging::{BringWindowToTop, GetForegroundWindow, SetForegroundWindow};
        let handle = window.0;
        // SAFETY: comparing and setting the foreground window; a rejected request just fails.
        unsafe {
            if GetForegroundWindow() == handle {
                return true;
            }
            let current = GetForegroundWindow();
            let thread = if current.is_invalid() { 0 } else { GetWindowThreadProcessId(current, None) };
            let this = GetCurrentThreadId();
            let attached = thread != 0 && thread != this && AttachThreadInput(this, thread, true).as_bool();
            let _ = SetForegroundWindow(handle);
            let _ = BringWindowToTop(handle);
            if attached {
                let _ = AttachThreadInput(this, thread, false);
            }
            GetForegroundWindow() == handle
        }
    }

    /// The C++ regression's Full-entry path, against a throwaway window. Skipped with a clear note
    /// when the foreground cannot be taken (K137), never failed.
    #[test]
    fn capture_enters_and_leaves_on_the_owning_thread() {
        let _serial = CAPTURE.lock().unwrap_or_else(|e| e.into_inner());
        let Some(window) = TestWindow::create() else {
            eprintln!("skip: the test window could not be created");
            return;
        };
        if !take_foreground(&window) {
            eprintln!("NOTE foreground unavailable; Full capture entry check skipped (K137)");
            return;
        }
        // The test executable may import no cursor calls through an IAT slot the scanner can see;
        // that is the K132 unavailable path, not a capture failure. Either way entering must never
        // panic and must leave the ownership state consistent with the reported active flag.
        let guard = shared();
        set_full_mode(true);
        let entered = guard.enter(window.handle());
        // Whether or not interception is available, entering must never panic and must leave the
        // ownership state consistent with `active`.
        assert_eq!(entered, guard.active(), "active() reflects a successful enter");
        if entered {
            guard.leave_window(window.handle(), true);
            assert!(!guard.active(), "leaving releases capture");
        }
        set_full_mode(false);
    }

    #[rstest]
    #[case(false, false)]
    #[case(true, false)]
    fn capture_state_needs_a_window(#[case] full_mode: bool, #[case] expected: bool) {
        let _serial = CAPTURE.lock().unwrap_or_else(|e| e.into_inner());
        set_full_mode(full_mode);
        ACTIVE.store(false, Ordering::Release);
        WINDOW.store(0, Ordering::Release);
        assert_eq!(capturing(), expected, "no window means no capture");
        assert_eq!(FULL_MODE.load(Ordering::Acquire), full_mode);
    }
}
