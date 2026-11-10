#include "runtime/integration.h"

#include "ffx_framegeneration.h"
#include "diagnostics/bookkeeping_faults.h"
#include "diagnostics/log.h"

#include <cstdio>
#include <list>
#include <mutex>
#include <new>

namespace sedici::fg {

namespace {

// The game holds the wrapper as an IDXGISwapChain4 and passes it back in
// configure and dispatch descs. Comparing resource identity avoids trusting a
// pointer the game could recycle.
std::mutex g_registry_mutex;
std::list<Swapchain*> g_registry;

// Reserve before wrap can consume the native swapchain. Null entries are
// invisible to lookup and erased on every unsuccessful creation path.
class PendingWrapperSlot {
public:
    bool reserve() noexcept
    {
        try {
            std::lock_guard lock(g_registry_mutex);
            if (consume_bookkeeping_fault(BookkeepingFault::WrapperRegistryAllocation)) throw std::bad_alloc();
            m_slot = g_registry.insert(g_registry.end(), nullptr);
            m_pending = true;
            return true;
        } catch (...) {
            return false;
        }
    }
    void publish(Swapchain* wrapper) noexcept
    {
        std::lock_guard lock(g_registry_mutex);
        *m_slot = wrapper;
        m_pending = false;
    }
    ~PendingWrapperSlot()
    {
        if (!m_pending) return;
        std::lock_guard lock(g_registry_mutex);
        g_registry.erase(m_slot);
    }

private:
    std::list<Swapchain*>::iterator m_slot{};
    bool m_pending = false;
};

void unregister_wrapper(Swapchain* wrapper) noexcept
{
    if (!wrapper) return;
    std::lock_guard lock(g_registry_mutex);
    for (auto it = g_registry.begin(); it != g_registry.end(); ++it) {
        if (*it == wrapper) {
            g_registry.erase(it);
            return;
        }
    }
}

// Returns an owning reference taken while the registry lock is held. A wrapper is
// registered only while its WrapHandle holds a reference, and unregistered before
// that reference is dropped, so the AddRef here can never touch a destroyed object.
Microsoft::WRL::ComPtr<Swapchain> find_wrapper(IDXGISwapChain4* pointer) noexcept
{
    Microsoft::WRL::ComPtr<Swapchain> found;
    if (!pointer) return found;
    std::lock_guard lock(g_registry_mutex);
    for (auto* candidate : g_registry) {
        if (static_cast<IDXGISwapChain4*>(candidate) == pointer) {
            found = candidate;
            break;
        }
    }
    return found;
}

} // namespace

WrapHandle::~WrapHandle()
{
    destroy();
}

bool WrapHandle::create(IDXGISwapChain4* native, ID3D12CommandQueue* queue, ULONG handedRefs) noexcept
{
    if (m_wrapper) return false;
    PendingWrapperSlot slot;
    if (!slot.reserve()) return false; // No native ownership has changed.
    Microsoft::WRL::ComPtr<Swapchain> wrapper;
    Swapchain* created = nullptr;
    const HRESULT wrapped = Swapchain::wrap(native, queue, &created, nullptr, handedRefs);
    if (FAILED(wrapped) || !created) {
        char text[64];
        std::snprintf(text, sizeof(text), "fg: swapchain wrap failed hr=0x%08lX",
                      static_cast<unsigned long>(wrapped));
        log_error(text);
        return false;
    }
    wrapper.Attach(created);
    m_wrapper = std::move(wrapper); // Registry is published only while owned.
    slot.publish(created);
    m_registered = true;
    return true;
}

void WrapHandle::destroy() noexcept
{
    if (!m_wrapper) return;
    // Stop driving generation before the wrapper goes away, so no in-flight
    // sink callback can reach a context that is being torn down.
    m_wrapper->close_generator();
    if (m_registered) unregister_wrapper(m_wrapper.Get());
    m_registered = false;
    m_wrapper.Reset();
}
IDXGISwapChain4* WrapHandle::swapchain() const noexcept
{
    return m_wrapper ? static_cast<IDXGISwapChain4*>(m_wrapper.Get()) : nullptr;
}

Session::~Session()
{
    detach();
    if (m_context.ready()) (void)m_context.destroy();
}

bool Session::record(void* user, ID3D12GraphicsCommandList* list, ID3D12Resource* real, ID3D12Resource* output,
                     D3D12_RESOURCE_STATES state) noexcept
{
    auto* session = static_cast<Session*>(user);
    if (!session || !list || !real || !output) return false;
    std::lock_guard lock(session->m_context_mutex);
    if (session->m_draining || session->m_stopped || session->m_context.device_lost()) return false;
    run_context_record_hook(); // Unarmed outside the offline test host.
    // The wrapper generates before either native Present, so the HUD and colour
    // belong to the current prepare and are consumed before returning to the game.
    // The game's buffer is in PRESENT when recorded.
    const HudSource hud{real, D3D12_RESOURCE_STATE_PRESENT};
    // Generation is recorded on the same queue the game presents on, after the
    // prepare it submitted for this frame, so queue order already holds.
    session->m_context.note_ordered_submission();
    return session->m_context.generate(list, output, state, &hud);
}

bool Session::record_phases(void* user, ID3D12GraphicsCommandList* list, ID3D12Resource* real,
                            const GeneratePhase* phases, uint32_t count) noexcept
{
    auto* session = static_cast<Session*>(user);
    if (!session || !list || !real || !phases || count == 0) return false;
    std::lock_guard lock(session->m_context_mutex);
    if (session->m_draining || session->m_stopped || session->m_context.device_lost()) return false;
    run_context_record_hook();
    // Same ordering and HUD contract as record(), for several phases at once.
    const HudSource hud{real, D3D12_RESOURCE_STATE_PRESENT};
    session->m_context.note_ordered_submission();
    return session->m_context.generate(list, phases, count, &hud);
}

bool Session::submitted(void* user, ID3D12Fence* fence, uint64_t value) noexcept
{
    auto* session = static_cast<Session*>(user);
    if (!session) return false;
    std::lock_guard lock(session->m_context_mutex);
    if (consume_bookkeeping_fault(BookkeepingFault::SubmissionRegistration)) return false;
    return session->m_context.track_submission(fence, value);
}

bool Session::needs_submission(void* user) noexcept
{
    auto* session = static_cast<Session*>(user);
    if (!session) return false;
    std::lock_guard lock(session->m_context_mutex);
    return session->m_context.has_untracked_work();
}

void Session::abandoned(void* user) noexcept
{
    auto* session = static_cast<Session*>(user);
    if (!session) return;
    std::lock_guard lock(session->m_context_mutex);
    session->m_context.abandon_generation();
}

void Session::detach() noexcept
{
    if (m_swapchain) m_swapchain->clear_generator_if_owner(this);
    m_swapchain.Reset();
}

const char* session_state_name(SessionState state) noexcept
{
    switch (state) {
    case SessionState::Empty:
        return "empty";
    case SessionState::Ready:
        return "ready";
    case SessionState::Suspended:
        return "suspended";
    case SessionState::Degraded:
        return "degraded";
    case SessionState::DeviceLost:
        return "device-lost";
    case SessionState::Warmup:
        return "warmup";
    case SessionState::Active:
        return "active";
    case SessionState::Draining:
        return "draining";
    case SessionState::Stopped:
        return "stopped";
    }
    return "unknown";
}

bool Session::ready() const noexcept
{
    std::lock_guard lock(m_context_mutex);
    return m_context.ready() && !m_context.device_lost() && !m_draining && !m_stopped;
}

const char* Session::last_error() const noexcept
{
    std::lock_guard lock(m_context_mutex);
    return m_context.device_lost() ? "D3D12 device removed" : m_context.last_error();
}

bool Session::retired() const noexcept
{
    std::lock_guard lock(m_context_mutex);
    return m_stopped || m_context.retired();
}

SessionState Session::state() const noexcept
{
    std::lock_guard operation(m_operation_mutex);
    std::lock_guard context(m_context_mutex);
    if (m_stopped) return SessionState::Stopped;
    if (m_draining) return SessionState::Draining;
    if (m_context.device_lost()) return SessionState::DeviceLost;
    if (!m_context.ready()) return SessionState::Empty;
    if (!m_configured) return SessionState::Ready;
    if (!m_context.enabled()) return SessionState::Suspended;
    if (!m_swapchain) return SessionState::Degraded;
    return m_context.have_history() ? SessionState::Active : SessionState::Warmup;
}

bool Session::create(const ffxApiHeader* createDesc) noexcept
{
    std::lock_guard operation(m_operation_mutex);
    std::lock_guard context(m_context_mutex);
    if (m_draining || m_stopped) return false;
    if (m_context.ready()) return false;
    return m_context.create(createDesc);
}

bool Session::configure(const ffxApiHeader* configureDesc) noexcept
{
    std::lock_guard operation(m_operation_mutex);
    bool enabled = false;
    {
        std::lock_guard context(m_context_mutex);
        if (!m_context.ready() || m_draining || m_stopped || m_context.device_lost()) return false;
        if (!m_context.configure(configureDesc)) return false;
        m_configured = true;
        enabled = m_context.enabled();
    }

    // Find the swapchain this configure names, if any. Resolving it by identity
    // keeps a recycled pointer from binding a context to a dead wrapper.
    Microsoft::WRL::ComPtr<Swapchain> target;
    for (const ffxApiHeader* item = configureDesc; item; item = item->pNext) {
        if (item->type == FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION) {
            const auto* desc = reinterpret_cast<const ffxConfigureDescFrameGeneration*>(item);
            target = find_wrapper(static_cast<IDXGISwapChain4*>(desc->swapChain));
            break;
        }
        if (item->type == FFX_API_FRAME_GENERATION_CONFIG) {
            const auto* desc = reinterpret_cast<const FfxFrameGenerationConfig*>(item);
            target = find_wrapper(static_cast<IDXGISwapChain4*>(desc->swapChain));
            break;
        }
    }

    // Offline tests may pause here to force destruction after lookup but before
    // attachment. Normal runtime never arms this internal, non-exported hook.
    if (target) run_wrapper_lookup_hook();
    if (target.Get() != m_swapchain.Get()) {
        detach();
        m_swapchain = std::move(target);
    }

    if (m_swapchain) {
        if (enabled) {
            const GenerateSink sink{this,
                                    &Session::record,
                                    &Session::record_phases,
                                    &Session::submitted,
                                    &Session::abandoned,
                                    m_callback_lifetime.lock(),
                                    &Session::needs_submission};
            if (!m_swapchain->set_generator(sink)) {
                // Destruction may have closed this wrapper after lookup.
                m_swapchain.Reset();
            }
        } else {
            // Another Session may have replaced our sink since the previous
            // configure; disabling this one must not detach the other owner.
            (void)m_swapchain->clear_generator_if_owner(this);
        }
    }
    return true;
}

bool Session::prepare(const ffxApiHeader* prepareDesc) noexcept
{
    std::lock_guard operation(m_operation_mutex);
    std::lock_guard context(m_context_mutex);
    if (!m_context.ready() || m_draining || m_stopped || m_context.device_lost()) return false;
    return m_context.prepare(prepareDesc);
}

bool Session::destroy() noexcept
{
    std::lock_guard operation(m_operation_mutex);
    {
        std::lock_guard context(m_context_mutex);
        if (m_stopped) return true;
        if (!m_context.ready()) {
            m_stopped = true;
            return true;
        }
        m_draining = true;
    }
    // Do not hold m_context_mutex while waiting for the wrapper: an in-flight
    // callback holds the wrapper mutex and may need the context mutex.
    detach();
    // Poll retirement with the context mutex released between looks (K104): the
    // wrapper's untracked-sink resolution (K103) takes this mutex under the
    // wrapper mutex, so holding it for the whole window could stall a Present.
    // New work is already refused (m_draining), so the state cannot advance
    // except through that registration or the fence completing.
    // 1000 ms, not 200: at game exit by Alt+F4 the 200 ms window was too short and
    // the context destroy returned an error (K127). A hung GPU still ends in the
    // refusal and the parked owner.
    constexpr ULONGLONG kRetireWaitMs = 1000;
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        {
            std::lock_guard context(m_context_mutex);
            if (m_context.retired() || GetTickCount64() - start >= kRetireWaitMs) break;
        }
        Sleep(1);
    }
    std::lock_guard context(m_context_mutex);
    const bool ok = m_context.destroy();
    // A refused destroy (work still in flight) leaves the session Draining: it
    // keeps refusing new work and the owner must retain it until it retires.
    if (ok) {
        m_draining = false;
        m_stopped = true;
    }
    return ok;
}

} // namespace sedici::fg
