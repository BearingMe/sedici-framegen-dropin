#pragma once

#include <windows.h>
#include <dxgi.h>
#include <cstdint>
#include <limits>

namespace sedici::fg {

enum class FenceProgress { Pending, Complete, DeviceRemoved };

inline FenceProgress classify_fence(uint64_t completed, uint64_t target) noexcept
{
    if (completed == std::numeric_limits<uint64_t>::max()) return FenceProgress::DeviceRemoved;
    return completed >= target ? FenceProgress::Complete : FenceProgress::Pending;
}

// Some drivers may return S_OK from GetDeviceRemovedReason before removal is
// observable there. A removed-fence sentinel must never become success.
inline HRESULT removed_device_error(HRESULT reason) noexcept
{
    return FAILED(reason) ? reason : DXGI_ERROR_DEVICE_REMOVED;
}

// A retirement wait is valid only after its queue signal was accepted. Keep the
// decision independent of D3D12 so both failure and success can be exercised
// without manufacturing a device failure.
template <typename Signal, typename Wait>
HRESULT signal_then_wait(Signal&& signal, Wait&& wait) noexcept
{
    const HRESULT signaled = signal();
    if (FAILED(signaled)) return signaled;
    return wait();
}

} // namespace sedici::fg
