#pragma once

#include "ffx_api.h"

// Installs a vtable hook on any swapchain returned by a create desc in the
// chain. No-op when logging is disabled or when no swapchain desc is present.
void present_hook_maybe_install(const ffxCreateContextDescHeader* desc) noexcept;
