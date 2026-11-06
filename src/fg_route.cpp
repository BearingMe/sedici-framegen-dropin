#include "fg_route.h"

#include "config.h"
#include "ffx_api_framegeneration_dx12.h"
#include "ffx_framegeneration.h"
#include "fg_integration.h"
#include "log.h"

#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace sedici::fg {

namespace {

enum class Kind {
    Session,
    Wrap,
};

struct Owned {
    Kind kind = Kind::Session;
    std::unique_ptr<Session> session;
    std::unique_ptr<WrapHandle> wrap;
};

std::mutex g_owned_mutex;
std::map<void*, Owned> g_owned;
void (*g_present_hook)(uint64_t) = nullptr;

constexpr size_t kMaxChainItems = 64;

template <typename T>
const T* find_desc(const ffxApiHeader* desc, uint64_t type) noexcept
{
    size_t visited = 0;
    for (const ffxApiHeader* item = desc; item && visited < kMaxChainItems;
         item = item->pNext, ++visited) {
        if (item->type == type) return reinterpret_cast<const T*>(item);
    }
    return nullptr;
}

void log_route(const char* what, const std::string& detail) noexcept
{
    try {
        log_event(1, std::string("fg: ") + what + detail);
    } catch (...) {
    }
}

// Only ever called with g_owned_mutex held.
Owned* find_owned(void* handle) noexcept
{
    if (!handle) return nullptr;
    const auto it = g_owned.find(handle);
    return it == g_owned.end() ? nullptr : &it->second;
}

ffxReturnCode_t failure(const char* reason) noexcept
{
    log_route("rejected: ", reason);
    return FFX_API_RETURN_ERROR;
}

} // namespace

bool routing_enabled() noexcept
{
    return sedici::settings().fg.mode == sedici::FrameGenerationMode::Sedici;
}

void route_set_present_hook(void (*hook)(uint64_t)) noexcept
{
    std::lock_guard lock(g_owned_mutex);
    g_present_hook = hook;
    // Already-wrapped swapchains take the hook too: the configuration is read
    // once, but the hook may be installed before or after the first wrap.
    for (auto& entry : g_owned) {
        if (entry.second.kind == Kind::Wrap && entry.second.wrap && entry.second.wrap->wrapper()) {
            entry.second.wrap->wrapper()->set_present_hook(hook);
        }
    }
}

bool route_create(ffxContext* context, const ffxApiHeader* desc, ffxReturnCode_t* result) noexcept
{
    if (!result || !context || !desc) return false;
    if (!routing_enabled()) return false;

    const auto* wrap = find_desc<ffxCreateContextDescFrameGenerationSwapChainWrapDX12>(
        desc, FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WRAP_DX12);
    const auto* frameGeneration = find_desc<ffxCreateContextDescFrameGeneration>(
        desc, FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION);
    if (!wrap && !frameGeneration) return false; // not a call this path can own

    std::lock_guard lock(g_owned_mutex);
    if (wrap) {
        if (!wrap->swapchain || !wrap->swapchain[0] || !wrap->gameQueue) {
            *result = failure("swapchain wrap without a swapchain or queue");
            return true;
        }
        auto owned = std::make_unique<WrapHandle>();
        IDXGISwapChain4* native = wrap->swapchain[0];
        if (!owned->create(native, wrap->gameQueue)) {
            // Nothing was replaced: the game keeps its own swapchain pointer.
            *result = failure("swapchain wrap failed");
            return true;
        }
        *wrap->swapchain = owned->swapchain();
        // Handing the pointer to the game transfers a reference to it. Without
        // this the game's Release() and our own ComPtr share a single count, and
        // the wrapper is destroyed with a live owner still holding the address.
        (*wrap->swapchain)->AddRef();
        owned->wrapper()->set_present_hook(g_present_hook);
        // The game needs a handle for this context too: it passes it back to
        // configure (UI registration) and destroy. Returning OK with a null
        // handle left the caller dereferencing null immediately afterwards.
        *context = owned.get();
        g_owned.emplace(owned.get(), Owned{Kind::Wrap, nullptr, std::move(owned)});
        *result = FFX_API_RETURN_OK;
        log_route("created swapchain wrapper", "");
        return true;
    }

    auto owned = std::make_unique<Session>();
    if (!owned->create(desc)) {
        *result = failure(owned->context().last_error());
        return true;
    }
    *context = owned.get();
    g_owned.emplace(owned.get(), Owned{Kind::Session, std::move(owned), nullptr});
    *result = FFX_API_RETURN_OK;
    log_route("created frame generation context", "");
    return true;
}

