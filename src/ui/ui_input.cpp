#include "ui/ui_input.h"
#include "diagnostics/log.h"
#include "settings/live.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <delayimp.h>
#include <psapi.h>

namespace sedici {
namespace {
using SetPos = BOOL(WINAPI*)(int, int);
using Clip = BOOL(WINAPI*)(const RECT*);
// The real user32 functions, looked up in the module's exports. Taking `::ClipCursor`'s address can give the
// linker's import thunk (clang-cl with lld-link does), and that thunk jumps through the very import slot this file
// patches, so "calling the original" would call the hook again and lock g_mutex a second time (K146).
template <typename F>
F user32_function(const char* name, F fallback) noexcept
{
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    const FARPROC address = user32 ? GetProcAddress(user32, name) : nullptr;
    return address ? reinterpret_cast<F>(address) : fallback;
}
SetPos g_set_pos = user32_function<SetPos>("SetCursorPos", ::SetCursorPos);
Clip g_clip = user32_function<Clip>("ClipCursor", ::ClipCursor);
std::atomic<bool> g_active{false};
std::atomic<HWND> g_window{nullptr};
std::mutex g_mutex;
std::mutex g_install_mutex;
std::atomic<bool> g_disabled{false};
RECT g_clip_rect{};
bool g_clip_set = false;
int g_show_adjustments = 0;
std::atomic<unsigned> g_suppressed_pos{0};
std::atomic<unsigned> g_suppressed_clip{0};

bool capturing() noexcept
{
    const HWND hwnd = g_window.load(std::memory_order_acquire);
    return !g_disabled.load(std::memory_order_acquire) && live_ui_mode() == UiMode::Full &&
           g_active.load(std::memory_order_acquire) && hwnd && GetForegroundWindow() == hwnd;
}

BOOL WINAPI set_pos_hook(int x, int y)
{
    if (capturing()) {
        ++g_suppressed_pos;
        return TRUE;
    }
    return g_set_pos(x, y);
}

BOOL WINAPI clip_hook(const RECT* rect)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!capturing()) return g_clip(rect);
    ++g_suppressed_clip;
    g_clip_set = rect != nullptr;
    if (rect) g_clip_rect = *rect;
    return TRUE;
}

bool replace_slot(ULONG_PTR* slot, ULONG_PTR expected, ULONG_PTR replacement) noexcept
{
    DWORD protection = 0;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &protection)) return false;
    const auto previous =
        InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(slot),
                                          reinterpret_cast<void*>(replacement), reinterpret_cast<void*>(expected));
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(*slot), protection, &ignored);
    return previous == reinterpret_cast<void*>(expected);
}

// Import slots already replaced, so a rescan never patches one twice.
struct PatchedSlot {
    ULONG_PTR* slot;
    bool position;
};
std::vector<PatchedSlot> g_patched;
InterceptionSummary g_summary{};
ULONGLONG g_last_scan = 0;

bool is_system_module(HMODULE module) noexcept
{
    wchar_t path[MAX_PATH]{};
    wchar_t system[MAX_PATH]{};
    wchar_t windows[MAX_PATH]{};
    if (!GetModuleFileNameW(module, path, MAX_PATH)) return true; // unknown: leave it alone
    const UINT systemLength = GetSystemDirectoryW(system, MAX_PATH);
    // /analyze cannot see that the lambda below examines windowsLength (C28193): false positive.
#pragma warning(suppress : 28193)
    const UINT windowsLength = GetWindowsDirectoryW(windows, MAX_PATH);
    const auto under = [&](const wchar_t* root, UINT length) {
        return length != 0 && length < MAX_PATH && _wcsnicmp(path, root, length) == 0;
    };
    return under(system, systemLength) || under(windows, windowsLength);
}

void patch_if_cursor_import(const char* api, ULONG_PTR* slot, bool* sawPosition, bool* sawClip) noexcept
{
    bool position = false;
    if (std::strcmp(api, "SetCursorPos") == 0)
        position = true;
    else if (std::strcmp(api, "ClipCursor") != 0)
        return;
    for (const PatchedSlot& done : g_patched)
        if (done.slot == slot) return;
    const ULONG_PTR expected =
        position ? reinterpret_cast<ULONG_PTR>(g_set_pos) : reinterpret_cast<ULONG_PTR>(g_clip);
    const ULONG_PTR replacement =
        position ? reinterpret_cast<ULONG_PTR>(&set_pos_hook) : reinterpret_cast<ULONG_PTR>(&clip_hook);
    (position ? *sawPosition : *sawClip) = true;
    if (*slot != expected) {
        // Another hook, or a delay-load stub that has not resolved yet. Chaining through a
        // foreign hook is not safe, so leave it; a delay-load slot is picked up by a later scan.
        ++g_summary.skippedSlots;
        return;
    }
    if (!replace_slot(slot, expected, replacement)) {
        ++g_summary.skippedSlots;
        return;
    }
    g_patched.push_back({slot, position});
    if (position)
        ++g_summary.setCursorPosSlots;
    else
        ++g_summary.clipCursorSlots;
}

