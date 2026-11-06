#include "present_hook.h"

#include "config.h"
#include "limiter.h"
#include "log.h"
#include "stats.h"
#include "timing.h"
#include "toggles.h"

#include "ffx_framegeneration.h"
#include "ffx_api_framegeneration_dx12.h"

#include <windows.h>
#include <dxgi.h>

#include <atomic>
#include <array>
#include <mutex>
#include <map>
#include <set>
#include <sstream>
#include <utility>

namespace {

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT,
                                               const DXGI_PRESENT_PARAMETERS*);

constexpr size_t kMaxTargets = 8;
constexpr size_t kMaxHookImplementations = 32;

std::atomic<void*> g_targets[kMaxTargets]{};
std::atomic<unsigned> g_target_count{0};

std::mutex g_mutex;
struct OriginalPresent {
    PresentFn present;
    Present1Fn present1;
};
std::map<void**, size_t> g_patched_vtables;
std::array<OriginalPresent, kMaxHookImplementations> g_originals{};
thread_local bool g_in_present = false;

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

template<size_t Index>
HRESULT STDMETHODCALLTYPE present_hook(IDXGISwapChain* self, UINT sync_interval, UINT flags)
{
    // The hook function itself identifies the immutable forwarding target.
    // Another overlay can copy/replace self's vtable and chain this same hook.
    PresentFn original = g_originals[Index].present;

    if (g_in_present) return original(self, sync_interval, flags);

    struct Guard {
        bool& value;
        ~Guard() { value = false; }
    } guard{g_in_present};
    g_in_present = true;

    if (is_target(self)) {
        if ((flags & DXGI_PRESENT_TEST) == 0) toggles_process_pacing_live();
        const uint64_t now = qpc_now();
        stats_on_present(now);
        sedici::timing_on_real_present(now);
        if ((flags & DXGI_PRESENT_TEST) == 0) limiter_before_present(now);
    }
    return original(self, sync_interval, flags);
}

template<size_t Index>
HRESULT STDMETHODCALLTYPE present1_hook(IDXGISwapChain* self, UINT sync_interval, UINT flags,
                                        const DXGI_PRESENT_PARAMETERS* parameters)
{
    Present1Fn original = g_originals[Index].present1;

    if (g_in_present) return original(self, sync_interval, flags, parameters);

    struct Guard {
        bool& value;
        ~Guard() { value = false; }
    } guard{g_in_present};
    g_in_present = true;

    if (is_target(self)) {
        if ((flags & DXGI_PRESENT_TEST) == 0) toggles_process_pacing_live();
        const uint64_t now = qpc_now();
        stats_on_present(now);
        sedici::timing_on_real_present(now);
        if ((flags & DXGI_PRESENT_TEST) == 0) limiter_before_present(now);
    }
    return original(self, sync_interval, flags, parameters);
}

template<size_t... Index>
constexpr auto present_hooks(std::index_sequence<Index...>)
{
    return std::array<PresentFn, sizeof...(Index)>{&present_hook<Index>...};
}

template<size_t... Index>
constexpr auto present1_hooks(std::index_sequence<Index...>)
{
    return std::array<Present1Fn, sizeof...(Index)>{&present1_hook<Index>...};
}

constexpr auto kPresentHooks = present_hooks(std::make_index_sequence<kMaxHookImplementations>{});
constexpr auto kPresent1Hooks = present1_hooks(std::make_index_sequence<kMaxHookImplementations>{});

bool patch_vtable(void** vtable)
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

    // A cloned table may already contain one of our complete hook pairs. Do
    // not wrap that pair again or consume another immutable implementation.
    for (size_t i = 0; i < g_patched_vtables.size(); ++i) {
        if (present == reinterpret_cast<void*>(kPresentHooks[i]) &&
            (!present1 || present1 == reinterpret_cast<void*>(kPresent1Hooks[i])))
            return true;
    }
    if (g_patched_vtables.size() == g_originals.size()) {
        log_error("present timing hook capacity reached; leaving this vtable unchanged");
        return false;
    }

    // Allocate bookkeeping before publishing either hook. Each implementation
    // keeps its own original; recreating a swapchain cannot change old targets.
    const size_t index = g_patched_vtables.size();
    const auto entry = g_patched_vtables.emplace(vtable, index).first;
    g_originals[index] = OriginalPresent{
        reinterpret_cast<PresentFn>(present), reinterpret_cast<Present1Fn>(present1)};

    DWORD old_present = 0;
    DWORD old_present1 = 0;
    if (!VirtualProtect(&vtable[8], sizeof(void*), PAGE_READWRITE, &old_present)) {
        g_patched_vtables.erase(entry);
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
        g_patched_vtables.erase(entry);
        VirtualProtect(&vtable[8], sizeof(void*), old_present, &old_present);
        std::ostringstream out;
        out << "VirtualProtect failed for swapchain Present1 (GetLastError=" << GetLastError() << ')';
        log_error(out.str());
        return false;
    }

    InterlockedExchangePointer(&vtable[8], reinterpret_cast<void*>(kPresentHooks[index]));
    if (present1) InterlockedExchangePointer(&vtable[22], reinterpret_cast<void*>(kPresent1Hooks[index]));

    if (present1) VirtualProtect(&vtable[22], sizeof(void*), old_present1, &old_present1);
    // Slots often share a page: undo protection changes in reverse order.
    VirtualProtect(&vtable[8], sizeof(void*), old_present, &old_present);
    FlushInstructionCache(GetCurrentProcess(), &vtable[8], sizeof(void*));

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
    // Install while the swapchain is available, even if initially uncapped and
    // silent: the overlay may enable the limiter later without recreating it.
    if (!desc) return;
    (void)sedici::settings();

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
