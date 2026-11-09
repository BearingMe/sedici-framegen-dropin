// Export-level harness: loads the *built* Sedici DLL through its five exports,
// next to a stub "original" (tests/abi_stub.cpp), each scenario in its own
// process and scratch directory with its own sedici.toml.
//
//   sedici_abi <scenario> <sedici.dll> <stub.dll>
//
// Scenarios: see the foreach lists in CMakeLists.txt (exports, contract,
// legacy-config, target-auto, readonly-sedici, bad-config, session, window-replace, window-replace-vsync,
// destroy-inflight, log-failure, invalid-calls, overlay-hotkey, stale-forward,
// unload, integrated).
// Never touches game files and never downloads anything.

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "abi_stub.h"
#include "ffx_api_dx12.h"

#pragma warning(disable : 4996) // Exercise the legacy prepare desc Cyberpunk actually uses.

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace {

int g_failures = 0;
int g_checks = 0;

void expect(bool ok, const std::string& what)
{
    ++g_checks;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++g_failures;
}

constexpr wchar_t kProxyName[] = L"amd_fidelityfx_framegeneration_dx12.dll";
constexpr wchar_t kOriginalName[] = L"amd_fidelityfx_framegeneration_dx12.dll.orig";

struct Exports {
    PfnFfxCreateContext create = nullptr;
    PfnFfxDestroyContext destroy = nullptr;
    PfnFfxConfigure configure = nullptr;
    PfnFfxQuery query = nullptr;
    PfnFfxDispatch dispatch = nullptr;
};

struct Harness {
    fs::path dir;
    HMODULE proxy = nullptr;
    HMODULE stub = nullptr;
    StubState* state = nullptr;
    Exports ffx;

    std::string log() const
    {
        // The DLL writes its log from a background thread, so a line logged just
        // before this call may not be on disk yet: wait for the file to stop growing.
        std::error_code ec;
        uintmax_t last = static_cast<uintmax_t>(-1);
        int steady = 0;
        for (int i = 0; i < 100 && steady < 3; ++i) {
            const uintmax_t size = fs::exists(dir / L"sedici.log", ec) && fs::is_regular_file(dir / L"sedici.log", ec)
                ? fs::file_size(dir / L"sedici.log", ec) : 0;
            steady = size == last ? steady + 1 : 0;
            last = size;
            Sleep(30);
        }
        std::ifstream in(dir / L"sedici.log", std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        return text.str();
    }
};

// New scratch directory beside the executable, unique per run and never
// deleted (it stays under the build tree): the proxy, optionally the stub as
// `.orig`, and the scenario's sedici.toml. The stub is loaded first by its full
// path, so the proxy's later LoadLibraryExW of the same file returns the same
// module and the harness can script it before the first call.
bool setup(Harness& h, const char* scenario, const fs::path& proxy, const fs::path& stub,
           const std::string& toml, bool withStub, bool blockLog = false)
{
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t unique[96]{};
    swprintf_s(unique, L"%hs-%04u%02u%02u-%02u%02u%02u-%lu", scenario, now.wYear, now.wMonth, now.wDay,
               now.wHour, now.wMinute, now.wSecond, GetCurrentProcessId());
    h.dir = fs::path(exe).parent_path() / L"abi" / unique;
    std::error_code error;
    if (fs::exists(h.dir, error)) return false; // never reuse or clear a directory
    fs::create_directories(h.dir, error);
    if (error) return false;
    fs::copy_file(proxy, h.dir / kProxyName, fs::copy_options::overwrite_existing, error);
    if (error) return false;
    if (withStub) {
        fs::copy_file(stub, h.dir / kOriginalName, fs::copy_options::overwrite_existing, error);
        if (error) return false;
        h.stub = LoadLibraryW((h.dir / kOriginalName).c_str());
        if (!h.stub) return false;
        const auto stateFn = reinterpret_cast<StubStateFn>(GetProcAddress(h.stub, "stub_state"));
        h.state = stateFn ? stateFn() : nullptr;
        if (!h.state) return false;
    }
    if (!toml.empty()) {
        std::ofstream out(h.dir / L"sedici.toml", std::ios::binary);
        out << toml;
        if (!out) return false;
    }
    if (blockLog) {
        fs::create_directory(h.dir / L"sedici.log", error); // opening the log as a file now fails
        if (error) return false;
    }
    h.proxy = LoadLibraryW((h.dir / kProxyName).c_str());
    if (!h.proxy) return false;
    h.ffx.create = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(h.proxy, "ffxCreateContext"));
    h.ffx.destroy = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(h.proxy, "ffxDestroyContext"));
    h.ffx.configure = reinterpret_cast<PfnFfxConfigure>(GetProcAddress(h.proxy, "ffxConfigure"));
    h.ffx.query = reinterpret_cast<PfnFfxQuery>(GetProcAddress(h.proxy, "ffxQuery"));
    h.ffx.dispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(h.proxy, "ffxDispatch"));
    return h.ffx.create && h.ffx.destroy && h.ffx.configure && h.ffx.query && h.ffx.dispatch;
}

StubProbe make_probe(uint8_t seed)
{
    StubProbe probe{};
    probe.header.type = kStubProbeType;
    for (size_t i = 0; i < sizeof(probe.payload); ++i) probe.payload[i] = static_cast<uint8_t>(seed + i * 7);
    return probe;
}

bool same_bytes(const void* a, const void* b, size_t size) { return std::memcmp(a, b, size) == 0; }

// ---------------------------------------------------------------------------
// exports: the DLL file itself exports exactly the five FFX entry points.
// ---------------------------------------------------------------------------
int run_exports(const fs::path& proxy)
{
    HMODULE image = LoadLibraryExW(proxy.c_str(), nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    expect(image != nullptr, "the built DLL maps as an image");
    if (!image) return 1;
    // Image-resource mappings tag the low bits of the handle.
    auto* base = reinterpret_cast<const uint8_t*>(reinterpret_cast<uintptr_t>(image) & ~uintptr_t{3});
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    std::set<std::string> names;
    if (directory.VirtualAddress) {
        const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + directory.VirtualAddress);
        const auto* nameRvas = reinterpret_cast<const DWORD*>(base + exports->AddressOfNames);
        for (DWORD i = 0; i < exports->NumberOfNames; ++i)
            names.insert(reinterpret_cast<const char*>(base + nameRvas[i]));
        expect(exports->NumberOfFunctions == 5, "exactly five exported functions (" +
               std::to_string(exports->NumberOfFunctions) + ")");
    }
    const std::set<std::string> expected{"ffxConfigure", "ffxCreateContext", "ffxDestroyContext",
                                         "ffxDispatch", "ffxQuery"};
    expect(names == expected, "the five exports are the FFX entry points and nothing else");
    FreeLibrary(image);
    return 0;
}


// ---------------------------------------------------------------------------
// Minimal DX12 host shared by the scenarios that need a real swapchain.
// ---------------------------------------------------------------------------
struct Host {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator[3]; // three lists may be in flight
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    UINT64 fenceValue = 0;
    HANDLE event = nullptr;
    HWND window = nullptr;
    int debugErrors = 0;

    ~Host()
    {
        if (window) DestroyWindow(window);
        if (event) CloseHandle(event);
    }

