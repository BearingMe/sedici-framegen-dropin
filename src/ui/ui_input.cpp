#include "ui/ui_input.h"
#include "diagnostics/log.h"
#include "settings/live.h"

#include <atomic>
#include <cstring>
#include <mutex>

namespace sedici {
namespace {
using SetPos = BOOL (WINAPI*)(int, int);
using Clip = BOOL (WINAPI*)(const RECT*);
SetPos g_set_pos = ::SetCursorPos;
Clip g_clip = ::ClipCursor;
std::atomic<bool> g_active{false};
std::atomic<HWND> g_window{nullptr};
std::mutex g_mutex;
std::mutex g_install_mutex;
std::atomic<bool> g_disabled{false};
RECT g_clip_rect{};
bool g_clip_set = false;
bool g_installed = false;
int g_show_adjustments = 0;
std::atomic<unsigned> g_suppressed_pos{0};
std::atomic<unsigned> g_suppressed_clip{0};

bool capturing() noexcept
{
    const HWND hwnd = g_window.load(std::memory_order_acquire);
    return !g_disabled.load(std::memory_order_acquire) &&
           live_ui_mode() == UiMode::Full &&
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
    const auto previous = InterlockedCompareExchangePointer(
        reinterpret_cast<void* volatile*>(slot), reinterpret_cast<void*>(replacement),
        reinterpret_cast<void*>(expected));
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(*slot), protection, &ignored);
    return previous == reinterpret_cast<void*>(expected);
}

bool install_hooks() noexcept
{
    std::lock_guard<std::mutex> lock(g_install_mutex);
    if (g_installed) return true;
    auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if (!base) return false;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    const auto imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!imports.VirtualAddress) return false;
    ULONG_PTR* pos_slot = nullptr;
    ULONG_PTR* clip_slot = nullptr;
    auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress);
    for (; desc->Name; ++desc) {
        if (!desc->OriginalFirstThunk) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            const auto* name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(name->Name), "SetCursorPos") == 0)
                pos_slot = &slots->u1.Function;
            else if (std::strcmp(reinterpret_cast<const char*>(name->Name), "ClipCursor") == 0)
                clip_slot = &slots->u1.Function;
        }
    }
    if (!pos_slot || !clip_slot) return false;
    if (!replace_slot(pos_slot, reinterpret_cast<ULONG_PTR>(g_set_pos),
                      reinterpret_cast<ULONG_PTR>(&set_pos_hook))) return false;
    if (!replace_slot(clip_slot, reinterpret_cast<ULONG_PTR>(g_clip),
                      reinterpret_cast<ULONG_PTR>(&clip_hook))) {
        replace_slot(pos_slot, reinterpret_cast<ULONG_PTR>(&set_pos_hook),
                     reinterpret_cast<ULONG_PTR>(g_set_pos));
        return false;
    }
    g_installed = true;
    log_event(1, "sedici: overlay cursor interception installed on executable imports");
    return true;
}
} // namespace

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
        while (g_show_adjustments > 0) { ShowCursor(FALSE); --g_show_adjustments; }
        if (restore_clip && GetForegroundWindow() == hwnd)
            g_clip(g_clip_set ? &g_clip_rect : nullptr);
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

void ui_input_disable() noexcept { g_disabled.store(true, std::memory_order_release); }

bool ui_input_active() noexcept { return capturing(); }

bool ui_input_available() noexcept { return install_hooks(); }

POINT ui_input_map_point(LONG x, LONG y, LONG client_width, LONG client_height,
                         LONG output_width, LONG output_height) noexcept
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
