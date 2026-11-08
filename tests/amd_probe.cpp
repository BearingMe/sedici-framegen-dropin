// P2.3 probe: drives the OFFICIAL AMD frame-generation binary through its five
// exports, in isolation from Sedici, to establish facts a stub cannot:
// private loading under another filename, dependency resolution, coexistence of
// two instances, provider/version queries, and a real FG + swapchain session
// (create order, callbacks, destruction order).
//
//   sedici_amd_probe <amd_fg.dll[.orig]> [--sha256 <hex>] [--variant N]
//
// The binary is supplied by the operator (a copy of the game's `.orig`); this
// tool never downloads anything, never touches the game folder and never
// launches the game. Each variant runs in its own child process so that a crash
// is a result, not the end of the probe. Output lines: PASS/FAIL (contract the
// refactor needs), INFO (observation to record in docs/refactor/amd-binary.md).

#include <windows.h>
#include <bcrypt.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "ffx_api_dx12.h"
#include "ffx_api_framegeneration_dx12.h"
#include "ffx_framegeneration.h"
#include "config.h"
#include "live.h"
#include "overlay.h"
#include "ui.h"

extern "C" const char g_sedici_proxy_marker[] = "SEDICI_PROXY_V1";

#pragma warning(disable : 4996) // legacy prepare desc, as Cyberpunk sends it

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace {

int g_fail = 0;

void pass_fail(bool ok, const std::string& what)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++g_fail;
    std::fflush(stdout);
}

void info(const std::string& what)
{
    std::printf("INFO %s\n", what.c_str());
    std::fflush(stdout);
}

std::string fmt(const char* f, ...)
{
    char buf[512];
    va_list a;
    va_start(a, f);
    std::vsnprintf(buf, sizeof buf, f, a);
    va_end(a);
    return buf;
}

struct Api {
    HMODULE module = nullptr;
    PfnFfxCreateContext create = nullptr;
    PfnFfxDestroyContext destroy = nullptr;
    PfnFfxConfigure configure = nullptr;
    PfnFfxQuery query = nullptr;
    PfnFfxDispatch dispatch = nullptr;
};

Api load(const fs::path& path)
{
    Api api;
    api.module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!api.module) return api;
    api.create = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(api.module, "ffxCreateContext"));
    api.destroy = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(api.module, "ffxDestroyContext"));
    api.configure = reinterpret_cast<PfnFfxConfigure>(GetProcAddress(api.module, "ffxConfigure"));
    api.query = reinterpret_cast<PfnFfxQuery>(GetProcAddress(api.module, "ffxQuery"));
    api.dispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(api.module, "ffxDispatch"));
    return api;
}

bool complete(const Api& a) { return a.create && a.destroy && a.configure && a.query && a.dispatch; }

std::string sha256_of(const fs::path& path)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return {};
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string out;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<unsigned char> buf(1 << 20);
        size_t n;
        while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0)
            BCryptHashData(hash, buf.data(), static_cast<ULONG>(n), 0);
        unsigned char digest[32]{};
        if (BCryptFinishHash(hash, digest, sizeof digest, 0) == 0)
            for (unsigned char b : digest) out += fmt("%02X", b);
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    std::fclose(f);
    return out;
}

// Number of named exports, read from the mapped image.
int export_count(HMODULE m)
{
    auto* base = reinterpret_cast<unsigned char*>(m);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress) return 0;
    return static_cast<int>(reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress)->NumberOfNames);
}

fs::path scratch_dir()
{
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    fs::path dir = fs::path(exe).parent_path() / "amd_probe" /
                   fmt("run-%lld-%lu", static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(now).count()),
                       GetCurrentProcessId());
    fs::create_directories(dir);
    return dir;
}

ComPtr<ID3D12Device> make_device()
{
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    ComPtr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) return nullptr;
    return device;
}

uint64_t debug_errors(ID3D12Device* device, bool& available)
{
    ComPtr<ID3D12InfoQueue> queue;
    available = device && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&queue)));
    if (!available) return 0;
    uint64_t errors = 0;
    const UINT64 count = queue->GetNumStoredMessages();
    for (UINT64 index = 0; index < count; ++index) {
        SIZE_T bytes = 0;
        if (FAILED(queue->GetMessage(index, nullptr, &bytes)) || !bytes) continue;
        std::vector<char> buffer(bytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
        if (FAILED(queue->GetMessage(index, message, &bytes))) continue;
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
            ++errors;
            if (errors <= 5) info(std::string("D3D12 validation error: ") + message->pDescription);
        }
    }
    return errors;
}

// ---- stage 1: files, loading, coexistence, queries -------------------------

