#include "ui.h"

#include "config.h"
#include "config_patch.h"
#include "describe.h"
#include "live.h"
#include "limiter.h"
#include "log.h"
#include "overlay_compose.h"
#include "stats.h"
#include "timing.h"
#include "toggles.h"
#include "ui_input.h"

#include "imgui.h"
#include "imgui_impl_dx12.h"
#include "imgui_impl_win32.h"

// The Win32 backend intentionally leaves this declaration commented out in its
// header to avoid pulling <windows.h> into every user; copy it here.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam,
                                                             LPARAM lParam);

#include <windows.h>
#include <d3d12.h>
#include <dxgi.h>

#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace sedici {

[[noreturn]] void imgui_assert_failed(const char* expression)
{
    throw std::runtime_error(expression);
}

namespace {

constexpr UINT kSrvDescriptors = 64;
constexpr UINT kRtvDescriptors = 8;
constexpr UINT kMaxQueuedMessages = 512;
constexpr UINT kHotkeyMessage = WM_APP + 1;

struct Hotkey {
    bool ctrl = false;
    bool alt = true;
    bool shift = false;
    UINT vk = VK_F10;
};

struct InputMessage {
    UINT msg = 0;
    WPARAM wparam = 0;
    LPARAM lparam = 0;
};

struct Snapshot {
    double limiter_fps = 0.0;
    int pacing_mode = 1;
    int async_mode = 0;
    int log_level = 1;
    int scale_mode = 0;
    float scale = 1.0f;
    int previous_mode = 0;
};

struct UiState {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12DescriptorHeap> srv_heap;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;

    UINT srv_size = kSrvDescriptors;
    UINT srv_next = 0;
    std::vector<UINT> srv_free;

    DXGI_FORMAT rtv_format = DXGI_FORMAT_UNKNOWN;
    UINT rtv_slot = 0;

    bool dx12_ready = false;
    bool win32_ready = false;
    bool ready = false;

    ImGuiStyle base_style{};
    float applied_scale = 0.0f;
    uint64_t last_frame_qpc = 0;

    UiMode last_mode = UiMode::Off;
    Snapshot snapshot{};
    bool snapshot_valid = false;

    ConfigFileStamp stamp{};
    bool stamp_valid = false;
    std::string save_status;

    float last_nonzero_limit = 36.5f;

