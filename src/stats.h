#pragma once

#include "ffx_api.h"

// Called after a successful ffxDispatch. Counts prepare descs only.
void stats_on_dispatch(const ffxDispatchDescHeader* desc) noexcept;

// Called from the swapchain Present/Present1 hooks with a QPC timestamp.
void stats_on_present(uint64_t qpc) noexcept;

// Records that a frame generation swapchain context was created, or that a
// successful ffxConfigure reached the original DLL. Used only by the stall
// detector, which reports whether a stall coincided with one of these. `kind`
// must be a string literal; only the pointer is stored.
void stats_on_swapchain_event(const char* kind) noexcept;

// Read-only view for the overlay. Locks internally; safe from any thread.
uint64_t stats_stall_count() noexcept;
double stats_last_stall_ms() noexcept;
void stats_render_size(uint32_t& width, uint32_t& height) noexcept;