void stage_load(const fs::path& source)
{
    const fs::path dir = scratch_dir();
    const fs::path a = dir / "sedici_amd_a.dll";
    const fs::path b = dir / "sedici_amd_b.dll";
    const fs::path orig = dir / "amd_fidelityfx_framegeneration_dx12.dll.orig";
    std::error_code ec;
    fs::copy_file(source, a, ec);
    fs::copy_file(source, b, ec);
    fs::copy_file(source, orig, ec);
    pass_fail(fs::exists(a) && fs::exists(b) && fs::exists(orig), "scratch copies made (never the game folder)");

    Api ra = load(a), rb = load(b), ro = load(orig);
    pass_fail(ra.module && complete(ra), "private load under a renamed file name resolves the five exports");
    pass_fail(rb.module && complete(rb), "a second copy under another name loads");
    pass_fail(ro.module && complete(ro), "load under the '.orig' name, as the proxy does today");
    if (ra.module && rb.module) {
        pass_fail(ra.module != rb.module, "two copies are two independent module instances");
        pass_fail(ra.create != rb.create, "two instances have distinct function addresses (separate globals)");
    }
    if (ra.module) {
        pass_fail(export_count(ra.module) == 5, fmt("exactly five named exports (%d)", export_count(ra.module)));
        pass_fail(GetProcAddress(ra.module, "ffxProbe") == nullptr, "no extra helper export resolves");
        const char* names[] = {"dxgi.dll", "d3d12.dll", "winmm.dll", "version.dll", "vcruntime140.dll", "msvcp140.dll"};
        std::string loaded;
        for (const char* n : names)
            if (GetModuleHandleA(n)) loaded += std::string(n) + " ";
        info("dependencies mapped into the process: " + loaded);
    }
    HMODULE again = LoadLibraryExW(a.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    pass_fail(again == ra.module, "loading the same path twice returns the same instance");
    if (again) FreeLibrary(again);

    ComPtr<ID3D12Device> device = make_device();
    pass_fail(device != nullptr, "D3D12 device for the probe");
    if (ra.query && device) {
        struct Q {
            const char* name;
            uint64_t type;
        };
        const Q queries[] = {
            {"FrameGeneration", FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION},
            {"SwapChainWrapDX12", FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WRAP_DX12},
        };
        for (const Q& q : queries) {
            uint64_t count = 0;
            ffxQueryDescGetVersions desc{};
            desc.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
            desc.createDescType = q.type;
            desc.device = device.Get();
            desc.outputCount = &count;
            const ffxReturnCode_t rc = ra.query(nullptr, &desc.header);
            info(fmt("GetVersions(%s): rc=%u count=%llu", q.name, rc, static_cast<unsigned long long>(count)));
            if (rc == FFX_API_RETURN_OK && count > 0 && count < 16) {
                uint64_t cap = count, ids[16]{};
                const char* vnames[16]{};
                desc.outputCount = &cap;
                desc.versionIds = ids;
                desc.versionNames = vnames;
                if (ra.query(nullptr, &desc.header) == FFX_API_RETURN_OK)
                    for (uint64_t i = 0; i < cap; ++i)
                        info(fmt("  version %llu id=0x%llx name=%s", static_cast<unsigned long long>(i),
                                 static_cast<unsigned long long>(ids[i]), vnames[i] ? vnames[i] : "(null)"));
            }
        }
    }
    if (rb.module) {
        FreeLibrary(rb.module);
        pass_fail(complete(ra) && GetModuleHandleW(a.c_str()) != nullptr, "freeing one instance leaves the other loaded");
    }
    info("scratch directory: " + dir.generic_string());
}

// ---- stage 2: a real FG + swapchain session --------------------------------

struct Session {
    Api api;
    ffxContext fg = nullptr;
    ffxContext sc = nullptr;
    volatile LONG generationCalls = 0;
    volatile LONG presentCalls = 0;
    volatile LONG presentGenerated = 0;
    volatile LONG generationErrors = 0;
    ffxReturnCode_t lastGenerationRc = FFX_API_RETURN_OK;
    LARGE_INTEGER freq{};
    std::vector<double> presentTimes;
};

Session* g_session = nullptr;

D3D12_RESOURCE_STATES to_d3d(uint32_t s)
{
    switch (s) {
    case FFX_API_RESOURCE_STATE_COMMON: return D3D12_RESOURCE_STATE_COMMON;
    case FFX_API_RESOURCE_STATE_UNORDERED_ACCESS: return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    case FFX_API_RESOURCE_STATE_COMPUTE_READ: return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    case FFX_API_RESOURCE_STATE_PIXEL_READ: return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    case FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ:
        return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    case FFX_API_RESOURCE_STATE_COPY_SRC: return D3D12_RESOURCE_STATE_COPY_SOURCE;
    case FFX_API_RESOURCE_STATE_COPY_DEST: return D3D12_RESOURCE_STATE_COPY_DEST;
    case FFX_API_RESOURCE_STATE_GENERIC_READ: return D3D12_RESOURCE_STATE_GENERIC_READ;
    case FFX_API_RESOURCE_STATE_PRESENT: return D3D12_RESOURCE_STATE_PRESENT;
    case FFX_API_RESOURCE_STATE_RENDER_TARGET: return D3D12_RESOURCE_STATE_RENDER_TARGET;
    default: return D3D12_RESOURCE_STATE_COMMON;
    }
}

void barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    if (from == to) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &b);
}