    // Client-area size Windows reports input in, and the output size ImGui
    // draws at. They differ when the game renders at a different resolution.
    LONG client_width = 0;
    LONG client_height = 0;
    LONG output_width = 0;
    LONG output_height = 0;
};

UiState g_ui;
std::atomic<HWND> g_hwnd{nullptr};
std::mutex g_window_mutex;
std::mutex g_render_mutex;
std::once_flag g_hotkey_once;
// Keep the chain for each window until it is destroyed. Another overlay may
// subclass above us, so replacing its top-level procedure is not safe.
std::map<HWND, WNDPROC> g_window_procs;
std::atomic<bool> g_disabled{false};
Hotkey g_hotkey{};

std::mutex g_message_mutex;
std::vector<InputMessage> g_messages;

uint64_t qpc_now() noexcept
{
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return static_cast<uint64_t>(value.QuadPart);
}

const uint64_t g_qpc_frequency = [] {
    LARGE_INTEGER frequency{};
    return QueryPerformanceFrequency(&frequency) ? static_cast<uint64_t>(frequency.QuadPart) : 0;
}();

bool iequals(std::string_view lhs, std::string_view rhs)
{
    if (lhs.size() != rhs.size()) return false;
    for (size_t i = 0; i < lhs.size(); ++i) {
        const char a = lhs[i] >= 'A' && lhs[i] <= 'Z' ? static_cast<char>(lhs[i] + 32) : lhs[i];
        const char b = rhs[i] >= 'A' && rhs[i] <= 'Z' ? static_cast<char>(rhs[i] + 32) : rhs[i];
        if (a != b) return false;
    }
    return true;
}

Hotkey parse_hotkey(const std::string& text)
{
    Hotkey hotkey;
    hotkey.ctrl = false;
    hotkey.alt = false;
    hotkey.shift = false;
    hotkey.vk = VK_F10;

    size_t start = 0;
    for (;;) {
        const size_t plus = text.find('+', start);
        const size_t end = plus == std::string::npos ? text.size() : plus;
        const std::string_view part(text.data() + start, end - start);
        if (iequals(part, "alt")) {
            hotkey.alt = true;
        } else if (iequals(part, "ctrl")) {
            hotkey.ctrl = true;
        } else if (iequals(part, "shift")) {
            hotkey.shift = true;
        } else if (part.size() >= 2 && (part[0] == 'f' || part[0] == 'F')) {
            int number = 0;
            for (size_t i = 1; i < part.size(); ++i) {
                if (part[i] < '0' || part[i] > '9') continue;
                number = number * 10 + (part[i] - '0');
            }
            if (number >= 1 && number <= 12) hotkey.vk = VK_F1 + static_cast<UINT>(number - 1);
        } else if (part.size() == 1) {
            if (part[0] >= 'a' && part[0] <= 'z') {
                hotkey.vk = 'A' + static_cast<UINT>(part[0] - 'a');
            } else if (part[0] >= '0' && part[0] <= '9') {
                hotkey.vk = '0' + static_cast<UINT>(part[0] - '0');
            }
        }
        if (plus == std::string::npos) break;
        start = plus + 1;
    }
    if (!hotkey.ctrl && !hotkey.alt && !hotkey.shift) hotkey.alt = true;
    return hotkey;
}

bool is_consumed_input(UINT msg) noexcept
{
    switch (msg) {
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_XBUTTONDBLCLK:
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
    case WM_CHAR:
    case WM_SYSCHAR:
        return true;
    default:
        return false;
    }
}

void cycle_mode() noexcept
{
    if (g_disabled.load(std::memory_order_acquire)) return;
    const UiMode mode = live_ui_mode();
    const UiMode next = mode == UiMode::Off ? UiMode::Mini
                       : mode == UiMode::Mini ? UiMode::Full
                                              : UiMode::Off;
    live_set_ui_mode(next);
    if (HWND hwnd = g_hwnd.load()) PostMessageW(hwnd, kHotkeyMessage, static_cast<WPARAM>(next), 0);
}

LRESULT ui_window_message(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (msg == kHotkeyMessage) {
        if (!g_disabled.load(std::memory_order_acquire) && live_ui_mode() == UiMode::Full) {
            if (!ui_input_available()) {
                live_set_ui_mode(UiMode::Mini);
                log_error("sedici: Full menu unavailable: the executable's cursor calls "
                          "could not be intercepted; using Mini");
            } else if (!ui_input_active() && GetForegroundWindow() == hwnd) {
                ui_input_enter(hwnd);
            }
        } else {
            ui_input_leave(true);
        }
        return 0;
    }

    if (msg == WM_KILLFOCUS || msg == WM_DESTROY) {
        ui_input_leave(false);
        std::lock_guard<std::mutex> lock(g_message_mutex);
        g_messages.clear();
        g_messages.push_back(InputMessage{msg, wparam, lparam});
    } else if (msg == WM_NCDESTROY) {
        ui_input_leave(false);
    } else if (msg == WM_SETFOCUS) {
        {
            std::lock_guard<std::mutex> lock(g_message_mutex);
            g_messages.push_back(InputMessage{msg, wparam, lparam});
        }
        if (live_ui_mode() == UiMode::Full) PostMessageW(hwnd, kHotkeyMessage, 0, 0);
    }

    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
        if (static_cast<UINT>(wparam) == g_hotkey.vk) {
            const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
            const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (ctrl == g_hotkey.ctrl && alt == g_hotkey.alt && shift == g_hotkey.shift) {
                if ((lparam & (1LL << 30)) == 0) cycle_mode();
                return 0;
            }
        }
    }

    if (!g_disabled.load(std::memory_order_acquire) &&
        live_ui_mode() == UiMode::Full && ui_input_active()) {
        if (msg == WM_SETCURSOR) {
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;
        }
        if (msg == WM_INPUT) return DefWindowProcW(hwnd, msg, wparam, lparam);
        if (is_consumed_input(msg)) {
            {
                std::lock_guard<std::mutex> lock(g_message_mutex);
                if (g_messages.size() >= kMaxQueuedMessages) g_messages.erase(g_messages.begin());
                g_messages.push_back(InputMessage{msg, wparam, lparam});
            }
            if (msg == WM_MOUSEMOVE) SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return 0;
        }
    }

    return 0; // Unhandled: forwarded by ui_wndproc below.
}

LRESULT CALLBACK ui_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    WNDPROC original = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_window_mutex);
        const auto found = g_window_procs.find(hwnd);
        if (found != g_window_procs.end()) original = found->second;
    }
    if (msg == kHotkeyMessage && (g_disabled.load() || hwnd != g_hwnd.load())) {
        ui_input_leave_window(hwnd, true);
        return 0;
    }
    if (msg == WM_DESTROY || msg == WM_NCDESTROY)
        ui_input_leave_window(hwnd, false);
    // Older windows may still have another overlay above our subclass. They
    // retain their own chain, but must not control the current window's input.
    if (hwnd == g_hwnd.load()) {
        try {
            const bool captured = !g_disabled.load() && live_ui_mode() == UiMode::Full &&
                                  ui_input_active();
            const bool hotkey = !g_disabled.load() &&
                                (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) &&
                                static_cast<UINT>(wparam) == g_hotkey.vk &&
                                ((GetKeyState(VK_CONTROL) & 0x8000) != 0) == g_hotkey.ctrl &&
                                ((GetKeyState(VK_MENU) & 0x8000) != 0) == g_hotkey.alt &&
                                ((GetKeyState(VK_SHIFT) & 0x8000) != 0) == g_hotkey.shift;
            const LRESULT result = ui_window_message(hwnd, msg, wparam, lparam);
            if (msg == kHotkeyMessage || hotkey ||
                (captured && (msg == WM_INPUT || msg == WM_SETCURSOR || is_consumed_input(msg))))
                return result;
        } catch (...) {
            ui_disable("an exception was thrown while processing input");
            ui_input_leave(true);
        }
    }
    // WM_NCDESTROY must reach the game's original procedure before we forget it.
    const LRESULT result = original ? CallWindowProcW(original, hwnd, msg, wparam, lparam)
                                    : DefWindowProcW(hwnd, msg, wparam, lparam);
    if (msg == WM_NCDESTROY) {
        std::lock_guard<std::mutex> lock(g_window_mutex);
        g_window_procs.erase(hwnd);
        HWND expected = hwnd;
        g_hwnd.compare_exchange_strong(expected, nullptr);
    }
    return result;
}