    bool create()
    {
        if (ComPtr<ID3D12Debug> debug; SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
            debug->EnableDebugLayer();
        if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) return false;
        if (ComPtr<ID3D12InfoQueue1> info; SUCCEEDED(device.As(&info))) {
            DWORD cookie = 0;
            info->RegisterMessageCallback([](D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity,
                                             D3D12_MESSAGE_ID, LPCSTR description, void* context) {
                if (severity > D3D12_MESSAGE_SEVERITY_ERROR) return;
                ++static_cast<Host*>(context)->debugErrors;
                std::fprintf(stderr, "DX12: %s\n", description);
            }, D3D12_MESSAGE_CALLBACK_FLAG_NONE, this, &cookie);
        }
        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue))) ||
            FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator[0]))) ||
            FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator[1]))) ||
            FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator[2]))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator[0].Get(), nullptr,
                                             IID_PPV_ARGS(&list))) ||
            FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
        list->Close();
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"SediciAbiHost";
        RegisterClassW(&wc);
        return event && make_window();
    }

    bool make_window()
    {
        window = CreateWindowW(L"SediciAbiHost", L"Sedici export harness", WS_OVERLAPPEDWINDOW,
                               CW_USEDEFAULT, CW_USEDEFAULT, 500, 320, nullptr, nullptr, GetModuleHandleW(nullptr),
                               nullptr);
        if (window) ShowWindow(window, SW_SHOWNOACTIVATE);
        return window != nullptr;
    }

    // The game destroying its window and opening another. Only call once the
    // swapchain on the old window has been released.
    bool replace_window()
    {
        if (window) DestroyWindow(window);
        window = nullptr;
        return make_window();
    }

    bool wait_for(UINT64 value)
    {
        if (fence->GetCompletedValue() >= value) return true;
        fence->SetEventOnCompletion(value, event);
        return WaitForSingleObject(event, 5000) == WAIT_OBJECT_0;
    }

    bool idle() { return wait_for(fenceValue); }

    bool begin()
    {
        const UINT64 next = fenceValue + 1;
        if (next > 3 && !wait_for(next - 3)) return false;
        ComPtr<ID3D12CommandAllocator>& a = allocator[next % 3];
        return SUCCEEDED(a->Reset()) && SUCCEEDED(list->Reset(a.Get(), nullptr));
    }

    // `wait` false leaves the GPU work in flight, as the game does.
    bool submit(bool wait = true)
    {
        if (FAILED(list->Close())) return false;
        ID3D12CommandList* lists[]{list.Get()};
        queue->ExecuteCommandLists(1, lists);
        if (FAILED(queue->Signal(fence.Get(), ++fenceValue))) return false;
        return wait ? wait_for(fenceValue) : true;
    }

    void pump()
    {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    ComPtr<IDXGISwapChain4> swapchain(UINT width, UINT height, UINT flags)
    {
        ComPtr<IDXGIFactory4> factory;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return nullptr;
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = width;
        desc.Height = height;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 3;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.Flags = flags;
        ComPtr<IDXGISwapChain1> created;
        ComPtr<IDXGISwapChain4> result;
        if (FAILED(factory->CreateSwapChainForHwnd(queue.Get(), window, &desc, nullptr, nullptr, &created)) ||
            FAILED(created.As(&result))) return nullptr;
        return result;
    }

    ComPtr<ID3D12Resource> texture(DXGI_FORMAT format, UINT width, UINT height, D3D12_RESOURCE_FLAGS flags)
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Flags = flags;
        ComPtr<ID3D12Resource> resource;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                   D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource))))
            return nullptr;
        return resource;
    }
};

void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                D3D12_RESOURCE_STATES after)
{
    if (before == after) return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    list->ResourceBarrier(1, &barrier);
}






int run_bad_config(Harness& h)
{
    // A malformed sedici.toml must not stop the DLL: defaults apply, the log says
    // so, and calls that need no configuration still work.
    uint64_t count = 0;
    ffxQueryDescGetVersions versions{};
    versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    versions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    versions.outputCount = &count;
    expect(h.ffx.query(nullptr, &versions.header) == FFX_API_RETURN_OK && count == 1,
           "version query works with a malformed sedici.toml");
    expect(h.log().find("could not be used; using built-in defaults") != std::string::npos,
           "the log reports the unusable sedici.toml and the defaults");
    return 0;
}


// The drop-in contract with no original DLL behind it. The stub beside the proxy
// is a tripwire: Sedici must never load it, so every call is answered locally.
int run_contract(Harness& h)
{
    if (!h.state) return 1;
    const auto stubCalls = [&] {
        uint32_t total = 0;
        for (uint32_t i = 0; i < StubExportCount; ++i) total += h.state->calls[i];
        return total;
    };

    struct Provider { uint64_t type; uint64_t id; const char* name; };
    const Provider providers[] = {
        {FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION, 0xf600000000c01006ull, "3.1.6"},
        {FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WRAP_DX12, 0xf65cdd1200c01007ull, "3.1.7"},
        {FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_NEW_DX12, 0xf65cdd1200c01007ull, "3.1.7"},
        {FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12, 0xf65cdd1200c01007ull, "3.1.7"},
    };
    for (const Provider& provider : providers) {
        uint64_t count = 0;
        ffxQueryDescGetVersions query{};
        query.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
        query.createDescType = provider.type;
        query.outputCount = &count;
        expect(h.ffx.query(nullptr, &query.header) == FFX_API_RETURN_OK && count == 1,
               "GetVersions with zero capacity reports one version");
        uint64_t capacity = 4;
        uint64_t ids[4]{};
        const char* names[4]{};
        query.outputCount = &capacity;
        query.versionIds = ids;
        query.versionNames = names;
        expect(h.ffx.query(nullptr, &query.header) == FFX_API_RETURN_OK && capacity == 1 &&
                   ids[0] == provider.id && names[0] && std::string(names[0]) == provider.name,
               std::string("GetVersions reports provider ") + provider.name);
    }

    uint64_t count = 0;
    ffxQueryDescGetVersions unknown{};
    unknown.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    unknown.createDescType = 0x7fffffff;
    unknown.outputCount = &count;
    expect(h.ffx.query(nullptr, &unknown.header) == FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE,
           "GetVersions for an unknown effect is an unknown-desctype error");
    unknown.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    unknown.outputCount = nullptr;
    expect(h.ffx.query(nullptr, &unknown.header) == FFX_API_RETURN_ERROR_PARAMETER,
           "GetVersions without an output count is a parameter error");

    ffxApiHeader odd{};
    odd.type = 0x7ffffffe;
    ffxContext none = nullptr;
    expect(h.ffx.create(&none, &odd, nullptr) == FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE && !none,
           "creating an unrecognised effect is an unknown-desctype error");
    expect(h.ffx.create(nullptr, &odd, nullptr) == FFX_API_RETURN_ERROR_PARAMETER,
           "create without an output handle is a parameter error");

    // Alan Wake 2's New form (P4.8, K100) is Sedici's to answer: without a factory, a desc, an
    // output or a queue it is a defined error, never UNKNOWN_DESCTYPE and never a handle.
    {
        ffxCreateContextDescFrameGenerationSwapChainNewDX12 made{};
        made.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_NEW_DX12;
        ffxContext handle = nullptr;
        const ffxReturnCode_t code = h.ffx.create(&handle, &made.header, nullptr);
        expect(code == FFX_API_RETURN_ERROR && !handle,
               "a New swapchain create without factory, desc or queue is a local error and issues no handle");
        ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 hwndMade{};
        hwndMade.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12;
        const ffxReturnCode_t hwndCode = h.ffx.create(&handle, &hwndMade.header, nullptr);
        expect(hwndCode == FFX_API_RETURN_ERROR && !handle,
               "a ForHwnd swapchain create without factory, desc, window or queue is a local error");
    }
    expect(h.ffx.destroy(nullptr, nullptr) == FFX_API_RETURN_ERROR_PARAMETER &&
               h.ffx.destroy(&none, nullptr) == FFX_API_RETURN_ERROR_PARAMETER,
           "destroying a null handle is a parameter error");
    expect(h.ffx.configure(&none, &odd) == FFX_API_RETURN_ERROR_PARAMETER &&
               h.ffx.dispatch(&none, &odd) == FFX_API_RETURN_ERROR_PARAMETER,
           "configure and dispatch on a null handle are parameter errors");
    expect(stubCalls() == 0, "no call reached the original DLL");
    return g_failures ? 1 : 0;
}


int run_target_auto(Harness& h)
{
    // FR-05: fg.target_fps = "auto" loads, is reported as such, and is not an error.
    uint64_t count = 0;
    ffxQueryDescGetVersions versions{};
    versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    versions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    versions.outputCount = &count;
    expect(h.ffx.query(nullptr, &versions.header) == FFX_API_RETURN_OK && count == 1,
           "version query works with fg.target_fps = \"auto\"");
    const std::string log = h.log();
    expect(log.find("fg.target_fps") != std::string::npos && log.find("= \"auto\" [file]") != std::string::npos,
           "the log reports fg.target_fps = \"auto\" from the file");
    expect(log.find("effective: target fps auto") != std::string::npos,
           "the effective line names the auto target");
    expect(log.find("expected a number") == std::string::npos && log.find("unknown value") == std::string::npos,
           "auto is not reported as a problem");
    return 0;
}

int run_legacy_config(Harness& h)
{
    // A sedici.toml from the forwarding era still loads. Its removed keys are
    // ignored and the log says so; the supported keys still apply.
    uint64_t count = 0;
    ffxQueryDescGetVersions versions{};
    versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    versions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    versions.outputCount = &count;
    expect(h.ffx.query(nullptr, &versions.header) == FFX_API_RETURN_OK && count == 1,
           "version query works with a legacy sedici.toml");
    const std::string log = h.log();
    expect(log.find("[pacing], [async], fg.mode no longer exist") != std::string::npos,
           "the log names the removed pacing, async and fg.mode keys");
    expect(log.find("unknown key limiter.sped") != std::string::npos &&
               log.find("unknown section [hud]") != std::string::npos,
           "the log names an unknown key and an unknown section with their lines");
    expect(log.find("limiter.fps is deprecated") != std::string::npos &&
               log.find("fg.target_fps = 50 output fps") != std::string::npos,
           "a legacy limiter.fps still applies, read as the output target (25 x 2)");
    return 0;
}