void scan_module(HMODULE module, bool* sawPosition, bool* sawClip) noexcept
{
    auto* base = reinterpret_cast<unsigned char*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    const auto imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (imports.VirtualAddress) {
        auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress);
        for (; desc->Name; ++desc) {
            if (!desc->OriginalFirstThunk) continue;
            auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->OriginalFirstThunk);
            auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
            for (; names->u1.AddressOfData; ++names, ++slots) {
                if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
                const auto* name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
                patch_if_cursor_import(reinterpret_cast<const char*>(name->Name), &slots->u1.Function, sawPosition,
                                       sawClip);
            }
        }
    }
    // Delay-loaded imports (some engines load user32 late). The slot holds a stub until the
    // first call resolves it, so these are only patched once resolved.
    const auto delayed = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (delayed.VirtualAddress) {
        auto* desc = reinterpret_cast<IMAGE_DELAYLOAD_DESCRIPTOR*>(base + delayed.VirtualAddress);
        for (; desc->DllNameRVA; ++desc) {
            if (!desc->Attributes.RvaBased || !desc->ImportNameTableRVA || !desc->ImportAddressTableRVA) continue;
            auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->ImportNameTableRVA);
            auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->ImportAddressTableRVA);
            for (; names->u1.AddressOfData; ++names, ++slots) {
                if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
                const auto* name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
                patch_if_cursor_import(reinterpret_cast<const char*>(name->Name), &slots->u1.Function, sawPosition,
                                       sawClip);
            }
        }
    }
}

// Scans the executable and every loaded module that is not part of Windows or Sedici itself
// (K132: Alan Wake 2's executable does not import both calls itself). Safe to repeat: patched
// slots are remembered, and a rescan finds modules loaded and delay-load slots resolved since.
bool install_hooks() noexcept
{
    std::lock_guard<std::mutex> lock(g_install_mutex);
    const ULONGLONG now = GetTickCount64();
    if (!g_patched.empty() && now - g_last_scan < 2000) return true;
    if (g_patched.empty() && g_last_scan != 0 && now - g_last_scan < 250) return false;
    g_last_scan = now;

    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&install_hooks), &self);
    const HMODULE executable = GetModuleHandleW(nullptr);
    std::vector<HMODULE> modules(256);
    DWORD needed = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), modules.data(),
                               static_cast<DWORD>(modules.size() * sizeof(HMODULE)), &needed)) {
        modules.assign(1, executable);
    } else {
        if (needed > modules.size() * sizeof(HMODULE)) {
            modules.resize(needed / sizeof(HMODULE) + 16);
            if (!K32EnumProcessModules(GetCurrentProcess(), modules.data(),
                                       static_cast<DWORD>(modules.size() * sizeof(HMODULE)), &needed))
                needed = sizeof(HMODULE), modules[0] = executable;
        }
        modules.resize(needed / sizeof(HMODULE));
    }

    const InterceptionSummary before = g_summary;
    bool sawPosition = false, sawClip = false;
    unsigned scanned = 0;
    for (HMODULE module : modules) {
        // Sedici's own imports stay alone, except when Sedici is the executable (the offline test host).
        if (!module || (module == self && module != executable)) continue;
        if (module != executable && is_system_module(module)) continue;
        ++scanned;
        scan_module(module, &sawPosition, &sawClip);
    }
    g_summary.modulesScanned = scanned;
    const bool changed = g_summary.setCursorPosSlots != before.setCursorPosSlots ||
                         g_summary.clipCursorSlots != before.clipCursorSlots;
    if (changed || (g_patched.empty() && before.modulesScanned == 0)) {
        log_event(1, std::string("sedici: overlay cursor interception ") +
                         (g_patched.empty() ? "unavailable" : "installed") +
                         ": modules=" + std::to_string(scanned) +
                         " slots{SetCursorPos=" + std::to_string(g_summary.setCursorPosSlots) +
                         " ClipCursor=" + std::to_string(g_summary.clipCursorSlots) +
                         "} skipped=" + std::to_string(g_summary.skippedSlots) + " seen{SetCursorPos=" +
                         (sawPosition ? "yes" : "no") + " ClipCursor=" + (sawClip ? "yes" : "no") + "}");
    }
    // One of the two is enough to be useful: the game then loses only the call it does not make
    // through an import we can see. The summary above records which.
    return !g_patched.empty();
}
} // namespace

