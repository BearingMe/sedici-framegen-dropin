#include "config.h"
#include "config_patch.h"
#include "forward.h"
#include "imgui_config.h"
#include "live.h"
#include "log.h"
#include "overlay.h"
#include "present_hook.h"
#include "toggles.h"
#include "ui.h"
#include "ui_input.h"
#include "ffx_api_framegeneration_dx12.h"

#include <windows.h>
#include <dxgi.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" const char g_sedici_proxy_marker[] = "SEDICI_PROXY_V1";

namespace {
int failures = 0;
int checks = 0;
void check(bool ok, const char* message)
{
    ++checks;
    if (!ok) ++failures;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", message);
}
void write(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << text;
    if (!file) throw std::runtime_error("test fixture write failed");
}
std::string read(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void config_checks(const std::filesystem::path& path)
{
    sedici::ConfigValues values;
    values.write_pacing = true;
    values.pacing = "hybrid";
    std::string error;
    write(path, "[pacing] # keep\r\nsafety_margin_ms = 0.2\r\n\r\n"
                "[[profiles]]\r\nmode = \"unrelated\" # keep too\r\n");
    check(sedici::config_save(path, values, error), "save accepts array-of-table boundary");
    const std::string output = read(path);
    check(output.find("mode = \"hybrid\"\r\n") < output.find("[[profiles]]"),
          "pacing value stays in its own table");
    check(output.find("mode = \"unrelated\" # keep too\r\n") != std::string::npos,
          "array table and comments stay unchanged");

    write(path, "[Pacing]\nmode = \"untouched\"\n");
    check(sedici::config_save(path, values, error), "case-sensitive target table can be appended");
    check(read(path).find("[Pacing]\nmode = \"untouched\"") != std::string::npos,
          "differently cased table is untouched");

    const std::string multiline = "[pacing]\ntext = '''\nmode = \"inside string\"\n'''\n";
    write(path, multiline);
    check(!sedici::config_save(path, values, error), "unsafe multiline patch is rejected");
    check(read(path) == multiline, "rejected save preserves every original byte");

    const std::string quoted = "[pacing]\n\"mode\" = \"off\"\n";
    write(path, quoted);
    check(!sedici::config_save(path, values, error), "unsupported quoted key fails safely");
    check(read(path) == quoted, "quoted-key rejection does not overwrite file");

    write(path, "[pacing]\nmode = \"off\" # keep\n[limiter]\nfps = 0\n");
    values.write_limiter_fps = true;
    values.limiter_fps = 36.5;
    check(sedici::config_save(path, values, error), "fractional FPS saves with comments");
    check(read(path).find("fps = 36.5") != std::string::npos, "fractional FPS survives patch");
}

constexpr UINT marker = WM_APP + 77;
int destroyed = 0;
LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l)
{
    if (msg == marker) return GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_NCDESTROY) ++destroyed;
    return DefWindowProcW(hwnd, msg, w, l);
}
WNDPROC above_original = nullptr;
LRESULT CALLBACK above_proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l)
{
    return CallWindowProcW(above_original, hwnd, msg, w, l);
}

