#pragma once

#include "ffx_api.h"
#include "runtime/context.h"
#include "presentation/swapchain.h"

#include <d3d12.h>
#include <mutex>
#include <memory>

// Binds one FFX frame generation context to one Sedici-wrapped swapchain.
//
// Split out of the DLL exports so the same production path (desc adapter, stage
// 1, HUD composite and presentation schedule) can be exercised offline. The
// exports route every recognized context here; Sedici is the only backend.
//
// Ownership rules:
//   * A wrapped swapchain is registered when created and unregistered when its
//     WrapHandle is destroyed. Game-held COM references may survive, but the
//     generator gate is closed before unregistration.
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
//   Ready     created but not yet configured. Configure and prepare may be
//             accepted; no frames are generated.
//   Suspended configured with generation off; configure and prepare may be accepted.
//   Degraded  generation requested but no live wrapper is bound.
//   DeviceLost the D3D12 device is removed; new configure/prepare are refused.
//   Warmup    generation on and a swapchain bound, but no motion history yet, so
//             the next generated frame is withheld until a second prepare arrives.
//   Active    generation on, bound, with history: frames are generated.
//   Draining  destroy has begun: new configure/prepare are refused while the last
//             submitted work retires.
//   Stopped   destroyed. Every call is refused; the object can be freed.
enum class SessionState {
    Empty,
    Ready,
    Suspended,
    Degraded,
    DeviceLost,
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

    // Called before publication; weak ownership avoids an owner/session cycle.
    void set_callback_lifetime(std::weak_ptr<void> lifetime) noexcept { m_callback_lifetime = std::move(lifetime); }
    bool create(const ffxApiHeader* createDesc) noexcept;
    bool configure(const ffxApiHeader* configureDesc) noexcept;
    bool prepare(const ffxApiHeader* prepareDesc) noexcept;
    bool destroy() noexcept;

    bool ready() const noexcept;
    const char* last_error() const noexcept;
    SessionState state() const noexcept;
    // True when no submitted GPU work can still reference this session.
    bool retired() const noexcept;
    // Offline inspection only: caller must exclude all concurrent session work.
    Context& context() noexcept { return m_context; }
    const Context& context() const noexcept { return m_context; }

private:
    static bool record(void* user, ID3D12GraphicsCommandList* list, ID3D12Resource* real, ID3D12Resource* output,
                       D3D12_RESOURCE_STATES state) noexcept;
    static bool record_phases(void* user, ID3D12GraphicsCommandList* list, ID3D12Resource* real,
                              const GeneratePhase* phases, uint32_t count) noexcept;
    static bool submitted(void* user, ID3D12Fence* fence, uint64_t value) noexcept;
    static bool needs_submission(void* user) noexcept;
    static void abandoned(void* user) noexcept;
    void detach() noexcept;

    // Public operations serialize on m_operation_mutex. Present callbacks never
    // take it: they hold the wrapper mutex and take only m_context_mutex.
    // Never hold m_context_mutex while calling into the wrapper.
    mutable std::mutex m_operation_mutex;
    mutable std::mutex m_context_mutex;
    Context m_context;
    std::weak_ptr<void> m_callback_lifetime;
    Microsoft::WRL::ComPtr<Swapchain> m_swapchain;
    bool m_configured = false;
    bool m_draining = false;
    bool m_stopped = false;
};

} // namespace sedici::fg
