// fgcheck group: windowed presentation (vsync, vrr) and the session/recovery faults.
#include "fgcheck_common.h"

namespace fgcheck {

// `[fg] present = "vrr"`: the native chain moves to Sedici's present queue and
// both outputs come from the presenter thread with the game's tearing flags.
// Every midpoint is either shown or dropped as stale, never lost; resize keeps
// the chain on the present queue; teardown drains the thread.
bool check_vrr_window(Device& state, sedici::FrameGenerationSettings::Multiplier multiplier, const char* label,
                      DWORD frameMs, bool signalFault, bool closeFault, bool resetFault, bool liveRecovery,
                      bool paramsFault)
{
    // Below ~20 ms per real frame at 1.5x the plan asks for more frames than a
    // 75 Hz panel can show. The game may then be held to the panel's pace, as
    // with vsync, but never slower: finding 009's game session spiralled to
    // 16 real fps because queueing was counted as latency.
    const bool overload = frameMs < 13;
    if (GetModuleHandleW(L"RTSSHooks64.dll") && GetEnvironmentVariableW(L"SEDICI_FG_ALLOW_RTSS", nullptr, 0) == 0) {
        // The wrapper refuses "vrr" under RTSS (see Swapchain::wrap); not a pass.
        std::printf("SKIP vrr presentation: RTSS is injected; close it or set SEDICI_FG_ALLOW_RTSS=1\n");
        return true;
    }
    const wchar_t* className = L"SediciFgVrr";
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = className;
    if (!RegisterClassW(&wc)) {
        std::printf("vrr: step failed at line %d\n", __LINE__);
        return false;
    }
    HWND window = CreateWindowW(className, L"Sedici vrr presentation", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                CW_USEDEFAULT, 500, 320, nullptr, nullptr, wc.hInstance, nullptr);
    if (!window) {
        UnregisterClassW(className, wc.hInstance);
        return false;
    }

    bool ok = [&] {
        ComPtr<IDXGIFactory5> factory;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
            std::printf("vrr: step failed at line %d\n", __LINE__);
            return false;
        }
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
        // Shaped like the game's chain: tearing allowed, latency waitable.
        desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
                     (canTear ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u);
        ComPtr<IDXGISwapChain1> created;
        if (FAILED(factory->CreateSwapChainForHwnd(state.queue.Get(), window, &desc, nullptr, nullptr, &created))) {
            std::printf("vrr: step failed at line %d\n", __LINE__);
            return false;
        }
        ComPtr<IDXGISwapChain4> native;
        if (FAILED(created.As(&native))) {
            std::printf("vrr: step failed at line %d\n", __LINE__);
            return false;
        }
        if (FAILED(native->SetMaximumFrameLatency(1))) {
            std::printf("vrr: step failed at line %d\n", __LINE__);
            return false;
        }

        sedici::fg::PresentChoice choice;
        choice.present = sedici::FrameGenerationSettings::Present::Vrr;
        choice.guard = sedici::FrameGenerationSettings::TearGuard::Scanline;
        choice.multiplier = multiplier;
        const bool doubled = multiplier == sedici::FrameGenerationSettings::Multiplier::X2;
        ComPtr<sedici::fg::Swapchain> wrapped;
        // Hand the only reference over, as the FFX wrap desc does for the game:
        // "vrr" destroys this chain and recreates it on the present queue.
        created.Reset();
        if (FAILED(sedici::fg::Swapchain::wrap(native.Get(), state.queue.Get(), &wrapped, &choice, 1))) {
            std::printf("vrr: step failed at line %d\n", __LINE__);
            return false;
        }
        native.Detach();
        if (!wrapped->async_present()) {
            std::printf("vrr: presenter did not start\n");
            return false;
        }
        uint32_t calls = 0;
        wrapped->set_generator(sedici::fg::GenerateSink{&calls, &record_counted, nullptr});

        ShowWindow(window, SW_SHOWNOACTIVATE);
        const UINT presentFlags = canTear ? DXGI_PRESENT_ALLOW_TEARING : 0u;
        if (paramsFault) {
            RECT dirty{0, 0, 16, 16};
            DXGI_PRESENT_PARAMETERS parameters{};
            parameters.DirtyRectsCount = 1;
            parameters.pDirtyRects = &dirty;
            const UINT beforeIndex = wrapped->GetCurrentBackBufferIndex();
            sedici::fg::arm_bookkeeping_fault(sedici::fg::BookkeepingFault::PresentParametersAllocation);
            const HRESULT first = wrapped->Present1(0, presentFlags, &parameters);
            const bool reached =
                !sedici::fg::consume_bookkeeping_fault(sedici::fg::BookkeepingFault::PresentParametersAllocation);
            sedici::fg::arm_bookkeeping_fault(sedici::fg::BookkeepingFault::None);
            if (!reached || first != E_OUTOFMEMORY || calls != 0 ||
                wrapped->GetCurrentBackBufferIndex() != beforeIndex) {
                std::puts("FAIL parameter-allocation failure changed frame admission");
                return false;
            }
            const HRESULT retry = wrapped->Present1(0, presentFlags, &parameters);
            const bool retried = SUCCEEDED(retry) && calls == 1;
            std::puts(retried ? "PASS Present1 retry with the same metadata is accepted"
                              : "FAIL Present1 retry after allocation error");
            if (!retried) return false;
            // K105: a refused presenter job leaves ownership unadvanced and a retry works.
            const UINT indexBefore = wrapped->GetCurrentBackBufferIndex();
            sedici::fg::arm_bookkeeping_fault(sedici::fg::BookkeepingFault::PresenterSubmit);
            const HRESULT refused = wrapped->Present(0, presentFlags);
            const bool hit = !sedici::fg::consume_bookkeeping_fault(sedici::fg::BookkeepingFault::PresenterSubmit);
            sedici::fg::arm_bookkeeping_fault(sedici::fg::BookkeepingFault::None);
            if (!hit || refused != E_OUTOFMEMORY || wrapped->GetCurrentBackBufferIndex() != indexBefore) {
                std::puts("FAIL a refused presenter job advanced buffer ownership");
                return false;
            }
            const HRESULT again = wrapped->Present(0, presentFlags);
            const bool accepted = SUCCEEDED(again) && wrapped->GetCurrentBackBufferIndex() != indexBefore &&
                                  g_debug_errors.load() == 0;
            std::puts(accepted
                          ? "PASS K105: a refused presenter job leaves ownership unchanged and a retry is accepted"
                          : "FAIL retry after a refused presenter job");
            return accepted;
        }
        if (signalFault || closeFault || resetFault) {
            ComPtr<ID3D12Fence> recoveryGate;
            if (liveRecovery) {
                if (FAILED(state.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&recoveryGate))) ||
                    FAILED(state.queue->Wait(recoveryGate.Get(), 1)))
                    return false;
            }
            const auto fault = resetFault   ? sedici::fg::BookkeepingFault::GenerationReset
                               : closeFault ? sedici::fg::BookkeepingFault::GenerationClose
                                            : sedici::fg::BookkeepingFault::GenerationSignal;
            sedici::fg::arm_bookkeeping_fault(fault);
            const HRESULT first = wrapped->Present(0, presentFlags);
            const bool reachedInjection = !sedici::fg::consume_bookkeeping_fault(fault);
            sedici::fg::arm_bookkeeping_fault(sedici::fg::BookkeepingFault::None);
            const HRESULT retry = wrapped->Present(0, presentFlags);
            const bool stopped = reachedInjection && first == E_FAIL && retry == E_FAIL &&
                                 calls == (resetFault ? 0u : 1u) && g_debug_errors.load() == 0;
            if (!g_debug_callback_installed) std::puts("INFO DX12 debug callback unavailable");
            if (liveRecovery) {
                if (!stopped) {
                    (void)recoveryGate->Signal(1);
                    return false;
                }
                // The generation list is queued behind recoveryGate. Release it
                // only once teardown has queued the replacement retirement fence.
                recoveryTestGate = recoveryGate.Get();
                recoveryTestReleased = false;
                const unsigned retainedBefore = sedici::fg::swapchain_retention_count();
                sedici::fg::arm_queue_recovery_hook(&release_recovery_test_gate);
                const auto before = std::chrono::steady_clock::now();
                auto* finalReference = wrapped.Detach();
                const ULONG remaining = finalReference->Release();
                const auto elapsed =
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - before)
                        .count();
                sedici::fg::arm_queue_recovery_hook(nullptr);
                recoveryTestGate = nullptr;
                (void)recoveryGate->Signal(1); // Unblock even if recovery failed.
                const bool recovered = remaining == 0 && recoveryTestReleased.load() && elapsed < 3000 &&
                                       sedici::fg::swapchain_retention_count() == retainedBefore &&
                                       SUCCEEDED(state.device->GetDeviceRemovedReason()) &&
                                       g_debug_errors.load() == 0;
                std::puts(recovered ? "PASS wrapper teardown recovered gated GPU work on a live device"
                                    : "FAIL wrapper teardown did not recover gated GPU work");
                return recovered;
            }
            ComPtr<ID3D12Device5> removable;
            if (FAILED(state.device.As(&removable))) return false;
            removable->RemoveDevice();
            std::puts(stopped ? "PASS VRR injected failure stops later admission"
                              : "FAIL VRR injected failure did not stop admission");
            return stopped;
        }
        const UINT cycles = 40;
        uint64_t skippedAtTen = 0;
        HANDLE latency = wrapped->GetFrameLatencyWaitableObject();
        LARGE_INTEGER loopStart{}, loopEnd{}, frequency{};
        LONGLONG waitableTicks = 0, presentTicks = 0;
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&loopStart);
        for (UINT cycle = 0; cycle < cycles; ++cycle) {
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            LARGE_INTEGER waitStart{}, waitEnd{};
            QueryPerformanceCounter(&waitStart);
            if (latency) WaitForSingleObjectEx(latency, 1000, TRUE);
            QueryPerformanceCounter(&waitEnd);
            waitableTicks += waitEnd.QuadPart - waitStart.QuadPart;
            Sleep(frameMs); // 27 ms: ~35 fps real rate, under half of a 75 Hz refresh like the game cap
            ComPtr<ID3D12Resource> buffer;
            if (FAILED(wrapped->GetBuffer(wrapped->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&buffer)))) {
                std::printf("vrr: step failed at line %d\n", __LINE__);
                return false;
            }
            if (FAILED(state.allocator->Reset()) || FAILED(state.list->Reset(state.allocator.Get(), nullptr))) {
                std::printf("vrr: step failed at line %d\n", __LINE__);
                return false;
            }
            transition(state.list.Get(), buffer.Get(), D3D12_RESOURCE_STATE_PRESENT,
                       D3D12_RESOURCE_STATE_COPY_DEST);
            transition(state.list.Get(), buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_PRESENT);
            if (!state.submit()) {
                std::printf("vrr: step failed at line %d\n", __LINE__);
                return false;
            }
            LARGE_INTEGER presentStart{}, presentEnd{};
            QueryPerformanceCounter(&presentStart);
            const HRESULT presentResult = wrapped->Present(0, presentFlags);
            QueryPerformanceCounter(&presentEnd);
            presentTicks += presentEnd.QuadPart - presentStart.QuadPart;
            if (const HRESULT hr = presentResult; FAILED(hr)) {
                std::printf("vrr: Present failed at cycle %u hr=0x%08lx\n", cycle, static_cast<unsigned long>(hr));
                return false;
            }
            if (wrapped->GetCurrentBackBufferIndex() != (cycle + 1) % 3) {
                std::printf("vrr: index\n");
                return false;
            }
            if (cycle == 9) skippedAtTen = wrapped->skipped_generates();
        }
        QueryPerformanceCounter(&loopEnd);
        const double perFrameMs = 1000.0 * static_cast<double>(loopEnd.QuadPart - loopStart.QuadPart) /
                                  static_cast<double>(frequency.QuadPart) / cycles;
        // Resize drains the presenter, so every output has been issued after it.
        if (const HRESULT hr = wrapped->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, desc.Flags); FAILED(hr)) {
            std::printf("vrr: resize failed hr=0x%08lx\n", static_cast<unsigned long>(hr));
            return false;
        }
        UINT presented = 0;
        if (FAILED(wrapped->GetLastPresentCount(&presented))) {
            std::printf("vrr: step failed at line %d\n", __LINE__);
            return false;
        }
        const uint64_t shown = wrapped->generated_presents();
        const uint64_t skipped = wrapped->skipped_generates();
        std::printf(
            "vrr presentation %s: %u recordings, %llu shown, %llu skipped, %u native presents, tearing=%s\n", label,
            calls, static_cast<unsigned long long>(shown), static_cast<unsigned long long>(skipped), presented,
            canTear ? "yes" : "no");
        const double toMs = 1000.0 / static_cast<double>(frequency.QuadPart) / cycles;
        std::printf("vrr: skipped in the first 10 cycles: %llu, game loop %.2f ms per frame (sleep %lu, "
                    "latency waitable %.2f ms, Present %.2f ms)\n",
                    static_cast<unsigned long long>(skippedAtTen), perFrameMs, frameMs, waitableTicks * toMs,
                    presentTicks * toMs);
        // Time the test game spent inside Sedici per frame. The loop time itself is
        // dominated by Sleep's 15.6 ms granularity (Sleep(21) and Sleep(27) both
        // take ~31 ms), so it says nothing about blocking.
        const double sediciMs = (waitableTicks + presentTicks) * toMs;
        if (overload) {
            // More frames are planned than the panel can show: the presenter
            // drops outputs and the game waits at most about one panel pace per
            // frame, and nothing beyond the plan is presented.
            const UINT planned = 1 + (cycles - 1) * 3 / 2;
            DEVMODEW mode{};
            mode.dmSize = sizeof(mode);
            const double refreshMs =
                EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1
                    ? 1000.0 / mode.dmDisplayFrequency
                    : 1000.0 / 60.0;
            const double panelPace = 1.5 * refreshMs; // 1.5 flips per real frame
            if (sediciMs > panelPace) {
                std::printf("vrr: game waited %.2f ms per frame in Sedici (panel pace %.2f ms)\n", sediciMs,
                            panelPace);
                return false;
            }
            if (presented > planned) {
                std::printf("vrr: step failed at line %d\n", __LINE__);
                return false;
            }
        } else if (doubled) {
            if (calls != cycles) {
                std::printf("vrr: step failed at line %d\n", __LINE__);
                return false;
            }
            if (shown + skipped != cycles - 1) {
                std::printf("vrr: step failed at line %d\n", __LINE__);
                return false;
            }
            // Every real frame plus every shown midpoint, less at most two outputs
            // the presenter dropped because a later one was already due (timing
            // dependent; seen once under CTest). Never more than planned.
            const UINT planned = static_cast<UINT>(cycles + shown);
            if (presented > planned || presented + 2 < planned) {
                std::printf("vrr: presented %u of %u planned\n", presented, planned);
                return false;
            }
        } else if (multiplier == sedici::FrameGenerationSettings::Multiplier::Adaptive) {
            // ~40 real fps towards 72: about 1.8 outputs per real frame, and the
            // game is not held back by the presenter (two frames may be in flight).
            const double expected = cycles * 72.0 * perFrameMs / 1000.0;
            if (sediciMs > 2.0) {
                std::printf("vrr: adaptive game waited %.2f ms per frame in Sedici\n", sediciMs);
                return false;
            }
            if (presented < 0.8 * expected || presented > expected + 4.0) {
                std::printf("vrr: adaptive presented %u, expected about %.0f\n", presented, expected);
                return false;
            }
        } else {
            // 1.5x: 3 outputs per 2 real frames after the first, two of them
            // generated. Allow a few start-up drops, never more than planned.
            const UINT planned = 1 + (cycles - 1) * 3 / 2;
            if (calls < cycles) {
                std::printf("vrr: step failed at line %d\n", __LINE__);
                return false;
            }
            if (presented > planned || presented + 6 < planned) {
                std::printf("vrr: step failed at line %d (presented %u of %u)\n", __LINE__, presented, planned);
                return false;
            }
            if (shown + 4 < (planned - 1) * 2 / 3) {
                std::printf("vrr: step failed at line %d\n", __LINE__);
                return false;
            }
        }
        // No flip may follow the previous one within 2 ms (finding 009: the
        // scan-line guard opened before the previous flip had latched).
        if (const uint64_t backToBack = wrapped->back_to_back_flips(); backToBack != 0) {
            std::printf("vrr: %llu back-to-back flips\n", static_cast<unsigned long long>(backToBack));
            return false;
        }
        // Presentation continues after the resize, still from the presenter.
        for (UINT cycle = 0; cycle < 3; ++cycle) {
            if (FAILED(wrapped->Present(0, presentFlags))) {
                std::printf("vrr: step failed at line %d\n", __LINE__);
                return false;
            }
        }
        wrapped->set_generator(sedici::fg::GenerateSink{});
        return true;
    }();

    DestroyWindow(window);
    UnregisterClassW(className, wc.hInstance);
    std::printf("%s vrr presentation %s: %s\n", ok ? "PASS" : "FAIL", label,
                paramsFault                                 ? "parameter allocation failure and retry"
                : (signalFault || closeFault || resetFault) ? "injected failure stops admission"
                                                            : "presenter thread, queue, resize, teardown");
    return ok;
}

