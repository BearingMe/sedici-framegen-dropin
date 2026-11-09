// fgcheck group: the five-export router, lease contract and creation faults.
#include "fgcheck_common.h"

namespace fgcheck {

HANDLE routingLookupArrived = nullptr;
HANDLE routingLookupResume = nullptr;
void pause_routing_after_lookup() noexcept
{
    SetEvent(routingLookupArrived);
    (void)WaitForSingleObject(routingLookupResume, 5000);
}

std::atomic<unsigned> routingHookA{0};
std::atomic<unsigned> routingHookB{0};
void routing_hook_a(uint64_t) { ++routingHookA; }
void routing_hook_b(uint64_t) { ++routingHookB; }

bool check_routing(Device& state, bool wrapFirst)
{
    const wchar_t* className = L"SediciFgRouting";
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = className;
    if (!RegisterClassW(&wc)) return false;
    HWND window = CreateWindowW(className, L"Sedici routing", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 500, 320, nullptr, nullptr, wc.hInstance, nullptr);
    if (!window) { UnregisterClassW(className, wc.hInstance); return false; }

    bool ok = [&] {
        ComPtr<IDXGIFactory4> factory;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return false;
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = kDisplayWidth;
        desc.Height = kDisplayHeight;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 3;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<IDXGISwapChain1> created;
        if (FAILED(factory->CreateSwapChainForHwnd(state.queue.Get(), window, &desc,
            nullptr, nullptr, &created))) return false;
        ComPtr<IDXGISwapChain4> native, caller;
        if (FAILED(created.As(&native))) return false;

        struct Contexts {
            ffxContext session = nullptr;
            ffxContext wrap = nullptr;
            ~Contexts() {
                ffxReturnCode_t result{};
                if (session) (void)sedici::fg::route_destroy(&session, &result);
                if (wrap) (void)sedici::fg::route_destroy(&wrap, &result);
            }
        } contexts;
        ffxReturnCode_t result = FFX_API_RETURN_ERROR;
        ffxCreateBackendDX12Desc backend{};
        backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        backend.device = state.device.Get();
        ffxCreateContextDescFrameGeneration create{};
        create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
        create.header.pNext = &backend.header;
        create.flags = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED;
        create.displaySize = {kDisplayWidth, kDisplayHeight};
        create.maxRenderSize = {kRenderWidth, kRenderHeight};
        create.backBufferFormat = FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
        IDXGISwapChain4* replacement = native.Get();
        ffxCreateContextDescFrameGenerationSwapChainWrapDX12 wrap{};
        wrap.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WRAP_DX12;
        wrap.header.pNext = &backend.header;
        wrap.swapchain = &replacement;
        wrap.gameQueue = state.queue.Get();
        const auto createSession = [&] {
            return sedici::fg::route_create(&contexts.session, &create.header, &result) &&
                   result == FFX_API_RETURN_OK && contexts.session;
        };
        const auto createWrap = [&] {
            // Each injected bookkeeping failure must reject locally before the
            // native chain is touched. Normal wrapping immediately afterwards
            // exercises rollback and proves the chain remains usable.
            for (const auto point : {sedici::fg::BookkeepingFault::OwnerAllocation,
                                     sedici::fg::BookkeepingFault::RouteRegistryAllocation,
                                     sedici::fg::BookkeepingFault::WrapperRegistryAllocation}) {
                IDXGISwapChain4* untouched = native.Get();
                ffxContext failedContext = nullptr;
                auto failedWrap = wrap;
                failedWrap.swapchain = &untouched;
                ffxReturnCode_t failedResult = FFX_API_RETURN_OK;
                sedici::fg::arm_bookkeeping_fault(point);
                const bool answered = sedici::fg::route_create(&failedContext, &failedWrap.header,
                                                                &failedResult);
                const bool reachedInjection = !sedici::fg::consume_bookkeeping_fault(point);
                sedici::fg::arm_bookkeeping_fault(sedici::fg::BookkeepingFault::None);
                if (!reachedInjection || !answered || failedResult == FFX_API_RETURN_OK ||
                    failedContext || untouched != native.Get()) return false;
            }
            std::puts("PASS injected create-bookkeeping failures leave the native swapchain untouched");
            // The game hands its reference over (K12): the wrap consumes one reference,
            // so this host gives one up with the call and takes it back if refused.
            native->AddRef();
            if (!sedici::fg::route_create(&contexts.wrap, &wrap.header, &result) ||
                result != FFX_API_RETURN_OK || !contexts.wrap || replacement == native.Get()) {
                native->Release();
                return false;
            }
            caller.Attach(replacement); // Adopt the reference returned to the caller.
            return true;
        };
        // Exercise hook updates concurrently with both creation orders. This
        // is stress coverage; it does not force every possible interleaving.
        std::atomic<bool> stopHooks{false};
        std::atomic<bool> hooksStarted{false};
        std::thread hookUpdater([&] {
            hooksStarted = true;
            while (!stopHooks.load()) {
                sedici::fg::route_set_present_hook(&routing_hook_a);
                sedici::fg::route_set_present_hook(&routing_hook_b);
                std::this_thread::yield();
            }
        });
        while (!hooksStarted.load()) std::this_thread::yield();
        const bool creationOk = wrapFirst ? (createWrap() && createSession())
                                         : (createSession() && createWrap());
        stopHooks = true;
        hookUpdater.join();
        if (!creationOk || contexts.wrap == contexts.session) {
            sedici::fg::route_set_present_hook(nullptr);
            return false;
        }
        routingHookA = 0;
        routingHookB = 0;
        sedici::fg::route_set_present_hook(&routing_hook_b);
        const HRESULT hookedPresent = caller->Present(0, 0);
        sedici::fg::route_set_present_hook(nullptr);
        if (FAILED(hookedPresent) || routingHookA != 0 || routingHookB != 1) return false;
        // K52: a DO_NOT_WAIT present never enters the (sleeping) limiter hook.
        routingHookB = 0;
        sedici::fg::route_set_present_hook(&routing_hook_b);
        const HRESULT nonBlocking = caller->Present(0, DXGI_PRESENT_DO_NOT_WAIT);
        const bool hookSkipped = routingHookB == 0;
        const HRESULT waiting = caller->Present(0, 0);
        sedici::fg::route_set_present_hook(nullptr);
        if ((FAILED(nonBlocking) && nonBlocking != DXGI_ERROR_WAS_STILL_DRAWING) || !hookSkipped ||
            FAILED(waiting) || routingHookB != 1) return false;
        std::puts("PASS K52: a DO_NOT_WAIT present skips the limiter hook and a waiting one runs it");

        ffxConfigureDescFrameGeneration configure{};
        configure.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        configure.swapChain = caller.Get();
        configure.frameGenerationEnabled = false;
        if (!sedici::fg::route_configure(&contexts.session, &configure.header, &result) ||
            result != FFX_API_RETURN_OK) return false;

        // A different generator may replace this Session's sink. Destroying the
        // old Session must not clear that newer owner (K79).
        struct ExtraContext {
            ffxContext value = nullptr;
            ~ExtraContext() {
                if (value) { ffxReturnCode_t ignored{}; (void)sedici::fg::route_destroy(&value, &ignored); }
            }
        } extra;
        if (!sedici::fg::route_create(&extra.value, &create.header, &result) ||
            result != FFX_API_RETURN_OK || !extra.value) return false;
        auto enabledConfigure = configure;
        enabledConfigure.frameGenerationEnabled = true;
        if (!sedici::fg::route_configure(&extra.value, &enabledConfigure.header, &result) ||
            result != FFX_API_RETURN_OK) return false;
        auto* wrapperObject = static_cast<sedici::fg::Swapchain*>(caller.Get());
        uint32_t replacementCalls = 0;
        const sedici::fg::GenerateSink replacementSink{&replacementCalls, &record_counted, nullptr};
        if (!wrapperObject->set_generator(replacementSink)) return false;
        auto disabledConfigure = enabledConfigure;
        disabledConfigure.frameGenerationEnabled = false;
        if (!sedici::fg::route_configure(&extra.value, &disabledConfigure.header, &result) ||
            result != FFX_API_RETURN_OK || FAILED(caller->Present(0, 0)) || replacementCalls != 1) return false;
        if (!sedici::fg::route_configure(&extra.value, &enabledConfigure.header, &result) ||
            result != FFX_API_RETURN_OK || !wrapperObject->set_generator(replacementSink)) return false;
        if (!sedici::fg::route_destroy(&extra.value, &result) || result != FFX_API_RETURN_OK) return false;
        extra.value = nullptr;
        if (FAILED(caller->Present(0, 0)) || replacementCalls != 2) return false;
        std::puts("PASS disabling or destroying an old session preserves a replacement generator");
        wrapperObject->set_generator(sedici::fg::GenerateSink{});

        // K41: a destroy refused because GPU work has not retired parks the owner. The
        // game's token is stale at once; the owner is freed on a later call after the
        // work retires, and nothing is freed or double-parked in between.
        {
            ffxApiHeader parkProbe{};
            parkProbe.type = UINT64_MAX;
            ffxContext parkedContexts[3] = {};
            bool allCreated = true;
            for (auto& parkedContext : parkedContexts)
                allCreated = allCreated && sedici::fg::route_create(&parkedContext, &create.header, &result) &&
                          result == FFX_API_RETURN_OK && parkedContext;
            const unsigned parkedBefore = sedici::fg::route_parked_count();
            sedici::fg::set_retirement_hold(true);
            bool refused = allCreated;
            for (auto& parkedContext : parkedContexts) {
                ffxContext token = parkedContext;
                refused = refused && sedici::fg::route_destroy(&token, &result) &&
                          result == FFX_API_RETURN_ERROR;
            }
            const unsigned parkedHeld = sedici::fg::route_parked_count();
            const bool snapshotRouter = sedici::diag_snapshot().router_owners_parked == parkedHeld &&
                sedici::diag_notable_line().find("router_parked=" + std::to_string(parkedHeld)) != std::string::npos;
            std::printf("%s P2.10(b): the diagnostics snapshot reports parked router owners%c",
                        snapshotRouter ? "PASS" : "FAIL", 10);
            if (!snapshotRouter) return false;
            // A repeated destroy and a configure of a stale token are rejected and
            // must not park the owner a second time.
            ffxContext repeated = parkedContexts[0];
            const bool secondDestroy = sedici::fg::route_destroy(&repeated, &result) && result == FFX_API_RETURN_ERROR;
            // P2.10(b): a rejection names the call and the context it refused.
            std::error_code identityError;
            const auto identityDir = std::filesystem::temp_directory_path(identityError) /
                ("sedici-identity-" + std::to_string(GetCurrentProcessId()));
            std::filesystem::create_directories(identityDir, identityError);
            sedici::log_test_set_directory(identityDir);
            const int savedLogLevel = log_level();
            sedici::live_set_log_level(1);
            const bool staleConfigure = sedici::fg::route_configure(&repeated, &disabledConfigure.header, &result) &&
                                        result == FFX_API_RETURN_ERROR;
            (void)sedici::log_flush(3000);
            std::string identityLog;
            {
                std::ifstream in(identityDir / L"sedici.log", std::ios::binary);
                identityLog.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            }
            sedici::live_set_log_level(savedLogLevel);
            sedici::log_test_set_directory({});
            std::filesystem::remove_all(identityDir, identityError);
            char expectedCtx[48];
            std::snprintf(expectedCtx, sizeof(expectedCtx), "[op=configure ctx=0x%llx]",
                          static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(repeated)));
            const bool identified = identityLog.find(expectedCtx) != std::string::npos &&
                                    identityLog.find("[code=E_STALE_HANDLE]") != std::string::npos;
            if (!identified)
                std::printf("INFO log level %d, captured %zu bytes: %.200s%c", log_level(), identityLog.size(),
                            identityLog.c_str(), 10);
            std::printf("%s P2.10(b): a rejected call is logged with its operation and context%c",
                        identified ? "PASS" : "FAIL", 10);
            if (!identified) return false;
            sedici::fg::route_query(&contexts.session, &parkProbe, &result); // reaps; still held
            const unsigned parkedStillHeld = sedici::fg::route_parked_count();
            sedici::fg::set_retirement_hold(false);
            sedici::fg::route_query(&contexts.session, &parkProbe, &result); // reaps
            const unsigned parkedAfter = sedici::fg::route_parked_count();
            const bool parking = allCreated && refused && secondDestroy && staleConfigure &&
                                 parkedHeld == parkedBefore + 3 && parkedStillHeld == parkedHeld &&
                                 parkedAfter == parkedBefore;
            std::printf("%s K41: refused destroys park their owners, a repeat does not double-park, and they are released once work retires (%u -> %u -> %u)%c",
                        parking ? "PASS" : "FAIL", parkedBefore, parkedHeld, parkedAfter, 10);
            if (!parking) return false;
        }

        ffxApiHeader unsupported{};
        unsupported.type = UINT64_MAX;
        for (auto* context : {&contexts.session, &contexts.wrap}) {
            if (!sedici::fg::route_query(context, &unsupported, &result) ||
                result != FFX_API_RETURN_ERROR) return false;
            if (!sedici::fg::route_dispatch(context, &unsupported, &result) ||
                result != FFX_API_RETURN_ERROR) return false;
        }
        int unrelated = 0;
        ffxContext foreign = &unrelated;
        if (sedici::fg::route_query(&foreign, &unsupported, &result)) return false;

        if (!wrapFirst) {
            // Force the K68 window: configure has an owning wrapper reference,
            // then wrapper-context destroy completes, then configure resumes.
            routingLookupArrived = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            routingLookupResume = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!routingLookupArrived || !routingLookupResume) {
                if (routingLookupArrived) CloseHandle(routingLookupArrived);
                if (routingLookupResume) CloseHandle(routingLookupResume);
                routingLookupArrived = routingLookupResume = nullptr;
                return false;
            }
            auto raceConfigure = configure;
            raceConfigure.frameGenerationEnabled = true;
            ffxContext session = contexts.session;
            ffxReturnCode_t configureResult = FFX_API_RETURN_ERROR;
            bool configureAnswered = false;
            sedici::fg::arm_wrapper_lookup_hook(&pause_routing_after_lookup);
            std::thread configuring([&] {
                configureAnswered = sedici::fg::route_configure(
                    &session, &raceConfigure.header, &configureResult);
            });
            const bool arrived = WaitForSingleObject(routingLookupArrived, 5000) == WAIT_OBJECT_0;
            ffxReturnCode_t destroyResult = FFX_API_RETURN_ERROR;
            const bool destroyed = arrived && sedici::fg::route_destroy(&contexts.wrap, &destroyResult) &&
                                   destroyResult == FFX_API_RETURN_OK;
            if (destroyed) contexts.wrap = nullptr;
            SetEvent(routingLookupResume);
            configuring.join();
            sedici::fg::arm_wrapper_lookup_hook(nullptr);
            CloseHandle(routingLookupArrived);
            CloseHandle(routingLookupResume);
            routingLookupArrived = routingLookupResume = nullptr;
            if (!arrived || !destroyed || !configureAnswered || configureResult != FFX_API_RETURN_OK)
                return false;
            std::puts("PASS configure paused after lookup cannot reattach after wrapper destroy");
        }