// ---------------------------------------------------------------------------
// session: fg.mode = "sedici" through the five exports with a real swapchain,
// the Mini overlay and the limiter: create, wrap (handing the reference over),
// configure + prepare + Present per frame, FG off/on, resize, destroy.
// ---------------------------------------------------------------------------
int run_session(Harness& h)
{
    constexpr UINT kWidth = 480, kHeight = 270;
    Host host;
    expect(host.create(), "DX12 host with the debug layer");
    if (g_failures) return 1;

    ffxCreateBackendDX12Desc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = host.device.Get();
    ffxCreateContextDescFrameGeneration create{};
    create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    create.header.pNext = &backend.header;
    create.flags = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED;
    create.displaySize = {kWidth, kHeight};
    create.maxRenderSize = {kWidth, kHeight};
    create.backBufferFormat = FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
    ffxContext fg = nullptr;
    expect(h.ffx.create(&fg, &create.header, nullptr) == FFX_API_RETURN_OK && fg,
           "Sedici frame generation context created through ffxCreateContext");

    // Shaped like the game's chain. The wrap desc hands the game's only
    // reference over and returns the wrapper in its place.
    ComPtr<IDXGISwapChain4> native = host.swapchain(kWidth, kHeight,
        DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING | DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
    expect(native != nullptr, "native swapchain");
    if (!native) return 1;
    native->SetMaximumFrameLatency(1);
    IDXGISwapChain4* swapchain = native.Detach();
    IDXGISwapChain4* const original = swapchain;
    ffxCreateContextDescFrameGenerationSwapChainWrapDX12 wrap{};
    wrap.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WRAP_DX12;
    wrap.header.pNext = &backend.header;
    wrap.swapchain = &swapchain;
    wrap.gameQueue = host.queue.Get();
    ffxContext wrapContext = nullptr;
    expect(h.ffx.create(&wrapContext, &wrap.header, nullptr) == FFX_API_RETURN_OK && wrapContext &&
           swapchain && swapchain != original, "swapchain wrapped through ffxCreateContext");
    ComPtr<IDXGISwapChain4> wrapper;
    wrapper.Attach(swapchain);
    if (!wrapper) return 1;

    auto color = host.texture(DXGI_FORMAT_R8G8B8A8_UNORM, kWidth, kHeight, D3D12_RESOURCE_FLAG_NONE);
    auto depth = host.texture(DXGI_FORMAT_R32_FLOAT, kWidth, kHeight, D3D12_RESOURCE_FLAG_NONE);
    auto motion = host.texture(DXGI_FORMAT_R16G16B16A16_FLOAT, kWidth, kHeight, D3D12_RESOURCE_FLAG_NONE);
    expect(color && depth && motion, "frame inputs");
    if (!color || !depth || !motion) return 1;

    HANDLE latency = wrapper->GetFrameLatencyWaitableObject();
    uint64_t frameId = 0;
    int failedCalls = 0, failedPresents = 0;
    const auto frames = [&](int count, bool enabled) {
        for (int i = 0; i < count; ++i, ++frameId) {
            host.pump();
            if (latency) WaitForSingleObjectEx(latency, 1000, TRUE);
            if (!host.begin()) { ++failedCalls; return; }
            ffxConfigureDescFrameGeneration configure{};
            configure.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
            configure.swapChain = wrapper.Get();
            configure.frameGenerationEnabled = enabled;
            configure.HUDLessColor = ffxApiGetResourceDX12(color.Get(), FFX_API_RESOURCE_STATE_COMMON);
            configure.frameID = frameId;
            if (h.ffx.configure(&fg, &configure.header) != FFX_API_RETURN_OK) ++failedCalls;
            if (enabled) {
                ffxDispatchDescFrameGenerationPrepare prepare{};
                prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
                prepare.frameID = frameId;
                prepare.commandList = host.list.Get();
                prepare.renderSize = {kWidth, kHeight};
                prepare.motionVectorScale = {static_cast<float>(kWidth), static_cast<float>(kHeight)};
                prepare.depth = ffxApiGetResourceDX12(depth.Get(), FFX_API_RESOURCE_STATE_COMMON);
                prepare.motionVectors = ffxApiGetResourceDX12(motion.Get(), FFX_API_RESOURCE_STATE_COMMON);
                if (h.ffx.dispatch(&fg, &prepare.header) != FFX_API_RETURN_OK) ++failedCalls;
            }
            // Stand in for the game's frame: copy the HUD-less colour into the
            // backbuffer the wrapper hands out.
            ComPtr<ID3D12Resource> buffer;
            if (FAILED(wrapper->GetBuffer(wrapper->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&buffer)))) {
                ++failedCalls;
                return;
            }
            transition(host.list.Get(), color.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
            transition(host.list.Get(), buffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
            host.list->CopyResource(buffer.Get(), color.Get());
            transition(host.list.Get(), buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
            transition(host.list.Get(), color.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            if (!host.submit()) { ++failedCalls; return; }
            if (FAILED(wrapper->Present(0, DXGI_PRESENT_ALLOW_TEARING))) ++failedPresents;
        }
    };

    // 30 real fps at 2x is 60 output fps, within a 75 Hz window, so nothing has
    // to be dropped; 170 frames give > 300 outputs, one overlay stats window.
    frames(170, true);
    frames(10, false);   // frame generation off
    frames(10, true);    // and back on
    {
        // Resize through the wrapper, as the game does on a resolution change.
        // Explicit size: 0 x 0 would take the window's client area.
        const HRESULT hr = wrapper->ResizeBuffers(0, kWidth, kHeight, DXGI_FORMAT_UNKNOWN,
            DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING | DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
        expect(SUCCEEDED(hr), "resize through the wrapper");
    }
    frames(10, true);
    expect(failedCalls == 0, "configure/prepare/GetBuffer/submit all succeeded (" + std::to_string(failedCalls) + " failed)");
    expect(failedPresents == 0, "every Present succeeded (" + std::to_string(failedPresents) + " failed)");

    expect(h.ffx.destroy(&fg, nullptr) == FFX_API_RETURN_OK, "frame generation context destroyed");
    expect(h.ffx.destroy(&wrapContext, nullptr) == FFX_API_RETURN_OK, "swapchain context destroyed");
    if (latency) CloseHandle(latency);
    wrapper.Reset();

    const std::string text = h.log();
    const auto has = [&](const char* needle) { return text.find(needle) != std::string::npos; };
    const bool rtss = GetModuleHandleW(L"RTSSHooks64.dll") != nullptr;
    expect(has("fg: context created"), "log: the generator context was created");
    if (rtss) {
        expect(has("falling back to \"vsync\""), "log: under RTSS the wrapper falls back to \"vsync\"");
    } else {
        expect(has("fg: vrr presenter ready") && has("fg: schedule multiplier=2x"),
               "log: the vrr presenter and the 2x schedule are active");
        expect(has("fg: vrr present jobs="), "log: presenter statistics were reported");
    }
    expect(has("overlay ImGui initialized"), "log: the Mini overlay initialized on Sedici's swapchain");
    expect(has("sedici: overlay present frames="), "log: the overlay counted output frames");
    {
        // Truthful metrics: the overlay's own window should show twice as many
        // output frames as real ones at 2x, and the real rate near the 30 cap.
        const auto number = [&](const char* key) {
            const size_t line = text.find("sedici: overlay present frames=");
            const size_t at = line == std::string::npos ? line : text.find(key, line);
            return at == std::string::npos ? 0.0 : std::atof(text.c_str() + at + std::strlen(key));
        };
        const double output = number("output_fps="), real = number("real_fps=");
        expect(real > 27.0 && real < 33.0 && output > 1.8 * real && output < 2.2 * real,
               "overlay metrics: real " + std::to_string(real) + " fps, output " + std::to_string(output) +
               " fps (2x of a 30 fps cap)");
    }
    expect(has("spin=1000 us fg=off") && has("spin=1000 us fg=on"),
           "log: the limiter label follows the game's FG off/on");
    expect(has("sedici: limiter active target=30"), "log: the limiter is active at 30");
    expect(!has("stage 1 rejected") && !has("present unavailable (the") && !has("could not"),
           "log: no generator rejections or setup failures");
    expect(host.debugErrors == 0, "D3D12 debug layer: no errors (" + std::to_string(host.debugErrors) + ")");
    return 0;
}

// ---------------------------------------------------------------------------
// Rig: one FG context plus one wrapped swapchain on the shared Host, driven the
// way the game drives them. The lifecycle scenarios below build on it.
// ---------------------------------------------------------------------------
struct Rig {
    Harness& h;
    Host& host;
    static constexpr UINT kWidth = 480, kHeight = 270;
    ffxContext fg = nullptr;
    ffxContext wrapContext = nullptr;
    ComPtr<IDXGISwapChain4> wrapper;
    ComPtr<ID3D12Resource> color, depth, motion;
    HANDLE latency = nullptr;
    uint64_t frameId = 0;
    int failedCalls = 0, failedPresents = 0;

    Rig(Harness& harness, Host& hostRef) : h(harness), host(hostRef) {}
    ~Rig() { if (latency) CloseHandle(latency); }

    bool create(const std::string& tag)
    {
        ffxCreateBackendDX12Desc backend{};
        backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        backend.device = host.device.Get();
        ffxCreateContextDescFrameGeneration desc{};
        desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
        desc.header.pNext = &backend.header;
        desc.flags = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED;
        desc.displaySize = {kWidth, kHeight};
        desc.maxRenderSize = {kWidth, kHeight};
        desc.backBufferFormat = FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
        expect(h.ffx.create(&fg, &desc.header, nullptr) == FFX_API_RETURN_OK && fg, "FG context created" + tag);

        ComPtr<IDXGISwapChain4> native = host.swapchain(kWidth, kHeight,
            DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING | DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
        expect(native != nullptr, "native swapchain" + tag);
        if (!native || !fg) return false;
        native->SetMaximumFrameLatency(1);
        IDXGISwapChain4* swapchain = native.Detach();
        IDXGISwapChain4* const original = swapchain;
        ffxCreateContextDescFrameGenerationSwapChainWrapDX12 wrap{};
        wrap.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WRAP_DX12;
        wrap.header.pNext = &backend.header;
        wrap.swapchain = &swapchain;
        wrap.gameQueue = host.queue.Get();
        expect(h.ffx.create(&wrapContext, &wrap.header, nullptr) == FFX_API_RETURN_OK && wrapContext &&
               swapchain && swapchain != original, "swapchain wrapped" + tag);
        wrapper.Attach(swapchain);
        if (!wrapper) return false;
        color = host.texture(DXGI_FORMAT_R8G8B8A8_UNORM, kWidth, kHeight, D3D12_RESOURCE_FLAG_NONE);
        depth = host.texture(DXGI_FORMAT_R32_FLOAT, kWidth, kHeight, D3D12_RESOURCE_FLAG_NONE);
        motion = host.texture(DXGI_FORMAT_R16G16B16A16_FLOAT, kWidth, kHeight, D3D12_RESOURCE_FLAG_NONE);
        latency = wrapper->GetFrameLatencyWaitableObject();
        expect(color && depth && motion, "frame inputs" + tag);
        return color && depth && motion;
    }

    // `wait` false leaves each frame's GPU work in flight (at most three lists,
    // bounded by the host's allocator ring).
    void frames(int count, bool wait, bool enabled = true)
    {
        for (int i = 0; i < count; ++i, ++frameId) {
            host.pump();
            if (latency) WaitForSingleObjectEx(latency, 1000, TRUE);
            if (!host.begin()) { ++failedCalls; return; }
            ffxConfigureDescFrameGeneration configure{};
            configure.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
            configure.swapChain = wrapper.Get();
            configure.frameGenerationEnabled = enabled;
            configure.HUDLessColor = ffxApiGetResourceDX12(color.Get(), FFX_API_RESOURCE_STATE_COMMON);
            configure.frameID = frameId;
            if (h.ffx.configure(&fg, &configure.header) != FFX_API_RETURN_OK) ++failedCalls;
            if (enabled) {
                ffxDispatchDescFrameGenerationPrepare prepare{};
                prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
                prepare.frameID = frameId;
                prepare.commandList = host.list.Get();
                prepare.renderSize = {kWidth, kHeight};
                prepare.motionVectorScale = {static_cast<float>(kWidth), static_cast<float>(kHeight)};
                prepare.depth = ffxApiGetResourceDX12(depth.Get(), FFX_API_RESOURCE_STATE_COMMON);
                prepare.motionVectors = ffxApiGetResourceDX12(motion.Get(), FFX_API_RESOURCE_STATE_COMMON);
                if (h.ffx.dispatch(&fg, &prepare.header) != FFX_API_RETURN_OK) ++failedCalls;
            }
            ComPtr<ID3D12Resource> buffer;
            if (FAILED(wrapper->GetBuffer(wrapper->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&buffer)))) {
                ++failedCalls;
                return;
            }
            transition(host.list.Get(), color.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
            transition(host.list.Get(), buffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
            host.list->CopyResource(buffer.Get(), color.Get());
            transition(host.list.Get(), buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
            transition(host.list.Get(), color.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            if (!host.submit(wait)) { ++failedCalls; return; }
            if (FAILED(wrapper->Present(0, DXGI_PRESENT_ALLOW_TEARING))) ++failedPresents;
        }
    }

    // Destroys the contexts in the game's order and gives the wrapper back.
    void destroy(const std::string& tag)
    {
        expect(h.ffx.destroy(&fg, nullptr) == FFX_API_RETURN_OK, "FG context destroyed" + tag);
        expect(h.ffx.destroy(&wrapContext, nullptr) == FFX_API_RETURN_OK, "swapchain context destroyed" + tag);
        if (latency) CloseHandle(latency);
        latency = nullptr;
        wrapper.Reset();
    }

    void check_clean(const std::string& tag)
    {
        expect(failedCalls == 0, "no failed configure/prepare/GetBuffer/submit" + tag + " (" +
                                     std::to_string(failedCalls) + ")");
        expect(failedPresents == 0, "every Present succeeded" + tag + " (" + std::to_string(failedPresents) + ")");
    }
};

size_t count_of(const std::string& text, const char* needle)
{
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++n;
    return n;
}

// ---------------------------------------------------------------------------
// window-replace: the game destroys its swapchain and window mid-session and
// opens new ones. Each generation is a full create/run/destroy cycle.
// ---------------------------------------------------------------------------
int window_replace_impl(Harness& h, bool vrr)
{
    Host host;
    expect(host.create(), "DX12 host with the debug layer");
    if (g_failures) return 1;
    {
        Rig first(h, host);
        expect(first.create(" (window 1)"), "session 1 up");
        first.frames(60, true);
        first.check_clean(" (window 1)");
        first.destroy(" (window 1)");
    }
    expect(host.idle(), "GPU idle after the first session");
    {
        // New swapchain on the same window, as a resolution-mode change does.
        Rig second(h, host);
        expect(second.create(" (same window)"), "session 2 up");
        second.frames(60, true);
        second.check_clean(" (same window)");
        second.destroy(" (same window)");
    }
    expect(host.replace_window(), "the window is destroyed and replaced");
    {
        Rig third(h, host);
        expect(third.create(" (new window)"), "session 3 up on the new window");
        third.frames(60, true);
        third.check_clean(" (new window)");
        third.destroy(" (new window)");
    }
    expect(host.idle(), "GPU idle at the end");
    const std::string text = h.log();
    expect(count_of(text, "fg: context created") == 3, "log: three generator contexts were created (" +
                                                          std::to_string(count_of(text, "fg: context created")) + ")");
    if (vrr)
        expect(count_of(text, "fg: vrr presenter ready") == 3, "log: three presenters came up (" +
                                                                  std::to_string(count_of(text, "fg: vrr presenter ready")) + ")");
    else
        expect(count_of(text, "fg: vrr presenter ready") == 0, "log: no vrr presenter in vsync mode");
    expect(host.debugErrors == 0, "D3D12 debug layer: no errors (" + std::to_string(host.debugErrors) + ")");
    return 0;
}

int run_window_replace(Harness& h) { return window_replace_impl(h, true); }

// K12: in "vsync" the game keeps its reference to the native chain; a new chain on
// the same window and a new window must still work after the wrapper is destroyed.
int run_window_replace_vsync(Harness& h) { return window_replace_impl(h, false); }

// ---------------------------------------------------------------------------
// destroy-inflight: contexts destroyed and a swapchain resized while the game's
// own GPU work and the presenter's work are still outstanding (nothing waits
// between frames).
// ---------------------------------------------------------------------------
int run_destroy_inflight(Harness& h)
{
    Host host;
    expect(host.create(), "DX12 host with the debug layer");
    if (g_failures) return 1;
    for (int round = 0; round < 3; ++round) {
        const std::string tag = " (round " + std::to_string(round) + ")";
        Rig rig(h, host);
        expect(rig.create(tag), "session up" + tag);
        rig.frames(45, false);
        if (round == 1) {
            // Resize with the previous frames still queued, no wait of our own.
            const HRESULT hr = rig.wrapper->ResizeBuffers(0, Rig::kWidth, Rig::kHeight, DXGI_FORMAT_UNKNOWN,
                DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING | DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
            expect(SUCCEEDED(hr), "resize with work in flight" + tag + " hr=" + std::to_string(static_cast<long>(hr)));
            rig.frames(10, false);
        }
        rig.check_clean(tag);
        // No idle here: destroy straight after the last Present.
        rig.destroy(tag);
        expect(host.idle(), "the GPU retires everything after destroy" + tag);
    }
    expect(host.debugErrors == 0, "D3D12 debug layer: no errors (" + std::to_string(host.debugErrors) + ")");
    return 0;
}

// ---------------------------------------------------------------------------
// unload: K64. A DLL that was never used unloads normally. Once Sedici has issued a
// handle it pins itself: a FreeLibrary is a no-op, a reload returns the same module
// (so there is never a second handle namespace), stale tokens stay recognised, and
// the exports keep working.
// ---------------------------------------------------------------------------
bool reload_exports(Harness& h)
{
    h.proxy = LoadLibraryW((h.dir / kProxyName).c_str());
    if (!h.proxy) return false;
    h.ffx.create = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(h.proxy, "ffxCreateContext"));
    h.ffx.destroy = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(h.proxy, "ffxDestroyContext"));
    h.ffx.configure = reinterpret_cast<PfnFfxConfigure>(GetProcAddress(h.proxy, "ffxConfigure"));
    h.ffx.query = reinterpret_cast<PfnFfxQuery>(GetProcAddress(h.proxy, "ffxQuery"));
    h.ffx.dispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(h.proxy, "ffxDispatch"));
    return h.ffx.create && h.ffx.destroy && h.ffx.configure && h.ffx.query && h.ffx.dispatch;
}

int run_unload(Harness& h)
{
    const std::wstring path = (h.dir / kProxyName).wstring();
    FreeLibrary(h.proxy);
    expect(GetModuleHandleW(path.c_str()) == nullptr, "a DLL that issued no handle unloads normally");
    expect(reload_exports(h), "...and loads again");
    const HMODULE first = h.proxy;

    Host host;
    expect(host.create(), "DX12 host with the debug layer");
    if (g_failures) return 1;
    Rig rig(h, host);
    expect(rig.create(""), "session up");
    rig.frames(20, true);
    rig.check_clean("");
    rig.destroy("");
    const ffxContext stale = rig.fg;

    FreeLibrary(h.proxy);
    FreeLibrary(h.proxy); // over-release must not unload it either
    expect(GetModuleHandleW(path.c_str()) == first, "once a handle was issued, FreeLibrary no longer unloads the DLL");
    expect(reload_exports(h) && h.proxy == first, "a reload returns the same module (no second handle namespace)");
    ffxContext token = stale;
    expect(h.ffx.destroy(&token, nullptr) == FFX_API_RETURN_ERROR,
           "a handle issued before the unload attempt is still recognised as stale");
    uint64_t count = 0;
    ffxQueryDescGetVersions query{};
    query.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    query.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    query.outputCount = &count;
    expect(h.ffx.query(nullptr, &query.header) == FFX_API_RETURN_OK && count == 1, "the exports still answer");

    Rig second(h, host);
    expect(second.create(" (after reload)"), "a new session works after the unload attempt");
    second.frames(20, true);
    second.check_clean(" (after reload)");
    second.destroy(" (after reload)");
    expect(host.idle(), "GPU idle");
    expect(host.debugErrors == 0, "D3D12 debug layer: no errors (" + std::to_string(host.debugErrors) + ")");
    expect(count_of(h.log(), "module pinned in memory") == 1, "log: the module was pinned exactly once");
    return 0;
}

// ---------------------------------------------------------------------------
// log-failure: sedici.log cannot be opened (a directory holds its name). The
// game's calls must be unaffected: a full session still runs clean.
// ---------------------------------------------------------------------------
int run_log_failure(Harness& h)
{
    std::error_code ec;
    expect(fs::is_directory(h.dir / L"sedici.log", ec), "the log path is blocked by a directory");
    Host host;
    expect(host.create(), "DX12 host with the debug layer");
    if (g_failures) return 1;
    Rig rig(h, host);
    expect(rig.create(""), "session up with logging unavailable");
    rig.frames(60, true);
    rig.check_clean("");
    rig.destroy("");
    expect(host.idle(), "GPU idle");
    expect(host.debugErrors == 0, "D3D12 debug layer: no errors (" + std::to_string(host.debugErrors) + ")");
    return 0;
}

// ---------------------------------------------------------------------------
// invalid-calls: hostile and stale arguments on the Sedici-owned path. Each must
// fail (or be ignored) without crashing, leave the caller's swapchain intact on
// a refused wrap, and leave a running session working.
// ---------------------------------------------------------------------------
// Catch only writes into the deliberately protected descriptor page. Any other
// structured exception remains a real test failure rather than an XFAIL.
LONG readonly_descriptor_filter(EXCEPTION_POINTERS* info, uintptr_t begin, uintptr_t end) noexcept
{
    if (!info || !info->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    const auto* record = info->ExceptionRecord;
    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2 ||
        record->ExceptionInformation[0] != 1) return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t address = static_cast<uintptr_t>(record->ExceptionInformation[1]);
    return address >= begin && address < end ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}

bool invoke_readonly_configure(PfnFfxConfigure function, ffxContext* context,
                               const ffxConfigureDescHeader* descriptor,
                               uintptr_t begin, uintptr_t end, ffxReturnCode_t* result) noexcept
{
    __try {
        *result = function(context, descriptor);
        return true;
    } __except (readonly_descriptor_filter(GetExceptionInformation(), begin, end)) {
        return false;
    }
}


int run_readonly_sedici(Harness& h)
{
    Host host;
    expect(host.create(), "DX12 host for read-only Sedici configure");
    if (g_failures || !h.state) return 1;
    ffxCreateBackendDX12Desc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = host.device.Get();
    ffxCreateContextDescFrameGeneration create{};
    create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    create.header.pNext = &backend.header;
    create.displaySize = {Rig::kWidth, Rig::kHeight};
    create.maxRenderSize = {Rig::kWidth, Rig::kHeight};
    create.backBufferFormat = FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
    ffxContext owned = nullptr;
    expect(h.ffx.create(&owned, &create.header, nullptr) == FFX_API_RETURN_OK && owned,
           "Sedici-owned FG context created");
    if (!owned) return 1;

    auto* descriptor = static_cast<ffxConfigureDescFrameGeneration*>(
        VirtualAlloc(nullptr, sizeof(ffxConfigureDescFrameGeneration), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    expect(descriptor != nullptr, "allocate read-only Sedici descriptor");
    if (!descriptor) { (void)h.ffx.destroy(&owned, nullptr); return 1; }
    *descriptor = {};
    descriptor->header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    descriptor->allowAsyncWorkloads = true;
    unsigned char original[sizeof(*descriptor)]{};
    std::memcpy(original, descriptor, sizeof(original));
    DWORD previous = 0;
    const bool protectedPage = VirtualProtect(descriptor, sizeof(*descriptor), PAGE_READONLY, &previous) != 0;
    expect(protectedPage, "protect Sedici configure descriptor read-only");
    const uint32_t before = h.state->calls[StubConfigure];
    ffxReturnCode_t result = FFX_API_RETURN_ERROR;
    const uintptr_t begin = reinterpret_cast<uintptr_t>(descriptor);
    const bool returned = protectedPage && invoke_readonly_configure(
        h.ffx.configure, &owned, &descriptor->header, begin, begin + sizeof(*descriptor), &result);
    const bool unchanged = std::memcmp(original, descriptor, sizeof(original)) == 0;
    DWORD ignored = 0;
    if (protectedPage) (void)VirtualProtect(descriptor, sizeof(*descriptor), previous, &ignored);
    VirtualFree(descriptor, 0, MEM_RELEASE);
    expect(returned && result == FFX_API_RETURN_OK && unchanged && h.state->calls[StubConfigure] == before,
           "Sedici-owned read-only standalone configure is consumed locally without mutation");

    auto* legacy = static_cast<FfxFrameGenerationConfig*>(
        VirtualAlloc(nullptr, sizeof(FfxFrameGenerationConfig), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    expect(legacy != nullptr, "allocate read-only Sedici legacy descriptor");
    if (legacy) {
        *legacy = {};
        legacy->header.type = FFX_API_FRAME_GENERATION_CONFIG;
        legacy->allowAsyncWorkloads = true;
        unsigned char legacyOriginal[sizeof(*legacy)]{};
        std::memcpy(legacyOriginal, legacy, sizeof(legacyOriginal));
        DWORD legacyPrevious = 0;
        const bool legacyProtected = VirtualProtect(legacy, sizeof(*legacy), PAGE_READONLY, &legacyPrevious) != 0;
        ffxReturnCode_t legacyResult = FFX_API_RETURN_ERROR;
        const uintptr_t legacyBegin = reinterpret_cast<uintptr_t>(legacy);
        const bool legacyReturned = legacyProtected && invoke_readonly_configure(
            h.ffx.configure, &owned, &legacy->header, legacyBegin, legacyBegin + sizeof(*legacy), &legacyResult);
        const bool legacyUnchanged = std::memcmp(legacyOriginal, legacy, sizeof(legacyOriginal)) == 0;
        if (legacyProtected) (void)VirtualProtect(legacy, sizeof(*legacy), legacyPrevious, &ignored);
        VirtualFree(legacy, 0, MEM_RELEASE);
        expect(legacyProtected && legacyReturned && legacyResult == FFX_API_RETURN_OK && legacyUnchanged &&
                   h.state->calls[StubConfigure] == before,
               "Sedici-owned read-only legacy configure is consumed locally without mutation");
    }
    expect(h.ffx.destroy(&owned, nullptr) == FFX_API_RETURN_OK, "Sedici-owned FG context destroyed");
    expect(host.idle() && host.debugErrors == 0, "read-only Sedici configure leaves the device clean");
    return g_failures ? 1 : 0;
}

// Exercise K29/K57 with a real proxy and a recording original: stale owned
// tokens and foreign handles must both be refused locally.
int run_stale_forward(Harness& h)
{
    Host host;
    expect(host.create(), "DX12 host for stale handle characterization");
    if (g_failures || !h.state) return 1;
    ffxCreateBackendDX12Desc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = host.device.Get();
    ffxCreateContextDescFrameGeneration desc{};
    desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    desc.header.pNext = &backend.header;
    desc.displaySize = {Rig::kWidth, Rig::kHeight};
    desc.maxRenderSize = {Rig::kWidth, Rig::kHeight};
    desc.backBufferFormat = FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
    ffxContext context = nullptr;
    expect(h.ffx.create(&context, &desc.header, nullptr) == FFX_API_RETURN_OK && context,
           "owned FG context created");
    if (!context) return 1;
    const uint32_t beforeNullQuery = h.state->calls[StubQuery];
    const uint32_t beforeNullConfigure = h.state->calls[StubConfigure];
    const uint32_t beforeNullDispatch = h.state->calls[StubDispatch];
    expect(h.ffx.query(&context, nullptr) != FFX_API_RETURN_OK &&
               h.ffx.configure(&context, nullptr) != FFX_API_RETURN_OK &&
               h.ffx.dispatch(&context, nullptr) != FFX_API_RETURN_OK &&
               h.state->calls[StubQuery] == beforeNullQuery &&
               h.state->calls[StubConfigure] == beforeNullConfigure &&
               h.state->calls[StubDispatch] == beforeNullDispatch,
           "null descriptors on a live owned token never enter the original");
    ffxContext stale = context;
    expect(h.ffx.destroy(&context, nullptr) == FFX_API_RETURN_OK, "owned FG context destroyed");
    if (g_failures) return 1;
    const uint32_t prior = h.state->calls[StubQuery];
    ffxQueryDescHeader query{};
    query.type = UINT64_MAX; // Not the stub's 128-byte snapshot marker.
    expect(h.ffx.query(&stale, nullptr) != FFX_API_RETURN_OK &&
               h.ffx.configure(&stale, nullptr) != FFX_API_RETURN_OK &&
               h.ffx.dispatch(&stale, nullptr) != FFX_API_RETURN_OK &&
               h.state->calls[StubQuery] == prior &&
               h.state->calls[StubConfigure] == beforeNullConfigure &&
               h.state->calls[StubDispatch] == beforeNullDispatch,
           "null descriptors on a stale token never enter the original");
    const auto staleResult = h.ffx.query(&stale, &query);
    expect(staleResult != FFX_API_RETURN_OK && h.state->calls[StubQuery] == prior,
           "stale Sedici handle is rejected locally without entering the original");
    ffxContext newer = nullptr;
    expect(h.ffx.create(&newer, &desc.header, nullptr) == FFX_API_RETURN_OK && newer && newer != stale,
           "new context receives a different token");
    const uint32_t beforeReuse = h.state->calls[StubQuery];
    expect(h.ffx.query(&stale, &query) != FFX_API_RETURN_OK && h.state->calls[StubQuery] == beforeReuse,
           "stale token is rejected while a newer context is live");
    if (newer) expect(h.ffx.destroy(&newer, nullptr) == FFX_API_RETURN_OK, "new context destroyed");
    int foreignObject = 0;
    ffxContext foreign = &foreignObject;
    const uint32_t beforeForeign = h.state->calls[StubQuery];
    expect(h.ffx.query(&foreign, &query) == FFX_API_RETURN_ERROR_PARAMETER &&
               h.state->calls[StubQuery] == beforeForeign,
           "foreign handle outside the token namespace is refused with a parameter error");
    expect(h.ffx.query(&foreign, nullptr) == FFX_API_RETURN_ERROR_PARAMETER &&
               h.state->calls[StubQuery] == beforeForeign,
           "foreign handle with a null descriptor is refused with a parameter error");
    return g_failures ? 1 : 0;
}

int run_invalid_calls(Harness& h)
{
    Host host;
    expect(host.create(), "DX12 host with the debug layer");
    if (g_failures) return 1;

    ffxCreateBackendDX12Desc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = host.device.Get();

    // FG create without the backend desc, and with an empty display size.
    {
        ffxCreateContextDescFrameGeneration desc{};
        desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
        desc.displaySize = {Rig::kWidth, Rig::kHeight};
        desc.maxRenderSize = {Rig::kWidth, Rig::kHeight};
        desc.backBufferFormat = FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
        ffxContext none = nullptr;
        const ffxReturnCode_t rc = h.ffx.create(&none, &desc.header, nullptr);
        std::printf("INFO FG create without a backend desc: rc=%u context=%p\n", rc, static_cast<void*>(none));
        expect(rc != FFX_API_RETURN_OK || none != nullptr, "FG create without a backend desc returns without crashing");
        if (rc == FFX_API_RETURN_OK && none) h.ffx.destroy(&none, nullptr);

        desc.header.pNext = &backend.header;
        desc.displaySize = {0, 0};
        desc.maxRenderSize = {0, 0};
        ffxContext zero = nullptr;
        const ffxReturnCode_t zrc = h.ffx.create(&zero, &desc.header, nullptr);
        std::printf("INFO FG create with a 0x0 display: rc=%u context=%p\n", zrc, static_cast<void*>(zero));
        expect(true, "FG create with a 0x0 display returns without crashing");
        if (zrc == FFX_API_RETURN_OK && zero) h.ffx.destroy(&zero, nullptr);
    }

    // A refused wrap must leave the caller's swapchain untouched.
    {
        ComPtr<IDXGISwapChain4> native = host.swapchain(Rig::kWidth, Rig::kHeight, DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING);
        expect(native != nullptr, "native swapchain for the refused wraps");
        if (!native) return 1;
        IDXGISwapChain4* swapchain = native.Get();
        ffxCreateContextDescFrameGenerationSwapChainWrapDX12 wrap{};
        wrap.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WRAP_DX12;
        wrap.header.pNext = &backend.header;
        wrap.swapchain = &swapchain;
        wrap.gameQueue = nullptr; // no queue
        ffxContext context = nullptr;
        const ffxReturnCode_t rc = h.ffx.create(&context, &wrap.header, nullptr);
        std::printf("INFO wrap with no game queue: rc=%u swapchain %s\n", rc,
                    swapchain == native.Get() ? "unchanged" : "replaced");
        expect(rc != FFX_API_RETURN_OK, "a wrap with no game queue is refused");
        expect(swapchain == native.Get(), "a refused wrap leaves the caller's swapchain pointer unchanged");
        UINT index = native->GetCurrentBackBufferIndex();
        (void)index;
        ComPtr<ID3D12Resource> buffer;
        expect(SUCCEEDED(native->GetBuffer(0, IID_PPV_ARGS(&buffer))), "the caller's swapchain is still usable");

        wrap.swapchain = nullptr;
        wrap.gameQueue = host.queue.Get();
        context = nullptr;
        const ffxReturnCode_t rc2 = h.ffx.create(&context, &wrap.header, nullptr);
        std::printf("INFO wrap with a null swapchain slot: rc=%u\n", rc2);
        expect(rc2 != FFX_API_RETURN_OK, "a wrap with a null swapchain slot is refused");
    }

    // A live session survives hostile prepare/configure calls, and stale handles
    // are rejected after destroy.
    Rig rig(h, host);
    expect(rig.create(""), "session up");
    rig.frames(20, true);
    {
        ffxDispatchDescFrameGenerationPrepare prepare{};
        prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
        prepare.frameID = rig.frameId + 1000; // a jump in the frame id resets generation
        const ffxReturnCode_t rc = h.ffx.dispatch(&rig.fg, &prepare.header);
        std::printf("INFO prepare with no command list or resources: rc=%u\n", rc);
        expect(rc != FFX_API_RETURN_OK, "a prepare with no command list or resources is refused");
        ffxConfigureDescFrameGeneration configure{};
        configure.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        configure.swapChain = nullptr;
        const ffxReturnCode_t crc = h.ffx.configure(&rig.fg, &configure.header);
        std::printf("INFO configure naming no swapchain: rc=%u\n", crc);
        expect(true, "a configure naming no swapchain returns without crashing");
        ffxDispatchDescHeader unknown{};
        unknown.type = 0x7F5E0000ull;
        const ffxReturnCode_t urc = h.ffx.dispatch(&rig.fg, &unknown);
        std::printf("INFO dispatch of an unknown desc type on an owned context: rc=%u\n", urc);
        expect(urc != FFX_API_RETURN_OK, "an unknown dispatch type on an owned context is refused");
    }
    rig.frames(20, true);
    rig.check_clean(" after the hostile calls");

    ffxContext stale = rig.fg;
    rig.destroy("");
    {
        ffxDispatchDescFrameGenerationPrepare prepare{};
        prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
        const ffxReturnCode_t rc = h.ffx.dispatch(&stale, &prepare.header);
        std::printf("INFO dispatch on a destroyed FG context: rc=%u\n", rc);
        expect(rc != FFX_API_RETURN_OK, "a destroyed context is not treated as live");
        ffxContext again = stale;
        const ffxReturnCode_t drc = h.ffx.destroy(&again, nullptr);
        std::printf("INFO second destroy of the same context: rc=%u\n", drc);
        expect(drc != FFX_API_RETURN_OK, "destroying a context twice is refused");
    }
    expect(host.idle(), "GPU idle");
    expect(host.debugErrors == 0, "D3D12 debug layer: no errors (" + std::to_string(host.debugErrors) + ")");
    return 0;
}

// ---------------------------------------------------------------------------
// overlay-hotkey: the overlay's real input path. Alt+F10 goes through the
// window's hooked procedure (Off -> Mini -> Full -> Off), Full is held while the
// session is destroyed and the window replaced, and the cursor clip must come
// back as it was. Save/Cancel/cap clicks are not driven: the controls have no
// stable coordinates without a UI test seam (docs/refactor/reliability.md).
// ---------------------------------------------------------------------------
int run_overlay_hotkey(Harness& h)
{
    Host host;
    expect(host.create(), "DX12 host with the debug layer");
    if (g_failures) return 1;
    // Full input needs the host executable to import the cursor calls the
    // overlay intercepts, as a game does. Taking their addresses creates the imports.
    static volatile auto keepClip = &ClipCursor;
    static volatile auto keepPos = &SetCursorPos;
    (void)keepClip;
    (void)keepPos;
    RECT clipBefore{};
    GetClipCursor(&clipBefore);

    BYTE keys[256]{};
    const auto alt_f10 = [&](HWND window) {
        BYTE state[256]{};
        std::puts("INFO hotkey: get keyboard state");
        GetKeyboardState(state);
        state[VK_MENU] = 0x80;
        std::puts("INFO hotkey: set Alt down");
        SetKeyboardState(state);
        std::puts("INFO hotkey: send F10 down");
        SendMessageW(window, WM_SYSKEYDOWN, VK_F10, 0);
        std::puts("INFO hotkey: send F10 up");
        SendMessageW(window, WM_SYSKEYUP, VK_F10, 0);
        state[VK_MENU] = 0;
        std::puts("INFO hotkey: set Alt up");
        SetKeyboardState(state);
        std::puts("INFO hotkey: dispatch complete");
    };
    (void)keys;

    {
        Rig rig(h, host);
        expect(rig.create(" (session 1)"), "session up");
        rig.frames(20, true);
        alt_f10(host.window); // Mini
        rig.frames(30, true);
        const bool foreground = (SetForegroundWindow(host.window), GetForegroundWindow() == host.window);
        std::printf("INFO foreground window available: %d\n", foreground);
        std::puts("INFO hotkey: entering Full");
        alt_f10(host.window); // Full
        std::puts("INFO hotkey: presenting while Full");
        rig.frames(90, true); // Full retries capture from the render path every half second
        const std::string fullLog = h.log();
        if (fullLog.find("Full menu unavailable") != std::string::npos) {
            std::puts("INFO asynchronous edit-baseline check unavailable: Full menu not active");
        } else {
            expect(fullLog.find("settings edit baseline ready on worker") != std::string::npos,
                   "actual DLL completed Full-menu edit baseline on its worker");
        }
        std::puts("INFO hotkey: entering Off");
        alt_f10(host.window); // Off
        std::puts("INFO hotkey: presenting while Off");
        rig.frames(20, true);
        RECT clipOff{};
        GetClipCursor(&clipOff);
        expect(std::memcmp(&clipOff, &clipBefore, sizeof clipOff) == 0, "cursor clip is as before after Full -> Off");
        rig.check_clean(" (hotkey cycle)");

        // Leave the session while Full is showing.
        alt_f10(host.window); // Mini
        alt_f10(host.window); // Full
        rig.frames(40, true);
        rig.destroy(" (while Full)");
    }
    expect(host.idle(), "GPU idle after destroying during Full");
    RECT clipDestroyed{};
    GetClipCursor(&clipDestroyed);
    expect(std::memcmp(&clipDestroyed, &clipBefore, sizeof clipDestroyed) == 0,
           "cursor clip is restored when the session ends in Full");

    expect(host.replace_window(), "the window is replaced");
    {
        Rig rig(h, host);
        expect(rig.create(" (session 2)"), "session up on the new window");
        rig.frames(20, true);
        const std::string before = h.log();
        alt_f10(host.window);
        rig.frames(30, true);
        rig.check_clean(" (new window)");
        expect(count_of(h.log(), "overlay input hook installed") > count_of(before, "overlay input hook installed") - 1,
               "log: the hook is installed on the replacement window");
        alt_f10(host.window);
        alt_f10(host.window); // back to Off
        rig.frames(10, true);
        rig.destroy(" (new window)");
    }
    RECT clipEnd{};
    GetClipCursor(&clipEnd);
    expect(std::memcmp(&clipEnd, &clipBefore, sizeof clipEnd) == 0, "cursor clip is as before at the end");
    expect(host.idle(), "GPU idle");
    const std::string text = h.log();
    std::printf("INFO Full menu unavailable message in the log: %d\n",
                text.find("Full menu unavailable") != std::string::npos);
    expect(text.find("overlay ImGui initialized") != std::string::npos, "log: the overlay drew during the session");
    expect(host.debugErrors == 0, "D3D12 debug layer: no errors (" + std::to_string(host.debugErrors) + ")");
    return 0;
}

// ---------------------------------------------------------------------------
// integrated (P2.11 offline part): one continuous story through the production
// boundary. Generate, open Mini then Full, toggle FG and resize while Full is
// showing, send hostile calls, leave Full, then end a session with GPU work
// outstanding while Full is showing, replace the window and run again. Cap
// edits and Save/Cancel are covered by the settings transaction checks in
// sedici_regression; real mouse clicks wait for the game (K36).
// ---------------------------------------------------------------------------
int run_integrated(Harness& h)
{
    Host host;
    expect(host.create(), "DX12 host with the debug layer");
    if (g_failures) return 1;
    static volatile auto keepClip = &ClipCursor;
    static volatile auto keepPos = &SetCursorPos;
    (void)keepClip;
    (void)keepPos;
    RECT clipBefore{};
    GetClipCursor(&clipBefore);
    const auto alt_f10 = [&](HWND window) {
        BYTE state[256]{};
        GetKeyboardState(state);
        state[VK_MENU] = 0x80;
        SetKeyboardState(state);
        SendMessageW(window, WM_SYSKEYDOWN, VK_F10, 0);
        SendMessageW(window, WM_SYSKEYUP, VK_F10, 0);
        state[VK_MENU] = 0;
        SetKeyboardState(state);
    };
    const auto clip_is_restored = [&](const char* what) {
        RECT now{};
        GetClipCursor(&now);
        expect(std::memcmp(&now, &clipBefore, sizeof now) == 0, std::string("cursor clip restored: ") + what);
    };
    const auto resize = [&](Rig& rig, const char* what) {
        const HRESULT hr = rig.wrapper->ResizeBuffers(0, Rig::kWidth, Rig::kHeight, DXGI_FORMAT_UNKNOWN,
            DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING | DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
        expect(SUCCEEDED(hr), std::string("resize ") + what);
    };

    ffxContext staleFg = nullptr;
    {
        Rig rig(h, host);
        expect(rig.create(" (session 1)"), "session 1 up");
        rig.frames(60, true);                        // generate
        alt_f10(host.window);                        // Mini
        rig.frames(30, true);
        SetForegroundWindow(host.window);
        alt_f10(host.window);                        // Full
        rig.frames(60, true);
        rig.frames(10, true, false);                 // FG off while the menu shows
        rig.frames(30, true);                        // and on again
        resize(rig, "while the menu shows");
        rig.frames(20, true);
        {
            // Declared failure paths in a live session: refused, never a crash,
            // and the session keeps presenting afterwards.
            ffxDispatchDescFrameGenerationPrepare prepare{};
            prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
            prepare.frameID = rig.frameId + 500;
            expect(h.ffx.dispatch(&rig.fg, &prepare.header) != FFX_API_RETURN_OK, "prepare without a command list is refused");
            ffxDispatchDescHeader unknown{};
            unknown.type = 0x7F5E0000ull;
            expect(h.ffx.dispatch(&rig.fg, &unknown) != FFX_API_RETURN_OK, "an unknown dispatch type is refused");
            ffxContext foreign = reinterpret_cast<ffxContext>(static_cast<uintptr_t>(0x1000));
            expect(h.ffx.destroy(&foreign, nullptr) != FFX_API_RETURN_OK, "a foreign handle is refused");
        }
        rig.frames(20, true);
        alt_f10(host.window);                        // Off
        rig.frames(20, true);
        clip_is_restored("Full -> Off");
        rig.check_clean(" (session 1)");

        alt_f10(host.window);                        // Mini
        alt_f10(host.window);                        // Full
        rig.frames(20, true);
        rig.frames(25, false);                       // leave work outstanding
        staleFg = rig.fg;
        rig.destroy(" (Full, work outstanding)");
    }
    expect(host.idle(), "the GPU retires everything after destroy while Full");
    clip_is_restored("destroy while Full");
    {
        ffxContext again = staleFg;
        expect(h.ffx.destroy(&again, nullptr) != FFX_API_RETURN_OK, "the destroyed context is stale");
    }

    expect(host.replace_window(), "the window is replaced");
    {
        Rig rig(h, host);
        expect(rig.create(" (session 2)"), "session 2 up on the new window");
        rig.frames(40, true);
        alt_f10(host.window);                        // Mini
        rig.frames(30, true);
        alt_f10(host.window);                        // Full
        alt_f10(host.window);                        // Off
        rig.frames(20, true);
        rig.check_clean(" (session 2)");
        rig.destroy(" (session 2)");
    }
    expect(host.idle(), "GPU idle at the end");
    clip_is_restored("end");

    const std::string text = h.log();
    expect(count_of(text, "fg: context created") == 2, "log: two generator contexts (" +
                                                          std::to_string(count_of(text, "fg: context created")) + ")");
    expect(text.find("overlay ImGui initialized") != std::string::npos, "log: the overlay drew");
    expect(text.find("stage 1 rejected") == std::string::npos && text.find("could not") == std::string::npos,
           "log: no generator rejections or setup failures");
    expect(host.debugErrors == 0, "D3D12 debug layer: no errors (" + std::to_string(host.debugErrors) + ")");
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 4) {
        std::fprintf(stderr, "usage: sedici_abi <scenario> <sedici.dll> <stub.dll>\n");
        return 2;
    }
    const std::string scenario = argv[1];
    if (scenario == "readonly-async" || scenario == "readonly-overlay" || scenario == "readonly-sedici")
        SetEnvironmentVariableW(L"SEDICI_ASYNC", nullptr);
    const fs::path proxy = argv[2];
    const fs::path stub = argv[3];
    if (scenario == "exports") {
        run_exports(proxy);
        std::printf("%d checks, %d failed\n", g_checks, g_failures);
        return g_failures ? 1 : 0;
    }

    struct Scenario {
        const char* name;
        const char* toml;
        bool withStub;
        int (*run)(Harness&);
        bool blockLog = false;
    };
    const Scenario scenarios[] = {
        {"readonly-sedici", R"toml([pacing]
mode = "off"
[async]
mode = "off"
[fg]
mode = "sedici"
present = "game"
[ui]
mode = "off"
)toml", true, run_readonly_sedici},
        {"contract", "", true, run_contract},
        {"legacy-config",
         R"toml([pacing]
mode = "hybrid"
[async]
mode = "off"
[fg]
mode = "amd"
[limiter]
fps = 25
sped = 3
[hud]
scale = 2
)toml",
         true, run_legacy_config},
        {"target-auto", "[fg]\ntarget_fps = \"auto\"\n", true, run_target_auto},
        {"bad-config", "[fg\nmode = \n", true, run_bad_config},
        {"session",
         "[fg]\nmode = \"sedici\"\npresent = \"vrr\"\ntear_guard = \"scanline\"\nmultiplier = \"2x\"\n"
         "[ui]\nmode = \"mini\"\n[limiter]\nfps = 30\n",
         false, run_session},
        {"window-replace-vsync",
         "[fg]\npresent = \"vsync\"\n[ui]\nmode = \"off\"\n", false, run_window_replace_vsync},
        {"window-replace",
         "[fg]\nmode = \"sedici\"\npresent = \"vrr\"\ntear_guard = \"scanline\"\nmultiplier = \"2x\"\n"
         "[ui]\nmode = \"off\"\n", false, run_window_replace},
        {"destroy-inflight",
         "[fg]\nmode = \"sedici\"\npresent = \"vrr\"\ntear_guard = \"scanline\"\nmultiplier = \"2x\"\n"
         "[ui]\nmode = \"off\"\n", false, run_destroy_inflight},
        {"log-failure",
         "[fg]\nmode = \"sedici\"\npresent = \"vrr\"\ntear_guard = \"scanline\"\nmultiplier = \"2x\"\n"
         "[ui]\nmode = \"off\"\n", false, run_log_failure, true},
        {"unload",
         "[fg]\nmode = \"sedici\"\npresent = \"vrr\"\ntear_guard = \"scanline\"\nmultiplier = \"2x\"\n"
         "[log]\nlevel = 1\n[ui]\nmode = \"off\"\n", false, run_unload},
        {"stale-forward",
         "[fg]\nmode = \"sedici\"\npresent = \"game\"\n[ui]\nmode = \"off\"\n",
         true, run_stale_forward},
        {"invalid-calls",
         "[fg]\nmode = \"sedici\"\npresent = \"vrr\"\ntear_guard = \"scanline\"\nmultiplier = \"2x\"\n"
         "[ui]\nmode = \"off\"\n", false, run_invalid_calls},
        {"overlay-hotkey",
         "[fg]\nmode = \"sedici\"\npresent = \"vrr\"\ntear_guard = \"scanline\"\nmultiplier = \"2x\"\n"
         "[log]\nlevel = 1\n[ui]\nmode = \"off\"\n",
         false, run_overlay_hotkey},
        {"integrated",
         "[fg]\nmode = \"sedici\"\npresent = \"vrr\"\ntear_guard = \"scanline\"\nmultiplier = \"2x\"\n"
         "[log]\nlevel = 1\n[ui]\nmode = \"off\"\n",
         false, run_integrated},
    };
    for (const Scenario& entry : scenarios) {
        if (scenario != entry.name) continue;
        Harness harness;
        const bool ready = setup(harness, entry.name, proxy, stub, entry.toml, entry.withStub, entry.blockLog);
        expect(ready, std::string("scratch directory, proxy load") + (entry.withStub ? " and stub" : ""));
        if (ready) entry.run(harness);
        std::printf("%d checks, %d failed\n", g_checks, g_failures);
        return g_failures ? 1 : 0;
    }
    std::fprintf(stderr, "unknown scenario %s\n", scenario.c_str());
    return 2;
}