DXGI_FORMAT translate_typeless(DXGI_FORMAT format) noexcept
{
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return format;
    }
}

void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) noexcept
{
    if (!list || !resource || before == after) return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    list->ResourceBarrier(1, &barrier);
}

void disable_overlay(const char* reason) noexcept
{
    ui_disable(reason);
}

void shutdown_imgui()
{
    if (g_ui.dx12_ready) {
        ImGui_ImplDX12_Shutdown();
        g_ui.dx12_ready = false;
    }
    if (g_ui.win32_ready) {
        ImGui_ImplWin32_Shutdown();
        g_ui.win32_ready = false;
    }
    if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
    g_ui.ready = false;
    g_ui.srv_heap.Reset();
    g_ui.rtv_heap.Reset();
    g_ui.queue.Reset();
    g_ui.device.Reset();
    g_ui.srv_next = 0;
    g_ui.srv_free.clear();
    g_ui.rtv_slot = 0;
    g_ui.applied_scale = 0.0f;
}

void srv_alloc(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* cpu,
               D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
{
    *cpu = {};
    *gpu = {};
    if (!info || !info->UserData) return;
    UiState& ui = *static_cast<UiState*>(info->UserData);
    if (!ui.srv_heap || !ui.device) return;

    UINT index = 0;
    if (!ui.srv_free.empty()) {
        index = ui.srv_free.back();
        ui.srv_free.pop_back();
    } else if (ui.srv_next < ui.srv_size) {
        index = ui.srv_next++;
    } else {
        throw std::runtime_error("ImGui SRV descriptor heap exhausted");
    }

    const UINT increment =
        ui.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    cpu->ptr = ui.srv_heap->GetCPUDescriptorHandleForHeapStart().ptr +
               static_cast<SIZE_T>(index) * increment;
    gpu->ptr = ui.srv_heap->GetGPUDescriptorHandleForHeapStart().ptr +
               static_cast<SIZE_T>(index) * increment;
}

void srv_free(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpu,
              D3D12_GPU_DESCRIPTOR_HANDLE)
{
    if (!info || !info->UserData) return;
    UiState& ui = *static_cast<UiState*>(info->UserData);
    if (!ui.srv_heap || !ui.device) return;
    const UINT increment =
        ui.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const SIZE_T base = ui.srv_heap->GetCPUDescriptorHandleForHeapStart().ptr;
    if (cpu.ptr < base) return;
    const UINT index = static_cast<UINT>((cpu.ptr - base) / increment);
    if (index < ui.srv_size) ui.srv_free.push_back(index);
}

D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle(UINT index) noexcept
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = g_ui.rtv_heap->GetCPUDescriptorHandleForHeapStart();
    const UINT increment =
        g_ui.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    handle.ptr += static_cast<SIZE_T>(index) * increment;
    return handle;
}

// Only contain faults in our backend calls, never in game/FFX code. Some of the
// unmodified backend's failed COM creations are unchecked even with assertions.
// These leaf bridges have no C++ objects requiring SEH unwinding.
bool backend_create_objects()
{
    __try { return ImGui_ImplDX12_CreateDeviceObjects(); }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ?
                  EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return false; }
}

bool backend_new_frame()
{
    __try { ImGui_ImplDX12_NewFrame(); return true; }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ?
                  EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return false; }
}

bool backend_draw(ImDrawData* data, ID3D12GraphicsCommandList* list)
{
    __try { ImGui_ImplDX12_RenderDrawData(data, list); return true; }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ?
                  EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return false; }
}

