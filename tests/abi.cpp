// Export-level harness: loads the *built* Sedici DLL through its five exports,
// next to a stub "original" (tests/abi_stub.cpp), each scenario in its own
// process and scratch directory with its own sedici.toml.
//
//   sedici_abi <scenario> <sedici.dll> <stub.dll>
//
// Scenarios: exports, forward, pacing, async-off, missing-orig, bad-config,
// session. See docs/refactor/reliability.md, "Current coverage and gaps".
// Never touches game files and never downloads anything.

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

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
           const std::string& toml, bool withStub)
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
// forward: fg.mode = "amd", pacing off, async as the game asks. Every argument,
// pointer and return code reaches the original unchanged, unknown descs too,
// and the caller's desc is byte-identical afterwards.
// ---------------------------------------------------------------------------
int run_forward(Harness& h)
{
    StubState& stub = *h.state;
    ffxAllocationCallbacks callbacks{};
    for (const ffxReturnCode_t scripted : {FFX_API_RETURN_OK, FFX_API_RETURN_ERROR_PARAMETER}) {
        for (auto& code : stub.script) code = scripted;
        const std::string tag = scripted == FFX_API_RETURN_OK ? " (OK)" : " (ERROR_PARAMETER)";

        StubProbe probe = make_probe(1);
        const StubProbe original = probe;
        ffxContext context = nullptr;
        ffxReturnCode_t result = h.ffx.create(&context, &probe.header, &callbacks);
        const StubCall& create = stub.last[StubCreate];
        expect(result == scripted && create.contextArg == &context && create.desc == &probe &&
               create.callbacks == &callbacks && same_bytes(create.probe, &original, sizeof(original)),
               "ffxCreateContext forwards context, desc, callbacks and result" + tag);
        expect(same_bytes(&probe, &original, sizeof(probe)), "ffxCreateContext leaves the caller's desc intact" + tag);
        if (scripted != FFX_API_RETURN_OK) context = reinterpret_cast<ffxContext>(0x4242);

        probe = make_probe(2);
        const StubProbe configureOriginal = probe;
        result = h.ffx.configure(&context, &probe.header);
        const StubCall& configure = stub.last[StubConfigure];
        expect(result == scripted && configure.contextArg == &context && configure.contextValue == context &&
               configure.desc == &probe && same_bytes(configure.probe, &configureOriginal, sizeof(probe)) &&
               same_bytes(&probe, &configureOriginal, sizeof(probe)),
               "ffxConfigure forwards an unknown desc unchanged" + tag);

        probe = make_probe(3);
        const StubProbe queryOriginal = probe;
        result = h.ffx.query(&context, &probe.header);
        const StubCall& query = stub.last[StubQuery];
        expect(result == scripted && query.contextValue == context && query.desc == &probe &&
               same_bytes(query.probe, &queryOriginal, sizeof(probe)),
               "ffxQuery forwards an unknown desc unchanged" + tag);

        probe = make_probe(4);
        const StubProbe dispatchOriginal = probe;
        result = h.ffx.dispatch(&context, &probe.header);
        const StubCall& dispatch = stub.last[StubDispatch];
        expect(result == scripted && dispatch.contextValue == context && dispatch.desc == &probe &&
               same_bytes(dispatch.probe, &dispatchOriginal, sizeof(probe)),
               "ffxDispatch forwards an unknown desc unchanged" + tag);

        result = h.ffx.destroy(&context, &callbacks);
        const StubCall& destroy = stub.last[StubDestroy];
        expect(result == scripted && destroy.contextArg == &context && destroy.callbacks == &callbacks,
               "ffxDestroyContext forwards context, callbacks and result" + tag);
    }
    const std::string text = h.log();
    expect(text.find("fg.mode") != std::string::npos && text.find("\"amd\"") != std::string::npos,
           "the log reports fg.mode = \"amd\" from the scenario's own sedici.toml");
    return 0;
}

