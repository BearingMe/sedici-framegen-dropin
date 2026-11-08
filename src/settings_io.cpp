#include "settings_io.h"
#include "log.h"

#include <windows.h>

#include <atomic>
#include <mutex>
#include <new>
#include <utility>

namespace sedici {
namespace {

struct Job {
    SettingsIoKind kind = SettingsIoKind::BeginEdit;
    uint64_t generation = 0;
    uint64_t viewEpoch = 0;
    SettingsSaveTicket ticket{};
    HMODULE module = nullptr;
    bool executable = false;
};

std::atomic<bool> g_busy{false};
std::mutex g_result_mutex;
bool g_result_ready = false;
SettingsIoCompletion g_result;
std::atomic<bool> g_result_lost{false};
std::atomic<uint64_t> g_lost_epoch{0};
std::atomic<SettingsIoKind> g_lost_kind{SettingsIoKind::BeginEdit};

DWORD WINAPI run_job(void* raw) noexcept
{
    auto* job = static_cast<Job*>(raw);
    const HMODULE module = job->module;
    const bool executable = job->executable;
    {
        SettingsIoCompletion result;
        result.kind = job->kind;
        result.viewEpoch = job->viewEpoch;
        result.workerThreadId = GetCurrentThreadId();
        try {
            if (job->kind == SettingsIoKind::BeginEdit) {
                result.beginOk = settings_begin_edit_for(job->generation);
                if (result.beginOk) log_event(1, "sedici: settings edit baseline ready on worker");
            } else {
                result.save = settings_save_for(job->ticket);
            }
            std::lock_guard lock(g_result_mutex);
            g_result = std::move(result);
            g_result_ready = true;
        } catch (...) {
            // Preserve a non-allocating failure completion even if publishing
            // the ordinary result fails; do not leave the UI waiting forever.
            g_lost_epoch.store(job->viewEpoch, std::memory_order_relaxed);
            g_lost_kind.store(job->kind, std::memory_order_relaxed);
            g_result_lost.store(true, std::memory_order_release);
        }
    }
    delete job;
    g_busy.store(false, std::memory_order_release);
    // The worker is executing code in this module. FreeLibraryAndExitThread
    // makes the last release and thread exit indivisible; never return into an
    // unloaded DLL. The executable itself cannot be unloaded this way.
    if (!executable) FreeLibraryAndExitThread(module, 0);
    return 0;
}

bool start(SettingsIoKind kind, uint64_t generation, SettingsSaveTicket ticket,
           uint64_t viewEpoch) noexcept
{
    bool expected = false;
    if (!g_busy.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return false;
    try {
        {
            std::lock_guard lock(g_result_mutex);
            if (g_result_ready || g_result_lost.load(std::memory_order_acquire)) {
                g_busy.store(false, std::memory_order_release);
                return false;
            }
        }
        auto* job = new (std::nothrow) Job;
        if (!job) {
            g_busy.store(false, std::memory_order_release);
            return false;
        }
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                reinterpret_cast<LPCWSTR>(&run_job), &module)) {
            delete job;
            g_busy.store(false, std::memory_order_release);
            return false;
        }
        job->kind = kind;
        job->generation = generation;
        job->viewEpoch = viewEpoch;
        job->ticket = ticket;
        job->module = module;
        job->executable = module == GetModuleHandleW(nullptr);
        HANDLE thread = CreateThread(nullptr, 0, &run_job, job, 0, nullptr);
        if (!thread) {
            if (!job->executable) FreeLibrary(module);
            delete job;
            g_busy.store(false, std::memory_order_release);
            return false;
        }
        CloseHandle(thread);
        return true;
    } catch (...) {
        g_busy.store(false, std::memory_order_release);
        return false;
    }
}

} // namespace

bool settings_io_start_begin(uint64_t generation, uint64_t viewEpoch) noexcept
{
    if (!generation) return false;
    return start(SettingsIoKind::BeginEdit, generation, {}, viewEpoch);
}

bool settings_io_start_save(SettingsSaveTicket ticket, uint64_t viewEpoch) noexcept
{
    if (!ticket.valid) return false;
    return start(SettingsIoKind::Save, 0, ticket, viewEpoch);
}

bool settings_io_busy() noexcept
{
    return g_busy.load(std::memory_order_acquire);
}

bool settings_io_poll(SettingsIoCompletion& result) noexcept
{
    try {
        std::lock_guard lock(g_result_mutex);
        if (g_result_ready) {
            result = std::move(g_result);
            g_result_ready = false;
            return true;
        }
        if (!g_result_lost.exchange(false, std::memory_order_acq_rel)) return false;
        result = {};
        result.kind = g_lost_kind.load(std::memory_order_relaxed);
        result.viewEpoch = g_lost_epoch.load(std::memory_order_relaxed);
        return true;
    } catch (...) { return false; }
}

} // namespace sedici
