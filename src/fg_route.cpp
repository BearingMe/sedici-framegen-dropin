#include "fg_route.h"

#include "config.h"
#include "ffx_api_framegeneration_dx12.h"
#include "ffx_framegeneration.h"
#include "fg_integration.h"
#include "fg_bookkeeping_faults.h"
#include "fg_token_namespace.h"
#include "log.h"

#include <windows.h>

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <string>

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
    // Intrusive retention needs no allocation after GPU retirement fails.
    std::shared_ptr<Owned> parked_self;
    Owned* parked_next = nullptr;
};

using Lease = std::shared_ptr<Owned>;

// Protects the map only. Never held across a backend call, a callback or a GPU wait.
std::mutex g_owned_mutex;
std::map<void*, Lease> g_owned;

// Opaque handles occupy a process-lifetime reserved virtual-address namespace.
// No pages are committed or dereferenced. Issued slots are never reused, so a
// stale token cannot name a newer Sedici context. The owner approved failure on
// reservation failure/exhaustion and the assumption that foreign FFX handles
// do not use values inside this reserved namespace (K29).
constexpr size_t kTokenBytes = size_t{256} * 1024 * 1024;
constexpr uintptr_t kTokenStride = 16;
TokenNamespace g_tokens;
bool g_token_reserved = false;

// Caller holds g_owned_mutex. Reserve before creating any swapchain resources,
// because wrap creation can consume the caller's native chain reference.
void* issue_token_locked() noexcept
{
    return issue_reserved_token(g_tokens, g_token_reserved, kTokenBytes, kTokenStride,
        [](size_t bytes) noexcept { return VirtualAlloc(nullptr, bytes, MEM_RESERVE, PAGE_NOACCESS); });
}

bool is_issued_token_locked(void* handle) noexcept
{
    return g_tokens.contains_issued(handle);
}

bool is_issued_token(void* handle) noexcept
{
    std::lock_guard lock(g_owned_mutex);
    return is_issued_token_locked(handle);
}

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

// Intrusive list with a self lease: parking and reaping never allocate, and the
// process-exit path does not release owners whose GPU work may still be live.
std::mutex g_parked_mutex;
Owned* g_parked = nullptr;

void reap_parked() noexcept
{
    for (;;) {
        Lease released;
        {
            std::lock_guard lock(g_parked_mutex);
            Owned** link = &g_parked;
            while (*link) {
                Owned* owned = *link;
                std::lock_guard call(owned->call);
                if (owned->session && owned->session->retired()) {
                    (void)owned->session->destroy();
                    *link = owned->parked_next;
                    owned->parked_next = nullptr;
                    released = std::move(owned->parked_self);
                    break;
                }
                link = &owned->parked_next;
            }
        }
        if (!released) return;
        log_route("released a retained frame generation context", "");
    }
}

Lease find_owned(void* handle) noexcept
{
    if (!handle) return nullptr;
    std::lock_guard lock(g_owned_mutex);
    const auto it = g_owned.find(handle);
    return it == g_owned.end() ? nullptr : it->second;
}

// Visit wrappers without allocating a snapshot and without holding the registry
// lock during wrapper calls. Concurrently created wrappers initialize from the
// latest globals; already-visited wrappers re-read them under their owner lock.
template <typename Visit>
void for_each_wrap(Visit&& visit) noexcept
{
    void* last = nullptr;
    bool first = true;
    for (;;) {
        Lease owned;
        {
            std::lock_guard lock(g_owned_mutex);
            auto it = first ? g_owned.begin() : g_owned.upper_bound(last);
            while (it != g_owned.end() && (!it->second || it->second->kind != Kind::Wrap)) ++it;
            if (it == g_owned.end()) return;
            last = it->first;
            first = false;
            owned = it->second;
        }
        visit(owned);
    }
}