double now_ms(const Session& s)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return 1000.0 * static_cast<double>(t.QuadPart) / static_cast<double>(s.freq.QuadPart);
}

// AMD calls this on its own thread to have the generator run; the descriptor is
// handed straight back to the FG context, as the game does.
ffxReturnCode_t generation_callback(ffxDispatchDescFrameGeneration* params, void*)
{
    Session* s = g_session;
    InterlockedIncrement(&s->generationCalls);
    if (!s->fg) return FFX_API_RETURN_ERROR_PARAMETER; // FG context is gone (variant 16)
    const ffxReturnCode_t rc = s->api.dispatch(&s->fg, &params->header);
    s->lastGenerationRc = rc;
    if (rc != FFX_API_RETURN_OK) InterlockedIncrement(&s->generationErrors);
    return rc;
}

// Present callback variant: do what AMD's default does (copy), and count.
ffxReturnCode_t present_callback(ffxCallbackDescFrameGenerationPresent* p, void*)
{
    Session* s = g_session;
    InterlockedIncrement(&s->presentCalls);
    if (p->isGeneratedFrame) InterlockedIncrement(&s->presentGenerated);
    s->presentTimes.push_back(now_ms(*s));
    auto* list = static_cast<ID3D12GraphicsCommandList*>(p->commandList);
    auto* src = static_cast<ID3D12Resource*>(p->currentBackBuffer.resource);
    auto* dst = static_cast<ID3D12Resource*>(p->outputSwapChainBuffer.resource);
    if (!list || !src || !dst) return FFX_API_RETURN_ERROR_PARAMETER;
    const auto srcState = to_d3d(p->currentBackBuffer.state);
    const auto dstState = to_d3d(p->outputSwapChainBuffer.state);
    barrier(list, src, srcState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    barrier(list, dst, dstState, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyResource(dst, src);
    barrier(list, src, D3D12_RESOURCE_STATE_COPY_SOURCE, srcState);
    barrier(list, dst, D3D12_RESOURCE_STATE_COPY_DEST, dstState);
    return FFX_API_RETURN_OK;
}

ComPtr<ID3D12Resource> texture(ID3D12Device* d, UINT w, UINT h, DXGI_FORMAT f, D3D12_RESOURCE_FLAGS flags,
                               D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w;
    rd.Height = h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = f;
    rd.SampleDesc.Count = 1;
    rd.Flags = flags;
    ComPtr<ID3D12Resource> r;
    d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&r));
    return r;
}

LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

