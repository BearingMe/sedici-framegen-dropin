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
#include <vector>

namespace sedici::fg {

namespace {

enum class Kind {
    Session,
    Wrap,
};

// One registered owner. The registry hands out shared leases, so a call that has
// looked a handle up keeps the owner alive while it runs, and the registry lock is
// never held across a backend call. `call` serialises calls on this owner only
// (Session and WrapHandle are not thread-safe); `dead` is set under it by
// destroy, so a lease obtained just before destroy cannot use a torn-down owner.
struct Owned {
    Kind kind = Kind::Session;
    std::unique_ptr<Session> session;
    std::unique_ptr<WrapHandle> wrap;
    std::mutex call;
    bool dead = false;
};

using Lease = std::shared_ptr<Owned>;

// Protects the map only. Never held across a backend call, a callback or a GPU wait.
std::mutex g_owned_mutex;
std::map<void*, Lease> g_owned;
void (*g_present_hook)(uint64_t) = nullptr;
OutputHooks g_output_hooks{};

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

// Owners whose destroy was refused because GPU work was still in flight. Freeing
// them would release resources the GPU is using, and a timeout is not permission
// to do that, so they are retained until their work retires and are reaped on a
// later call. The vector is deliberately leaked: a process that exits with work
// still stuck must not free it either. Retention is bounded by the number of
// destroys the game issues while the GPU is stuck; the count is logged.
std::mutex g_parked_mutex;
std::vector<Lease>& parked() noexcept
{
    static auto* list = new std::vector<Lease>();
    return *list;
}

void reap_parked() noexcept
{
    try {
        std::vector<Lease> pending;
        {
            std::lock_guard lock(g_parked_mutex);
            if (parked().empty()) return;
            pending.swap(parked());
        }
        std::vector<Lease> keep;
        for (Lease& owned : pending) {
            std::lock_guard call(owned->call);
            if (owned->session && owned->session->retired()) {
                (void)owned->session->destroy();
                log_route("released a retained frame generation context", "");
            } else {
                keep.push_back(owned);
            }
        }
        if (!keep.empty()) {
            std::lock_guard lock(g_parked_mutex);
            for (Lease& owned : keep) parked().push_back(std::move(owned));
        }
    } catch (...) {
    }
}

Lease find_owned(void* handle) noexcept
{
    if (!handle) return nullptr;
    std::lock_guard lock(g_owned_mutex);
    const auto it = g_owned.find(handle);
    return it == g_owned.end() ? nullptr : it->second;
}

// Leases for every registered owner of a kind, taken under the registry lock and
// used after it is released.
std::vector<Lease> wrap_leases() noexcept
{
    std::vector<Lease> out;
    try {
        std::lock_guard lock(g_owned_mutex);
        for (auto& entry : g_owned) {
            if (entry.second->kind == Kind::Wrap) out.push_back(entry.second);
        }
    } catch (...) {
    }
    return out;
}

ffxReturnCode_t failure(const char* reason) noexcept
{
    log_route("rejected: ", reason);
    return FFX_API_RETURN_ERROR;
}

bool failure_result(ffxReturnCode_t* result, const char* reason) noexcept
{
    *result = failure(reason);
    return true;
}

} // namespace

bool routing_enabled() noexcept
{
    return sedici::settings().fg.mode == sedici::FrameGenerationMode::Sedici;
}

void route_set_output_hooks(const OutputHooks& hooks) noexcept
{
    {
        std::lock_guard lock(g_owned_mutex);
        g_output_hooks = hooks;
    }
    for (const Lease& owned : wrap_leases()) {
        std::lock_guard call(owned->call);
        if (!owned->dead && owned->wrap && owned->wrap->wrapper()) {
            owned->wrap->wrapper()->set_output_hooks(hooks);
        }
    }
}

void route_set_present_hook(void (*hook)(uint64_t)) noexcept
{
    {
        std::lock_guard lock(g_owned_mutex);
        g_present_hook = hook;
    }
    // Already-wrapped swapchains take the hook too: the configuration is read
    // once, but the hook may be installed before or after the first wrap.
    for (const Lease& owned : wrap_leases()) {
        std::lock_guard call(owned->call);
        if (!owned->dead && owned->wrap && owned->wrap->wrapper()) {
            owned->wrap->wrapper()->set_present_hook(hook);
        }
    }
}

bool route_create(ffxContext* context, const ffxApiHeader* desc, ffxReturnCode_t* result) noexcept
{
    if (!result || !context || !desc) return false;
    if (!routing_enabled()) return false;
    reap_parked();

    const auto* wrap = find_desc<ffxCreateContextDescFrameGenerationSwapChainWrapDX12>(
        desc, FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WRAP_DX12);
    const auto* frameGeneration = find_desc<ffxCreateContextDescFrameGeneration>(
        desc, FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION);
    if (!wrap && !frameGeneration) return false; // not a call this path can own

    // Owners are built without the registry lock (creation touches the device),
    // and registered only once they are complete.
    auto owned = std::make_shared<Owned>();
    if (wrap) {
        if (!wrap->swapchain || !wrap->swapchain[0] || !wrap->gameQueue) {
            *result = failure("swapchain wrap without a swapchain or queue");
            return true;
        }
        owned->kind = Kind::Wrap;
        owned->wrap = std::make_unique<WrapHandle>();
        IDXGISwapChain4* native = wrap->swapchain[0];
        // The wrap desc replaces the game's pointer with the wrapper, so the
        // game's reference to the native chain is handed over (AMD destroys
        // that chain and presents from its own). Only "vrr" makes use of it.
        if (!owned->wrap->create(native, wrap->gameQueue, 1)) {
            // Nothing was replaced: the game keeps its own swapchain pointer.
            *result = failure("swapchain wrap failed");
            return true;
        }
        void* handle = owned->wrap.get();
        {
            std::lock_guard lock(g_owned_mutex);
            owned->wrap->wrapper()->set_present_hook(g_present_hook);
            owned->wrap->wrapper()->set_output_hooks(g_output_hooks);
            if (!g_owned.emplace(handle, owned).second) {
                // Unreachable for live objects; refuse rather than overwrite.
                owned->dead = true;
                *result = failure("duplicate swapchain context handle");
                return true;
            }
        }
        *wrap->swapchain = owned->wrap->swapchain();
        // Handing the pointer to the game transfers a reference to it. Without
        // this the game's Release() and our own ComPtr share a single count, and
        // the wrapper is destroyed with a live owner still holding the address.
        (*wrap->swapchain)->AddRef();
        // The game needs a handle for this context too: it passes it back to
        // configure (UI registration) and destroy. Returning OK with a null
        // handle violates the context contract.
        *context = handle;
        *result = FFX_API_RETURN_OK;
        log_route("created swapchain wrapper", "");
        return true;
    }

    owned->kind = Kind::Session;
    owned->session = std::make_unique<Session>();
    if (!owned->session->create(desc)) {
        *result = failure(owned->session->context().last_error());
        return true;
    }
    void* handle = owned->session.get();
    {
        std::lock_guard lock(g_owned_mutex);
        if (!g_owned.emplace(handle, owned).second) {
            *result = failure("duplicate frame generation context handle");
            return true;
        }
    }
    *context = handle;
    *result = FFX_API_RETURN_OK;
    log_route("created frame generation context", "");
    return true;
}

bool route_configure(ffxContext* context, const ffxApiHeader* desc, ffxReturnCode_t* result) noexcept
{
    if (!result || !context || !*context || !desc) return false;
    const Lease owned = find_owned(*context);
    if (!owned) return false; // AMD's context: leave it alone

    std::lock_guard call(owned->call);
    if (owned->dead) return failure_result(result, "configure on a destroyed context");
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
    const Lease owned = find_owned(*context);
    if (!owned) return false;

    std::lock_guard call(owned->call);
    if (owned->dead) return failure_result(result, "dispatch on a destroyed context");
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
    const Lease owned = find_owned(*context);
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

    // Unregister first so no new call can find the owner, then wait for calls
    // already running on it, then tear it down. The registry lock is not held
    // through any of that: retirement waits for GPU work.
    Lease owned;
    {
        std::lock_guard lock(g_owned_mutex);
        const auto it = g_owned.find(*context);
        if (it == g_owned.end()) return false;
        owned = std::move(it->second);
        g_owned.erase(it);
    }

    bool ok = true;
    {
        std::lock_guard call(owned->call);
        owned->dead = true;
        if (owned->kind == Kind::Session) {
            ok = owned->session ? owned->session->destroy() : true;
        } else {
            owned->wrap.reset();
        }
    }
    if (!ok && owned->kind == Kind::Session) {
        // Not retired: keep the owner (and so its resources) alive.
        size_t retained = 0;
        {
            std::lock_guard lock(g_parked_mutex);
            parked().push_back(owned);
            retained = parked().size();
        }
        log_route("retaining a frame generation context until GPU work retires, retained=",
                  std::to_string(retained));
    }
    reap_parked();
    // The caller's handle is left exactly as it was: the original DLL does not
    // modify it on destroy, and a game that inspects its own context afterwards
    // must see the same value. The registry entry is gone, so a later call with
    // the same pointer no longer matches and is forwarded instead.
    *result = ok ? FFX_API_RETURN_OK : failure("destroy before GPU retirement");
    return true;
}

} // namespace sedici::fg