ffxReturnCode_t failure(const char* reason) noexcept
{
    // Converting the reason to a string can allocate; an error report must not
    // terminate the game when memory is already exhausted.
    try { log_route("rejected: ", reason); } catch (...) {}
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

bool route_is_sedici_token(ffxContext* context) noexcept
{
    return context && *context && is_issued_token(*context);
}

void route_set_output_hooks(const OutputHooks& hooks) noexcept
{
    {
        std::lock_guard lock(g_owned_mutex);
        g_output_hooks = hooks;
    }
    for_each_wrap([&](const Lease& owned) {
        std::lock_guard call(owned->call);
        if (!owned->dead && owned->wrap && owned->wrap->wrapper()) {
            OutputHooks current{};
            {
                std::lock_guard registry(g_owned_mutex);
                current = g_output_hooks;
            }
            // Read the newest value after taking the owner lock: an older
            // propagation may not overwrite a newer concurrent update.
            owned->wrap->wrapper()->set_output_hooks(current);
        }
    });
}

void route_set_present_hook(void (*hook)(uint64_t)) noexcept
{
    {
        std::lock_guard lock(g_owned_mutex);
        g_present_hook = hook;
    }
    // Already-wrapped swapchains take the hook too: the configuration is read
    // once, but the hook may be installed before or after the first wrap.
    for_each_wrap([&](const Lease& owned) {
        std::lock_guard call(owned->call);
        if (!owned->dead && owned->wrap && owned->wrap->wrapper()) {
            void (*current)(uint64_t) = nullptr;
            {
                std::lock_guard registry(g_owned_mutex);
                current = g_present_hook;
            }
            owned->wrap->wrapper()->set_present_hook(current);
        }
    });
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
    if (wrap && (!wrap->swapchain || !wrap->swapchain[0] || !wrap->gameQueue)) {
        *result = failure("swapchain wrap without a swapchain or queue");
        return true;
    }

    // Mint the identity before any backend call that can consume game resources.
    void* token = nullptr;
    {
        std::lock_guard lock(g_owned_mutex);
        token = issue_token_locked();
    }
    if (!token) {
        *result = failure("Sedici handle namespace unavailable or exhausted");
        return true;
    }

    // Allocate owner and registry node before a backend call can consume game
    // resources. The owner approved a local error if this bookkeeping fails.
    Lease owned;
    try {
        if (consume_bookkeeping_fault(BookkeepingFault::OwnerAllocation)) throw std::bad_alloc();
        owned = std::make_shared<Owned>();
        owned->kind = wrap ? Kind::Wrap : Kind::Session;
        if (wrap) owned->wrap = std::make_unique<WrapHandle>();
        else {
            owned->session = std::make_unique<Session>();
            owned->session->set_callback_lifetime(owned);
        }
    } catch (...) {
        *result = FFX_API_RETURN_ERROR;
        return true;
    }

    // A null placeholder reserves the node without publishing an initializing
    // owner. No registry or owner lock is held during backend creation.
    try {
        std::lock_guard lock(g_owned_mutex);
        if (consume_bookkeeping_fault(BookkeepingFault::RouteRegistryAllocation)) throw std::bad_alloc();
        if (!g_owned.emplace(token, Lease{}).second) {
            *result = FFX_API_RETURN_ERROR;
            return true;
        }
    } catch (...) {
        *result = FFX_API_RETURN_ERROR;
        return true;
    }
    const auto rollback = [&] {
        std::lock_guard lock(g_owned_mutex);
        const auto it = g_owned.find(token);
        if (it != g_owned.end() && !it->second) g_owned.erase(it);
    };

    if (wrap) {
        IDXGISwapChain4* native = wrap->swapchain[0];
        if (!owned->wrap->create(native, wrap->gameQueue, 1)) {
            rollback();
            *result = failure("swapchain wrap failed");
            return true;
        }
        {
            // Once published, concurrent hook propagation waits for initial
            // application and then re-reads the newest global hooks.
            std::lock_guard call(owned->call);
            void (*presentHook)(uint64_t) = nullptr;
            OutputHooks outputHooks{};
            {
                std::lock_guard lock(g_owned_mutex);
                g_owned.find(token)->second = owned; // Placeholder cannot be destroyed.
                presentHook = g_present_hook;
                outputHooks = g_output_hooks;
            }
            owned->wrap->wrapper()->set_present_hook(presentHook);
            owned->wrap->wrapper()->set_output_hooks(outputHooks);
        }
        *wrap->swapchain = owned->wrap->swapchain();
        (*wrap->swapchain)->AddRef();
        *context = token;
        *result = FFX_API_RETURN_OK;
        log_route("created swapchain wrapper", "");
        return true;
    }

    if (!owned->session->create(desc)) {
        rollback();
        *result = failure(owned->session->last_error());
        return true;
    }
    {
        std::lock_guard lock(g_owned_mutex);
        g_owned.find(token)->second = owned;
    }
    *context = token;
    *result = FFX_API_RETURN_OK;
    log_route("created frame generation context", "");
    return true;
}

bool route_configure(ffxContext* context, const ffxApiHeader* desc, ffxReturnCode_t* result) noexcept
{
    if (!result || !context || !*context) return false;
    const Lease owned = find_owned(*context);
    if (!owned) return is_issued_token(*context)
        ? failure_result(result, "configure on a destroyed context") : false;
    if (!desc) return failure_result(result, "null descriptor on a Sedici-owned context");

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
                                              : failure(owned->session->last_error());
    return true;
}

bool route_dispatch(ffxContext* context, const ffxApiHeader* desc, ffxReturnCode_t* result) noexcept
{
    if (!result || !context || !*context) return false;
    const Lease owned = find_owned(*context);
    if (!owned) return is_issued_token(*context)
        ? failure_result(result, "dispatch on a destroyed context") : false;
    if (!desc) return failure_result(result, "null descriptor on a Sedici-owned context");

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
                                            : failure(owned->session->last_error());
    return true;
}

bool route_query(ffxContext* context, const ffxApiHeader* desc, ffxReturnCode_t* result) noexcept
{
    if (!result || !context || !*context) return false;
    const Lease owned = find_owned(*context);
    if (!owned) return is_issued_token(*context)
        ? failure_result(result, "query on a destroyed context") : false;
    if (!desc) return failure_result(result, "null descriptor on a Sedici-owned context");

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
        if (it == g_owned.end()) {
            if (!is_issued_token_locked(*context)) return false;
            *result = FFX_API_RETURN_ERROR;
            return true;
        }
        if (!it->second) {
            *result = FFX_API_RETURN_ERROR; // Creation has not published this token.
            return true;
        }
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
        // Establish the self lease before dropping the local lease. No allocation
        // is allowed on this failure path.
        {
            std::lock_guard lock(g_parked_mutex);
            owned->parked_self = owned;
            owned->parked_next = g_parked;
            g_parked = owned.get();
        }
        log_route("retaining a frame generation context until GPU work retires", "");
    }
    reap_parked();
    // The caller's handle is left exactly as it was: the original DLL does not
    // modify it on destroy, and a game that inspects its own context afterwards
    // must see the same value. The issued token remains identifiable as stale,
    // so later calls are rejected without entering the foreign backend.
    *result = ok ? FFX_API_RETURN_OK : failure("destroy before GPU retirement");
    return true;
}

} // namespace sedici::fg