bool ensure_imgui(ID3D12Device* device, DXGI_FORMAT format, HWND hwnd)
{
    if (g_disabled.load()) return false;
    if (device && FAILED(device->GetDeviceRemovedReason())) {
        disable_overlay("the D3D12 device was removed");
        return false;
    }
    if (g_ui.ready) {
        if (ImGui::GetMainViewport()->PlatformHandleRaw != hwnd) {
            // The DX12 resources stay alive; only the platform backend changes.
            ImGui_ImplWin32_Shutdown();
            g_ui.win32_ready = false;
            if (!ImGui_ImplWin32_Init(hwnd)) {
                disable_overlay("could not rebind the game window");
                return false;
            }
            g_ui.win32_ready = true;
        }
        if (g_ui.device.Get() != device) {
            disable_overlay("the D3D12 device changed");
            return false;
        }
        if (g_ui.rtv_format != format) {
            disable_overlay("the output format changed");
            return false;
        }
        return true;
    }
    if (!device || !hwnd) {
        disable_overlay("no device or game window");
        return false;
    }

    g_ui.device = device;

    D3D12_DESCRIPTOR_HEAP_DESC srv_desc{};
    srv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv_desc.NumDescriptors = kSrvDescriptors;
    srv_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&srv_desc, IID_PPV_ARGS(g_ui.srv_heap.GetAddressOf())))) {
        disable_overlay("could not create the SRV descriptor heap");
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{};
    rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_desc.NumDescriptors = kRtvDescriptors;
    if (FAILED(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(g_ui.rtv_heap.GetAddressOf())))) {
        disable_overlay("could not create the RTV descriptor heap");
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(g_ui.queue.GetAddressOf())))) {
        disable_overlay("could not create the texture upload queue");
        return false;
    }

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    g_ui.base_style = ImGui::GetStyle();

    if (!ImGui_ImplWin32_Init(hwnd)) {
        shutdown_imgui();
        disable_overlay("ImGui Win32 backend initialization failed");
        return false;
    }
    g_ui.win32_ready = true;

    ImGui_ImplDX12_InitInfo info{};
    info.Device = device;
    info.CommandQueue = g_ui.queue.Get();
    info.NumFramesInFlight = 4;
    info.RTVFormat = format;
    info.DSVFormat = DXGI_FORMAT_UNKNOWN;
    info.UserData = &g_ui;
    info.SrvDescriptorHeap = g_ui.srv_heap.Get();
    info.SrvDescriptorAllocFn = &srv_alloc;
    info.SrvDescriptorFreeFn = &srv_free;

    if (!ImGui_ImplDX12_Init(&info)) {
        shutdown_imgui();
        disable_overlay("ImGui DX12 backend initialization failed");
        return false;
    }
    g_ui.dx12_ready = true;
    if (!backend_create_objects()) {
        disable_overlay("could not create ImGui DX12 device objects");
        return false;
    }
    g_ui.rtv_format = format;
    g_ui.ready = true;
    g_ui.last_frame_qpc = qpc_now();

    log_event(1, "sedici: overlay ImGui initialized");
    return true;
}

float effective_scale(uint32_t height) noexcept
{
    if (live_ui_scale_mode() == UiScaleMode::Auto) {
        const float scale = height > 0 ? static_cast<float>(height) / 1080.0f : 1.0f;
        return std::clamp(scale, 0.5f, 2.0f);
    }
    return std::clamp(live_ui_scale(), 0.5f, 2.0f);
}

void apply_scale(float scale)
{
    if (std::fabs(scale - g_ui.applied_scale) < 0.001f) return;
    ImGui::GetStyle() = g_ui.base_style;
    ImGui::GetStyle().ScaleAllSizes(scale);
    ImGui::GetIO().FontGlobalScale = scale;
    g_ui.applied_scale = scale;
}

void enter_full()
{
    g_ui.snapshot.limiter_fps = live_limiter_fps();
    g_ui.snapshot.pacing_mode = static_cast<int>(live_pacing_mode());
    g_ui.snapshot.async_mode = static_cast<int>(live_async_mode());
    g_ui.snapshot.log_level = live_log_level();
    g_ui.snapshot.scale_mode = static_cast<int>(live_ui_scale_mode());
    g_ui.snapshot.scale = live_ui_scale();
    g_ui.snapshot.previous_mode = static_cast<int>(g_ui.last_mode);
    g_ui.snapshot_valid = true;

    const std::filesystem::path path = dll_directory() / L"sedici.toml";
    g_ui.stamp = config_file_stamp(path);
    g_ui.stamp_valid = true;
    g_ui.save_status.clear();
}

void apply_mode_transition(UiMode mode)
{
    if (mode == g_ui.last_mode) return;
    if (mode == UiMode::Full) enter_full();
    if (mode != UiMode::Full) {
        std::lock_guard<std::mutex> lock(g_message_mutex);
        g_messages.clear();
    }
    if (HWND hwnd = g_hwnd.load()) {
        PostMessageW(hwnd, kHotkeyMessage, static_cast<WPARAM>(mode), 0);
    }
    g_ui.last_mode = mode;
}

