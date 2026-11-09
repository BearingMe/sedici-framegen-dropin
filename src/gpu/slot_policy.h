#pragma once

#include <windows.h>
#include <cstdint>

namespace sedici::fg {

// Advance a bounded slot ring only after its current slot is reusable. `wait`
// returns an HRESULT and must not manufacture fence completion.
template <typename Wait>
HRESULT acquire_frame_slot(uint32_t& next, uint32_t count, uint32_t& selected, Wait&& wait) noexcept
{
    if (count == 0 || next >= count) return E_INVALIDARG;
    const uint32_t candidate = next;
    const HRESULT ready = wait(candidate);
    if (ready != S_OK) return FAILED(ready) ? ready : E_FAIL;
    selected = candidate;
    next = (candidate + 1) % count;
    return S_OK;
}

} // namespace sedici::fg
