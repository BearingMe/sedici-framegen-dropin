#pragma once

#include "ffx_framegeneration.h"

#include <windows.h>
#include <cstdint>

namespace sedici {

// Records the game window from the frame-generation swapchain. Installs the
// lightweight input hook that detects the hotkey; no ImGui work happens here.
void ui_capture_window(HWND hwnd) noexcept;

// Called from the present callback after the output image has been composed.
// Draws Mini/Full when enabled, and does nothing when the mode is Off.
void ui_on_present(const ffxCallbackDescFrameGenerationPresent* params) noexcept;

struct UiRenderStatus {
    bool ready = false;
    uint64_t realDraws = 0;
    uint64_t generatedDraws = 0;
};
// Snapshot of successfully recorded overlay draws; it does not claim the GPU
// displayed them. Intended for status and offline integration checks.
UiRenderStatus ui_render_status() noexcept;

// Permanent session failure: stop rendering and input capture, including hotkeys.
void ui_disable(const char* reason) noexcept;

} // namespace sedici
