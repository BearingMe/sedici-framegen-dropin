#pragma once

#include "ffx_api.h"

#include <string>

// Called after a successful ffxDispatch. Counts prepare descs only.
void stats_on_dispatch(const ffxDispatchDescHeader* desc) noexcept;

// Called from the swapchain Present/Present1 hooks with a QPC timestamp.
void stats_on_present(uint64_t qpc) noexcept;

// Records that a frame generation swapchain context was created, or that a
// successful ffxConfigure reached the original DLL. Used only by the stall
// detector, which reports whether a stall coincided with one of these. `kind`
// must be a string literal; only the pointer is stored.
void stats_on_swapchain_event(const char* kind) noexcept;
// Number of swapchain create/resize events so far (counted at any log level), so a reporter can
// tell that the swapchain changed since it last looked.
uint64_t stats_swapchain_event_count() noexcept;

// An optional extra line written after each periodic report (the "diag:" line,
// registered by the DLL entry; diagnostics must not depend on the modules that
// know the parked-object counters). Empty string = nothing to say.
using StatsExtraLineFn = std::string (*)() noexcept;
void stats_set_extra_line(StatsExtraLineFn fn) noexcept;

// Read-only view for the overlay. Locks internally; safe from any thread.
uint64_t stats_stall_count() noexcept;

// Real frames the presenter dropped because a later output was already due (K125).
// The overlay's Real fps counts shown frames; this is the difference to the game's own count.
void stats_on_real_dropped() noexcept;
uint64_t stats_real_dropped() noexcept;
double stats_last_stall_ms() noexcept;
void stats_render_size(uint32_t& width, uint32_t& height) noexcept;