void gpu_checks(bool full)
{
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D12Debug> debug;
    const bool validation = SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
    if (validation) debug->EnableDebugLayer();
    else std::puts("NOTE DX12 debug layer unavailable; no SDK/runtime will be downloaded");
    ComPtr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
        throw std::runtime_error("create GPU test device");
    ComPtr<ID3D12InfoQueue> messages;
    device.As(&messages);
    if (messages) messages->ClearStoredMessages();
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    if (FAILED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(),
                                        nullptr, IID_PPV_ARGS(&list))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
        throw std::runtime_error("create GPU test command objects");
    list->Close();
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 1280;
    desc.Height = 720;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> output;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&output))))
        throw std::runtime_error("create GPU test output");
    WNDCLASSW cls{};
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpfnWndProc = window_proc;
    cls.lpszClassName = L"SediciGpuRegression";
    RegisterClassW(&cls);
    HWND hwnd = CreateWindowW(cls.lpszClassName, L"GPU regression", WS_OVERLAPPEDWINDOW,
                               0, 0, 640, 360, nullptr, nullptr, cls.hInstance, nullptr);
    if (!hwnd) throw std::runtime_error("create GPU test window");
    sedici::ui_capture_window(hwnd);
    sedici::live_set_ui_mode(full ? sedici::UiMode::Full : sedici::UiMode::Mini);
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) throw std::runtime_error("create GPU test fence event");
    for (UINT64 frame = 1; frame <= 12; ++frame) {
        if (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(), nullptr)))
            throw std::runtime_error("reset GPU test command list");
        ffxCallbackDescFrameGenerationPresent params{};
        params.device = device.Get();
        params.commandList = list.Get();
        params.outputSwapChainBuffer.resource = output.Get();
        params.outputSwapChainBuffer.state = FFX_API_RESOURCE_STATE_RENDER_TARGET;
        sedici::ui_on_present(&params);
        check(sedici::live_ui_mode() != sedici::UiMode::Off, "GPU overlay remains enabled");
        if (FAILED(list->Close())) throw std::runtime_error("close GPU test command list");
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        if (FAILED(queue->Signal(fence.Get(), frame)) ||
            FAILED(fence->SetEventOnCompletion(frame, event)) ||
            WaitForSingleObject(event, 5000) != WAIT_OBJECT_0)
            throw std::runtime_error("GPU test submission did not finish");
        // Exercise platform-backend rebinding as well as renderer ring reuse.
        if (frame == 6) {
            HWND replacement = CreateWindowW(cls.lpszClassName, L"Replacement", WS_OVERLAPPEDWINDOW,
                0, 0, 960, 540, nullptr, nullptr, cls.hInstance, nullptr);
            if (!replacement) throw std::runtime_error("create replacement GPU test window");
            sedici::ui_capture_window(replacement);
            DestroyWindow(hwnd);
            hwnd = replacement;
        }
    }
    CloseHandle(event);
    check(device->GetDeviceRemovedReason() == S_OK, "GPU device remains healthy");
    if (messages) {
        bool clean = true;
        for (UINT64 i = 0; i < messages->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
            SIZE_T bytes = 0;
            messages->GetMessage(i, nullptr, &bytes);
            std::vector<unsigned char> storage(bytes);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if (SUCCEEDED(messages->GetMessage(i, message, &bytes)) &&
                message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                std::printf("DX12: %s\n", message->pDescription);
                clean = false;
            }
        }
        check(clean, "DX12 debug layer reports no error or corruption");
    }
    DestroyWindow(hwnd);
}
void window_checks()
{
    WNDCLASSW cls{};
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpfnWndProc = window_proc;
    cls.lpszClassName = L"SediciRegression";
    if (!RegisterClassW(&cls)) throw std::runtime_error("register test window");
    HWND a = CreateWindowW(cls.lpszClassName, L"A", WS_OVERLAPPEDWINDOW,
                           0, 0, 320, 240, nullptr, nullptr, cls.hInstance, nullptr);
    HWND b = CreateWindowW(cls.lpszClassName, L"B", WS_OVERLAPPEDWINDOW,
                           0, 0, 320, 240, nullptr, nullptr, cls.hInstance, nullptr);
    if (!a || !b) throw std::runtime_error("create test window");
    SetWindowLongPtrW(a, GWLP_USERDATA, 101);
    SetWindowLongPtrW(b, GWLP_USERDATA, 202);
    sedici::ui_capture_window(a);
    sedici::ui_capture_window(b);
    check(SendMessageW(a, marker, 0, 0) == 101, "old window retains its original procedure");
    check(SendMessageW(b, marker, 0, 0) == 202, "new window forwards its own messages");
    above_original = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        b, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&above_proc)));
    sedici::ui_capture_window(b);
    check(SendMessageW(b, marker, 0, 0) == 202,
          "recapture below another subclass does not form a recursive chain");
    DestroyWindow(a);
    check(destroyed == 1, "old window WM_NCDESTROY reaches the game procedure");
    check(SendMessageW(b, marker, 0, 0) == 202, "old destruction leaves new window intact");
    BYTE keys[256]{};
    keys[VK_MENU] = 0x80;
    SetKeyboardState(keys);
    SendMessageW(b, WM_SYSKEYDOWN, VK_F10, 0);
    check(sedici::live_ui_mode() == sedici::UiMode::Mini, "hotkey still works after replacement");
    const HWND previous_foreground = GetForegroundWindow();
    ShowWindow(b, SW_SHOW);
    SetForegroundWindow(b);
    if (GetForegroundWindow() == b) {
        sedici::live_set_ui_mode(sedici::UiMode::Full);
        SendMessageW(b, WM_APP + 1, 0, 0);
        check(sedici::ui_input_active(), "Full capture enters on the owning window thread");
    } else {
        std::puts("NOTE foreground unavailable; active capture failure check skipped");
    }
    sedici::ui_disable("injected regression failure");
    check(!sedici::ui_input_active(), "overlay failure immediately stops cursor suppression");
    SendMessageW(b, WM_APP + 1, 0, 0);
    SendMessageW(b, WM_SYSKEYDOWN, VK_F10, 0);
    SendMessageW(b, WM_SYSKEYDOWN, VK_F10, 0);
    check(sedici::live_ui_mode() == sedici::UiMode::Off, "disabled overlay cannot reopen Full");
    keys[VK_MENU] = 0;
    SetKeyboardState(keys);
    DestroyWindow(b);
    if (previous_foreground) SetForegroundWindow(previous_foreground);
    check(destroyed == 2, "current window WM_NCDESTROY reaches the game procedure");
}

