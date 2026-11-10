// fgcheck group: the window host and timeout retention of swapchains and presenters.
#include "fgcheck_common.h"

namespace fgcheck {

bool check_window(Device& state)
{
    std::printf("window host: creating window\n");
    const wchar_t* className = L"SediciFgAcceptance";
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = className;
    if (!RegisterClassW(&wc)) return false;
    HWND window = CreateWindowW(className, L"Sedici offline swapchain", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                CW_USEDEFAULT, 500, 320, nullptr, nullptr, wc.hInstance, nullptr);
    if (!window) {
        UnregisterClassW(className, wc.hInstance);
        return false;
    }
    bool ok = [&] {
        std::printf("window host: creating native swapchain\n");
        ComPtr<IDXGIFactory4> factory;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return false;
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = 480;
        desc.Height = 270;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 3;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<IDXGISwapChain1> created;
        if (FAILED(factory->CreateSwapChainForHwnd(state.queue.Get(), window, &desc, nullptr, nullptr, &created)))
            return false;
        ComPtr<IDXGISwapChain4> native;
        if (FAILED(created.As(&native))) return false;
        ComPtr<sedici::fg::Swapchain> wrapped;
        std::printf("window host: allocating wrapper\n");
        if (FAILED(sedici::fg::Swapchain::wrap(native.Get(), state.queue.Get(), &wrapped))) return false;
        ComPtr<IUnknown> identity, identityAgain;
        ComPtr<IDXGISwapChain1> older;
        if (FAILED(wrapped.As(&identity)) || FAILED(wrapped.As(&older)) || FAILED(older.As(&identityAgain)) ||
            identity.Get() != identityAgain.Get())
            return false;
        ComPtr<ID3D12DescriptorHeap> rtv;
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap.NumDescriptors = 1;
        if (FAILED(state.device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&rtv)))) return false;
        ShowWindow(window, SW_SHOWNOACTIVATE);
        std::printf("window host: presenting\n");
        for (UINT frame = 0; frame < 8; ++frame) {
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            const UINT index = wrapped->GetCurrentBackBufferIndex();
            if (FAILED(wrapped->Present(0, DXGI_PRESENT_TEST)) || index != wrapped->GetCurrentBackBufferIndex())
                return false;
            ComPtr<ID3D12Resource> buffer;
            if (FAILED(wrapped->GetBuffer(index, IID_PPV_ARGS(&buffer)))) return false;
            const auto target = rtv->GetCPUDescriptorHandleForHeapStart();
            state.device->CreateRenderTargetView(buffer.Get(), nullptr, target);
            if (FAILED(state.allocator->Reset()) || FAILED(state.list->Reset(state.allocator.Get(), nullptr)))
                return false;
            transition(state.list.Get(), buffer.Get(), D3D12_RESOURCE_STATE_PRESENT,
                       D3D12_RESOURCE_STATE_RENDER_TARGET);
            const float color[4]{}; // Matches the wrapper's optimized clear value.
            state.list->ClearRenderTargetView(target, color, 0, nullptr);
            transition(state.list.Get(), buffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                       D3D12_RESOURCE_STATE_PRESENT);
            if (!state.submit()) return false;
            if (frame % 2) {
                DXGI_PRESENT_PARAMETERS parameters{};
                if (FAILED(wrapped->Present1(1, 0, &parameters))) return false;
            } else if (FAILED(wrapped->Present(1, 0)))
                return false;
            if (wrapped->GetCurrentBackBufferIndex() != (index + 1) % 3) return false;
        }
        ComPtr<ID3D12Resource> held;
        std::printf("window host: resizing\n");
        if (FAILED(wrapped->GetBuffer(0, IID_PPV_ARGS(&held)))) return false;
        if (wrapped->ResizeBuffers(3, 320, 180, desc.Format, 0) != DXGI_ERROR_INVALID_CALL) return false;
        std::printf("window host: outstanding-reference rejection passed\n");
        held.Reset();
        if (FAILED(wrapped->ResizeBuffers(3, 320, 180, desc.Format, 0))) return false;
        std::printf("window host: resize completed\n");
        if (FAILED(wrapped->GetBuffer(0, IID_PPV_ARGS(&held))) || held->GetDesc().Width != 320) return false;
        held.Reset();
        std::printf("window host: releasing swapchain\n");
        // Wrapper teardown drains its own copy submission before releasing resources.
        return true;
    }();
    DestroyWindow(window);
    UnregisterClassW(className, wc.hInstance);
    std::printf("%s windowed real-frame wrapper: identity, Present/Present1, TEST, resize, teardown\n",
                ok ? "PASS" : "FAIL");
    return ok;
}

