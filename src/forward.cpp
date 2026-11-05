#include "forward.h"

#include "log.h"

#include <windows.h>

#include <filesystem>
#include <mutex>
#include <sstream>

namespace {

constexpr wchar_t kOriginalName[] = L"amd_fidelityfx_framegeneration_dx12.dll.orig";

extern "C" const char g_sedici_proxy_marker[];

HMODULE g_original_module = nullptr;
std::once_flag g_load_once;

void log_line(std::string_view text) noexcept
{
    log_event(1, text);
}

bool load_original() noexcept
{
    try {
        const auto directory = dll_directory();
        if (directory.empty()) {
            log_error("cannot determine the proxy DLL directory");
            return false;
        }

        const auto path = directory / kOriginalName;
        if (!std::filesystem::exists(path)) {
            log_error("original DLL not found next to the proxy: " + path.string());
            return false;
        }

        g_original_module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!g_original_module) {
            std::ostringstream out;
            out << "LoadLibraryExW failed for " << path.string() << " (GetLastError=" << GetLastError() << ')';
            log_error(out.str());
            return false;
        }

        FARPROC create = GetProcAddress(g_original_module, "ffxCreateContext");
        FARPROC destroy = GetProcAddress(g_original_module, "ffxDestroyContext");
        FARPROC configure = GetProcAddress(g_original_module, "ffxConfigure");
        FARPROC query = GetProcAddress(g_original_module, "ffxQuery");
        FARPROC dispatch = GetProcAddress(g_original_module, "ffxDispatch");

        if (!create || !destroy || !configure || !query || !dispatch) {
            auto report = [](const char* name, FARPROC value) {
                if (!value) log_error(std::string("missing export in original DLL: ") + name);
            };
            report("ffxCreateContext", create);
            report("ffxDestroyContext", destroy);
            report("ffxConfigure", configure);
            report("ffxQuery", query);
            report("ffxDispatch", dispatch);
            FreeLibrary(g_original_module);
            g_original_module = nullptr;
            return false;
        }

        g_ffxCreateContext = reinterpret_cast<PfnFfxCreateContext>(create);
        g_ffxDestroyContext = reinterpret_cast<PfnFfxDestroyContext>(destroy);
        g_ffxConfigure = reinterpret_cast<PfnFfxConfigure>(configure);
        g_ffxQuery = reinterpret_cast<PfnFfxQuery>(query);
        g_ffxDispatch = reinterpret_cast<PfnFfxDispatch>(dispatch);

        std::ostringstream out;
        out << "loaded original " << path.string() << " base=0x" << std::hex
            << reinterpret_cast<uintptr_t>(g_original_module) << std::dec
            << " proxy=" << g_sedici_proxy_marker;
        log_line(out.str());
        return true;
    } catch (...) {
        log_error("exception while loading the original DLL");
        return false;
    }
}

} // namespace

PfnFfxCreateContext g_ffxCreateContext = nullptr;
PfnFfxDestroyContext g_ffxDestroyContext = nullptr;
PfnFfxConfigure g_ffxConfigure = nullptr;
PfnFfxQuery g_ffxQuery = nullptr;
PfnFfxDispatch g_ffxDispatch = nullptr;

bool ensure_original_loaded() noexcept
{
    std::call_once(g_load_once, [] {
        if (!load_original()) {
            log_error("proxy is forwarding nothing because the original DLL could not be loaded");
        }
    });
    return g_ffxCreateContext && g_ffxDestroyContext && g_ffxConfigure && g_ffxQuery && g_ffxDispatch;
}