// One generated frame is shown per cycle starting with the second: the midpoint
// for a pair cannot be shown before the pair exists.
bool check_generated_window(Device& state, bool* knownSignature)
{
    if (knownSignature) *knownSignature = false;
    const wchar_t* className = L"SediciFgGenerated";
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = className;
    if (!RegisterClassW(&wc)) return false;
    HWND window = CreateWindowW(className, L"Sedici generated presentation", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                CW_USEDEFAULT, 500, 320, nullptr, nullptr, wc.hInstance, nullptr);
    if (!window) {
        UnregisterClassW(className, wc.hInstance);
        return false;
    }

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
        if (FAILED(sedici::fg::Swapchain::wrap(native.Get(), state.queue.Get(), &wrapped))) return false;

        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = 1;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ComPtr<ID3D12DescriptorHeap> uavHeap;
        if (FAILED(state.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&uavHeap)))) return false;
        TestGenerator generator{0, native.Get()};
        sedici::fg::GenerateSink sink{&generator, &record_solid, nullptr};
        wrapped->set_generator(sink);

        ShowWindow(window, SW_SHOWNOACTIVATE);
        const UINT cycles = 6;
        for (UINT cycle = 0; cycle < cycles; ++cycle) {
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            ComPtr<ID3D12Resource> buffer;
            if (FAILED(wrapped->GetBuffer(wrapped->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&buffer))))
                return false;
            if (FAILED(state.allocator->Reset()) || FAILED(state.list->Reset(state.allocator.Get(), nullptr)))
                return false;
            transition(state.list.Get(), buffer.Get(), D3D12_RESOURCE_STATE_PRESENT,
                       D3D12_RESOURCE_STATE_COPY_DEST);
            transition(state.list.Get(), buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_PRESENT);
            if (!state.submit()) return false;
            if (FAILED(wrapped->Present(1, 0))) return false;
            if (wrapped->GetCurrentBackBufferIndex() != (cycle + 1) % 3) return false;
        }

        std::printf("generated presentation: %u recordings, %llu shown, %llu skipped\n", generator.calls,
                    static_cast<unsigned long long>(wrapped->generated_presents()),
                    static_cast<unsigned long long>(wrapped->skipped_generates()));
        if (generator.calls != cycles) return false;
        if (!generator.before_present || wrapped->generated_presents() != cycles - 1) {
            // The historical 2-shown/3-skipped result also causes the
            // before-present count check to fail after the first skipped frame.
            if (knownSignature && wrapped->generated_presents() == 2 && wrapped->skipped_generates() == 3)
                *knownSignature = true;
            return false;
        }
        if (wrapped->skipped_generates() != 0) return false;
        UINT presented = 0;
        if (FAILED(wrapped->GetLastPresentCount(&presented))) return false;
        if (presented != 2 * cycles - 1) return false;

        // Clearing the sink must stop generated presentation without stalling
        // real frames.
        wrapped->set_generator(sedici::fg::GenerateSink{});
        for (UINT cycle = 0; cycle < 2; ++cycle) {
            if (FAILED(wrapped->Present(1, 0))) return false;
        }
        if (FAILED(wrapped->GetLastPresentCount(&presented)) || presented != 2 * cycles + 1) return false;

        wrapped->set_generator(sedici::fg::GenerateSink{});
        return true;
    }();

    DestroyWindow(window);
    UnregisterClassW(className, wc.hInstance);
    std::printf("%s generated presentation: one midpoint per cycle, bounded, sink removable\n",
                ok ? "PASS" : "FAIL");
    return ok;
}

