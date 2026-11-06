#pragma once

#include "ffx_framegeneration.h"

#include <windows.h>

namespace sedici {

// Records the game window from the frame-generation swapchain. Installs the
// lightweight input hook that detects the hotkey; no ImGui work happens here.
void ui_capture_window(HWND hwnd) noexcept;

// Called from the present callback after the output image has been composed.
// Draws Mini/Full when enabled, and does nothing when the mode is Off.
void ui_on_present(const ffxCallbackDescFrameGenerationPresent* params) noexcept;

// Permanent session failure: stop rendering and input capture, including hotkeys.
void ui_disable(const char* reason) noexcept;

} // namespace sedici