// ---------------------------------------------------------------------------
// Minimal DX12 host shared by the scenarios that need a real swapchain.
// ---------------------------------------------------------------------------
struct Host {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
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
            FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                             IID_PPV_ARGS(&list))) ||
            FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
        list->Close();
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"SediciAbiHost";
        RegisterClassW(&wc);
        window = CreateWindowW(wc.lpszClassName, L"Sedici export harness", WS_OVERLAPPEDWINDOW,
                               CW_USEDEFAULT, CW_USEDEFAULT, 500, 320, nullptr, nullptr, wc.hInstance, nullptr);
        if (window) ShowWindow(window, SW_SHOWNOACTIVATE);
        return event && window;
    }

    bool begin()
    {
        return SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator.Get(), nullptr));
    }

    bool submit()
    {
        if (FAILED(list->Close())) return false;
        ID3D12CommandList* lists[]{list.Get()};
        queue->ExecuteCommandLists(1, lists);
        if (FAILED(queue->Signal(fence.Get(), ++fenceValue))) return false;
        if (fence->GetCompletedValue() >= fenceValue) return true;
        fence->SetEventOnCompletion(fenceValue, event);
        return WaitForSingleObject(event, 5000) == WAIT_OBJECT_0;
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

// ---------------------------------------------------------------------------
// pacing: fg.mode = "amd", default pacing. After a frame generation configure
// that names a swapchain, Sedici sends the hybrid FramePacingTuning to the
// original, and the caller's configure desc is restored.
// ---------------------------------------------------------------------------
int run_pacing(Harness& h)
{
    Host host;
    expect(host.create(), "DX12 host for the pacing scenario");
    if (g_failures) return 1;
    ComPtr<IDXGISwapChain4> native = host.swapchain(320, 180, 0);
    expect(native != nullptr, "native swapchain");
    if (!native) return 1;

    ffxCreateBackendDX12Desc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = host.device.Get();
    ffxCreateContextDescFrameGeneration create{};
    create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    create.header.pNext = &backend.header;
    create.flags = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED;
    create.displaySize = {320, 180};
    create.maxRenderSize = {320, 180};
    create.backBufferFormat = FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
    ffxContext fg = nullptr;
    expect(h.ffx.create(&fg, &create.header, nullptr) == FFX_API_RETURN_OK && fg, "frame generation create forwarded");

    IDXGISwapChain4* swapchain = native.Get();
    ffxCreateContextDescFrameGenerationSwapChainWrapDX12 wrap{};
    wrap.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WRAP_DX12;
    wrap.swapchain = &swapchain;
    wrap.gameQueue = host.queue.Get();
    ffxContext wrapContext = nullptr;
    expect(h.ffx.create(&wrapContext, &wrap.header, nullptr) == FFX_API_RETURN_OK && wrapContext,
           "swapchain wrap create forwarded");

    ffxConfigureDescFrameGeneration configure{};
    configure.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    configure.swapChain = swapchain;
    configure.frameGenerationEnabled = true;
    const ffxConfigureDescFrameGeneration before = configure;
    const uint32_t sendsBefore = h.state->pacingSends;
    expect(h.ffx.configure(&fg, &configure.header) == FFX_API_RETURN_OK, "frame generation configure forwarded");
    const FfxApiSwapchainFramePacingTuning& tuning = h.state->tuning;
    expect(h.state->pacingSends == sendsBefore + 1, "one FramePacingTuning send after the configure");
    expect(tuning.allowHybridSpin && tuning.hybridSpinTime == 2 && tuning.safetyMarginInMs == 0.1f &&
           tuning.varianceFactor == 0.1f && !tuning.allowWaitForSingleObjectOnFence,
           "the tuning carries the shipped hybrid values (0.1 ms, 0.1, spin 2, no fence wait)");
    expect(same_bytes(&configure, &before, sizeof(configure)), "the caller's configure desc is restored");

    expect(h.ffx.destroy(&wrapContext, nullptr) == FFX_API_RETURN_OK, "swapchain context destroy forwarded");
    expect(h.ffx.destroy(&fg, nullptr) == FFX_API_RETURN_OK, "frame generation context destroy forwarded");
    return 0;
}

// ---------------------------------------------------------------------------
// async-off: the original sees allowAsyncWorkloads = false in configure and
// the game's create flags unchanged; the caller's descs are restored.
// ---------------------------------------------------------------------------
int run_async_off(Harness& h)
{
    ffxCreateContextDescFrameGeneration create{};
    create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    create.flags = FFX_FRAMEGENERATION_ENABLE_ASYNC_WORKLOAD_SUPPORT | FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED;
    create.displaySize = {320, 180};
    create.maxRenderSize = {320, 180};
    const ffxCreateContextDescFrameGeneration createBefore = create;
    ffxContext fg = nullptr;
    expect(h.ffx.create(&fg, &create.header, nullptr) == FFX_API_RETURN_OK, "create forwarded");
    // README: "off" leaves the create flags as the game set them (since
    // 6c2c180); only the per-frame configure value changes.
    expect(h.state->sawFgCreate &&
           h.state->fgCreateFlags == (FFX_FRAMEGENERATION_ENABLE_ASYNC_WORKLOAD_SUPPORT |
                                      FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED),
           "the original sees the game's create flags unchanged");
    expect(same_bytes(&create, &createBefore, sizeof(create)), "the caller's create desc is restored");

    ffxConfigureDescFrameGeneration configure{};
    configure.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    configure.allowAsyncWorkloads = true;
    configure.frameGenerationEnabled = true;
    const ffxConfigureDescFrameGeneration configureBefore = configure;
    expect(h.ffx.configure(&fg, &configure.header) == FFX_API_RETURN_OK, "configure forwarded");
    expect(h.state->sawFgConfigure && !h.state->fgConfigureAllowAsync,
           "the original sees allowAsyncWorkloads = false");
    expect(same_bytes(&configure, &configureBefore, sizeof(configure)), "the caller's configure desc is restored");
    expect(h.ffx.destroy(&fg, nullptr) == FFX_API_RETURN_OK, "destroy forwarded");
    return 0;
}

// ---------------------------------------------------------------------------
// missing-orig: without the original beside it every export fails cleanly.
// ---------------------------------------------------------------------------
int run_missing_orig(Harness& h)
{
    StubProbe probe = make_probe(9);
    ffxContext context = nullptr;
    expect(h.ffx.create(&context, &probe.header, nullptr) != FFX_API_RETURN_OK && !context,
           "ffxCreateContext fails without the original and leaves the context null");
    expect(h.ffx.configure(&context, &probe.header) != FFX_API_RETURN_OK, "ffxConfigure fails without the original");
    expect(h.ffx.query(&context, &probe.header) != FFX_API_RETURN_OK, "ffxQuery fails without the original");
    expect(h.ffx.dispatch(&context, &probe.header) != FFX_API_RETURN_OK, "ffxDispatch fails without the original");
    expect(h.ffx.destroy(&context, nullptr) != FFX_API_RETURN_OK, "ffxDestroyContext fails without the original");
    expect(h.log().find("original DLL not found") != std::string::npos, "the log names the missing original");
    return 0;
}

// ---------------------------------------------------------------------------
// bad-config: a malformed sedici.toml is reported and forwarding still works.
// ---------------------------------------------------------------------------
int run_bad_config(Harness& h)
{
    StubProbe probe = make_probe(5);
    ffxContext context = nullptr;
    expect(h.ffx.create(&context, &probe.header, nullptr) == FFX_API_RETURN_OK && context,
           "forwarding works with a malformed sedici.toml");
    expect(h.ffx.destroy(&context, nullptr) == FFX_API_RETURN_OK, "destroy forwarded");
    expect(h.log().find("could not be used; using built-in defaults") != std::string::npos,
           "the log reports the unusable sedici.toml and the defaults");
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

} // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 4) {
        std::fprintf(stderr, "usage: sedici_abi <scenario> <sedici.dll> <stub.dll>\n");
        return 2;
    }
    const std::string scenario = argv[1];
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
    };
    const Scenario scenarios[] = {
        {"forward", "[pacing]\nmode = \"off\"\n[async]\nmode = \"game\"\n[fg]\nmode = \"amd\"\n", true, run_forward},
        {"pacing", "[fg]\nmode = \"amd\"\n", true, run_pacing},
        {"async-off", "[pacing]\nmode = \"off\"\n[async]\nmode = \"off\"\n[fg]\nmode = \"amd\"\n", true, run_async_off},
        {"missing-orig", "[fg]\nmode = \"amd\"\n", false, run_missing_orig},
        {"bad-config", "[fg\nmode = \n", true, run_bad_config},
        {"session",
         "[fg]\nmode = \"sedici\"\npresent = \"vrr\"\ntear_guard = \"scanline\"\nmultiplier = \"2x\"\n"
         "[ui]\nmode = \"mini\"\n[limiter]\nfps = 30\n",
         false, run_session},
    };
    for (const Scenario& entry : scenarios) {
        if (scenario != entry.name) continue;
        Harness harness;
        const bool ready = setup(harness, entry.name, proxy, stub, entry.toml, entry.withStub);
        expect(ready, std::string("scratch directory, proxy load") + (entry.withStub ? " and stub" : ""));
        if (ready) entry.run(harness);
        std::printf("%d checks, %d failed\n", g_checks, g_failures);
        return g_failures ? 1 : 0;
    }
    std::fprintf(stderr, "unknown scenario %s\n", scenario.c_str());
    return 2;
}
