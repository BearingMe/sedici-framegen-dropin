// Isolated host for the test-only settings worker module. It verifies that a
// worker holds the module through file I/O and releases it after completion.
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <cstdio>

using StartWorker = BOOL (WINAPI*)(HANDLE, HANDLE, const wchar_t*);

int wmain(int argc, wchar_t** argv)
{
    if (argc != 2) return 2;
    wchar_t executable[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, executable, MAX_PATH)) return 1;
    const auto fixture = std::filesystem::path(executable).parent_path() /
        (L"settings-worker-" + std::to_wstring(GetCurrentProcessId()) + L".toml");
    if (std::filesystem::exists(fixture)) return 1;
    {
        std::ofstream output(fixture, std::ios::binary);
        output << "[ui]\nmode = \"off\"\n";
        if (!output) return 1;
    }
    HANDLE arrived = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE resume = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HMODULE module = LoadLibraryW(argv[1]);
    auto start = module ? reinterpret_cast<StartWorker>(GetProcAddress(module, "start_settings_worker_test")) : nullptr;
    const bool started = arrived && resume && start && start(arrived, resume, fixture.c_str());
    const bool reached = started && WaitForSingleObject(arrived, 5000) == WAIT_OBJECT_0;
    if (module) FreeLibrary(module);

    HMODULE observed = nullptr;
    const bool held = reached && GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(start), &observed) != 0;
    if (resume) SetEvent(resume);

    bool unloaded = false;
    const ULONGLONG deadline = GetTickCount64() + 5000;
    while (held && GetTickCount64() < deadline) {
        observed = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(start), &observed)) {
            unloaded = true;
            break;
        }
        Sleep(1);
    }
    // If a failing test leaves a worker running, do not close handles it may
    // still be waiting on; process exit will reclaim this isolated fixture.
    if (!started || unloaded) {
        if (arrived) CloseHandle(arrived);
        if (resume) CloseHandle(resume);
        std::error_code ignored;
        std::filesystem::remove(fixture, ignored);
    }
    std::puts(held && unloaded ? "PASS worker held its module until exit and then released it"
                               : "FAIL worker module lifetime");
    return held && unloaded ? 0 : 1;
}
