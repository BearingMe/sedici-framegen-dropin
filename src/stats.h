#pragma once

#include "ffx_api.h"

// Called after a successful ffxDispatch. Counts prepare descs only.
void stats_on_dispatch(const ffxDispatchDescHeader* desc) noexcept;

// Called from the swapchain Present/Present1 hooks with a QPC timestamp.
void stats_on_present(uint64_t qpc) noexcept;