HWND make_window(UINT w, UINT h)
{
    WNDCLASSW wc{};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SediciAmdProbe";
    RegisterClassW(&wc);
    RECT r{0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    return CreateWindowW(wc.lpszClassName, L"probe", WS_OVERLAPPEDWINDOW, 40, 40, r.right - r.left, r.bottom - r.top,
                         nullptr, nullptr, wc.hInstance, nullptr);
}

// variant bit 0: install a present callback (else AMD's default composition)
// variant bit 1: destroy the FG context before the swapchain context
// variant bit 2: no frame-generation callback (AMD must cope on its own)
// variant bit 8: protect the configure descriptor after the call returns
// variant bit 9: use the legacy FG configure descriptor
// variant bit 10: install Sedici's production Mini overlay callback
int run_session(const fs::path& amdPath, int variant)
{
    const bool presentCb = (variant & 1) != 0;
    const bool fgFirst = (variant & 2) != 0;
    const bool noGenCb = (variant & 4) != 0;
    const bool productionOverlay = (variant & 1024) != 0;
    info(fmt("session variant %d: presentCallback=%d productionOverlay=%d destroyFgFirst=%d generationCallback=%d",
             variant, presentCb, productionOverlay, fgFirst, !noGenCb));

    Session s;
    g_session = &s;
    QueryPerformanceFrequency(&s.freq);
    s.api = load(amdPath);
    if (!s.api.module || !complete(s.api)) {
        pass_fail(false, "load for session");
        return 1;
    }

    constexpr UINT W = 640, H = 360, kBuffers = 3;
    ComPtr<ID3D12Device> device = make_device();
    if (!device) {
        pass_fail(false, "device");
        return 1;
    }
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
    ComPtr<IDXGIFactory4> factory;
    CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    HWND hwnd = make_window(W, H);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    if (productionOverlay) {
        // Mini requires no foreground cursor capture. Use the same production
        // callback and renderer as the proxy, not a separate demo overlay.
        (void)sedici::settings();
        sedici::live_set_ui_mode(sedici::UiMode::Mini);
        sedici::ui_capture_window(hwnd);
    }

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = W;
    sd.Height = H;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = kBuffers;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    ComPtr<IDXGISwapChain1> sc1;
    if (FAILED(factory->CreateSwapChainForHwnd(queue.Get(), hwnd, &sd, nullptr, nullptr, &sc1))) {
        pass_fail(false, "native swapchain");
        return 1;
    }
    ComPtr<IDXGISwapChain4> chain;
    sc1.As(&chain);
    sc1.Reset();

    ffxCreateBackendDX12Desc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = device.Get();

    // The game's order: swapchain wrap first, FG context second.
    ffxCreateContextDescFrameGenerationSwapChainWrapDX12 wrap{};
    wrap.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WRAP_DX12;
    IDXGISwapChain4* wrapped = chain.Get();
    wrap.swapchain = &wrapped;
    wrap.gameQueue = queue.Get();
    ffxCreateContextDescFrameGenerationSwapChainVersionDX12 scVersion{};
    scVersion.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12;
    scVersion.version = FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION;
    // The chain shape the game uses is not recorded, so try the plausible ones.
    ffxReturnCode_t rc = FFX_API_RETURN_ERROR_PARAMETER;
    for (int shape = 0; shape < 4 && rc != FFX_API_RETURN_OK; ++shape) {
        scVersion.header.pNext = nullptr;
        wrap.header.pNext = nullptr;
        if (shape == 0) {
        } else if (shape == 1) {
            wrap.header.pNext = &scVersion.header;
        } else if (shape == 2) {
            wrap.header.pNext = &backend.header;
        } else {
            wrap.header.pNext = &scVersion.header;
            scVersion.header.pNext = &backend.header;
        }
        wrapped = chain.Get();
        s.sc = nullptr;
        rc = s.api.create(&s.sc, &wrap.header, nullptr);
        info(fmt("create swapchain context, chain shape %d (%s%s%s): rc=%u", shape, shape == 0 ? "bare" : "",
                 (shape == 1 || shape == 3) ? "+version " : "", (shape == 2 || shape == 3) ? "+backend" : "", rc));
    }
    info(fmt("wrapped=%p native=%p", static_cast<void*>(wrapped), static_cast<void*>(chain.Get())));
    pass_fail(rc == FFX_API_RETURN_OK && s.sc, "swapchain wrap context created");
    if (rc != FFX_API_RETURN_OK) return 1;
    ComPtr<IDXGISwapChain4> amdChain;
    amdChain.Attach(wrapped); // the create call hands over a reference
    // Hypothesis under test: the wrap consumes the caller's native reference.
    if (!std::getenv("PROBE_KEEP_NATIVE_REF")) chain.Detach();
    info("native chain reference given up to AMD (detached); the wrapped pointer may reuse its address");

    ffxCreateContextDescFrameGeneration fgDesc{};
    fgDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    fgDesc.flags = FFX_FRAMEGENERATION_ENABLE_ASYNC_WORKLOAD_SUPPORT | FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED;
    fgDesc.displaySize = {W, H};
    fgDesc.maxRenderSize = {W, H};
    fgDesc.backBufferFormat = FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
    ffxCreateContextDescFrameGenerationVersion fgVersion{};
    fgVersion.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
    fgVersion.version = FFX_FRAMEGENERATION_VERSION;
    fgDesc.header.pNext = &fgVersion.header;
    fgVersion.header.pNext = &backend.header;
    rc = s.api.create(&s.fg, &fgDesc.header, nullptr);
    info(fmt("create FG context: rc=%u", rc));
    pass_fail(rc == FFX_API_RETURN_OK && s.fg, "frame-generation context created after the swapchain context");
    if (rc != FFX_API_RETURN_OK) return 1;

    if (variant & 128) {
        // A failed FG create next to a live pair must be refused and must not poison the session.
        ffxCreateContextDescFrameGeneration bad = fgDesc;
        bad.displaySize = {0, 0};
        bad.maxRenderSize = {0, 0};
        ffxContext none = nullptr;
        const ffxReturnCode_t brc = s.api.create(&none, &bad.header, nullptr);
        info(fmt("FG create with a 0x0 display next to a live pair: rc=%u context=%p", brc, static_cast<void*>(none)));
        pass_fail(brc != FFX_API_RETURN_OK && !none, "AMD refuses an FG context with an empty display size");
        if (brc == FFX_API_RETURN_OK && none) s.api.destroy(&none, nullptr);
    }

    auto hudless = texture(device.Get(), W, H, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                           D3D12_RESOURCE_STATE_RENDER_TARGET);
    auto depth = texture(device.Get(), W, H, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto motion = texture(device.Get(), W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (!hudless || !depth || !motion) {
        pass_fail(false, "create probe input resources");
        return 1;
    }

    // Initialize the HUD-less texture once, then keep it in compute-read state
    // for the whole session. AMD consumes it asynchronously on its own queue;
    // transitioning it back for the next game frame without a cross-queue fence
    // caused validation errors in the earlier probe (K90).
    ComPtr<ID3D12DescriptorHeap> initialRtv;
    D3D12_DESCRIPTOR_HEAP_DESC initialHeap{};
    initialHeap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    initialHeap.NumDescriptors = 1;
    ComPtr<ID3D12CommandAllocator> initialAllocator;
    ComPtr<ID3D12GraphicsCommandList> initialList;
    ComPtr<ID3D12Fence> initialFence;
    HRESULT initial = device->CreateDescriptorHeap(&initialHeap, IID_PPV_ARGS(&initialRtv));
    if (SUCCEEDED(initial)) initial = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                                       IID_PPV_ARGS(&initialAllocator));
    if (SUCCEEDED(initial)) initial = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        initialAllocator.Get(), nullptr, IID_PPV_ARGS(&initialList));
    if (SUCCEEDED(initial)) initial = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&initialFence));
    if (FAILED(initial)) { pass_fail(false, "create probe input initialization commands"); return 1; }
    const auto initialHandle = initialRtv->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(hudless.Get(), nullptr, initialHandle);
    const float initialColor[4] = {0.2f, 0.3f, 0.4f, 1.0f};
    initialList->ClearRenderTargetView(initialHandle, initialColor, 0, nullptr);
    barrier(initialList.Get(), hudless.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    initial = initialList->Close();
    if (SUCCEEDED(initial)) {
        ID3D12CommandList* initialLists[]{initialList.Get()};
        queue->ExecuteCommandLists(1, initialLists);
        initial = queue->Signal(initialFence.Get(), 1);
    }
    HANDLE initialEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (SUCCEEDED(initial) && initialEvent) initial = initialFence->SetEventOnCompletion(1, initialEvent);
    const bool initialized = SUCCEEDED(initial) && initialEvent && WaitForSingleObject(initialEvent, 5000) == WAIT_OBJECT_0;
    if (initialized && initialEvent) CloseHandle(initialEvent);
    pass_fail(initialized, "initialize static HUD-less input before AMD configure");
    if (!initialized) return 1;

    ffxConfigureDescFrameGeneration cfg{};
    cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    cfg.swapChain = amdChain.Get();
    cfg.presentCallback = presentCb ? present_callback : nullptr;
    cfg.frameGenerationCallback = noGenCb ? nullptr : generation_callback;
    cfg.frameGenerationEnabled = true;
    cfg.allowAsyncWorkloads = true;
    cfg.HUDLessColor = ffxApiGetResourceDX12(hudless.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
    cfg.generationRect = {0, 0, W, H};
    cfg.frameID = 0;
    // Variant 256 places the configure descriptor on a dedicated page, then
    // makes that page inaccessible as soon as configure returns. The page stays
    // inaccessible through presentation and both context destructions. A child
    // crash demonstrates post-call access; survival is bounded evidence for this
    // binary and workload, not proof about every possible internal path.
    FfxFrameGenerationConfig legacy{};
    const bool useLegacy = (variant & 512) != 0;
    if (useLegacy) {
        legacy.header.type = FFX_API_FRAME_GENERATION_CONFIG;
        legacy.swapChain = cfg.swapChain;
        legacy.presentCallback = cfg.presentCallback;
        legacy.frameGenerationCallback = cfg.frameGenerationCallback;
        legacy.frameGenerationEnabled = cfg.frameGenerationEnabled;
        legacy.allowAsyncWorkloads = cfg.allowAsyncWorkloads;
        legacy.HUDLessColor = cfg.HUDLessColor;
        legacy.interpolationRect = cfg.generationRect;
        legacy.frameID = cfg.frameID;
    }
    ffxConfigureDescHeader* configureDesc = useLegacy ? &legacy.header : &cfg.header;
    const size_t configureBytes = useLegacy ? sizeof(legacy) : sizeof(cfg);
    void* guardedDescriptor = nullptr;
    if (variant & 256) {
        guardedDescriptor = VirtualAlloc(nullptr, configureBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        pass_fail(guardedDescriptor != nullptr, "allocate guarded configure descriptor");
        if (!guardedDescriptor) return 1;
        std::memcpy(guardedDescriptor, configureDesc, configureBytes);
        configureDesc = static_cast<ffxConfigureDescHeader*>(guardedDescriptor);
    }
    if (productionOverlay) {
        // Install the actual Sedici callback around the AMD configure call.
        // The scope restores the caller's descriptor before any guard is set.
        ScopedOverlayCallback overlay(configureDesc, s.fg);
        rc = s.api.configure(&s.fg, configureDesc);
        overlay.complete(rc);
    } else {
        rc = s.api.configure(&s.fg, configureDesc);
    }
    if (guardedDescriptor) {
        DWORD previousProtection = 0;
        const bool protectedPage = VirtualProtect(guardedDescriptor, configureBytes, PAGE_NOACCESS,
                                                  &previousProtection) != 0;
        pass_fail(protectedPage, "configure descriptor inaccessible after return");
        if (!protectedPage) return 1;
    }
    info(fmt("configure sent to the FG context: rc=%u", rc));
    pass_fail(rc == FFX_API_RETURN_OK, "FG configure accepted by the FG context");
    if (rc != FFX_API_RETURN_OK) return 1;
    const ffxReturnCode_t other = s.api.configure(&s.sc, &cfg.header);
    info(fmt("same configure sent to the swapchain context instead: rc=%u", other));

    ComPtr<ID3D12CommandAllocator> alloc;
    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    ComPtr<ID3D12GraphicsCommandList> list;
    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list));
    list->Close();
    ComPtr<ID3D12Fence> fence;
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = kBuffers;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap));
    const UINT rtvStep = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    ComPtr<ID3D12Resource> backs[kBuffers];
    for (UINT i = 0; i < kBuffers; ++i) {
        if (FAILED(amdChain->GetBuffer(i, IID_PPV_ARGS(&backs[i])))) {
            pass_fail(false, "GetBuffer on the AMD chain");
            return 1;
        }
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(i) * rtvStep;
        device->CreateRenderTargetView(backs[i].Get(), nullptr, h);
    }

    uint64_t fenceValue = 0, frame = 0;
    std::vector<double> presentCallMs;
    HRESULT lastPresent = S_OK;
    UINT presentFailures = 0;
    constexpr int kFrames = 90;
    auto drive = [&](int count) {
    for (int i = 0; i < count; ++i) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        const UINT idx = amdChain->GetCurrentBackBufferIndex();
        ID3D12Resource* back = backs[idx].Get();
        alloc->Reset();
        list->Reset(alloc.Get(), nullptr);
        barrier(list.Get(), back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const float v = static_cast<float>((i * 7) % 100) / 100.0f;
        const float color[4] = {v, 0.2f, 1.0f - v, 1.0f};
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(idx) * rtvStep;
        list->ClearRenderTargetView(h, color, 0, nullptr);
        barrier(list.Get(), back, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);

        ffxDispatchDescFrameGenerationPrepare prep{};
        prep.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
        prep.frameID = ++frame;
        prep.commandList = list.Get();
        prep.renderSize = {W, H};
        prep.motionVectorScale = {static_cast<float>(W), static_cast<float>(H)};
        prep.frameTimeDelta = 13.3f;
        prep.cameraNear = 0.1f;
        prep.cameraFar = 1000.0f;
        prep.cameraFovAngleVertical = 1.0f;
        prep.viewSpaceToMetersFactor = 1.0f;
        prep.depth = ffxApiGetResourceDX12(depth.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
        prep.motionVectors = ffxApiGetResourceDX12(motion.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
        const ffxReturnCode_t prc = s.fg ? s.api.dispatch(&s.fg, &prep.header) : FFX_API_RETURN_ERROR_PARAMETER;
        if (i < 2 || prc != FFX_API_RETURN_OK) info(fmt("frame %d prepare rc=%u", i, prc));
        list->Close();
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(fence.Get(), ++fenceValue);

        const double t0 = now_ms(s);
        lastPresent = amdChain->Present(0, DXGI_PRESENT_ALLOW_TEARING);
        presentCallMs.push_back(now_ms(s) - t0);
        if (FAILED(lastPresent)) {
            ++presentFailures;
            if (presentFailures < 3) info(fmt("Present hr=0x%08lx", static_cast<unsigned long>(lastPresent)));
        }
        // The game does not wait per frame; keep at most two frames of work ahead.
        if (fenceValue > 2 && fence->GetCompletedValue() < fenceValue - 2) {
            fence->SetEventOnCompletion(fenceValue - 2, evt);
            WaitForSingleObject(evt, 2000);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    };
    drive(kFrames);
    fence->SetEventOnCompletion(fenceValue, evt);
    WaitForSingleObject(evt, 5000);
    std::this_thread::sleep_for(std::chrono::milliseconds(200)); // let AMD's present thread finish

    std::sort(presentCallMs.begin(), presentCallMs.end());
    info(fmt("%d game Presents: failures=%u median call %.2f ms max %.2f ms", kFrames, presentFailures,
             presentCallMs[presentCallMs.size() / 2], presentCallMs.back()));
    info(fmt("AMD called our generation callback %ld times (last rc=%u, errors=%ld)", s.generationCalls,
             s.lastGenerationRc, s.generationErrors));
    if (presentCb) {
        info(fmt("AMD called our present callback %ld times, %ld for generated frames", s.presentCalls, s.presentGenerated));
        std::vector<double> gaps;
        for (size_t i = 1; i < s.presentTimes.size(); ++i) gaps.push_back(s.presentTimes[i] - s.presentTimes[i - 1]);
        if (gaps.size() > 4) {
            std::sort(gaps.begin(), gaps.end());
            info(fmt("present-callback gaps: median %.2f ms, p10 %.2f ms, p90 %.2f ms", gaps[gaps.size() / 2],
                     gaps[gaps.size() / 10], gaps[gaps.size() * 9 / 10]));
        }
    }
    pass_fail(presentFailures == 0, "every Present on the AMD chain succeeded");
    if (!noGenCb) pass_fail(s.generationCalls > 0, "AMD invokes the host's generation callback");
    if (productionOverlay) {
        const sedici::UiRenderStatus status = sedici::ui_render_status();
        info(fmt("production Mini overlay: ready=%d real draws=%llu generated draws=%llu",
                 status.ready ? 1 : 0, static_cast<unsigned long long>(status.realDraws),
                 static_cast<unsigned long long>(status.generatedDraws)));
        pass_fail(status.ready && status.realDraws > 0 && status.generatedDraws > 0,
                  "production Mini overlay records on real and generated AMD callback outputs");
        bool debugAvailable = false;
        const uint64_t errors = debug_errors(device.Get(), debugAvailable);
        if (debugAvailable) pass_fail(errors == 0, fmt("D3D12 debug layer reports %llu errors",
                                                       static_cast<unsigned long long>(errors)));
        else info("D3D12 debug-layer message queue unavailable; validation not performed");
        info("overlay draw counts are command recording only; pixels and physical presentation are not verified");
    }

    if (variant & (16 | 64)) {
        const bool quiesce = (variant & 64) != 0;
        // Mid-session: drop only the FG context, keep presenting, bring a new one up.
        const LONG genBefore = s.generationCalls;
        if (quiesce) {
            // Disable generation, drop the callbacks and let AMD's present thread run dry first.
            ffxConfigureDescFrameGeneration off{};
            off.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
            off.swapChain = amdChain.Get();
            off.frameGenerationEnabled = false;
            const ffxReturnCode_t orc = s.api.configure(&s.fg, &off.header);
            info(fmt("mid-session: disable generation and clear callbacks: rc=%u", orc));
            fence->SetEventOnCompletion(fenceValue, evt);
            WaitForSingleObject(evt, 5000);
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
        ffxContext old = s.fg;
        const ffxReturnCode_t drc = s.api.destroy(&old, nullptr);
        s.fg = nullptr;
        info(fmt("mid-session: destroy FG context only: rc=%u", drc));
        pass_fail(drc == FFX_API_RETURN_OK, "the FG context can be destroyed while the swapchain context lives");
        if (!quiesce) drive(8);
        info(fmt("mid-session: frames presented with no FG context: generation callbacks +%ld, Present failures so far %u",
                 s.generationCalls - genBefore, presentFailures));
        const ffxReturnCode_t crc = s.api.create(&s.fg, &fgDesc.header, nullptr);
        info(fmt("mid-session: recreate FG context: rc=%u", crc));
        pass_fail(crc == FFX_API_RETURN_OK && s.fg, "a new FG context can be created for the live swapchain");
        const ffxReturnCode_t cfg2 = s.api.configure(&s.fg, &cfg.header);
        info(fmt("mid-session: configure the new context: rc=%u", cfg2));
        const LONG genMid = s.generationCalls;
        drive(30);
        fence->SetEventOnCompletion(fenceValue, evt);
        WaitForSingleObject(evt, 5000);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        info(fmt("mid-session: after recreation generation callbacks +%ld over 30 frames (errors=%ld)",
                 s.generationCalls - genMid, s.generationErrors));
        pass_fail(s.generationCalls - genMid >= 25, "generation resumes after the FG context is recreated");
    }

    // Destruction order. Buffer references are released first, as a game does on resize.
    for (auto& b : backs) b.Reset();
    ffxContext first = fgFirst ? s.fg : s.sc;
    ffxContext second = fgFirst ? s.sc : s.fg;
    const ffxReturnCode_t d1 = s.api.destroy(&first, nullptr);
    const ffxReturnCode_t d2 = s.api.destroy(&second, nullptr);
    info(fmt("destroy %s first: rc=%u, then %s: rc=%u", fgFirst ? "FG" : "swapchain", d1, fgFirst ? "swapchain" : "FG", d2));
    pass_fail(d1 == FFX_API_RETURN_OK && d2 == FFX_API_RETURN_OK, "both contexts destroyed cleanly");
    amdChain.Reset();
    DestroyWindow(hwnd);
    info(fmt("session variant %d finished without a crash", variant));
    return g_fail ? 1 : 0;
}

// variant 8: refused creates. A refused wrap must leave the caller's native chain
// with the caller; a failed FG create must not poison a retry.
int run_refusal(const fs::path& amdPath, bool nullQueue)
{
    info("refusal variant: refused wrap and failed FG create, then recovery");
    Session s;
    g_session = &s;
    s.api = load(amdPath);
    if (!complete(s.api)) {
        pass_fail(false, "load for refusal");
        return 1;
    }
    ComPtr<ID3D12Device> device = make_device();
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
    ComPtr<IDXGIFactory4> factory;
    CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    HWND hwnd = make_window(640, 360);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = 640;
    sd.Height = 360;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 3;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    ComPtr<IDXGISwapChain1> sc1;
    const HRESULT schr = factory->CreateSwapChainForHwnd(queue.Get(), hwnd, &sd, nullptr, nullptr, &sc1);
    ComPtr<IDXGISwapChain4> chain;
    sc1.As(&chain);
    info(fmt("native chain: hr=0x%08lx chain=%p", static_cast<unsigned long>(schr), static_cast<void*>(chain.Get())));
    pass_fail(chain != nullptr, "native swapchain for the refusal variant");
    if (!chain) return 1;

    ffxCreateContextDescFrameGenerationSwapChainWrapDX12 wrap{};
    wrap.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WRAP_DX12;
    IDXGISwapChain4* slot = chain.Get();
    wrap.swapchain = &slot;
    ffxContext sc = nullptr;
    ffxReturnCode_t rc = FFX_API_RETURN_OK;
    if (nullQueue) {
        // AMD does not validate this: the call is expected to crash the process.
        wrap.gameQueue = nullptr;
        rc = s.api.create(&sc, &wrap.header, nullptr);
        info(fmt("wrap with no game queue returned rc=%u (AMD validated it)", rc));
        return 0;
    }
    wrap.gameQueue = queue.Get();
    slot = chain.Get();
    sc = nullptr;
    rc = s.api.create(&sc, &wrap.header, nullptr);
    info(fmt("wrap retried with the queue: rc=%u", rc));
    pass_fail(rc == FFX_API_RETURN_OK && sc, "the same chain can be wrapped after a refusal");
    ComPtr<IDXGISwapChain4> wrapped;
    wrapped.Attach(slot);
    chain.Detach(); // consumed by the successful wrap

    ffxCreateBackendDX12Desc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = device.Get();
    ffxCreateContextDescFrameGeneration fgDesc{};
    fgDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    fgDesc.header.pNext = &backend.header;
    fgDesc.displaySize = {0, 0};
    fgDesc.maxRenderSize = {0, 0};
    fgDesc.backBufferFormat = FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
    ffxContext fg = nullptr;
    rc = s.api.create(&fg, &fgDesc.header, nullptr);
    info(fmt("FG create with a 0x0 display after the wrap succeeded: rc=%u context=%p", rc, static_cast<void*>(fg)));
    pass_fail(rc != FFX_API_RETURN_OK, "AMD refuses an FG context with an empty display size");
    fgDesc.displaySize = {640, 360};
    fgDesc.maxRenderSize = {640, 360};
    fg = nullptr;
    rc = s.api.create(&fg, &fgDesc.header, nullptr);
    pass_fail(rc == FFX_API_RETURN_OK && fg, "a valid FG create succeeds after the failed one");
    ffxContext a = fg, b = sc;
    pass_fail(s.api.destroy(&a, nullptr) == FFX_API_RETURN_OK && s.api.destroy(&b, nullptr) == FFX_API_RETURN_OK,
              "both contexts destroyed");
    wrapped.Reset();
    DestroyWindow(hwnd);
    return g_fail ? 1 : 0;
}

int spawn_variant(const fs::path& self, const fs::path& amd, int variant)
{
    std::wstring cmd = L"\"" + self.wstring() + L"\" \"" + amd.wstring() + L"\" --variant " + std::to_wstring(variant);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, 120000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) {
        std::printf("usage: sedici_amd_probe <amd_fg.dll> [--sha256 HEX] [--variant N]\n");
        return 77;
    }
    const fs::path amd = argv[1];
    if (!fs::exists(amd)) {
        std::printf("SKIP no binary at the given path\n");
        return 77;
    }
    int variant = -1;
    std::string wantHash;
    for (int i = 2; i + 1 < argc; ++i) {
        if (std::wstring(argv[i]) == L"--variant") variant = _wtoi(argv[i + 1]);
        if (std::wstring(argv[i]) == L"--sha256") {
            wantHash = fs::path(argv[i + 1]).string();
        }
    }
    if (variant >= 0 && !wantHash.empty()) {
        for (char& c : wantHash) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        const bool matches = sha256_of(amd) == wantHash;
        pass_fail(matches, "variant binary matches the pinned SHA-256");
        if (!matches) return 1;
    }
    if (variant == 9) return run_refusal(amd, true);
    if (variant >= 0) {
        // bit 5 (32): tear everything down and bring a second session up in the same process.
        int result = run_session(amd, variant & ~32);
        if ((variant & 32) && result == 0) {
            info("second session in the same process after full destruction");
            result = run_session(amd, variant & ~32);
        }
        return result;
    }

    const std::string have = sha256_of(amd);
    info("file: " + amd.filename().string() + fmt(" %llu bytes", static_cast<unsigned long long>(fs::file_size(amd))));
    info("sha256: " + have);
    if (!wantHash.empty()) {
        for (char& c : wantHash) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        pass_fail(have == wantHash, "sha256 matches the pinned value");
    }
    stage_load(amd);

    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    for (int v : {0, 1, 2, 3, 4, 9, 16, 64, 128, 32, 33}) {
        const int code = spawn_variant(exe, amd, v);
        if (v == 16) {
            info(fmt("variant 16 (destroy only the FG context, keep presenting) exit code %d: %s", code,
                     code == 0 ? "survived" : "crashes; the swapchain cannot outlive its FG context"));
            continue;
        }
        if (v == 128) {
            info(fmt("variant 128 (FG create with a 0x0 display) exit code %d: %s", code,
                     code == 0 ? "AMD refused it" : "AMD aborts the process; Sedici must validate sizes"));
            continue;
        }
        if (v == 9) {
            info(fmt("variant 9 (wrap with a null game queue) exit code %d: %s", code,
                     code == 0 ? "AMD validated the argument" : "AMD does not validate it; Sedici must"));
            continue;
        }
        pass_fail(code == 0, fmt("variant %d exit code %d", v, code));
    }
    std::printf("%s\n", g_fail ? "RESULT FAIL" : "RESULT OK");
    return g_fail ? 1 : 0;
}