// K41: a live GPU stalled behind an unsignalled queue wait makes the bounded
// drain time out. Teardown must then retain every GPU object (K39), return
// within the wait bound, and leave the device healthy once the stall clears.
bool check_retention_timeout(Device& state)
{
    const wchar_t* className = L"SediciFgRetention";
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = className;
    if (!RegisterClassW(&wc)) return false;
    HWND window = CreateWindowW(className, L"Sedici retention", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                500, 320, nullptr, nullptr, wc.hInstance, nullptr);
    if (!window) {
        UnregisterClassW(className, wc.hInstance);
        return false;
    }
    ComPtr<ID3D12Fence> gate;
    bool ok = [&] {
        ComPtr<IDXGIFactory4> factory;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return false;
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = 320;
        desc.Height = 180;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 3;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<IDXGISwapChain1> created;
        if (FAILED(factory->CreateSwapChainForHwnd(state.queue.Get(), window, &desc, nullptr, nullptr, &created)))
            return false;
        ComPtr<IDXGISwapChain4> native;
        if (FAILED(created.As(&native))) return false;
        ComPtr<sedici::fg::Swapchain> wrapped;
        created.Reset();
        if (FAILED(sedici::fg::Swapchain::wrap(native.Get(), state.queue.Get(), &wrapped))) return false;
        native.Detach();
        if (FAILED(state.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate))) ||
            FAILED(state.queue->Wait(gate.Get(), 1)))
            return false;
        ComPtr<ID3D12Resource> buffer;
        if (FAILED(wrapped->GetBuffer(wrapped->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&buffer)))) return false;
        if (FAILED(state.allocator->Reset()) || FAILED(state.list->Reset(state.allocator.Get(), nullptr)))
            return false;
        transition(state.list.Get(), buffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
        transition(state.list.Get(), buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
        if (FAILED(state.list->Close())) return false;
        ID3D12CommandList* gated[] = {state.list.Get()};
        state.queue->ExecuteCommandLists(1, gated); // queued behind the gate; do not wait
        buffer.Reset();
        if (FAILED(wrapped->Present(0, 0))) return false;
        const unsigned retainedBefore = sedici::fg::swapchain_retention_count();
        const auto before = std::chrono::steady_clock::now();
        auto* finalReference = wrapped.Detach();
        const ULONG remaining = finalReference->Release();
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - before)
                .count();
        const bool retained = sedici::fg::swapchain_retention_count() == retainedBefore + 1;
        const bool alive = SUCCEEDED(state.device->GetDeviceRemovedReason());
        const bool bounded = elapsed >= 4500 && elapsed < 15000;
        std::printf("%s K41: a timed-out drain retains GPU state within the wait bound (%lld ms)\n",
                    remaining == 0 && retained && alive && bounded ? "PASS" : "FAIL",
                    static_cast<long long>(elapsed));

        // K47: while the GPU is still stalled the parked swapchain must stay intact,
        // however often the reaper runs; once the stall clears it must be destroyed.
        const unsigned releasedBefore = sedici::fg::swapchain_release_count();
        for (int i = 0; i < 5; ++i) sedici::fg::Swapchain::reap_retained();
        // The snapshot must tell the truth about the parked swapchain while it is held.
        const sedici::DiagSnapshot parkedView = sedici::diag_snapshot();
        const std::string parkedLine = sedici::diag_notable_line();
        const bool snapshotTruthful =
            parkedView.swapchains_parked == 1 &&
            parkedView.swapchains_parked_total == sedici::fg::swapchain_retention_count() &&
            parkedLine.find("swapchains_parked=1") != std::string::npos &&
            parkedLine.find("physical_flip_time") != std::string::npos;
        std::printf(
            "%s P2.10(b): the diagnostics snapshot reports the parked swapchain and names what it cannot measure\n",
            snapshotTruthful ? "PASS" : "FAIL");
        const bool heldWhileStalled = snapshotTruthful && sedici::fg::Swapchain::retained_now() == 1 &&
                                      sedici::fg::swapchain_release_count() == releasedBefore;
        (void)gate->Signal(1); // End the stall so the retained work can finish.
        const auto waitStart = std::chrono::steady_clock::now();
        while (sedici::fg::Swapchain::retained_now() != 0 &&
               std::chrono::steady_clock::now() - waitStart < std::chrono::seconds(5)) {
            sedici::fg::Swapchain::reap_retained();
            Sleep(5);
        }
        const bool released = sedici::fg::Swapchain::retained_now() == 0 &&
                              sedici::fg::swapchain_release_count() == releasedBefore + 1;
        std::printf(
            "%s K47: a parked swapchain is kept while the GPU is stalled and destroyed once its work retires\n",
            heldWhileStalled && released ? "PASS" : "FAIL");
        const bool healthy = SUCCEEDED(state.device->GetDeviceRemovedReason());
        return remaining == 0 && retained && alive && bounded && heldWhileStalled && released && healthy &&
               g_debug_errors.load() == 0;
    }();
    // The window is left alone: the retained chain still references it.
    return ok;
}