bool route_configure(ffxContext* context, const ffxApiHeader* desc, ffxReturnCode_t* result) noexcept
{
    if (!result || !context || !*context || !desc) return false;
    std::lock_guard lock(g_owned_mutex);
    Owned* owned = find_owned(*context);
    if (!owned) return false; // AMD's context: leave it alone

    if (owned->kind == Kind::Wrap) {
        // Sedici owns output pacing, so a request to tune AMD's swapchain pacing
        // has no meaning here. Accept it so the game's own call sequence is
        // unchanged, and log it once per configuration.
        log_event(2, "fg: swapchain configure accepted (Sedici owns pacing)");
        *result = FFX_API_RETURN_OK;
        return true;
    }
    *result = owned->session->configure(desc) ? FFX_API_RETURN_OK
                                              : failure(owned->session->context().last_error());
    return true;
}

bool route_dispatch(ffxContext* context, const ffxApiHeader* desc, ffxReturnCode_t* result) noexcept
{
    if (!result || !context || !*context || !desc) return false;
    std::lock_guard lock(g_owned_mutex);
    Owned* owned = find_owned(*context);
    if (!owned) return false;

    if (owned->kind == Kind::Wrap) {
        *result = failure("dispatch on the swapchain wrapper");
        return true;
    }
    if (desc->type != FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE &&
        desc->type != FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2) {
        *result = failure("unsupported frame generation dispatch type");
        return true;
    }
    *result = owned->session->prepare(desc) ? FFX_API_RETURN_OK
                                            : failure(owned->session->context().last_error());
    return true;
}

bool route_query(ffxContext* context, const ffxApiHeader* desc, ffxReturnCode_t* result) noexcept
{
    if (!result || !context || !*context || !desc) return false;
    std::lock_guard lock(g_owned_mutex);
    Owned* owned = find_owned(*context);
    if (!owned) return false;

    // The recorded Cyberpunk contract does not establish which queries the game
    // issues, and a wrong answer is worse than an honest refusal. Answering
    // nothing is deliberate: it is logged so the first game session can name the
    // exact queries to implement.
    (void)desc;
    *result = failure("unimplemented query on a Sedici-owned context");
    return true;
}

bool route_destroy(ffxContext* context, ffxReturnCode_t* result) noexcept
{
    if (!result || !context || !*context) return false;

    // Move the object out under the lock, then destroy outside it: retirement
    // waits for GPU work, and holding the registry lock through that wait would
    // block every other FFX call.
    std::unique_ptr<Owned> owned;
    {
        std::lock_guard lock(g_owned_mutex);
        const auto it = g_owned.find(*context);
        if (it == g_owned.end()) return false;
        owned = std::make_unique<Owned>(std::move(it->second));
        g_owned.erase(it);
    }

    bool ok = true;
    if (owned->kind == Kind::Session) {
        ok = owned->session ? owned->session->destroy() : true;
    } else {
        owned->wrap.reset();
    }
    // The caller's handle is left exactly as it was: the original DLL does not
    // modify it on destroy, and a game that inspects its own context afterwards
    // must see the same value. The registry entry is gone, so a later call with
    // the same pointer no longer matches and is forwarded instead.
    *result = ok ? FFX_API_RETURN_OK : failure("destroy before GPU retirement");
    return true;
}

} // namespace sedici::fg