void drain_messages()
{
    std::vector<InputMessage> messages;
    {
        std::lock_guard<std::mutex> lock(g_message_mutex);
        messages.swap(g_messages);
    }
    const HWND hwnd = g_hwnd.load();
    if (!hwnd) return;

    const auto map_position = [](LPARAM lparam) {
        const POINT point = ui_input_map_point(static_cast<short>(LOWORD(lparam)),
                                               static_cast<short>(HIWORD(lparam)),
                                               g_ui.client_width, g_ui.client_height,
                                               g_ui.output_width, g_ui.output_height);
        return MAKELPARAM(static_cast<WORD>(point.x), static_cast<WORD>(point.y));
    };

    for (const InputMessage& message : messages) {
        switch (message.msg) {
        case WM_MOUSEMOVE:
            ImGui_ImplWin32_WndProcHandler(hwnd, message.msg, message.wparam,
                                           map_position(message.lparam));
            break;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
        case WM_XBUTTONDBLCLK:
            // Button messages carry a position too, but ImGui reacts with the
            // last known one. Feed the current position first so a click lands
            // where the cursor visibly is.
            ImGui_ImplWin32_WndProcHandler(hwnd, WM_MOUSEMOVE, message.wparam,
                                           map_position(message.lparam));
            ImGui_ImplWin32_WndProcHandler(hwnd, message.msg, message.wparam, message.lparam);
            break;
        default:
            ImGui_ImplWin32_WndProcHandler(hwnd, message.msg, message.wparam, message.lparam);
            break;
        }
    }
}