// The production path end to end: a wrapped swapchain, an FFX-shaped frame
// generation session bound to it, and generated frames coming out of Sedici's
// own presentation schedule. Native buffer readback verifies temporal order,
// current-frame HUD labels, and the actual midpoint image rather than counts.
HANDLE contextRecordArrived = nullptr;
HANDLE contextRecordResume = nullptr;
void pause_context_record() noexcept
{
    SetEvent(contextRecordArrived);
    (void)WaitForSingleObject(contextRecordResume, 5000);
}

bool check_session_window(Device& state, bool loseDevice, bool signalFault, bool closeFault, bool resetFault,
                          bool contextRace, bool sessionRecovery, bool registrationFault, bool* knownSignature)
{
    if (knownSignature) *knownSignature = false;
    const wchar_t* className = L"SediciFgSession";
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = className;
    if (!RegisterClassW(&wc)) return false;
    HWND window = CreateWindowW(className, L"Sedici session", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                500, 320, nullptr, nullptr, wc.hInstance, nullptr);
    if (!window) {
        UnregisterClassW(className, wc.hInstance);
        return false;
    }

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
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        ComPtr<IDXGISwapChain1> created;
        if (FAILED(factory->CreateSwapChainForHwnd(state.queue.Get(), window, &desc, nullptr, nullptr, &created)))
            return false;
        ComPtr<IDXGISwapChain4> native;
        if (FAILED(created.As(&native))) return false;

        sedici::fg::WrapHandle handle;
        if (!handle.create(native.Get(), state.queue.Get()) || !handle.swapchain()) {
            std::printf("session: wrap failed\n");
            return false;
        }

        sedici::fg::Session stackSession;
        std::shared_ptr<sedici::fg::Session> ownedSession;
        if (sessionRecovery) {
            ownedSession = std::make_shared<sedici::fg::Session>();
            ownedSession->set_callback_lifetime(ownedSession);
        }
        sedici::fg::Session& session = ownedSession ? *ownedSession : stackSession;
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
        if (session.state() != sedici::fg::SessionState::Empty || session.configure(&create.header) ||
            session.prepare(&create.header))
            return false; // refused before create
        if (!session.create(&create.header)) {
            std::printf("session: create failed: %s\n", session.context().last_error());
            return false;
        }
        if (session.state() != sedici::fg::SessionState::Ready) return false;
        ffxConfigureDescFrameGeneration stateProbe{};
        stateProbe.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        stateProbe.frameGenerationEnabled = true;
        if (!session.configure(&stateProbe.header) || session.state() != sedici::fg::SessionState::Degraded)
            return false;
        stateProbe.frameGenerationEnabled = false;
        if (!session.configure(&stateProbe.header) || session.state() != sedici::fg::SessionState::Suspended)
            return false;

        // Five consecutive moving frames, not repeating/resetting one pair.
        constexpr uint32_t cycles = 5;
        FrameTextures frames[cycles];
        for (uint32_t index = 0; index < cycles; ++index) {
            const auto display = make_scene(kScenes[0], static_cast<float>(index), kDisplayWidth, kDisplayHeight);
            const auto render = make_scene(kScenes[0], static_cast<float>(index), kRenderWidth, kRenderHeight);
            frames[index].color = create_texture(state.device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, kDisplayWidth,
                                                 kDisplayHeight, D3D12_RESOURCE_FLAG_NONE);
            frames[index].depth = create_texture(state.device.Get(), DXGI_FORMAT_R32_FLOAT, kRenderWidth,
                                                 kRenderHeight, D3D12_RESOURCE_FLAG_NONE);
            frames[index].motion = create_texture(state.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, kRenderWidth,
                                                  kRenderHeight, D3D12_RESOURCE_FLAG_NONE);
            if (!frames[index].color || !frames[index].depth || !frames[index].motion) return false;

            const auto colorBytes = quantise(display.color);
            const auto motionBytes = pack_ffx_motion(render.motion);
            const auto colorPitch = align_to(kDisplayWidth * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
            const auto renderPitch = align_to(kRenderWidth * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
            const auto motionPitch = align_to(kRenderWidth * 8, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);

            auto colorStaging = create_buffer(state.device.Get(), colorPitch * kDisplayHeight,
                                              D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
            auto depthStaging = create_buffer(state.device.Get(), renderPitch * kRenderHeight,
                                              D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
            auto motionStaging = create_buffer(state.device.Get(), motionPitch * kRenderHeight,
                                               D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
            if (!colorStaging || !depthStaging || !motionStaging) return false;

            if (FAILED(state.allocator->Reset()) || FAILED(state.list->Reset(state.allocator.Get(), nullptr)))
                return false;
            upload(state.list.Get(), frames[index].color.Get(), colorStaging.Get(), colorBytes.data(),
                   kDisplayWidth, kDisplayHeight, 4);
            upload(state.list.Get(), frames[index].depth.Get(), depthStaging.Get(), render.depth.data(),
                   kRenderWidth, kRenderHeight, 4);
            upload(state.list.Get(), frames[index].motion.Get(), motionStaging.Get(), motionBytes.data(),
                   kRenderWidth, kRenderHeight, 8);
            if (!state.submit()) return false;
        }

        const uint32_t hudPitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
        auto hudUpload = create_buffer(state.device.Get(), hudPitch * 8, D3D12_HEAP_TYPE_UPLOAD,
                                       D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!hudUpload) return false;
        ShowWindow(window, SW_SHOWNOACTIVATE);
        for (uint32_t cycle = 0; cycle < cycles; ++cycle) {
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            const uint32_t source = cycle;
            if (FAILED(state.allocator->Reset()) || FAILED(state.list->Reset(state.allocator.Get(), nullptr)))
                return false;

            ffxConfigureDescFrameGeneration configure{};
            configure.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
            configure.frameGenerationEnabled = true;
            configure.swapChain = handle.swapchain();
            configure.HUDLessColor =
                ffxApiGetResourceDX12(frames[source].color.Get(), FFX_API_RESOURCE_STATE_COMMON);
            configure.frameID = cycle;

            ffxDispatchDescFrameGenerationPrepare prepare{};
            prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
            prepare.frameID = cycle;
            prepare.commandList = state.list.Get();
            prepare.renderSize = {kRenderWidth, kRenderHeight};
            prepare.motionVectorScale = {static_cast<float>(kRenderWidth), static_cast<float>(kRenderHeight)};
            prepare.depth = ffxApiGetResourceDX12(frames[source].depth.Get(), FFX_API_RESOURCE_STATE_COMMON);
            prepare.motionVectors =
                ffxApiGetResourceDX12(frames[source].motion.Get(), FFX_API_RESOURCE_STATE_COMMON);
            prepare.unused_reset = source == 0;
            if (!session.configure(&configure.header)) {
                std::printf("session: configure failed: %s\n", session.context().last_error());
                return false;
            }
            if (cycle == 0 && session.state() != sedici::fg::SessionState::Warmup) return false;
            if (!session.prepare(&prepare.header)) {
                std::printf("session: prepare failed: %s\n", session.context().last_error());
                return false;
            }

            // Stand in for the game's own rendering of this frame.
            ComPtr<ID3D12Resource> buffer;
            if (FAILED(handle.swapchain()->GetBuffer(handle.swapchain()->GetCurrentBackBufferIndex(),
                                                     IID_PPV_ARGS(&buffer))))
                return false;
            transition(state.list.Get(), frames[source].color.Get(), D3D12_RESOURCE_STATE_COMMON,
                       D3D12_RESOURCE_STATE_COPY_SOURCE);
            transition(state.list.Get(), buffer.Get(), D3D12_RESOURCE_STATE_PRESENT,
                       D3D12_RESOURCE_STATE_COPY_DEST);
            state.list->CopyResource(buffer.Get(), frames[source].color.Get());
            transition(state.list.Get(), frames[source].color.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                       D3D12_RESOURCE_STATE_COMMON);
            const float marker[] = {static_cast<float>(cycle + 1) / 8.0f, 1.0f, 0.0f, 1.0f};
            void* mappedHud = nullptr;
            if (FAILED(hudUpload->Map(0, nullptr, &mappedHud))) return false;
            for (uint32_t y = 0; y < 8; ++y) {
                for (uint32_t x = 0; x < 8; ++x) {
                    auto* pixel = static_cast<uint8_t*>(mappedHud) + y * hudPitch + x * 4;
                    pixel[0] = static_cast<uint8_t>(std::lround(marker[0] * 255.0f));
                    pixel[1] = pixel[3] = 255;
                    pixel[2] = 0;
                }
            }
            hudUpload->Unmap(0, nullptr);
            D3D12_TEXTURE_COPY_LOCATION hudFrom{};
            hudFrom.pResource = hudUpload.Get();
            hudFrom.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            hudFrom.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, 8, 8, 1, hudPitch};
            D3D12_TEXTURE_COPY_LOCATION hudTo{};
            hudTo.pResource = buffer.Get();
            hudTo.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            state.list->CopyTextureRegion(&hudTo, 0, kDisplayHeight - 8, 0, &hudFrom, nullptr);
            transition(state.list.Get(), buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_PRESENT);
            if (!state.submit()) return false;

            if (contextRace && cycle == 0) {
                contextRecordArrived = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                contextRecordResume = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                HANDLE configureStarted = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                HANDLE configureDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                HANDLE prepareStarted = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                HANDLE prepareDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                if (!contextRecordArrived || !contextRecordResume || !configureStarted || !configureDone ||
                    !prepareStarted || !prepareDone) {
                    if (contextRecordArrived) CloseHandle(contextRecordArrived);
                    if (contextRecordResume) CloseHandle(contextRecordResume);
                    if (configureStarted) CloseHandle(configureStarted);
                    if (configureDone) CloseHandle(configureDone);
                    if (prepareStarted) CloseHandle(prepareStarted);
                    if (prepareDone) CloseHandle(prepareDone);
                    contextRecordArrived = contextRecordResume = nullptr;
                    return false;
                }
                sedici::fg::arm_context_record_hook(&pause_context_record);
                HRESULT presentResult = E_FAIL;
                std::thread presenting([&] { presentResult = handle.swapchain()->Present(1, 0); });
                const bool arrived = WaitForSingleObject(contextRecordArrived, 5000) == WAIT_OBJECT_0;
                bool configured = false;
                bool prepareAccepted = true;
                bool blocked = false;
                std::thread configuring;
                std::thread preparing;
                if (arrived) {
                    auto configureAgain = configure;
                    configuring = std::thread([&, configureAgain]() mutable {
                        SetEvent(configureStarted);
                        configured = session.configure(&configureAgain.header);
                        SetEvent(configureDone);
                    });
                    preparing = std::thread([&] {
                        ffxDispatchDescFrameGenerationPrepare invalid{};
                        invalid.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
                        SetEvent(prepareStarted);
                        prepareAccepted = session.prepare(&invalid.header);
                        SetEvent(prepareDone);
                    });
                    const bool started = WaitForSingleObject(configureStarted, 5000) == WAIT_OBJECT_0 &&
                                         WaitForSingleObject(prepareStarted, 5000) == WAIT_OBJECT_0;
                    blocked = started && WaitForSingleObject(configureDone, 50) == WAIT_TIMEOUT &&
                              WaitForSingleObject(prepareDone, 50) == WAIT_TIMEOUT;
                }
                SetEvent(contextRecordResume);
                presenting.join();
                if (configuring.joinable()) configuring.join();
                if (preparing.joinable()) preparing.join();
                sedici::fg::arm_context_record_hook(nullptr);
                CloseHandle(contextRecordArrived);
                CloseHandle(contextRecordResume);
                CloseHandle(configureStarted);
                CloseHandle(configureDone);
                CloseHandle(prepareStarted);
                CloseHandle(prepareDone);
                contextRecordArrived = contextRecordResume = nullptr;
                const bool serialized = arrived && blocked && SUCCEEDED(presentResult) && configured &&
                                        !prepareAccepted && g_debug_errors.load() == 0;
                std::puts(serialized ? "PASS configure and prepare wait for an active context callback"
                                     : "FAIL configure/context callback synchronization");
                return serialized;
            }

            ComPtr<ID3D12Fence> sessionRecoveryGate;
            if (sessionRecovery && cycle == 0) {
                if (FAILED(
                        state.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&sessionRecoveryGate))) ||
                    FAILED(state.queue->Wait(sessionRecoveryGate.Get(), 1)))
                    return false;
            }
            const UINT firstNative = native->GetCurrentBackBufferIndex();
            const auto fault = registrationFault ? sedici::fg::BookkeepingFault::SubmissionRegistration
                               : resetFault      ? sedici::fg::BookkeepingFault::GenerationReset
                               : closeFault      ? sedici::fg::BookkeepingFault::GenerationClose
                                                 : sedici::fg::BookkeepingFault::GenerationSignal;
            if ((signalFault || closeFault || resetFault || sessionRecovery) && cycle == 0)
                sedici::fg::arm_bookkeeping_fault(fault);
            const HRESULT presentResult = handle.swapchain()->Present(1, 0);
            if ((signalFault || closeFault || resetFault || sessionRecovery) && cycle == 0) {
                const bool reachedInjection = !sedici::fg::consume_bookkeeping_fault(fault);
                sedici::fg::arm_bookkeeping_fault(sedici::fg::BookkeepingFault::None);
                const HRESULT retry = handle.swapchain()->Present(1, 0);
                const bool stopped = reachedInjection && presentResult == E_FAIL && retry == E_FAIL &&
                                     !session.retired() && g_debug_errors.load() == 0;
                if (!g_debug_callback_installed) std::puts("INFO DX12 debug callback unavailable");
                if (sessionRecovery) {
                    if (!stopped) {
                        (void)sessionRecoveryGate->Signal(1);
                        return false;
                    }
                    // K104: while destroy polls its bounded window, other callers of
                    // the context mutex must not be held for that whole window.
                    bool destroyRefused = false;
                    std::atomic<bool> destroyStarted{false};
                    std::thread destroying([&] {
                        destroyStarted = true;
                        destroyRefused = !session.destroy();
                    });
                    while (!destroyStarted.load()) std::this_thread::yield();
                    std::this_thread::sleep_for(std::chrono::milliseconds(40));
                    const auto probeStart = std::chrono::steady_clock::now();
                    (void)session.ready();
                    const auto probeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                             std::chrono::steady_clock::now() - probeStart)
                                             .count();
                    destroying.join();
                    const bool lockFree = probeMs < 60;
                    std::printf("%s K104: a context call during the destroy wait is not blocked (%lld ms)%c",
                                lockFree ? "PASS" : "FAIL", static_cast<long long>(probeMs), 10);
                    const bool firstDestroyRefused =
                        lockFree && destroyRefused && session.state() == sedici::fg::SessionState::Draining;
                    recoveryTestGate = sessionRecoveryGate.Get();
                    recoveryTestReleased = false;
                    const unsigned retainedBefore = sedici::fg::swapchain_retention_count();
                    sedici::fg::arm_queue_recovery_hook(&release_recovery_test_gate);
                    const auto before = std::chrono::steady_clock::now();
                    handle.destroy();
                    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                             std::chrono::steady_clock::now() - before)
                                             .count();
                    sedici::fg::arm_queue_recovery_hook(nullptr);
                    recoveryTestGate = nullptr;
                    (void)sessionRecoveryGate->Signal(1);
                    const bool retired = session.retired();
                    const bool destroyed = session.destroy();
                    const bool recovered = firstDestroyRefused && recoveryTestReleased.load() && elapsed < 3000 &&
                                           sedici::fg::swapchain_retention_count() == retainedBefore && retired &&
                                           destroyed && session.state() == sedici::fg::SessionState::Stopped &&
                                           SUCCEEDED(state.device->GetDeviceRemovedReason()) &&
                                           g_debug_errors.load() == 0;
                    std::puts(recovered ? "PASS session retires after a replacement queue fence"
                                        : "FAIL session retirement after signal failure");
                    return recovered;
                }
                // Remove only this isolated test process's device before teardown.
                ComPtr<ID3D12Device5> removable;
                if (FAILED(state.device.As(&removable))) return false;
                removable->RemoveDevice();
                std::puts(stopped ? "PASS injected generation failure stops later admission"
                                  : "FAIL injected generation failure did not stop admission");
                return stopped;
            }
            if (FAILED(presentResult)) {
                std::puts("session: present failed");
                return false;
            }
            if (cycle == 0 && session.state() != sedici::fg::SessionState::Active) return false;
            std::printf("session: cycle %u generated %llu shown %llu skipped %llu\n", cycle,
                        static_cast<unsigned long long>(session.context().frames_generated()),
                        static_cast<unsigned long long>(handle.wrapper()->generated_presents()),
                        static_cast<unsigned long long>(handle.wrapper()->skipped_generates()));

            if (loseDevice && cycle == 1) {
                // P2.5 device loss after generation; this does not claim that
                // the earlier GPU work is still in flight at removal time.
                // Present must fail and teardown must remain bounded (K38/K73).
                ComPtr<ID3D12Device5> device5;
                if (FAILED(state.device.As(&device5))) {
                    std::printf("SKIP device loss: ID3D12Device5 unavailable\n");
                    return true;
                }
                device5->RemoveDevice();
                if (session.state() != sedici::fg::SessionState::DeviceLost || session.ready() ||
                    session.configure(&configure.header) || session.prepare(&prepare.header))
                    return false;
                const HRESULT afterLoss = handle.swapchain()->Present(1, 0);
                std::printf("session: present after device removal -> 0x%08lx\n",
                            static_cast<unsigned long>(afterLoss));
                if (SUCCEEDED(afterLoss)) return false;
                const auto begin = std::chrono::steady_clock::now();
                if (!session.destroy()) {
                    std::printf("session: destroy refused after device removal: %s\n",
                                session.context().last_error());
                    return false;
                }
                if (session.state() != sedici::fg::SessionState::Stopped) return false;
                handle.destroy();
                const auto ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin)
                        .count();
                std::printf("session: teardown after device removal took %lld ms\n", static_cast<long long>(ms));
                return ms < 3000;
            }

            // Each native image must belong to this cycle. FLIP_SEQUENTIAL
            // preserves these buffers after Present. Exclude the top-left OSD
            // area from the scene comparison, but verify the HUD label below it.
            for (UINT image = 0; image < (cycle ? 2u : 1u); ++image) {
                ComPtr<ID3D12Resource> presented;
                if (FAILED(native->GetBuffer((firstNative + image) % desc.BufferCount, IID_PPV_ARGS(&presented))))
                    return false;
                const auto actual = read_presented(state, presented.Get());
                if (actual.empty()) return false;
                const bool midpoint = cycle && image == 0;
                const float time = static_cast<float>(cycle) - (midpoint ? 0.5f : 0.0f);
                const auto expected = quantise(make_scene(kScenes[0], time, kDisplayWidth, kDisplayHeight).color);
                std::vector<uint8_t> sceneActual, sceneExpected;
                for (uint32_t y = 80; y < kDisplayHeight - 16; ++y) {
                    const size_t begin = (static_cast<size_t>(y) * kDisplayWidth + 160) * 4;
                    const size_t end = static_cast<size_t>(y + 1) * kDisplayWidth * 4;
                    sceneActual.insert(sceneActual.end(), actual.begin() + begin, actual.begin() + end);
                    sceneExpected.insert(sceneExpected.end(), expected.begin() + begin, expected.begin() + end);
                }
                const double score = psnr(sceneExpected, sceneActual);
                const size_t label = (static_cast<size_t>(kDisplayHeight - 4) * kDisplayWidth + 4) * 4;
                const int red = static_cast<int>(std::lround(marker[0] * 255.0f));
                const bool matchingHud = std::abs(static_cast<int>(actual[label]) - red) <= 1 &&
                                         actual[label + 1] == 255 && actual[label + 2] == 0;
                std::printf("session: frame %u %s time=%.1f psnr=%.2f HUD=%s\n", cycle,
                            midpoint ? "midpoint" : "real", time, score, matchingHud ? "current" : "WRONG");
                if (!matchingHud || (midpoint ? score < 35.0 : sceneActual != sceneExpected)) {
                    if (knownSignature && cycle == 2 && midpoint && matchingHud && score >= 15.5 && score <= 16.5 &&
                        handle.wrapper()->generated_presents() == 1 && handle.wrapper()->skipped_generates() == 1)
                        *knownSignature = true;
                    return false;
                }
            }
        }

        if (session.context().frames_generated() != cycles) {
            std::printf("session: generated %llu of %u\n",
                        static_cast<unsigned long long>(session.context().frames_generated()), cycles);
            std::printf("session: context last error '%s'\n", session.context().last_error());
            return false;
        }
        if (handle.wrapper()->generated_presents() != cycles - 1) {
            std::printf("session: presented %llu of %u, skipped %llu\n",
                        static_cast<unsigned long long>(handle.wrapper()->generated_presents()), cycles - 1,
                        static_cast<unsigned long long>(handle.wrapper()->skipped_generates()));
            return false;
        }
        if (handle.wrapper()->skipped_generates() != 0) return false;

        // Turning frame generation off must stop generation without breaking
        // presentation, and destruction must respect GPU retirement.
        ffxConfigureDescFrameGeneration off{};
        off.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        off.frameGenerationEnabled = false;
        off.swapChain = handle.swapchain();
        off.frameID = cycles;
        if (!session.configure(&off.header)) return false;
        if (session.state() != sedici::fg::SessionState::Suspended) return false;
        if (FAILED(handle.swapchain()->Present(1, 0))) return false;
        if (handle.wrapper()->generated_presents() != cycles - 1) return false;

        if (!session.destroy()) return false;
        if (session.state() != sedici::fg::SessionState::Stopped) return false;
        // Detached: the wrapper keeps presenting and never calls the destroyed owner.
        if (FAILED(handle.swapchain()->Present(1, 0))) return false;
        if (handle.wrapper()->generated_presents() != cycles - 1) return false;
        if (session.configure(&off.header) || session.prepare(&off.header)) return false; // refused after stop
        if (!session.destroy()) return false;                                             // idempotent
        handle.destroy();
        return true;
    }();

    DestroyWindow(window);
    UnregisterClassW(className, wc.hInstance);
    std::printf("%s session: %s\n", ok ? "PASS" : "FAIL",
                contextRace       ? "context callback synchronization"
                : sessionRecovery ? "session retirement recovery"
                : (signalFault || closeFault || resetFault)
                    ? "injected failure stops admission"
                    : "describe, prepare, generate and present through the wrapper");
    return ok;
}

} // namespace fgcheck
