// Leak checks shared by the offline test hosts (docs/dev/workflow.md, tools/check.ps1).
//
// Everything here compiles to nothing unless the build sets SEDICI_CRTDBG (cmake -DSEDICI_CRTDBG=ON), so the
// normal test run is unchanged. Put `sedici_test::LeakGuard leak_guard;` as the first line of a host's main().
//
//  * CPU: with the debug CRT (a Debug build) every heap block still allocated at process exit is listed, and
//    the process then exits with code 3. The release CRT has no such list, so Release builds skip this part.
//  * GPU/COM: when main() returns, DXGI's live-object report is read back. Any object still alive that the
//    test created is listed, and the process exits with code 4. The D3D12 debug layer must be installed
//    (the hosts already need it for their own validation).
//
// Objects that Sedici keeps on purpose for the life of the process are reported too. A host that expects some
// declares them with `leak_guard.allow(type, count, reason)` (or the presets below): the count is exact, so one
// more leaked object of that type still fails. A host whose whole point is to park objects uses `skip(reason)`.
#pragma once

#ifdef SEDICI_CRTDBG

#include <windows.h>
#include <dxgidebug.h>
#include <dxgi1_3.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(_DEBUG)
#include <crtdbg.h>
#endif

namespace sedici_test {

constexpr int kAnyCount = -1;

// `count` live objects of this D3D12/DXGI type are expected when main() returns.
struct Allowance {
    std::string type;
    int remaining;
};

// "Live ID3D12Device at 0x..., Refcount: 21" -> "ID3D12Device" (child objects are indented with a tab).
inline std::string live_type(const std::string& text)
{
    const size_t start = text.find("Live ");
    if (start == std::string::npos) return {};
    const size_t from = start + 5;
    return text.substr(from, text.find(' ', from) - from);
}

#if defined(_DEBUG)
inline int __cdecl crt_report_hook(int type, char* message, int*)
{
    if (type == _CRT_WARN && message && std::strstr(message, "Detected memory leaks")) {
        std::fputs("LEAK: the CRT debug heap found blocks still allocated at exit (details above)\n", stderr);
        std::fflush(stderr);
        // The report runs inside exit(); the only way to change the exit code from here is to end the process.
        TerminateProcess(GetCurrentProcess(), 3);
    }
    return FALSE; // let the default reporter print the message
}
#endif

// Returns the number of live DXGI/D3D objects (not counting the allow list) and prints each to stderr.
inline int report_live_objects(std::vector<Allowance>& allowed)
{
    using Microsoft::WRL::ComPtr;
    ComPtr<IDXGIDebug1> debug;
    ComPtr<IDXGIInfoQueue> queue;
    if (FAILED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&debug))) ||
        FAILED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&queue))))
        return 0; // no Graphics Tools on this machine: nothing to read, not a failure
    queue->ClearStoredMessages(DXGI_DEBUG_ALL);
    debug->ReportLiveObjects(
        DXGI_DEBUG_ALL, static_cast<DXGI_DEBUG_RLO_FLAGS>(DXGI_DEBUG_RLO_DETAIL | DXGI_DEBUG_RLO_IGNORE_INTERNAL));
    int live = 0;
    const UINT64 count = queue->GetNumStoredMessages(DXGI_DEBUG_ALL);
    for (UINT64 i = 0; i < count; ++i) {
        SIZE_T size = 0;
        if (FAILED(queue->GetMessage(DXGI_DEBUG_ALL, i, nullptr, &size)) || size == 0) continue;
        std::vector<char> buffer(size);
        auto* message = reinterpret_cast<DXGI_INFO_QUEUE_MESSAGE*>(buffer.data());
        if (FAILED(queue->GetMessage(DXGI_DEBUG_ALL, i, message, &size))) continue;
        const std::string text(message->pDescription,
                               message->DescriptionByteLength ? message->DescriptionByteLength - 1 : 0);
        if (text.find("Live ") == std::string::npos || text.find("Refcount") == std::string::npos) continue;
        const std::string type = live_type(text);
        bool expected = false;
        for (Allowance& entry : allowed) {
            if (entry.type != type || entry.remaining == 0) continue;
            if (entry.remaining > 0) --entry.remaining;
            expected = true;
            break;
        }
        if (expected) continue;
        std::fprintf(stderr, "LEAK: %s\n", text.c_str());
        ++live;
    }
    return live;
}

class LeakGuard {
    std::vector<Allowance> allowed_;
    bool skip_ = false;

public:
    LeakGuard()
    {
#if defined(_DEBUG)
        _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
        _CrtSetReportHook(crt_report_hook);
        _CrtSetDbgFlag(_CrtSetDbgFlag(_CRTDBG_REPORT_FLAG) | _CRTDBG_LEAK_CHECK_DF);
#endif
    }
    // `count` objects of `type` (an ID3D12/IDXGI interface name) may be alive at exit; the reason says why.
    void allow(const char* type, int count, const char* /*reason: documented at the call site*/)
    {
        allowed_.push_back({type, count});
    }
    // This host parks objects on purpose; its end-of-main report is not checked.
    void skip(const char* /*reason*/) { skip_ = true; }

    // Sedici's overlay (ImGui DX12 backend) is created on the first drawn frame and kept for the life of the
    // process; it only moves when the game's device changes, and then it is disabled (src/ui/ui.cpp,
    // ensure_imgui). So its device and the objects it created are alive when the host's main() returns. K144.
    void allow_overlay_singleton()
    {
        allow("ID3D12Device", kAnyCount, "the overlay keeps its device");
        allow("ID3D12DescriptorHeap", 2, "overlay SRV and RTV heaps");
        allow("ID3D12CommandQueue", 1, "overlay texture upload queue");
        allow("ID3D12RootSignature", 2, "overlay ImGui backend");
        allow("ID3D12PipelineState", 2, "overlay ImGui backend");
        allow("ID3D12CommandAllocator", 1, "overlay copy/draw recording");
        allow("ID3D12GraphicsCommandList", 1, "overlay copy/draw recording");
        allow("ID3D12Fence", 1, "overlay copy/draw fence");
        allow("ID3D12Resource", 10, "overlay font texture and vertex/index buffers");
    }
    // After an injected GPU fault the presenter's drain fails, and a queue whose work did not retire is kept
    // (presenter.cpp, release_shared_present_queue), together with the device it references. K144.
    void allow_kept_present_queue()
    {
        allow("ID3D12Device", kAnyCount, "the kept queue references its device");
        allow("ID3D12CommandQueue", 1, "the shared present queue, kept after a failed drain");
    }

    ~LeakGuard()
    {
        std::fflush(stdout);
        if (!skip_ && report_live_objects(allowed_) > 0) {
            std::fputs("LEAK: DXGI objects were still alive when main() returned\n", stderr);
            std::fflush(stderr);
            std::_Exit(4);
        }
    }
    LeakGuard(const LeakGuard&) = delete;
    LeakGuard& operator=(const LeakGuard&) = delete;
};

} // namespace sedici_test

#else

namespace sedici_test {
struct LeakGuard {
    LeakGuard() {}
    void allow(const char*, int, const char*) {}
    void skip(const char*) {}
    void allow_overlay_singleton() {}
    void allow_kept_present_queue() {}
};
} // namespace sedici_test

#endif