        // Calls racing a destroy of the same handle: each either runs on the live
        // owner or is refused as a stale issued token; none may reach a torn-down
        // owner (P2.4 lease contract). The registry lock is not held across these calls.
        {
            const ffxContext racing = contexts.session;
            std::atomic<bool> go{false};
            std::atomic<int> answered{0};
            std::vector<std::thread> threads;
            for (int t = 0; t < 4; ++t) {
                threads.emplace_back([&, t] {
                    while (!go.load()) std::this_thread::yield();
                    for (int i = 0; i < 400; ++i) {
                        ffxContext handle = racing;
                        ffxReturnCode_t local = FFX_API_RETURN_OK;
                        const bool routed = (t & 1)
                            ? sedici::fg::route_configure(&handle, &configure.header, &local)
                            : sedici::fg::route_dispatch(&handle, &unsupported, &local);
                        if (routed) answered.fetch_add(1);
                    }
                });
            }
            go = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            bool wrapDestroyed = true;
            if (wrapFirst) {
                wrapDestroyed = sedici::fg::route_destroy(&contexts.wrap, &result) &&
                                result == FFX_API_RETURN_OK;
                if (wrapDestroyed) contexts.wrap = nullptr;
            }
            const bool sessionDestroyed = sedici::fg::route_destroy(&contexts.session, &result) &&
                                          result == FFX_API_RETURN_OK;
            // Join on every path: returning with joinable threads terminates the host.
            for (auto& thread : threads) thread.join();
            if (!wrapDestroyed || !sessionDestroyed) return false;
            ffxContext stale = racing;
            if (!sedici::fg::route_configure(&stale, &configure.header, &result) ||
                result == FFX_API_RETURN_OK) return false;
            if (!sedici::fg::route_destroy(&stale, &result) ||
                result == FFX_API_RETURN_OK) return false; // no double destroy
            contexts.session = nullptr;
        }
        // Hold a wrapper reference before destroying its FFX context, then
        // force an attempted attachment only after destruction returns. This
        // deterministically exercises the late-attachment gate (K68).
        auto* survivingWrapper = static_cast<sedici::fg::Swapchain*>(caller.Get());
        uint32_t lateCalls = 0;
        const sedici::fg::GenerateSink lateSink{&lateCalls, &record_counted, nullptr};
        if (contexts.wrap) {
            ComPtr<sedici::fg::Swapchain> held = survivingWrapper;
            std::atomic<bool> heldReady{false}, tryAttach{false}, attached{false};
            std::thread late([&] {
                heldReady = true;
                while (!tryAttach.load()) std::this_thread::yield();
                attached = held->set_generator(lateSink);
            });
            while (!heldReady.load()) std::this_thread::yield();
            const bool destroyed = sedici::fg::route_destroy(&contexts.wrap, &result) &&
                                   result == FFX_API_RETURN_OK;
            tryAttach = true;
            late.join();
            if (!destroyed || attached.load()) return false;
            contexts.wrap = nullptr;
        }
        // The caller's COM reference survives FFX wrapper-context destruction,
        // but the closed generator gate must reject any late reattachment.
        if (survivingWrapper->set_generator(lateSink)) return false;
        if (FAILED(caller->Present(0, 0)) || lateCalls != 0) return false;
        DXGI_SWAP_CHAIN_DESC1 after{};
        return SUCCEEDED(caller->GetDesc1(&after)) && after.Width == kDisplayWidth;
    }();
    DestroyWindow(window);
    UnregisterClassW(className, wc.hInstance);
    std::printf("%s routing: two live handles (%s), configure/query/dispatch, foreign handle, racing destroy\n",
                ok ? "PASS" : "FAIL", wrapFirst ? "wrap-first" : "session-first");
    return ok;
}