void draw_mini()
{
    const TimingStats real = timing_real_stats();
    const TimingStats output = timing_output_stats();

    ImGui::SetNextWindowPos(ImVec2(12.0f, 12.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.65f);
    ImGui::Begin("##sedici-mini", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoInputs);
    ImGui::Text("real %.1f  |  out %.1f  |  p99 %.1f ms", real.fps, output.fps,
                output.p99_ms);
    ImGui::End();
}

void draw_graph(const char* label, const float* samples, size_t count, float scale)
{
    if (count == 0) {
        ImGui::TextDisabled("%s: waiting for samples", label);
        return;
    }
    ImGui::PlotLines(label, samples, static_cast<int>(count), 0, nullptr, 0.0f, 40.0f,
                     ImVec2(-1.0f, 84.0f * scale));
}

void draw_scale_combo()
{
    static const float kScales[] = {0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f};
    static const char* kItems = "Auto\0"
                                "0.50\0"
                                "0.75\0"
                                "1.00\0"
                                "1.25\0"
                                "1.50\0"
                                "1.75\0"
                                "2.00\0";

    int index = 0;
    if (live_ui_scale_mode() == UiScaleMode::Fixed) {
        const float value = live_ui_scale();
        int best = 0;
        float best_distance = 1e9f;
        for (int i = 0; i < static_cast<int>(std::size(kScales)); ++i) {
            const float distance = std::fabs(value - kScales[i]);
            if (distance < best_distance) {
                best_distance = distance;
                best = i;
            }
        }
        index = best + 1;
    }

    if (ImGui::Combo("Menu scale", &index, kItems)) {
        if (index == 0) {
            live_set_ui_scale_mode(UiScaleMode::Auto);
        } else {
            live_set_ui_scale_mode(UiScaleMode::Fixed);
            live_set_ui_scale(kScales[index - 1]);
        }
    }
    ImGui::Text("Effective %.2fx", g_ui.applied_scale);
}

void save_config()
{
    const std::filesystem::path path = dll_directory() / L"sedici.toml";
    const ConfigFileStamp current = config_file_stamp(path);
    if (g_ui.stamp_valid &&
        (current.exists != g_ui.stamp.exists || current.last_write != g_ui.stamp.last_write ||
         current.size != g_ui.stamp.size)) {
        g_ui.save_status = "sedici.toml changed on disk; not saved";
        log_error("sedici: overlay save refused: sedici.toml changed on disk");
        return;
    }

    const Settings& config = settings();
    ConfigValues values;
    if (config.limiter.fps_source != Source::Env) {
        values.write_limiter_fps = true;
        values.limiter_fps = live_limiter_fps();
    }
    if (config.pacing.mode_source != Source::Env) {
        values.write_pacing = true;
        values.pacing = live_pacing_mode() == PacingMode::Hybrid ? "hybrid" : "off";
    }
    if (config.async.mode_source != Source::Env) {
        values.write_async = true;
        values.async = live_async_mode() == AsyncMode::Off ? "off" : "game";
    }
    if (config.log.level_source != Source::Env) {
        values.write_log = true;
        values.log_level = live_log_level();
    }
    // `[ui] mode` is a startup preference, not a control in this menu. Leaving
    // it alone means Save does not unexpectedly make the menu open on launch.
    values.write_ui_scale = true;
    values.ui_scale_auto = live_ui_scale_mode() == UiScaleMode::Auto;
    values.ui_scale = live_ui_scale();

    std::string error;
    if (config_save(path, values, error)) {
        g_ui.save_status = "Saved";
        g_ui.stamp = config_file_stamp(path);
        g_ui.stamp_valid = true;
        log_event(1, "sedici: overlay saved sedici.toml");
    } else {
        g_ui.save_status = "Save failed: " + error;
        log_error("sedici: overlay save failed: " + error);
    }
}

void cancel_config()
{
    if (!g_ui.snapshot_valid) return;

    live_set_limiter_fps(g_ui.snapshot.limiter_fps);
    live_set_pacing_mode(static_cast<PacingMode>(g_ui.snapshot.pacing_mode));
    live_set_async_mode(static_cast<AsyncMode>(g_ui.snapshot.async_mode));
    live_set_log_level(g_ui.snapshot.log_level);
    live_set_ui_scale_mode(static_cast<UiScaleMode>(g_ui.snapshot.scale_mode));
    live_set_ui_scale(g_ui.snapshot.scale);

    limiter_on_config_changed();
    toggles_apply_pacing_live();
    live_set_ui_mode(static_cast<UiMode>(g_ui.snapshot.previous_mode));
    g_ui.save_status = "Cancelled";
}

void draw_full()
{
    const float scale = g_ui.applied_scale > 0.0f ? g_ui.applied_scale : 1.0f;
    const TimingStats real = timing_real_stats();
    const TimingStats output = timing_output_stats();
    uint32_t render_width = 0;
    uint32_t render_height = 0;
    stats_render_size(render_width, render_height);

    const uint32_t output_width = static_cast<uint32_t>(ImGui::GetIO().DisplaySize.x);
    const uint32_t output_height = static_cast<uint32_t>(ImGui::GetIO().DisplaySize.y);

    ImGui::SetNextWindowSize(ImVec2(560.0f * scale, 690.0f * scale), ImGuiCond_FirstUseEver);
    ImGui::Begin("Sedici", nullptr, ImGuiWindowFlags_NoCollapse);

    ImGui::SeparatorText("Status");
    ImGui::Text("Frame generation: %s", toggles_fg_active() ? "on" : "off");
    if (render_width != 0 && render_height != 0) {
        ImGui::Text("Render %ux%u  ->  output %ux%u", render_width, render_height,
                    output_width, output_height);
    } else {
        ImGui::Text("Output %ux%u", output_width, output_height);
    }
    ImGui::Text("Real %.1f fps  |  Output %.1f fps", real.fps, output.fps);
    ImGui::Text("Pacing fix: %s", live_pacing_mode() == PacingMode::Hybrid ? "on" : "off");
    const int pacing_result = toggles_last_pacing_result();
    if (pacing_result == INT_MIN) {
        ImGui::TextDisabled("Last pacing send: not yet");
    } else {
        ImGui::Text("Last pacing send: %d (%s)", pacing_result,
                    ffx_return_code_name(static_cast<ffxReturnCode_t>(pacing_result)));
    }
    ImGui::Text("Stalls: %llu  |  last %.1f ms",
                static_cast<unsigned long long>(stats_stall_count()), stats_last_stall_ms());

    ImGui::SeparatorText("Frame times");
    static float output_samples[kTimingSamples];
    static float real_samples[kTimingSamples];
    const size_t output_count = timing_output_samples(output_samples, kTimingSamples);
    const size_t real_count = timing_real_samples(real_samples, kTimingSamples);

    ImGui::Text("Output  p50 %.2f  p99 %.2f  max %.2f ms", output.p50_ms, output.p99_ms,
                output.max_ms);
    draw_graph("##output_graph", output_samples, output_count, scale);
    ImGui::Text("Real    p50 %.2f  p99 %.2f  max %.2f ms", real.p50_ms, real.p99_ms,
                real.max_ms);
    draw_graph("##real_graph", real_samples, real_count, scale);

    ImGui::SeparatorText("FPS limit");
    const bool limiter_env = settings().limiter.fps_source == Source::Env;
    const double live_fps = live_limiter_fps();
    bool limit_on = live_fps > 0.0;
    if (limit_on) g_ui.last_nonzero_limit = static_cast<float>(live_fps);

    ImGui::BeginDisabled(limiter_env);
    if (ImGui::Checkbox("Limit real frames", &limit_on)) {
        if (limit_on) {
            live_set_limiter_fps(g_ui.last_nonzero_limit > 0.0f ? g_ui.last_nonzero_limit : 36.5f);
        } else {
            if (live_fps > 0.0) g_ui.last_nonzero_limit = static_cast<float>(live_fps);
            live_set_limiter_fps(0.0);
        }
        limiter_on_config_changed();
    }
    if (limit_on) {
        float target = static_cast<float>(live_limiter_fps());
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::SliderFloat("##limit_value", &target, 20.0f, 240.0f, "%.1f fps",
                               ImGuiSliderFlags_AlwaysClamp)) {
            target = std::round(target * 2.0f) / 2.0f;
            g_ui.last_nonzero_limit = target;
            live_set_limiter_fps(target);
            limiter_on_config_changed();
        }
        if (ImGui::Button("36.5")) {
            g_ui.last_nonzero_limit = 36.5f;
            live_set_limiter_fps(36.5);
            limiter_on_config_changed();
        }
        ImGui::SameLine();
        if (ImGui::Button("37")) {
            g_ui.last_nonzero_limit = 37.0f;
            live_set_limiter_fps(37.0);
            limiter_on_config_changed();
        }
    }
    ImGui::EndDisabled();
    if (limiter_env) ImGui::TextDisabled("overridden by SEDICI_LIMIT_FPS");

    ImGui::SeparatorText("Pacing fix");
    const bool pacing_env = settings().pacing.mode_source == Source::Env;
    bool pacing_on = live_pacing_mode() == PacingMode::Hybrid;
    ImGui::BeginDisabled(pacing_env);
    if (ImGui::Checkbox("Send hybrid pacing tuning", &pacing_on)) {
        live_set_pacing_mode(pacing_on ? PacingMode::Hybrid : PacingMode::Off);
        toggles_apply_pacing_live();
    }
    ImGui::EndDisabled();
    if (pacing_env) ImGui::TextDisabled("overridden by SEDICI_PACING");

    ImGui::SeparatorText("Async workloads");
    const bool async_env = settings().async.mode_source == Source::Env;
    int async_index = live_async_mode() == AsyncMode::Off ? 1 : 0;
    ImGui::BeginDisabled(async_env);
    if (ImGui::Combo("Async workloads", &async_index, "game\0off\0")) {
        live_set_async_mode(async_index == 1 ? AsyncMode::Off : AsyncMode::Game);
    }
    ImGui::EndDisabled();
    if (async_env) ImGui::TextDisabled("overridden by SEDICI_ASYNC");
    ImGui::TextWrapped("Takes effect after frame generation restarts (toggle FG or restart "
                       "the game).");

    ImGui::SeparatorText("Logging");
    const bool log_env = settings().log.level_source == Source::Env;
    int log_level = live_log_level();
    ImGui::BeginDisabled(log_env);
    if (ImGui::Combo("Log level", &log_level, "errors\0normal\0verbose\0")) {
        live_set_log_level(log_level);
    }
    ImGui::EndDisabled();
    if (log_env) ImGui::TextDisabled("overridden by MFSRFG_LOG");

    ImGui::SeparatorText("Menu");
    draw_scale_combo();

    ImGui::Separator();
    if (ImGui::Button("Save")) save_config();
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) cancel_config();
    ImGui::SameLine();
    if (ImGui::Button("Close")) live_set_ui_mode(UiMode::Off);
    if (!g_ui.save_status.empty()) ImGui::TextWrapped("%s", g_ui.save_status.c_str());

    ImGui::End();
}

