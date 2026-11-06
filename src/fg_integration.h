#pragma once

#include "ffx_api.h"
#include "fg_context.h"
#include "fg_swapchain.h"

#include <d3d12.h>

// Binds one FFX frame generation context to one Sedici-wrapped swapchain.
//
// Split out of the DLL exports so the whole production path (desc adapter, stage
// 1, HUD composite, presentation schedule) can be exercised offline. Nothing here
// is reachable from the five exports yet; exports still forward to the original
// DLL, which keeps the shipped behaviour unchanged while this is finished.
//
// Ownership rules:
//   * A wrapped swapchain is registered when created and unregistered only after
//     its last reference is released here.
//   * A frame generation session never frees a swapchain it did not wrap.
//   * Neither object reaches into the other's GPU work; the swapchain drives the
//     recording and reports the fence it submitted, so retirement stays with the
//     object that owns the command list.

namespace sedici::fg {

// Wraps a native swapchain and registers it so a later frame generation
// configure can find it by the pointer the game holds.
class WrapHandle {
public:
    WrapHandle() = default;
    ~WrapHandle();
    WrapHandle(const WrapHandle&) = delete;
    WrapHandle& operator=(const WrapHandle&) = delete;

    // On success `*out` receives the game-facing wrapper and `swapchain()`
    // returns it. On failure nothing is registered and `*out` is untouched.
    bool create(IDXGISwapChain4* native, ID3D12CommandQueue* queue) noexcept;
    void destroy() noexcept;

    IDXGISwapChain4* swapchain() const noexcept;
    Swapchain* wrapper() const noexcept { return m_wrapper.Get(); }

private:
    Microsoft::WRL::ComPtr<Swapchain> m_wrapper;
    bool m_registered = false;
};

// One frame generation context plus, once configured, the swapchain it presents
// through.
class Session {
public:
    Session() = default;
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    bool create(const ffxApiHeader* createDesc) noexcept;
    bool configure(const ffxApiHeader* configureDesc) noexcept;
    bool prepare(const ffxApiHeader* prepareDesc) noexcept;
    bool destroy() noexcept;

    bool ready() const noexcept { return m_context.ready(); }
    Context& context() noexcept { return m_context; }
    const Context& context() const noexcept { return m_context; }

private:
    static bool record(void* user, ID3D12GraphicsCommandList* list, ID3D12Resource* real,
                       ID3D12Resource* output, D3D12_RESOURCE_STATES state) noexcept;
    static void submitted(void* user, ID3D12Fence* fence, uint64_t value) noexcept;
    void detach() noexcept;

    Context m_context;
    Microsoft::WRL::ComPtr<Swapchain> m_swapchain;
};

} // namespace sedici::fg
