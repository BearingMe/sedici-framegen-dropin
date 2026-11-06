#include "fg_integration.h"

#include "ffx_framegeneration.h"
#include "log.h"

#include <mutex>
#include <vector>

namespace sedici::fg {

namespace {

// The game holds the wrapper as an IDXGISwapChain4 and passes it back in
// configure and dispatch descs. Comparing resource identity avoids trusting a
// pointer the game could recycle.
std::mutex g_registry_mutex;
std::vector<Swapchain*> g_registry;

void register_wrapper(Swapchain* wrapper) noexcept
{
    if (!wrapper) return;
    std::lock_guard lock(g_registry_mutex);
    for (auto* existing : g_registry) {
        if (existing == wrapper) return;
    }
    g_registry.push_back(wrapper);
}

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

Swapchain* find_wrapper(IDXGISwapChain4* pointer) noexcept
{
    if (!pointer) return nullptr;
    std::lock_guard lock(g_registry_mutex);
    for (auto* candidate : g_registry) {
        if (static_cast<IDXGISwapChain4*>(candidate) == pointer) return candidate;
    }
    return nullptr;
}

} // namespace

WrapHandle::~WrapHandle() { destroy(); }

bool WrapHandle::create(IDXGISwapChain4* native, ID3D12CommandQueue* queue) noexcept
{
    if (m_wrapper) return false;
    Microsoft::WRL::ComPtr<Swapchain> wrapper;
    Swapchain* created = nullptr;
    if (FAILED(Swapchain::wrap(native, queue, &created)) || !created) return false;
    wrapper.Attach(created);
    register_wrapper(created);
    m_wrapper = std::move(wrapper);
    m_registered = true;
    return true;
}

void WrapHandle::destroy() noexcept
{
    if (!m_wrapper) return;
    // Stop driving generation before the wrapper goes away, so no in-flight
    // sink callback can reach a context that is being torn down.
    m_wrapper->set_generator(GenerateSink{});
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

bool Session::record(void* user, ID3D12GraphicsCommandList* list, ID3D12Resource* real,
                     ID3D12Resource* output, D3D12_RESOURCE_STATES state) noexcept
{
    auto* session = static_cast<Session*>(user);
    if (!session || !list || !real || !output) return false;
    // The wrapper generates before either native Present, so the HUD and colour
    // belong to the current prepare and are consumed before returning to the game.
    // The game's buffer is in PRESENT when recorded.
    const HudSource hud{real, D3D12_RESOURCE_STATE_PRESENT};
    // Generation is recorded on the same queue the game presents on, after the
    // prepare it submitted for this frame, so queue order already holds.
    session->m_context.note_ordered_submission();
    return session->m_context.generate(list, output, state, &hud);
}

void Session::submitted(void* user, ID3D12Fence* fence, uint64_t value) noexcept
{
    auto* session = static_cast<Session*>(user);
    if (!session) return;
    (void)session->m_context.track_submission(fence, value);
}

void Session::detach() noexcept
{
    if (m_swapchain) m_swapchain->set_generator(GenerateSink{});
    m_swapchain.Reset();
}

bool Session::create(const ffxApiHeader* createDesc) noexcept
{
    if (m_context.ready()) return false;
    return m_context.create(createDesc);
}

bool Session::configure(const ffxApiHeader* configureDesc) noexcept
{
    if (!m_context.configure(configureDesc)) return false;

    // Find the swapchain this configure names, if any. Resolving it by identity
    // keeps a recycled pointer from binding a context to a dead wrapper.
    Swapchain* target = nullptr;
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

    if (target != m_swapchain.Get()) {
        detach();
        if (target) {
            target->AddRef();
            m_swapchain.Attach(target);
        }
    }

    if (m_swapchain) {
        if (m_context.enabled()) {
            const GenerateSink sink{this, &Session::record, &Session::submitted};
            m_swapchain->set_generator(sink);
        } else {
            m_swapchain->set_generator(GenerateSink{});
        }
    }
    return true;
}

bool Session::prepare(const ffxApiHeader* prepareDesc) noexcept
{
    return m_context.prepare(prepareDesc);
}

bool Session::destroy() noexcept
{
    if (!m_context.ready()) return true;
    // Stop driving generation first, then let the last submitted frame retire so
    // teardown never releases resources the GPU is still using.
    detach();
    (void)m_context.wait_for_retirement(200);
    return m_context.destroy();
}

} // namespace sedici::fg
