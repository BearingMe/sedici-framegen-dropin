#pragma once

#include "ffx_api.h"
#include "fg_presenter.h"

// Routing of the five exports between AMD's original DLL and Sedici's own frame
// generation path.
//
// Selected once, at the first create, from `fg.mode`:
//
//   "amd"    (default) every call is forwarded exactly as before. Sedici never
//            owns a context and the shipped behaviour is unchanged.
//   "sedici" Sedici creates the generator context and the swapchain wrapper.
//            There is no per-call fallback: a context cannot be handed to AMD
//            mid-life, so an unsupported call is answered by Sedici and logged.
//
// Every function returns true when Sedici answered the call and false when the
// caller must forward it to the original DLL. A call is only answered here when
// it names a context Sedici itself created, so a context AMD owns can never be
// routed into Sedici by mistake.
namespace sedici::fg {

bool routing_enabled() noexcept;

bool route_create(ffxContext* context, const ffxApiHeader* desc, ffxReturnCode_t* result) noexcept;
bool route_configure(ffxContext* context, const ffxApiHeader* desc, ffxReturnCode_t* result) noexcept;
bool route_dispatch(ffxContext* context, const ffxApiHeader* desc, ffxReturnCode_t* result) noexcept;
bool route_query(ffxContext* context, const ffxApiHeader* desc, ffxReturnCode_t* result) noexcept;
bool route_destroy(ffxContext* context, ffxReturnCode_t* result) noexcept;

// Installs a hook that runs once per application Present on every swapchain
// Sedici wraps. The DLL registers the real-frame limiter here; the offline host
// leaves it unset, so nothing in the wrapper depends on the limiter.
void route_set_present_hook(void (*hook)(uint64_t now_qpc_ticks)) noexcept;

// Overlay drawing and output-frame timing for every swapchain Sedici wraps,
// standing in for AMD's present callback. Registered by the DLL only.
void route_set_output_hooks(const OutputHooks& hooks) noexcept;

} // namespace sedici::fg
