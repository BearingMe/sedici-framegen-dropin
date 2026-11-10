#pragma once
#include <windows.h>

namespace sedici {
// Called on the window thread. Interception is confined to the executable's
// cursor imports and is inert except during foreground Full capture.
bool ui_input_enter(HWND hwnd) noexcept;
void ui_input_leave(bool restore_clip) noexcept;
void ui_input_leave_window(HWND hwnd, bool restore_clip) noexcept;
void ui_input_disable() noexcept;
bool ui_input_active() noexcept;
// True when at least one SetCursorPos or ClipCursor import of the executable or of a
// non-system module could be intercepted. Rescans (at most every two seconds) so a module
// loaded or a delay-load slot resolved since the last call is found. Does not change
// capture state.
bool ui_input_available() noexcept;

// What the last scan found, for the log and the tests.
struct InterceptionSummary {
    unsigned modulesScanned = 0;
    unsigned setCursorPosSlots = 0;
    unsigned clipCursorSlots = 0;
    unsigned skippedSlots = 0; // an import slot that held something other than the real call
};
InterceptionSummary ui_input_interception_summary() noexcept;

// Maps a client-area point to output pixels. The overlay is drawn at the
// output resolution while Windows reports input in client coordinates, so the
// two only match when the window is not scaled.
POINT ui_input_map_point(LONG x, LONG y, LONG client_width, LONG client_height, LONG output_width,
                         LONG output_height) noexcept;
} // namespace sedici