// K77: consecutive timeouts. Two swapchains released while the same stall holds the
// GPU are both parked (each after its own bounded wait), neither is freed early, and
// both are destroyed once the stall clears.
bool check_consecutive_retention(Device& state)
{
    const wchar_t* className = L"SediciFgConsecutive";
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = className;
    if (!RegisterClassW(&wc)) return false;
    HWND windows[2] = {};
    for (auto& w : windows)
        w = CreateWindowW(className, L"Sedici consecutive", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 500,
                          320, nullptr, nullptr, wc.hInstance, nullptr);
    if (!windows[0] || !windows[1]) return false;
    ComPtr<ID3D12Fence> gate;
    bool ok = [&] {
        ComPtr<IDXGIFactory4> factory;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return false;
        if (FAILED(state.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate))) ||
            FAILED(state.queue->Wait(gate.Get(), 1)))
            return false;
        sedici::fg::Swapchain* chains[2] = {};
        for (int i = 0; i < 2; ++i) {
            DXGI_SWAP_CHAIN_DESC1 desc{};
            desc.Width = 320;
            desc.Height = 180;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            desc.BufferCount = 3;
            desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            ComPtr<IDXGISwapChain1> created;
            if (FAILED(factory->CreateSwapChainForHwnd(state.queue.Get(), windows[i], &desc, nullptr, nullptr,
                                                       &created))) {
                (void)gate->Signal(1);
                return false;
            }
            ComPtr<IDXGISwapChain4> native;
            if (FAILED(created.As(&native))) {
                (void)gate->Signal(1);
                return false;
            }
            ComPtr<sedici::fg::Swapchain> wrapped;
            created.Reset();
            if (FAILED(sedici::fg::Swapchain::wrap(native.Get(), state.queue.Get(), &wrapped))) {
                (void)gate->Signal(1);
                return false;
            }
            native.Detach();
            if (FAILED(wrapped->Present(0, 0))) {
                (void)gate->Signal(1);
                return false;
            }
            chains[i] = wrapped.Detach();
        }
        const unsigned retainedBefore = sedici::fg::swapchain_retention_count();
        const unsigned releasedBefore = sedici::fg::swapchain_release_count();
        const ULONG first = chains[0]->Release();
        const unsigned afterFirst = sedici::fg::Swapchain::retained_now();
        const ULONG second = chains[1]->Release();
        const unsigned afterSecond = sedici::fg::Swapchain::retained_now();
        for (int i = 0; i < 3; ++i) sedici::fg::Swapchain::reap_retained();
        const bool heldTogether =
            sedici::fg::Swapchain::retained_now() == 2 && sedici::fg::swapchain_release_count() == releasedBefore;
        (void)gate->Signal(1);
        const auto waitStart = std::chrono::steady_clock::now();
        while (sedici::fg::Swapchain::retained_now() != 0 &&
               std::chrono::steady_clock::now() - waitStart < std::chrono::seconds(5)) {
            sedici::fg::Swapchain::reap_retained();
            Sleep(5);
        }
        const bool released = sedici::fg::Swapchain::retained_now() == 0 &&
                              sedici::fg::swapchain_release_count() == releasedBefore + 2;
        const bool parked = first == 0 && second == 0 && afterFirst == 1 && afterSecond == 2 &&
                            sedici::fg::swapchain_retention_count() == retainedBefore + 2;
        const bool healthy = SUCCEEDED(state.device->GetDeviceRemovedReason()) && g_debug_errors.load() == 0;
        std::printf(
            "%s K77: two consecutive timeouts are both parked, kept while stalled and destroyed once it clears\n",
            parked && heldTogether && released && healthy ? "PASS" : "FAIL");
        return parked && heldTogether && released && healthy;
    }();
    return ok;
}

