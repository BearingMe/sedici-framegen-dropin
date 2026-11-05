#include "present_hook.h"

#include "log.h"
#include "stats.h"

#include "ffx_framegeneration.h"
#include "ffx_api_framegeneration_dx12.h"

#include <windows.h>
#include <dxgi.h>

#include <atomic>
#include <mutex>
#include <set>
#include <sstream>

namespace {

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, const DXGI_PRESENT_PARAMETERS*);

constexpr size_t kMaxTargets = 8;

std::atomic<PresentFn> g_present_original{nullptr};
std::atomic<Present1Fn> g_present1_original{nullptr};
std::atomic<void*> g_targets[kMaxTargets]{};
std::atomic<unsigned> g_target_count{0};

std::mutex g_mutex;
std::set<void**> g_patched_vtables;

bool is_target(IDXGISwapChain* self) noexcept
{
    const unsigned count = g_target_count.load(std::memory_order_acquire);
    const unsigned limit = count < kMaxTargets ? count : static_cast<unsigned>(kMaxTargets);
    for (unsigned i = 0; i < limit; ++i) {
        if (g_targets[i].load(std::memory_order_relaxed) == self) return true;
    }
    return false;
}

void remember_target(IDXGISwapChain* self) noexcept
{
    if (!self) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    const unsigned count = g_target_count.load(std::memory_order_relaxed);
    for (unsigned i = 0; i < count && i < kMaxTargets; ++i) {
        if (g_targets[i].load(std::memory_order_relaxed) == self) return;
    }
    if (count >= kMaxTargets) return;
    g_targets[count].store(self, std::memory_order_relaxed);
    g_target_count.store(count + 1, std::memory_order_release);
}

uint64_t qpc_now() noexcept
{
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return static_cast<uint64_t>(value.QuadPart);
}

HRESULT STDMETHODCALLTYPE present_hook(IDXGISwapChain* self, UINT sync_interval, UINT flags)
{
    static thread_local bool in_hook = false;

    PresentFn original = g_present_original.load(std::memory_order_acquire);
    if (!original) return E_FAIL;

    if (in_hook) return original(self, sync_interval, flags);

    struct Guard {
        bool& value;
        ~Guard() { value = false; }
    } guard{in_hook};
    in_hook = true;

    if (is_target(self)) stats_on_present(qpc_now());
    return original(self, sync_interval, flags);
}

HRESULT STDMETHODCALLTYPE present1_hook(IDXGISwapChain* self, UINT sync_interval,
                                        const DXGI_PRESENT_PARAMETERS* parameters)
{
    static thread_local bool in_hook = false;

    Present1Fn original = g_present1_original.load(std::memory_order_acquire);
    if (!original) return E_FAIL;

    if (in_hook) return original(self, sync_interval, parameters);

    struct Guard {
        bool& value;
        ~Guard() { value = false; }
    } guard{in_hook};
    in_hook = true;

    if (is_target(self)) stats_on_present(qpc_now());
    return original(self, sync_interval, parameters);
}

bool patch_vtable(void** vtable) noexcept
{
    if (!vtable) return false;

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_patched_vtables.count(vtable)) return true;

    void* present = vtable[8];
    void* present1 = vtable[22];
    if (!present) {
        log_error("swapchain vtable Present slot is null; present timing hook skipped");
        return false;
    }

    DWORD old_present = 0;
    DWORD old_present1 = 0;
    if (!VirtualProtect(&vtable[8], sizeof(void*), PAGE_READWRITE, &old_present)) {
        std::ostringstream out;
        out << "VirtualProtect failed for swapchain Present (GetLastError=" << GetLastError() << ')';
        log_error(out.str());
        return false;
    }

    bool present1_protected = true;
    if (present1 && !VirtualProtect(&vtable[22], sizeof(void*), PAGE_READWRITE, &old_present1)) {
        present1_protected = false;
    }
    if (present1 && !present1_protected) {
        VirtualProtect(&vtable[8], sizeof(void*), old_present, &old_present);
        std::ostringstream out;
        out << "VirtualProtect failed for swapchain Present1 (GetLastError=" << GetLastError() << ')';
        log_error(out.str());
        return false;
    }

    // Preserve whatever was in the slot (D3D runtime or another overlay hook).
    g_present_original.store(reinterpret_cast<PresentFn>(present), std::memory_order_release);
    g_present1_original.store(present1 ? reinterpret_cast<Present1Fn>(present1) : nullptr,
                              std::memory_order_release);

    vtable[8] = reinterpret_cast<void*>(&present_hook);
    if (present1) vtable[22] = reinterpret_cast<void*>(&present1_hook);

    VirtualProtect(&vtable[8], sizeof(void*), old_present, &old_present);
    if (present1) VirtualProtect(&vtable[22], sizeof(void*), old_present1, &old_present1);
    FlushInstructionCache(GetCurrentProcess(), &vtable[8], sizeof(void*));

    g_patched_vtables.insert(vtable);

    std::ostringstream out;
    out << "present timing hook installed vtable=" << std::hex << reinterpret_cast<uintptr_t>(vtable)
        << " present=0x" << reinterpret_cast<uintptr_t>(present)
        << " present1=0x" << reinterpret_cast<uintptr_t>(present1);
    log_event(1, out.str());
    return true;
}

bool install(IDXGISwapChain* swapchain) noexcept
{
    if (!swapchain) return false;
    try {
        remember_target(swapchain);
        void** vtable = *reinterpret_cast<void***>(swapchain);
        return patch_vtable(vtable);
    } catch (...) {
        log_error("exception while installing the present timing hook");
        return false;
    }
}

} // namespace

void present_hook_maybe_install(const ffxCreateContextDescHeader* desc) noexcept
{
    if (log_level() < 1 || !desc) return;

    try {
        std::set<const ffxApiHeader*> seen;
        for (const ffxApiHeader* item = desc; item; item = item->pNext) {
            if (!seen.insert(item).second) break;

            IDXGISwapChain4* swapchain = nullptr;
            switch (item->type) {
            case FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WRAP_DX12: {
                const auto& d = reinterpret_cast<const ffxCreateContextDescFrameGenerationSwapChainWrapDX12&>(*item);
                if (d.swapchain) swapchain = *d.swapchain;
                break;
            }
            case FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_NEW_DX12: {
                const auto& d = reinterpret_cast<const ffxCreateContextDescFrameGenerationSwapChainNewDX12&>(*item);
                if (d.swapchain) swapchain = *d.swapchain;
                break;
            }
            case FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12: {
                const auto& d = reinterpret_cast<const ffxCreateContextDescFrameGenerationSwapChainForHwndDX12&>(*item);
                if (d.swapchain) swapchain = *d.swapchain;
                break;
            }
            default:
                break;
            }

            if (swapchain) install(static_cast<IDXGISwapChain*>(swapchain));
        }
    } catch (...) {
        log_error("failed to inspect the create chain for a swapchain");
    }
}
