// Test-only module: exercises the production settings worker's module reference.
// This is not the Sedici proxy and is never packaged or deployed.
#include "settings/settings_io.h"
#include "settings/settings_tx.h"

#include <windows.h>

extern "C" const char g_sedici_proxy_marker[] = "SEDICI_PROXY_V1";

namespace {
HANDLE g_arrived = nullptr;
HANDLE g_resume = nullptr;

void pause_file_io() noexcept
{
    SetEvent(g_arrived);
    (void)WaitForSingleObject(g_resume, 5000);
}
}

extern "C" __declspec(dllexport) BOOL WINAPI start_settings_worker_test(
    HANDLE arrived, HANDLE resume, const wchar_t* path)
{
    if (!arrived || !resume || !path) return FALSE;
    g_arrived = arrived;
    g_resume = resume;
    sedici::settings_use_file(path);
    const uint64_t generation = sedici::settings_start_edit();
    if (!generation) return FALSE;
    sedici::settings_arm_io_test_hook(&pause_file_io);
    if (sedici::settings_io_start_begin(generation, 1)) return TRUE;
    sedici::settings_arm_io_test_hook(nullptr);
    return FALSE;
}
