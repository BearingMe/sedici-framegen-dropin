#include "abi/route.h"

#include "settings/config.h"
#include "ffx_api_framegeneration_dx12.h"
#include "ffx_framegeneration.h"
#include "runtime/integration.h"
#include "diagnostics/bookkeeping_faults.h"
#include "diagnostics/error_code.h"
#include "abi/token_namespace.h"
#include "diagnostics/log.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
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
    // What the game last asked for, so the limiter and the menu can say whether
    // frame generation is on without reaching into a session another thread may
    // be using.
    std::atomic<bool> generation_on{false};
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
    if (consume_bookkeeping_fault(BookkeepingFault::TokenReservation)) return nullptr;
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

// Which router entry point is running on this thread and on which context, so a
// rejection in the log names the call that was refused (P2.10).
thread_local const char* t_call_op = nullptr;
thread_local const void* t_call_context = nullptr;

void log_slow_call(const char* op, const void* context, double ms) noexcept;

struct CallScope {
    const char* previous_op;
    const void* previous_context;
    const char* op;
    const void* context;
    LARGE_INTEGER start{};
    CallScope(const char* op_, const void* context_) noexcept
        : previous_op(t_call_op), previous_context(t_call_context), op(op_), context(context_)
    {
        t_call_op = op_;
        t_call_context = context_;
        QueryPerformanceCounter(&start);
    }
    ~CallScope()
    {
        // K121/K122: say how long the game was inside Sedici, so a stall in the
        // game's own prepare interval can be attributed to Sedici or not.
        LARGE_INTEGER now{}, frequency{};
        if (QueryPerformanceCounter(&now) && QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0) {
            const double ms = 1000.0 * static_cast<double>(now.QuadPart - start.QuadPart) /
                              static_cast<double>(frequency.QuadPart);
            if (ms >= 50.0) log_slow_call(op, context, ms);
        }
        t_call_op = previous_op;
        t_call_context = previous_context;
    }
};

// Once the process-lifetime token namespace is reserved the module holds state that
// cannot be unwound by a FreeLibrary: the namespace itself (stale tokens must stay
// recognisable), parked swapchains and owners whose vtables live in this module, and
// presenter threads (K47, K48). Pin the module so an unload becomes a no-op and a
// reload cannot create a second namespace (K64). An unused DLL still unloads normally.
void pin_module_once() noexcept
{
    static std::atomic<bool> pinned{false};
    if (pinned.exchange(true)) return;
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCWSTR>(&pin_module_once), &module))
        log_event(1, "fg: module pinned in memory; a FreeLibrary is now a no-op (handle namespace is process-lifetime)");
}

std::string call_identity()
{
    char text[64];
    std::snprintf(text, sizeof(text), " [op=%s ctx=0x%llx]", t_call_op ? t_call_op : "?",
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(t_call_context)));
    return text;
}

void log_slow_call(const char* op, const void* context, double ms) noexcept
{
    try {
        char text[96];
        std::snprintf(text, sizeof(text), "fg: slow call op=%s ctx=0x%llx took %.1f ms", op ? op : "?",
                      static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(context)), ms);
        log_event(1, text);
    } catch (...) {}
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
std::atomic<unsigned> g_parked_count{0};

void reap_parked() noexcept
{
    if (!g_parked_count.load(std::memory_order_acquire)) return; // the common case: one atomic load
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
                    g_parked_count.fetch_sub(1, std::memory_order_release);
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
    try {
        log_route("rejected: ", std::string(reason) + " [code=" +
                                    sedici::error_code_name(sedici::classify_reason(reason)) + "]" +
                                    call_identity());
    } catch (...) {}
    return FFX_API_RETURN_ERROR;
}

bool failure_result(ffxReturnCode_t* result, const char* reason) noexcept
{
    *result = failure(reason);
    return true;
}

} // namespace

bool route_is_sedici_token(ffxContext* context) noexcept
{
    return context && *context && is_issued_token(*context);
}

