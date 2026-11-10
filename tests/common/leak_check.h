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
// Objects that Sedici keeps on purpose for the life of the process (parked contexts, the pinned module) are
// reported too; they belong in the allow list below with a reason, not silently ignored.
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

// Live-object lines that are expected at the end of main() and why. Match is a substring of the DXGI line.
inline const std::vector<const char*>& live_object_allow_list()
{
    static const std::vector<const char*> list = {
        // Add entries as "substring", // reason. Empty on purpose: every entry needs a documented reason.
    };
    return list;
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
inline int report_live_objects()
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
        if (text.rfind("Live ", 0) != 0 || text.find("Refcount") == std::string::npos) continue;
        bool allowed = false;
        for (const char* entry : live_object_allow_list()) allowed |= text.find(entry) != std::string::npos;
        if (allowed) continue;
        std::fprintf(stderr, "LEAK: %s\n", text.c_str());
        ++live;
    }
    return live;
}

class LeakGuard {
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
    ~LeakGuard()
    {
        std::fflush(stdout);
        if (report_live_objects() > 0) {
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
};
} // namespace sedici_test

#endif
