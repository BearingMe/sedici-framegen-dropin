#include "settings/settings_io.h"
#include "diagnostics/log.h"

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
std::atomic<ULONGLONG> g_job_started{0};
std::atomic<bool> g_stall_reported{false};
std::atomic<uint64_t> g_stall_count{0};
std::atomic<unsigned> g_stall_ms{kSettingsIoStallMs};
std::atomic<void*> g_test_block{nullptr};
std::atomic<uint64_t> g_active_epoch{0};
std::atomic<SettingsIoKind> g_active_kind{SettingsIoKind::BeginEdit};
std::mutex g_thread_mutex;
HANDLE g_thread = nullptr; // guarded by g_thread_mutex; closed by the worker when it ends
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
            if (void* block = g_test_block.load(std::memory_order_acquire))
                WaitForSingleObject(static_cast<HANDLE>(block), 30000); // offline: a hung filesystem
            if (job->kind == SettingsIoKind::BeginEdit) {
                result.beginOk = settings_begin_edit_for(job->generation);
                if (result.beginOk) log_event(1, "sedici: settings edit baseline ready on worker");
            } else {
                result.save = settings_save_for(job->ticket);
            }
            std::lock_guard lock(g_result_mutex);
            // A job already reported as stalled has had its completion delivered; its
            // late result must not reach a menu that has moved on (decided under the
            // same lock the stall report takes).
            if (!g_stall_reported.load(std::memory_order_acquire)) {
                g_result = std::move(result);
                g_result_ready = true;
            }
        } catch (...) {
            // Preserve a non-allocating failure completion even if publishing
            // the ordinary result fails; do not leave the UI waiting forever.
            g_lost_epoch.store(job->viewEpoch, std::memory_order_relaxed);
            g_lost_kind.store(job->kind, std::memory_order_relaxed);
            g_result_lost.store(true, std::memory_order_release);
        }
    }
    delete job;
    {
        std::lock_guard lock(g_thread_mutex);
        if (g_thread) { CloseHandle(g_thread); g_thread = nullptr; }
    }
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
        g_job_started.store(GetTickCount64(), std::memory_order_release);
        g_stall_reported.store(false, std::memory_order_release);
        g_active_kind.store(kind, std::memory_order_relaxed);
        g_active_epoch.store(viewEpoch, std::memory_order_relaxed);
        {
            // Held across CreateThread so the worker cannot finish and look for the
            // handle before it is stored.
            std::lock_guard lock(g_thread_mutex);
            g_thread = CreateThread(nullptr, 0, &run_job, job, 0, nullptr);
            if (!g_thread) {
                if (!job->executable) FreeLibrary(module);
                delete job;
                g_busy.store(false, std::memory_order_release);
                return false;
            }
        }
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
        if (!g_result_ready && g_busy.load(std::memory_order_acquire) &&
            !g_stall_reported.load(std::memory_order_acquire) &&
            GetTickCount64() - g_job_started.load(std::memory_order_acquire) >= g_stall_ms.load()) {
            g_stall_reported.store(true, std::memory_order_release);
            g_stall_count.fetch_add(1, std::memory_order_relaxed);
            {
                // Best effort: unblocks a synchronous call on a local volume; a hung
                // network share may ignore it, which is why the report does not wait.
                std::lock_guard thread_lock(g_thread_mutex);
                if (g_thread) (void)CancelSynchronousIo(g_thread);
            }
            log_error("sedici: settings file I/O has not completed; the request is reported as unconfirmed "
                      "and no new settings job starts until the worker exits");
            result = {};
            result.kind = g_active_kind.load(std::memory_order_relaxed);
            result.viewEpoch = g_active_epoch.load(std::memory_order_relaxed);
            result.stalled = true;
            result.save.message = "Settings file is not responding; the change was not confirmed";
            return true;
        }
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

uint64_t settings_io_stall_count() noexcept { return g_stall_count.load(std::memory_order_relaxed); }

bool settings_io_stalled() noexcept
{
    return g_busy.load(std::memory_order_acquire) && g_stall_reported.load(std::memory_order_acquire);
}

void settings_io_test_set_stall_ms(unsigned ms) noexcept { g_stall_ms.store(ms); }
void settings_io_test_set_block(void* event_handle) noexcept { g_test_block.store(event_handle, std::memory_order_release); }

} // namespace sedici