InterceptionSummary ui_input_interception_summary() noexcept
{
    std::lock_guard<std::mutex> lock(g_install_mutex);
    return g_summary;
}

bool ui_input_enter(HWND hwnd) noexcept
{
    if (g_disabled.load() || !hwnd || GetForegroundWindow() != hwnd || !install_hooks()) return false;
    std::unique_lock<std::mutex> lock(g_mutex);
    // Entry/exit and ShowCursor accounting belong to the owning window thread.
    if (GetWindowThreadProcessId(hwnd, nullptr) != GetCurrentThreadId()) return false;
    if (g_active.load()) return g_window.load() == hwnd;
    RECT clip{};
    if (!GetClipCursor(&clip)) return false;
    g_clip_rect = clip;
    g_clip_set = true;
    g_window.store(hwnd, std::memory_order_release);
    g_active.store(true, std::memory_order_release);
    if (!g_clip(nullptr)) {
        g_active.store(false, std::memory_order_release);
        return false;
    }
    g_show_adjustments = 0;
    // Raise this input thread's ShowCursor counter to >= 0 (visible) and
    // record only the calls needed, so leaving restores the original count.
    int count = ShowCursor(TRUE);
    if (count > 0) {
        ShowCursor(FALSE);
    } else {
        g_show_adjustments = 1;
        while (count < 0 && g_show_adjustments < 32) {
            count = ShowCursor(TRUE);
            ++g_show_adjustments;
        }
    }
    // Publish all bookkeeping before releasing capture: ReleaseCapture sends
    // WM_CAPTURECHANGED synchronously, and the game's handler may call our
    // ClipCursor hook or leave Full. Neither may run under this mutex.
    lock.unlock();
    if (GetCapture() == hwnd) ReleaseCapture();
    if (!capturing()) return false;
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    return true;
}

void ui_input_leave(bool restore_clip) noexcept
{
    ui_input_leave_window(g_window.load(), restore_clip);
}

void ui_input_leave_window(HWND hwnd, bool restore_clip) noexcept
{
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!hwnd || g_window.load() != hwnd || !g_active.load()) return;
        if (GetWindowThreadProcessId(hwnd, nullptr) != GetCurrentThreadId()) return;
        g_active.store(false, std::memory_order_release);
        g_window.store(nullptr, std::memory_order_release);
        while (g_show_adjustments > 0) {
            ShowCursor(FALSE);
            --g_show_adjustments;
        }
        if (restore_clip && GetForegroundWindow() == hwnd) g_clip(g_clip_set ? &g_clip_rect : nullptr);
        const unsigned positions = g_suppressed_pos.exchange(0);
        const unsigned clips = g_suppressed_clip.exchange(0);
        if (positions || clips) {
            log_event(1, "sedici: overlay cursor interception released suppressed{sets=" +
                             std::to_string(positions) + " clips=" + std::to_string(clips) + "}");
        }
    } catch (...) {
        // Capture is already released before diagnostic formatting can throw.
    }
}

void ui_input_disable() noexcept
{
    g_disabled.store(true, std::memory_order_release);
}

bool ui_input_active() noexcept
{
    return capturing();
}

bool ui_input_available() noexcept
{
    return install_hooks();
}

POINT ui_input_map_point(LONG x, LONG y, LONG client_width, LONG client_height, LONG output_width,
                         LONG output_height) noexcept
{
    POINT point{x, y};
    if (client_width <= 0 || client_height <= 0 || output_width <= 0 || output_height <= 0) {
        return point;
    }
    point.x = static_cast<LONG>((static_cast<long long>(x) * output_width) / client_width);
    point.y = static_cast<LONG>((static_cast<long long>(y) * output_height) / client_height);
    if (point.x < 0) point.x = 0;
    if (point.y < 0) point.y = 0;
    if (point.x >= output_width) point.x = output_width - 1;
    if (point.y >= output_height) point.y = output_height - 1;
    return point;
}
} // namespace sedici
