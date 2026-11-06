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
// True when the executable's cursor imports could be intercepted. Does not
// change capture state.
bool ui_input_available() noexcept;

// Maps a client-area point to output pixels. The overlay is drawn at the
// output resolution while Windows reports input in client coordinates, so the
// two only match when the window is not scaled.
POINT ui_input_map_point(LONG x, LONG y, LONG client_width, LONG client_height,
                         LONG output_width, LONG output_height) noexcept;
}