void render_overlay(UiMode mode, ID3D12GraphicsCommandList* command_list, ID3D12Resource* output,
                    DXGI_FORMAT format, uint32_t output_state, uint32_t width,
                     uint32_t height)
{
    if (g_disabled.load() || !command_list || !output || width == 0 || height == 0) return;

    // Windows gives input in client coordinates; the overlay draws at the
    // output resolution. Record both so drain_messages() can map clicks.
    g_ui.output_width = static_cast<LONG>(width);
    g_ui.output_height = static_cast<LONG>(height);
    RECT client{};
    if (const HWND hwnd = g_hwnd.load(); hwnd && GetClientRect(hwnd, &client) &&
                                       client.right > 0 && client.bottom > 0) {
        g_ui.client_width = client.right;
        g_ui.client_height = client.bottom;
    } else {
        g_ui.client_width = 0;
        g_ui.client_height = 0;
    }
    if (!backend_new_frame()) {
        disable_overlay("the DX12 backend failed while starting a frame");
        return;
    }
    ImGui_ImplWin32_NewFrame();
    drain_messages();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
    const uint64_t now = qpc_now();
    if (g_qpc_frequency && g_ui.last_frame_qpc != 0 && now > g_ui.last_frame_qpc) {
        const double elapsed =
            static_cast<double>(now - g_ui.last_frame_qpc) / static_cast<double>(g_qpc_frequency);
        io.DeltaTime = static_cast<float>(std::clamp(elapsed, 0.001, 0.1));
    } else {
        io.DeltaTime = 1.0f / 60.0f;
    }
    g_ui.last_frame_qpc = now;

    apply_scale(effective_scale(height));

    ImGui::NewFrame();
    if (mode == UiMode::Mini) {
        draw_mini();
    } else {
        draw_full();
    }
    ImGui::Render();

    ImDrawData* draw_data = ImGui::GetDrawData();
    if (!draw_data) return;

    const UINT slot = g_ui.rtv_slot++ % kRtvDescriptors;
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_handle(slot);
    D3D12_RENDER_TARGET_VIEW_DESC rtv_desc{};
    rtv_desc.Format = format;
    rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    g_ui.device->CreateRenderTargetView(output, &rtv_desc, rtv);

    const D3D12_RESOURCE_STATES before =
        static_cast<D3D12_RESOURCE_STATES>(dx12_state_from_ffx(output_state));
    if (before != D3D12_RESOURCE_STATE_RENDER_TARGET) {
        transition(command_list, output, before, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }

    try {
        ID3D12DescriptorHeap* heaps[] = {g_ui.srv_heap.Get()};
        command_list->SetDescriptorHeaps(1, heaps);
        command_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height),
                                0.0f, 1.0f};
        D3D12_RECT scissor{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
        command_list->RSSetViewports(1, &viewport);
        command_list->RSSetScissorRects(1, &scissor);
        if (!backend_draw(draw_data, command_list))
            disable_overlay("the DX12 backend failed while drawing");
    } catch (...) {
        if (before != D3D12_RESOURCE_STATE_RENDER_TARGET) {
            transition(command_list, output, D3D12_RESOURCE_STATE_RENDER_TARGET, before);
        }
        disable_overlay("an exception was thrown while drawing");
        return;
    }

    if (before != D3D12_RESOURCE_STATE_RENDER_TARGET) {
        transition(command_list, output, D3D12_RESOURCE_STATE_RENDER_TARGET, before);
    }
}

} // namespace