struct FakeSwapchain { void** vtable; };
int presents_a = 0;
int presents_b = 0;
int pacing_calls = 0;
HRESULT STDMETHODCALLTYPE present_a(IDXGISwapChain*, UINT, UINT) { ++presents_a; return S_FALSE; }
HRESULT STDMETHODCALLTYPE present_b(IDXGISwapChain*, UINT, UINT) { ++presents_b; return DXGI_STATUS_OCCLUDED; }
HRESULT STDMETHODCALLTYPE present1_a(IDXGISwapChain*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*)
{ ++presents_a; return S_FALSE; }
HRESULT STDMETHODCALLTYPE present1_b(IDXGISwapChain*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*)
{ ++presents_b; return DXGI_STATUS_OCCLUDED; }
HRESULT STDMETHODCALLTYPE get_desc(IDXGISwapChain*, DXGI_SWAP_CHAIN_DESC*) { return E_FAIL; }
ffxReturnCode_t configure_stub(ffxContext*, const ffxConfigureDescHeader*)
{ ++pacing_calls; return FFX_API_RETURN_OK; }

void present_checks()
{
    // Static storage: hooks and original functions must outlive every invocation.
    static void* va[23]{};
    static void* vb[23]{};
    static FakeSwapchain a{va}, b{vb};
    va[8] = reinterpret_cast<void*>(&present_a);
    va[12] = reinterpret_cast<void*>(&get_desc);
    va[22] = reinterpret_cast<void*>(&present1_a);
    vb[8] = reinterpret_cast<void*>(&present_b);
    vb[12] = reinterpret_cast<void*>(&get_desc);
    vb[22] = reinterpret_cast<void*>(&present1_b);
    auto* pa = reinterpret_cast<IDXGISwapChain4*>(&a);
    auto* pb = reinterpret_cast<IDXGISwapChain4*>(&b);
    ffxCreateContextDescFrameGenerationSwapChainWrapDX12 desc{};
    desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WRAP_DX12;
    desc.swapchain = &pa;
    present_hook_maybe_install(&desc.header);
    desc.swapchain = &pb;
    present_hook_maybe_install(&desc.header);
    check(va[8] != reinterpret_cast<void*>(&present_a), "Present installs with startup log=0 and fps=0");
    using Present = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
    using Present1 = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
    check(reinterpret_cast<Present>(va[8])(pa, 0, 0) == S_FALSE, "first vtable keeps its Present original");
    check(reinterpret_cast<Present>(vb[8])(pb, 0, 0) == DXGI_STATUS_OCCLUDED, "second vtable keeps its Present original");
    check(reinterpret_cast<Present1>(va[22])(pa, 0, 0, nullptr) == S_FALSE, "first vtable keeps its Present1 original");
    check(reinterpret_cast<Present1>(vb[22])(pb, 0, 0, nullptr) == DXGI_STATUS_OCCLUDED, "second vtable keeps its Present1 original");
    check(presents_a == 2 && presents_b == 2, "all Present calls forward exactly once");
    sedici::live_set_limiter_fps(1000);
    reinterpret_cast<Present>(va[8])(pa, 0, 0);
    LARGE_INTEGER before{}, after{}, frequency{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&before);
    reinterpret_cast<Present>(va[8])(pa, 0, 0);
    QueryPerformanceCounter(&after);
    sedici::live_set_limiter_fps(0);
    check(presents_a == 4, "limiter can be enabled live after silent uncapped startup");
    check(static_cast<double>(after.QuadPart - before.QuadPart) / frequency.QuadPart >= 0.0005,
          "live limiter actually delays Present after silent uncapped startup");

    g_ffxConfigure = &configure_stub;
    ffxContext context = reinterpret_cast<void*>(0x1234);
    desc.swapchain = &pa;
    toggles_after_create(&context, &desc.header);
    toggles_apply_pacing_live();
    check(pacing_calls == 0, "live pacing request does not enter FFX from callback");
    reinterpret_cast<Present>(va[8])(pa, 0, 0);
    check(pacing_calls == 1, "pending pacing is sent on next application Present");
    toggles_on_destroy(context);
    toggles_apply_pacing_live();
    reinterpret_cast<Present>(va[8])(pa, 0, 0);
    check(pacing_calls == 1, "destroyed context is not used by pending live pacing");
}