// K48: a "vrr" swapchain released while the GPU is stalled has presenter jobs
// admitted but not started. Parking cancels the presenter; the jobs that never
// started must be discarded so the swapchain can be reaped once the stall clears.
bool check_presenter_retention(Device& state)
{
    const wchar_t* className = L"SediciFgPresenterRetention";
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = className;
    if (!RegisterClassW(&wc)) return false;
    HWND window = CreateWindowW(className, L"Sedici presenter retention", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                CW_USEDEFAULT, 500, 320, nullptr, nullptr, wc.hInstance, nullptr);
    if (!window) {
        UnregisterClassW(className, wc.hInstance);
        return false;
    }
    ComPtr<ID3D12Fence> gate;
    bool ok = [&] {
        ComPtr<IDXGIFactory5> factory;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return false;
        BOOL tearing = FALSE;
        const bool canTear = SUCCEEDED(factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing,
                                                                    sizeof(tearing))) &&
                             tearing;
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = 320;
        desc.Height = 180;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 3;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
                     (canTear ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u);
        ComPtr<IDXGISwapChain1> created;
        if (FAILED(factory->CreateSwapChainForHwnd(state.queue.Get(), window, &desc, nullptr, nullptr, &created)))
            return false;
        ComPtr<IDXGISwapChain4> native;
        if (FAILED(created.As(&native))) return false;
        sedici::fg::PresentChoice choice;
        choice.present = sedici::FrameGenerationSettings::Present::Vrr;
        choice.guard = sedici::FrameGenerationSettings::TearGuard::Scanline;
        choice.multiplier = sedici::FrameGenerationSettings::Multiplier::X2;
        ComPtr<sedici::fg::Swapchain> wrapped;
        created.Reset();
        if (FAILED(sedici::fg::Swapchain::wrap(native.Get(), state.queue.Get(), &wrapped, &choice, 1)))
            return false;
        native.Detach();
        if (!wrapped->async_present()) {
            std::puts("FAIL K48: presenter did not start");
            return false;
        }
        ShowWindow(window, SW_SHOWNOACTIVATE);
        const UINT presentFlags = canTear ? DXGI_PRESENT_ALLOW_TEARING : 0u;
        if (FAILED(wrapped->Present(0, presentFlags))) return false; // warm-up, completes
        if (FAILED(state.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate))) ||
            FAILED(state.queue->Wait(gate.Get(), 1)))
            return false;
        // Admit as many presents as the stalled GPU lets through, from a helper
        // thread so a blocked admission cannot hang the test.
        std::atomic<int> admitted{0};
        std::atomic<bool> finished{false};
        std::thread presenting([&] {
            for (int i = 0; i < 3; ++i) {
                if (FAILED(wrapped->Present(0, presentFlags | DXGI_PRESENT_DO_NOT_WAIT))) break;
                ++admitted;
            }
            finished = true;
        });
        const auto begin = std::chrono::steady_clock::now();
        while (!finished.load() && std::chrono::steady_clock::now() - begin < std::chrono::seconds(3)) Sleep(10);
        if (!finished.load()) {
            (void)gate->Signal(1);
            presenting.join();
            return false;
        }
        presenting.join();
        Sleep(300); // let the presenter pick up the first job and block on the gated GPU
        const size_t outstanding = wrapped->presenter_outstanding();
        std::printf("INFO K48: %d present(s) admitted, %zu presenter job(s) outstanding at release\n",
                    admitted.load(), outstanding);
        if (outstanding < 2) {
            (void)gate->Signal(1);
            return false;
        } // one running, one queued
        // Cancel now, while the first job is blocked on the gated GPU, so the queued
        // job is certain never to start (parking does the same after its drain bound).
        wrapped->cancel_presenter();
        const unsigned retainedBefore = sedici::fg::swapchain_retention_count();
        const unsigned releasedBefore = sedici::fg::swapchain_release_count();
        auto* finalReference = wrapped.Detach();
        const ULONG remaining = finalReference->Release();
        const bool parked = sedici::fg::swapchain_retention_count() == retainedBefore + 1 &&
                            sedici::fg::Swapchain::retained_now() == 1;
        (void)gate->Signal(1);
        const auto waitStart = std::chrono::steady_clock::now();
        while (sedici::fg::Swapchain::retained_now() != 0 &&
               std::chrono::steady_clock::now() - waitStart < std::chrono::seconds(10)) {
            sedici::fg::Swapchain::reap_retained();
            Sleep(5);
        }
        const bool released = sedici::fg::Swapchain::retained_now() == 0 &&
                              sedici::fg::swapchain_release_count() == releasedBefore + 1;
        const bool healthy = SUCCEEDED(state.device->GetDeviceRemovedReason()) && g_debug_errors.load() == 0;
        std::printf(
            "%s K48: a presenter with unstarted jobs is parked on release and destroyed once the stall clears\n",
            remaining == 0 && parked && released && healthy ? "PASS" : "FAIL");
        return remaining == 0 && parked && released && healthy;
    }();
    return ok;
}

} // namespace fgcheck