void ui_disable(const char* reason) noexcept
{
    if (g_disabled.exchange(true, std::memory_order_acq_rel)) return;
    ui_input_disable(); // Stop suppression immediately, even before the message is pumped.
    live_set_ui_mode(UiMode::Off);
    try {
        std::lock_guard<std::mutex> lock(g_window_mutex);
        for (const auto& window : g_window_procs)
            PostMessageW(window.first, kHotkeyMessage, 0, 0);
    } catch (...) {
    }
    log_error("sedici: overlay disabled");
    log_error(reason);
}

void ui_capture_window(HWND hwnd) noexcept
{
    if (!hwnd) return;
    try {
        std::lock_guard<std::mutex> lock(g_window_mutex);
        std::call_once(g_hotkey_once, [] { g_hotkey = parse_hotkey(settings().ui.hotkey); });
        if (!g_window_procs.count(hwnd)) {
            // Allocate before changing the window, so allocation failure cannot
            // leave an installed hook with no forwarding target.
            auto inserted = g_window_procs.emplace(hwnd, nullptr).first;
            SetLastError(0);
            const LONG_PTR previous =
                SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&ui_wndproc));
            if (previous == 0 && GetLastError() != 0) {
                g_window_procs.erase(inserted);
                log_error("sedici: could not install the overlay input hook");
                return;
            }
            inserted->second = reinterpret_cast<WNDPROC>(previous);
        }
        const HWND previous_window = g_hwnd.exchange(hwnd);
        if (previous_window && previous_window != hwnd) {
            PostMessageW(previous_window, kHotkeyMessage, 0, 0);
        }
        log_event(1, "sedici: overlay input hook installed");
    } catch (...) {
        log_error("sedici: failed to capture the game window for the overlay");
    }
}

static void request_full_capture() noexcept
{
    const HWND hwnd = g_hwnd.load();
    if (!hwnd || GetForegroundWindow() != hwnd) return;

    // Focus can arrive after the hotkey, so Full retries capture from the
    // render path. Rate-limited: this is a window-thread round trip.
    static std::atomic<uint64_t> last_attempt{0};
    const uint64_t now = qpc_now();
    const uint64_t gate = g_qpc_frequency / 2;
    uint64_t previous = last_attempt.load(std::memory_order_relaxed);
    if (previous != 0 && now - previous < gate) return;
    if (!last_attempt.compare_exchange_strong(previous, now, std::memory_order_relaxed)) return;
    PostMessageW(hwnd, kHotkeyMessage, 0, 0);
}

void ui_on_present(const ffxCallbackDescFrameGenerationPresent* params) noexcept
{
    try {
        // FFX may invoke real/generated callbacks on different threads. Never
        // concurrently mutate the single ImGui context or its renderer buffers.
        std::lock_guard<std::mutex> render_lock(g_render_mutex);
        if (g_disabled.load()) return;
        const UiMode mode = live_ui_mode();
        apply_mode_transition(mode);
        if (mode == UiMode::Off) return;
        if (mode == UiMode::Full && !ui_input_active()) request_full_capture();
        if (!params || !params->commandList || !params->outputSwapChainBuffer.resource) return;

        auto* device = static_cast<ID3D12Device*>(params->device);
        auto* command_list = static_cast<ID3D12GraphicsCommandList*>(params->commandList);
        auto* output = static_cast<ID3D12Resource*>(params->outputSwapChainBuffer.resource);

        const D3D12_RESOURCE_DESC desc = output->GetDesc();
        const DXGI_FORMAT format = translate_typeless(desc.Format);
        if (!ensure_imgui(device, format, g_hwnd.load())) return;

        render_overlay(mode, command_list, output, format, params->outputSwapChainBuffer.state,
                       static_cast<uint32_t>(desc.Width), desc.Height);
    } catch (...) {
        disable_overlay("an exception was thrown while drawing");
    }
}

} // namespace sedici