bool route_any_generation_enabled() noexcept
{
    std::lock_guard lock(g_owned_mutex);
    for (const auto& entry : g_owned)
        if (entry.second && entry.second->kind == Kind::Session &&
            entry.second->generation_on.load(std::memory_order_relaxed))
            return true;
    return false;
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
    const CallScope scope("create", context ? *context : nullptr);
    if (!result || !context || !desc) return false;
    reap_parked();

    const auto* wrap = find_desc<ffxCreateContextDescFrameGenerationSwapChainWrapDX12>(
        desc, FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WRAP_DX12);
    const auto* frameGeneration = find_desc<ffxCreateContextDescFrameGeneration>(
        desc, FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION);
    // Alan Wake 2 asks for a chain (New form) instead of handing one over (K100); the
    // HWND form is the same request with a window handle. Both are built on the game's
    // queue and then owned like a wrapped chain.
    const auto* created = find_desc<ffxCreateContextDescFrameGenerationSwapChainNewDX12>(
        desc, FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_NEW_DX12);
    const auto* forHwnd = find_desc<ffxCreateContextDescFrameGenerationSwapChainForHwndDX12>(
        desc, FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12);
    if (!wrap && !frameGeneration && !created && !forHwnd) return false; // not a call this path can own
    if ((wrap ? 1 : 0) + (frameGeneration ? 1 : 0) + (created ? 1 : 0) + (forHwnd ? 1 : 0) > 1) {
        // One create makes one context. Answering OK as a wrapper alone would drop
        // the generation context the caller asked for without telling it (K102).
        *result = failure("create chain names both a swapchain wrap and frame generation");
        return true;
    }
    if (wrap && (!wrap->swapchain || !wrap->swapchain[0] || !wrap->gameQueue)) {
        *result = failure("swapchain wrap without a swapchain or queue");
        return true;
    }
    if (created && (!created->swapchain || !created->desc || !created->dxgiFactory || !created->gameQueue)) {
        *result = failure("swapchain new without a factory, desc, output or queue");
        return true;
    }
    if (forHwnd && (!forHwnd->swapchain || !forHwnd->desc || !forHwnd->hwnd || !forHwnd->dxgiFactory ||
                    !forHwnd->gameQueue)) {
        *result = failure("swapchain for hwnd without a factory, desc, window, output or queue");
        return true;
    }
    const bool swapchainForm = wrap || created || forHwnd;

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
    pin_module_once();

    // Allocate owner and registry node before a backend call can consume game
    // resources. The owner approved a local error if this bookkeeping fails.
    Lease owned;
    try {
        if (consume_bookkeeping_fault(BookkeepingFault::OwnerAllocation)) throw std::bad_alloc();
        owned = std::make_shared<Owned>();
        owned->kind = swapchainForm ? Kind::Wrap : Kind::Session;
        if (swapchainForm) owned->wrap = std::make_unique<WrapHandle>();
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

    if (swapchainForm) {
        IDXGISwapChain4* native = wrap ? wrap->swapchain[0] : nullptr;
        ID3D12CommandQueue* const queue = wrap ? wrap->gameQueue : created ? created->gameQueue : forHwnd->gameQueue;
        IDXGISwapChain4** const output = wrap ? wrap->swapchain : created ? created->swapchain : forHwnd->swapchain;
        // The New and HWND forms ask for a chain: build it as requested on the game's
        // queue. The reference from creation is handed to the wrapper like the game's
        // own would be, so a failure below releases a chain the game never saw.
        Microsoft::WRL::ComPtr<IDXGISwapChain4> madeNative;
        if (!wrap) {
            Microsoft::WRL::ComPtr<IDXGISwapChain1> chain;
            HRESULT made = E_FAIL;
            if (created) {
                DXGI_SWAP_CHAIN_DESC request = *created->desc;
                Microsoft::WRL::ComPtr<IDXGISwapChain> legacy;
                made = created->dxgiFactory->CreateSwapChain(queue, &request, &legacy);
                if (SUCCEEDED(made)) made = legacy.As(&chain);
            } else {
                Microsoft::WRL::ComPtr<IDXGIFactory2> factory2;
                made = forHwnd->dxgiFactory->QueryInterface(IID_PPV_ARGS(&factory2));
                if (SUCCEEDED(made)) made = factory2->CreateSwapChainForHwnd(
                    queue, forHwnd->hwnd, forHwnd->desc, forHwnd->fullscreenDesc, nullptr, &chain);
            }
            if (FAILED(made) || FAILED(chain.As(&madeNative))) {
                rollback();
                *result = failure("swapchain new: the factory refused the chain");
                return true;
            }
            native = madeNative.Get();
        }
        // Injected backend allocation failure: rejected before the native chain is touched.
        if (consume_bookkeeping_fault(BookkeepingFault::BackendCreation)) {
            rollback();
            *result = failure("swapchain wrap backend allocation failed");
            return true;
        }
        if (!owned->wrap->create(native, queue, 1)) {
            rollback();
            *result = failure(wrap ? "swapchain wrap failed" : "swapchain new: wrapping the created chain failed");
            return true; // madeNative releases a chain the game never saw
        }
        madeNative.Detach(); // the wrapper owns the reference handed over
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
            run_creation_hook(); // Offline window for K60; unarmed at runtime.
            owned->wrap->wrapper()->set_present_hook(presentHook);
            owned->wrap->wrapper()->set_output_hooks(outputHooks);
        }
        *output = owned->wrap->swapchain();
        (*output)->AddRef();
        *context = token;
        *result = FFX_API_RETURN_OK;
        log_route(wrap ? "created swapchain wrapper" : "created swapchain (new)", "");
        return true;
    }

    if (consume_bookkeeping_fault(BookkeepingFault::BackendCreation)) {
        rollback();
        *result = failure("frame generation backend allocation failed");
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
    const CallScope scope("configure", context ? *context : nullptr);
    if (!result || !context || !*context) return false;
    reap_parked(); // K41: parked owners are released on any router call
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
    const bool configured = owned->session->configure(desc);
    if (configured) {
        const auto* modern = find_desc<ffxConfigureDescFrameGeneration>(
            desc, FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION);
        const auto* legacy = find_desc<FfxFrameGenerationConfig>(desc, FFX_API_FRAME_GENERATION_CONFIG);
        if (modern) owned->generation_on.store(modern->frameGenerationEnabled, std::memory_order_relaxed);
        else if (legacy) owned->generation_on.store(legacy->frameGenerationEnabled, std::memory_order_relaxed);
    }
    *result = configured ? FFX_API_RETURN_OK : failure(owned->session->last_error());
    return true;
}

bool route_dispatch(ffxContext* context, const ffxApiHeader* desc, ffxReturnCode_t* result) noexcept
{
    const CallScope scope("dispatch", context ? *context : nullptr);
    if (!result || !context || !*context) return false;
    reap_parked(); // K41: parked owners are released on any router call
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
    if (desc->type == FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION) {
        // The game's own generation dispatch into the interpolation list it got from the
        // swapchain query (K131). Sedici generates the frame itself; acknowledge it.
        log_event(2, "fg: game frame generation dispatch acknowledged (Sedici generates its own)");
        *result = FFX_API_RETURN_OK;
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

// GetVersions is answered without a context: the game asks before it creates
// anything. The ids and names are the providers the original AMD binary reported
// for the same create descriptor types (docs/refactor/amd-binary.md); Sedici
// reports exactly one version for each recognised type so a game that enumerates
// versions sees the same shape it saw before.
struct ProviderVersion {
    uint64_t createDescType;
    uint64_t id;
    const char* name;
};

constexpr ProviderVersion kProviderVersions[] = {
    {FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION, 0xf600000000c01006ull, "3.1.6"},
    {FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WRAP_DX12, 0xf65cdd1200c01007ull, "3.1.7"},
    // The New and HWND forms share the swapchain provider (Alan Wake 2 sends 0xc01007 with the New
    // form, finding 014); the id above is the one recorded for the wrap form.
    {FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_NEW_DX12, 0xf65cdd1200c01007ull, "3.1.7"},
    {FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12, 0xf65cdd1200c01007ull, "3.1.7"},
};

bool route_get_versions(const ffxQueryDescGetVersions& query, ffxReturnCode_t* result) noexcept
{
    const ProviderVersion* found = nullptr;
    for (const ProviderVersion& version : kProviderVersions)
        if (version.createDescType == query.createDescType) found = &version;
    if (!found) {
        *result = FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
        return true;
    }
    if (!query.outputCount) {
        *result = FFX_API_RETURN_ERROR_PARAMETER;
        return true;
    }
    const uint64_t capacity = *query.outputCount;
    *query.outputCount = 1;
    if (capacity >= 1) {
        if (query.versionIds) query.versionIds[0] = found->id;
        if (query.versionNames) query.versionNames[0] = found->name;
    }
    *result = FFX_API_RETURN_OK;
    return true;
}

bool route_query(ffxContext* context, const ffxApiHeader* desc, ffxReturnCode_t* result) noexcept
{
    const CallScope scope("query", context ? *context : nullptr);
    if (!result) return false;
    reap_parked(); // K41: parked owners are released on any router call
    if (desc && desc->type == FFX_API_QUERY_DESC_TYPE_GET_VERSIONS)
        return route_get_versions(*reinterpret_cast<const ffxQueryDescGetVersions*>(desc), result);
    if (!context || !*context) return false;
    const Lease owned = find_owned(*context);
    if (!owned) return is_issued_token(*context)
        ? failure_result(result, "query on a destroyed context") : false;
    if (!desc) return failure_result(result, "null descriptor on a Sedici-owned context");

    // Alan Wake 2 asks the swapchain for an interpolation command list and texture (K131).
    // Sedici generates the frame itself, so these are the wrapper's scratch objects and
    // whatever the game records into them is discarded.
    if (owned->kind == Kind::Wrap && owned->wrap && owned->wrap->wrapper()) {
        Swapchain* const wrapper = owned->wrap->wrapper();
        if (desc->type == FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_INTERPOLATIONCOMMANDLIST_DX12) {
            const auto* query = reinterpret_cast<const ffxQueryDescFrameGenerationSwapChainInterpolationCommandListDX12*>(desc);
            ID3D12GraphicsCommandList* list = nullptr;
            if (!query->pOutCommandList || FAILED(wrapper->interpolation_list(&list)))
                return failure_result(result, "interpolation command list unavailable");
            *query->pOutCommandList = list;
            log_event(2, "fg: interpolation command list query answered with a scratch list");
            *result = FFX_API_RETURN_OK;
            return true;
        }
        if (desc->type == FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_INTERPOLATIONTEXTURE_DX12) {
            const auto* query = reinterpret_cast<const ffxQueryDescFrameGenerationSwapChainInterpolationTextureDX12*>(desc);
            ID3D12Resource* texture = nullptr;
            if (!query->pOutTexture || FAILED(wrapper->interpolation_texture(&texture)))
                return failure_result(result, "interpolation texture unavailable");
            *query->pOutTexture = ffxApiGetResourceDX12(texture, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
            log_event(2, "fg: interpolation texture query answered with a scratch texture");
            *result = FFX_API_RETURN_OK;
            return true;
        }
    }

    // Other queries stay refused and logged so a game session can name them.
    *result = failure("unimplemented query on a Sedici-owned context");
    return true;
}

bool route_destroy(ffxContext* context, ffxReturnCode_t* result) noexcept
{
    const CallScope scope("destroy", context ? *context : nullptr);
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
            g_parked_count.fetch_add(1, std::memory_order_release);
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

unsigned route_parked_count() noexcept { return g_parked_count.load(std::memory_order_acquire); }

} // namespace sedici::fg