int callback_a_token = 0;
int callback_b_token = 0;
ffxReturnCode_t callback_a(ffxCallbackDescFrameGenerationPresent*, void* user)
{ return user == &callback_a_token ? FFX_API_RETURN_OK : FFX_API_RETURN_ERROR; }
ffxReturnCode_t callback_b(ffxCallbackDescFrameGenerationPresent*, void* user)
{ return user == &callback_b_token ? FFX_API_RETURN_OK : FFX_API_RETURN_ERROR; }
void callback_checks()
{
    ffxConfigureDescFrameGeneration a{}, b{};
    a.header.type = b.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    a.presentCallback = callback_a;
    a.presentCallbackUserContext = &callback_a_token;
    b.presentCallback = callback_b;
    b.presentCallbackUserContext = &callback_b_token;
    FfxApiPresentCallbackFunc saved_a = nullptr, saved_b = nullptr;
    void* user_a = nullptr;
    void* user_b = nullptr;
    {
        ScopedOverlayCallback patch(&a.header);
        saved_a = a.presentCallback;
        user_a = a.presentCallbackUserContext;
    }
    {
        ScopedOverlayCallback patch(&b.header);
        saved_b = b.presentCallback;
        user_b = b.presentCallbackUserContext;
        patch.complete(FFX_API_RETURN_ERROR);
    }
    ffxCallbackDescFrameGenerationPresent present{};
    check(saved_a(&present, user_a) == FFX_API_RETURN_OK, "in-flight callback retains its own context");
    check(saved_b(&present, user_b) == FFX_API_RETURN_OK, "another callback has its own immutable binding");
    check(a.presentCallback == callback_a && a.presentCallbackUserContext == &callback_a_token,
          "scoped callback patch restores caller fields");
    check(saved_a(&present, user_a) == FFX_API_RETURN_OK,
          "rejected configure cannot overwrite accepted callback binding");
    ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12 registration{};
    registration.header.type =
        FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_REGISTERUIRESOURCE_DX12;
    registration.uiResource.resource = reinterpret_cast<void*>(1);
    void* context = reinterpret_cast<void*>(0x5678);
    {
        ScopedOverlayCallback patch(&registration.header, context);
        patch.complete(FFX_API_RETURN_ERROR);
    }
    ffxConfigureDescFrameGeneration default_composition{};
    default_composition.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    {
        ScopedOverlayCallback patch(&default_composition.header, context);
        check(default_composition.presentCallback != nullptr,
              "rejected UI registration cannot change composition policy");
    }
}
} // namespace

int main(int argc, char** argv)
{
    const auto config = dll_directory() / L"sedici.toml";
    const auto fixture = dll_directory() / L"patch-fixture.toml";
    if (std::filesystem::exists(config) || std::filesystem::exists(fixture)) {
        std::puts("Refusing to overwrite existing regression fixtures");
        return 1;
    }
    try {
        for (const auto* name : {L"SEDICI_PACING", L"SEDICI_ASYNC", L"SEDICI_LIMIT_FPS", L"MFSRFG_LOG"})
            SetEnvironmentVariableW(name, nullptr);
        write(config, "[log]\nlevel = 0\n[limiter]\nfps = 0\n[pacing]\nmode = \"off\"\n");
        (void)sedici::settings();
        if (argc > 1) {
            gpu_checks(std::string(argv[1]) == "gpu-full");
        } else {
            config_checks(fixture);
            const POINT mapped = sedici::ui_input_map_point(-10, -20, 640, 360, 1280, 720);
            check(mapped.x == 0 && mapped.y == 0, "negative client coordinates map to the near edge");
            try { sedici::imgui_assert_failed("injected assertion"); }
            catch (const std::runtime_error&) { check(true, "ImGui assertion throws instead of aborting game"); }
            present_checks();
            callback_checks();
            window_checks();
        }
    } catch (const std::exception& error) {
        check(false, error.what());
    }
    std::filesystem::remove(config);
    std::filesystem::remove(fixture);
    std::printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
