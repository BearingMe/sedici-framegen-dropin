#pragma once

#include "ffx_api.h"
#include "presentation/presenter.h"

// Routing of the five exports to Sedici's own frame generation.
//
// Sedici is the only backend: it creates the generator context and the swapchain
// wrapper, and there is no second implementation to hand a call to.
//
// Every function returns true when Sedici recognised the handle or descriptor and
// answered the call, and false when it did not; the exports then answer with an
// error code. Issued Sedici tokens remain locally identifiable after destroy and
// are rejected. Handles outside the reserved token namespace are never ours; the
// owner explicitly approved the namespace-separation assumption and exhaustion
// policy. `GetVersions` queries need no context and are answered here.
namespace sedici::fg {

// True for any issued Sedici token, including one already destroyed.
bool route_is_sedici_token(ffxContext* context) noexcept;
// True when at least one live Sedici-owned session was last configured with
// frame generation enabled. Used for the limiter and menu status.
bool route_any_generation_enabled() noexcept;

bool route_create(ffxContext* context, const ffxApiHeader* desc, ffxReturnCode_t* result) noexcept;
bool route_configure(ffxContext* context, const ffxApiHeader* desc, ffxReturnCode_t* result) noexcept;
bool route_dispatch(ffxContext* context, const ffxApiHeader* desc, ffxReturnCode_t* result) noexcept;
bool route_query(ffxContext* context, const ffxApiHeader* desc, ffxReturnCode_t* result) noexcept;
bool route_destroy(ffxContext* context, ffxReturnCode_t* result) noexcept;

// Owners kept alive because their GPU work had not retired when the game
// destroyed them. They are released on a later router call once it has (K41).
unsigned route_parked_count() noexcept;

// Installs a hook that runs once per application Present on every swapchain
// Sedici wraps. The DLL registers the real-frame limiter here; the offline host
// leaves it unset, so nothing in the wrapper depends on the limiter.
void route_set_present_hook(void (*hook)(uint64_t now_qpc_ticks)) noexcept;

// Overlay drawing and output-frame timing for every swapchain Sedici wraps,
// standing in for AMD's present callback. Registered by the DLL only.
void route_set_output_hooks(const OutputHooks& hooks) noexcept;

} // namespace sedici::fg