// P2.4 creation-path failures and hook propagation during creation:
//   K70  unsupported back buffer format -> local error, no native change
//   K29  handle-namespace reservation failure -> local error, retry succeeds
//   K63  backend allocation failure before creation -> local error, chain untouched
//   K60  hook change racing wrap creation -> the newest hook is installed
HANDLE routingCreationArrived = nullptr;
HANDLE routingCreationResume = nullptr;
void pause_creation_hook() noexcept
{
    SetEvent(routingCreationArrived);
    (void)WaitForSingleObject(routingCreationResume, 5000);
}

bool check_routing_faults(Device& state)
{
    const wchar_t* className = L"SediciFgCreation";
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = className;
    if (!RegisterClassW(&wc)) return false;
    HWND window = CreateWindowW(className, L"Sedici creation", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 500, 320, nullptr, nullptr, wc.hInstance, nullptr);
    if (!window) { UnregisterClassW(className, wc.hInstance); return false; }

    bool ok = [&] {
        ComPtr<IDXGIFactory4> factory;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return false;
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = kDisplayWidth;
        desc.Height = kDisplayHeight;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 3;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<IDXGISwapChain1> created;
        if (FAILED(factory->CreateSwapChainForHwnd(state.queue.Get(), window, &desc,
            nullptr, nullptr, &created))) return false;
        ComPtr<IDXGISwapChain4> native;
        if (FAILED(created.As(&native))) return false;
        ComPtr<IDXGISwapChain4> caller;

        struct Release {
            ffxContext* context;
            ~Release() {
                if (*context) { ffxReturnCode_t ignored{}; (void)sedici::fg::route_destroy(context, &ignored); }
            }
        };
        ffxReturnCode_t result = FFX_API_RETURN_ERROR;
        ffxCreateBackendDX12Desc backend{};
        backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        backend.device = state.device.Get();
        ffxCreateContextDescFrameGeneration create{};
        create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
        create.header.pNext = &backend.header;
        create.flags = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED;
        create.displaySize = {kDisplayWidth, kDisplayHeight};
        create.maxRenderSize = {kRenderWidth, kRenderHeight};
        create.backBufferFormat = FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;

        // K70: an unsupported back buffer format is a local, logged error.
        {
            auto unsupported = create;
            unsupported.backBufferFormat = FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM;
            ffxContext context = nullptr;
            const bool answered = sedici::fg::route_create(&context, &unsupported.header, &result);
            if (!answered || result == FFX_API_RETURN_OK || context) return false;
            std::puts("PASS K70: unsupported back buffer format is a local error");
        }

        // K29: a refused handle reservation issues no token; a retry succeeds.
        {
            ffxContext denied = nullptr;
            sedici::fg::arm_bookkeeping_fault(sedici::fg::BookkeepingFault::TokenReservation);
            const bool answered = sedici::fg::route_create(&denied, &create.header, &result);
            const bool reached = !sedici::fg::consume_bookkeeping_fault(sedici::fg::BookkeepingFault::TokenReservation);
            sedici::fg::arm_bookkeeping_fault(sedici::fg::BookkeepingFault::None);
            if (!answered || !reached || result == FFX_API_RETURN_OK || denied) return false;
            ffxContext session = nullptr;
            Release releaseSession{&session};
            if (!sedici::fg::route_create(&session, &create.header, &result) ||
                result != FFX_API_RETURN_OK || !session) return false;
            std::puts("PASS K29: a refused handle reservation issues no token and a retry succeeds");
        }

        // K63: backend allocation failure before creation rejects locally.
        {
            ffxContext denied = nullptr;
            sedici::fg::arm_bookkeeping_fault(sedici::fg::BookkeepingFault::BackendCreation);
            const bool answered = sedici::fg::route_create(&denied, &create.header, &result);
            const bool reached = !sedici::fg::consume_bookkeeping_fault(sedici::fg::BookkeepingFault::BackendCreation);
            sedici::fg::arm_bookkeeping_fault(sedici::fg::BookkeepingFault::None);
            if (!answered || !reached || result == FFX_API_RETURN_OK || denied) return false;
        }
        IDXGISwapChain4* nativePointer = native.Get();
        {
            IDXGISwapChain4* replacement = nativePointer;
            ffxCreateContextDescFrameGenerationSwapChainWrapDX12 wrap{};
            wrap.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WRAP_DX12;
            wrap.header.pNext = &backend.header;
            wrap.swapchain = &replacement;
            wrap.gameQueue = state.queue.Get();
            ffxContext denied = nullptr;
            sedici::fg::arm_bookkeeping_fault(sedici::fg::BookkeepingFault::BackendCreation);
            const bool answered = sedici::fg::route_create(&denied, &wrap.header, &result);
            const bool reached = !sedici::fg::consume_bookkeeping_fault(sedici::fg::BookkeepingFault::BackendCreation);
            sedici::fg::arm_bookkeeping_fault(sedici::fg::BookkeepingFault::None);
            if (!answered || !reached || result == FFX_API_RETURN_OK || denied ||
                replacement != nativePointer) return false;
        }
        std::puts("PASS K63: backend allocation failure rejects creation before the native chain is touched");

        // K102: a chain naming both a wrap and frame generation is refused, not half-created.
        {
            IDXGISwapChain4* replacement = nativePointer;
            auto both = create; // frame generation desc, chained behind the wrap desc
            ffxCreateContextDescFrameGenerationSwapChainWrapDX12 wrap{};
            wrap.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WRAP_DX12;
            wrap.header.pNext = &both.header;
            wrap.swapchain = &replacement;
            wrap.gameQueue = state.queue.Get();
            ffxContext refused = nullptr;
            const bool answered = sedici::fg::route_create(&refused, &wrap.header, &result);
            if (!answered || result == FFX_API_RETURN_OK || refused || replacement != nativePointer)
                return false;
            std::puts("PASS K102: a create naming both a wrap and frame generation is refused");
        }

        // K60: a hook change while wrap creation is paused must end on the newest hook.
        {
            routingCreationArrived = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            routingCreationResume = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!routingCreationArrived || !routingCreationResume) {
                if (routingCreationArrived) CloseHandle(routingCreationArrived);
                if (routingCreationResume) CloseHandle(routingCreationResume);
                routingCreationArrived = routingCreationResume = nullptr;
                return false;
            }
            IDXGISwapChain4* replacement = nativePointer;
            ffxCreateContextDescFrameGenerationSwapChainWrapDX12 wrap{};
            wrap.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WRAP_DX12;
            wrap.header.pNext = &backend.header;
            wrap.swapchain = &replacement;
            wrap.gameQueue = state.queue.Get();
            ffxContext wrapContext = nullptr;
            ffxReturnCode_t wrapResult = FFX_API_RETURN_ERROR;
            bool wrapAnswered = false;
            sedici::fg::route_set_present_hook(&routing_hook_a);
            sedici::fg::arm_creation_hook(&pause_creation_hook);
            native->AddRef(); // the wrap consumes the game's reference (K12)
            std::thread creating([&] {
                wrapAnswered = sedici::fg::route_create(&wrapContext, &wrap.header, &wrapResult);
            });
            const bool arrived = WaitForSingleObject(routingCreationArrived, 5000) == WAIT_OBJECT_0;
            // Give the change 200 ms to finish while creation is paused. Correct code
            // blocks it on the paused owner's call lock, so the wait times out. If the
            // change completes here, creation then installs the stale hook and fails.
            std::atomic<bool> changed{false};
            std::thread changing([&] {
                sedici::fg::route_set_present_hook(&routing_hook_b);
                changed = true;
            });
            for (int waited = 0; waited < 200 && !changed.load(); ++waited) Sleep(1);
            // Correct code must still be holding the change back here.
            const bool heldBack = !changed.load();
            SetEvent(routingCreationResume);
            creating.join();
            changing.join();
            sedici::fg::arm_creation_hook(nullptr);
            CloseHandle(routingCreationArrived);
            CloseHandle(routingCreationResume);
            routingCreationArrived = routingCreationResume = nullptr;
            Release releaseWrap{&wrapContext};
            if (!arrived || !heldBack || !wrapAnswered || wrapResult != FFX_API_RETURN_OK ||
                !wrapContext || replacement == nativePointer) return false;
            caller.Attach(replacement); // adopt the reference the wrap returned
            routingHookA = 0;
            routingHookB = 0;
            const HRESULT presented = caller->Present(0, 0);
            sedici::fg::route_set_present_hook(nullptr);
            if (FAILED(presented) || routingHookA != 0 || routingHookB != 1) return false;
        }
        std::puts("PASS K60: a hook change during wrap creation ends on the newest hook");
        return true;
    }();
    DestroyWindow(window);
    UnregisterClassW(className, wc.hInstance);
    std::printf("%s routing faults: K70, K29, K63 and K60\n", ok ? "PASS" : "FAIL");
    return ok;
}


} // namespace fgcheck
