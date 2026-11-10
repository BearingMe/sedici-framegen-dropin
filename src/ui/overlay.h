#pragma once

#include "ffx_framegeneration.h"

#include <cstddef>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;
#include <cstdint>

// Output cadence recording and overlay drawing for Sedici's own swapchain.
//
// The wrapper calls `overlay_draw_output` once per presented output frame, real
// or generated, so the overlay is drawn after generation and is never
// interpolated, and `overlay_output_presented` after each successful Present.
// `overlay_on_present` records output timing from the fields it is given.

void overlay_on_present(const ffxCallbackDescFrameGenerationPresent* params) noexcept;

// Sedici's own frame-generation swapchain stands in for AMD's callback with
// these two (fg::OutputHooks). `overlay_draw_output` records the overlay onto
// a native backbuffer in PRESENT state on the list executed right before it is
// presented; `overlay_output_presented` follows each successful Present.
void overlay_draw_output(ID3D12Device* device, ID3D12GraphicsCommandList* list, ID3D12Resource* backbuffer,
                         bool generated) noexcept;
void overlay_output_presented(bool generated) noexcept;
