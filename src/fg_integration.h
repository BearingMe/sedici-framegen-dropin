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
    // `handedRefs`: references to `native` the caller gives up (see Swapchain::wrap).
    bool create(IDXGISwapChain4* native, ID3D12CommandQueue* queue, ULONG handedRefs = 0) noexcept;
    void destroy() noexcept;

    IDXGISwapChain4* swapchain() const noexcept;
    Swapchain* wrapper() const noexcept { return m_wrapper.Get(); }

private:
    Microsoft::WRL::ComPtr<Swapchain> m_wrapper;
    bool m_registered = false;
};

// Observable lifecycle of a Session. Accepted work per state:
//
//   Empty     nothing created yet, or creation failed. Every call is refused.
//   Ready     created; generation is off or no swapchain is bound. configure and
//             prepare are accepted; no frames are generated.
//   Warmup    generation on and a swapchain bound, but no motion history yet, so
//             the next generated frame is withheld until a second prepare arrives.
//   Active    generation on, bound, with history: frames are generated.
//   Draining  destroy has begun: new configure/prepare are refused while the last
//             submitted work retires.
//   Stopped   destroyed. Every call is refused; the object can be freed.
enum class SessionState {
    Empty,
    Ready,
    Warmup,
    Active,
    Draining,
    Stopped,
};

const char* session_state_name(SessionState state) noexcept;

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
    SessionState state() const noexcept;
    Context& context() noexcept { return m_context; }
    const Context& context() const noexcept { return m_context; }

private:
    static bool record(void* user, ID3D12GraphicsCommandList* list, ID3D12Resource* real,
                       ID3D12Resource* output, D3D12_RESOURCE_STATES state) noexcept;
    static bool record_phases(void* user, ID3D12GraphicsCommandList* list, ID3D12Resource* real,
                              const GeneratePhase* phases, uint32_t count) noexcept;
    static void submitted(void* user, ID3D12Fence* fence, uint64_t value) noexcept;
    void detach() noexcept;

    Context m_context;
    Microsoft::WRL::ComPtr<Swapchain> m_swapchain;
    bool m_draining = false;
    bool m_stopped = false;
};

} // namespace sedici::fg
