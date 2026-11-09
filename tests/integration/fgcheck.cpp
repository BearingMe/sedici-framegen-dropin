// Offline acceptance host for the frame generator (see MILESTONES.md, P1.4). The check
// groups are in the fgcheck_*.cpp files next to this one.
#include "fgcheck_common.h"

using namespace fgcheck;

extern "C" const char g_sedici_proxy_marker[] = "SEDICI_PROXY_V1";

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    bool keepImages = false, present = false, routing = false, deviceLoss = false, signalFault = false;
    bool signalFaultVrr = false, closeFault = false, closeFaultVrr = false;
    bool resetFault = false, resetFaultVrr = false, contextRace = false, recoveryVrr = false;
    bool paramsFaultVrr = false, sessionRecovery = false, registrationRecovery = false, retention = false;
    for (int i = 1; i < argc; ++i) {
        deviceLoss |= std::string(argv[i]) == "--device-loss";
        signalFault |= std::string(argv[i]) == "--submission-fault";
        signalFaultVrr |= std::string(argv[i]) == "--submission-fault-vrr";
        closeFault |= std::string(argv[i]) == "--recording-fault";
        closeFaultVrr |= std::string(argv[i]) == "--recording-fault-vrr";
        resetFault |= std::string(argv[i]) == "--reset-fault";
        resetFaultVrr |= std::string(argv[i]) == "--reset-fault-vrr";
        recoveryVrr |= std::string(argv[i]) == "--submission-recovery-vrr";
        paramsFaultVrr |= std::string(argv[i]) == "--params-fault-vrr";
        sessionRecovery |= std::string(argv[i]) == "--session-recovery";
        registrationRecovery |= std::string(argv[i]) == "--registration-recovery";
        contextRace |= std::string(argv[i]) == "--context-race";
        keepImages |= std::string(argv[i]) == "--images";
        g_profile |= std::string(argv[i]) == "--profile";
        present |= std::string(argv[i]) == "--present";
        routing |= std::string(argv[i]) == "--routing";
        retention |= std::string(argv[i]) == "--retention-timeout";
        if (std::string(argv[i]) == "--resolution" && i + 1 < argc) {
            unsigned w = 0, h = 0;
            if (std::sscanf(argv[i + 1], "%ux%u", &w, &h) == 2 && w >= 64 && h >= 64 && w <= 7680 && h <= 4320) {
                kDisplayWidth = kRenderWidth = w;
                kDisplayHeight = kRenderHeight = h;
            } else {
                std::fprintf(stderr, "bad --resolution (use WxH, 64..7680 x 64..4320)\n");
                return 2;
            }
        }
    }

    if (routing) {
        // Use only the offline executable's directory, never the game's config.
        // Settings are read once; remove our fixture immediately afterwards.
        const auto config = dll_directory() / L"sedici.toml";
        if (std::filesystem::exists(config)) {
            std::fprintf(stderr, "routing check refuses to overwrite existing sedici.toml\n");
            return 1;
        }
        {
            std::ofstream fixture(config);
            fixture << "[fg]\nmode = \"sedici\"\npresent = \"game\"\n[log]\nlevel = 0\n";
            if (!fixture) return 1;
        }
        (void)sedici::settings();
        std::filesystem::remove(config);
    }

    {
        ComPtr<ID3D12Debug> debug;
        // `--no-debug-layer` is for timing runs only: the layer's validation skews them.
        bool layer = true;
        for (int i = 1; i < argc; ++i) layer &= std::string(argv[i]) != "--no-debug-layer";
        if (layer && SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
        }
    }

    Device state;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
                                 IID_PPV_ARGS(&state.device)))) {
        std::fprintf(stderr, "no D3D12 device\n");
        return 1;
    }
    ComPtr<ID3D12InfoQueue1> messages;
    DWORD callbackCookie = 0;
    if (SUCCEEDED(state.device.As(&messages))) {
        const HRESULT registered = messages->RegisterMessageCallback(
            [](D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity,
               D3D12_MESSAGE_ID, LPCSTR description, void*) {
                if (severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                    ++g_debug_errors;
                    std::fputs("DX12 error: ", stderr);
                    std::fputs(description, stderr);
                    std::fputc(10, stderr);
                }
            }, D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &callbackCookie);
        g_debug_callback_installed = SUCCEEDED(registered);
    }

    D3D12_COMMAND_QUEUE_DESC queueDescription{};
    queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

    if (FAILED(state.device->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&state.queue))) ||
        FAILED(state.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                    IID_PPV_ARGS(&state.allocator))) ||
        FAILED(state.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               state.allocator.Get(), nullptr,
                                               IID_PPV_ARGS(&state.list))) ||
        FAILED(state.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&state.fence)))) {
        std::fprintf(stderr, "command objects failed\n");
        return 1;
    }
    state.list->Close();

    std::printf("display %ux%u, render %ux%u, alpha 0.5, %u iterations\n",
                kDisplayWidth, kDisplayHeight, kRenderWidth, kRenderHeight, kIterations);

    bool ok = true;
    if (retention) return (check_retention_timeout(state) && check_presenter_retention(state) &&
                       check_consecutive_retention(state)) ? 0 : 1;
    if (contextRace) return check_session_window(state, false, false, false, false, true) ? 0 : 1;
    if (sessionRecovery || registrationRecovery)
        return check_session_window(state, false, false, false, false, false, true, registrationRecovery) ? 0 : 1;
    if (signalFaultVrr || closeFaultVrr || resetFaultVrr || recoveryVrr || paramsFaultVrr) {
        ComPtr<ID3D12Device5> removable;
        if (FAILED(state.device.As(&removable)) ||
            (GetModuleHandleW(L"RTSSHooks64.dll") &&
             GetEnvironmentVariableW(L"SEDICI_FG_ALLOW_RTSS", nullptr, 0) == 0)) {
            std::puts("SKIP VRR submission fault: device removal or VRR unavailable");
            return 77;
        }
        return check_vrr_window(state, sedici::FrameGenerationSettings::Multiplier::X2,
                                "submission fault", 27, signalFaultVrr || recoveryVrr,
                                closeFaultVrr, resetFaultVrr, recoveryVrr, paramsFaultVrr) ? 0 : 1;
    }
    if (signalFault || closeFault || resetFault) {
        ComPtr<ID3D12Device5> removable;
        if (FAILED(state.device.As(&removable))) {
            std::puts("SKIP submission fault: ID3D12Device5 unavailable");
            return 77;
        }
        return check_session_window(state, false, signalFault, closeFault, resetFault) ? 0 : 1;
    }
    if (deviceLoss) {
        // Own process: removing the device ends every later check on it.
        ComPtr<ID3D12Device5> removable;
        if (FAILED(state.device.As(&removable))) {
            std::puts("SKIP device loss: ID3D12Device5 unavailable");
            return 77;
        }
        const bool lost = check_session_window(state, true);
        std::printf("%s device loss: session and wrapper tear down cleanly after RemoveDevice\n",
                    lost ? "PASS" : "FAIL");
        return lost ? 0 : 1;
    }
    for (const auto& scene : kScenes) {
        ok = run_case(state, scene, keepImages) && ok;
    }

    ok = check_ten_bit(state) && ok;
    const int contractFailures = check_contract(state);
    if (contractFailures != 0) ok = false;
    {
        std::string why;
        auto presenter = sedici::fg::Presenter::create(
            state.device.Get(), sedici::FrameGenerationSettings::TearGuard::Off, why);
        bool rejected = false;
        if (presenter) {
            presenter->request_stop();
            sedici::fg::PresentJob job;
            rejected = presenter->submit(std::move(job)) == DXGI_ERROR_INVALID_CALL &&
                       presenter->outstanding() == 0;
        }
        std::printf("%s presenter refuses a job after stop without advancing its queue\n",
                    rejected ? "PASS" : "FAIL");
        ok = rejected && ok;
    }
    ok = check_pacing() && ok;
    if (present) ok = check_window(state) && ok;
    // Known failures, tracked in docs/refactor/reliability.md ("Current coverage
    // and gaps"). They are reported as XFAIL and do not fail the run; if one
    // starts passing it is reported as XPASS and fails the run, so the record
    // is updated instead of silently turning green.
    const auto knownFailure = [](bool passed, bool knownSignature, const char* name, const char* evidence) {
        if (passed) {
            std::printf("XPASS %s: tracked failure now passes; update reliability record", name);
            std::putchar(10);
            return false;
        }
        if (!knownSignature) {
            std::printf("FAIL %s: failure did not match the tracked signature", name);
            std::putchar(10);
            return false;
        }
        std::printf("XFAIL %s (%s)", name, evidence);
        std::putchar(10);
        return true;
    };
    if (present) {
        bool generatedKnown = false;
        const bool generatedPass = check_generated_window(state, &generatedKnown);
        ok = knownFailure(generatedPass, generatedKnown, "generated presentation",
                          "vsync path skips 3 of 5 midpoints; finding 009") && ok;
        bool sessionKnown = false;
        const bool sessionPass = check_session_window(state, false, false, false, false, false, false, false, &sessionKnown);
        ok = knownFailure(sessionPass, sessionKnown, "session",
                          "vsync path frame 2 midpoint reads back at 16 dB; finding 009") && ok;
    }
    if (routing) ok = check_routing(state) && check_routing(state, true) && ok;
    if (routing) ok = check_routing_faults(state) && ok;
    // Last: overlays that hook Present (RTSS) keep using the queue of the most
    // recent swapchain, so the present queue would leak into later checks.
    if (present) ok = check_vrr_window(state, sedici::FrameGenerationSettings::Multiplier::X2, "2x") && ok;
    if (present) ok = check_vrr_window(state, sedici::FrameGenerationSettings::Multiplier::X1_5, "1.5x") && ok;
    if (present) ok = check_vrr_window(state, sedici::FrameGenerationSettings::Multiplier::X1_5, "1.5x overload", 6) && ok;
    if (present) ok = check_vrr_window(state, sedici::FrameGenerationSettings::Multiplier::Adaptive, "adaptive at ~40 fps", 21) && ok;

    if (ComPtr<ID3D12InfoQueue> info; SUCCEEDED(state.device.As(&info))) {
        const UINT64 count = info->GetNumStoredMessages();
        if (count > 0) {
            std::printf("debug layer reported %llu message(s):\n",
                        static_cast<unsigned long long>(count));
            for (UINT64 index = 0; index < count && index < 32; ++index) {
                SIZE_T length = 0;
                if (FAILED(info->GetMessage(index, nullptr, &length)) || length == 0) continue;
                std::vector<char> buffer(length);
                auto* message = reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
                if (FAILED(info->GetMessage(index, message, &length))) continue;
                std::printf("  [%d] %.*s\n", static_cast<int>(message->Severity),
                            static_cast<int>(message->DescriptionByteLength), message->pDescription);
            }
            ok = false;
        }
    }

    return ok ? 0 : 1;
}

#pragma warning(pop)
