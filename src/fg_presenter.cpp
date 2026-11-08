#include "fg_presenter.h"
#include "fg_bookkeeping_faults.h"
#include "fg_retirement_policy.h"
#include "fg_queue_policy.h"

#include <algorithm>
#include <new>
#include <iterator>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <new>

#include "fg_pacing.h"
#include "log.h"

namespace sedici::fg {

namespace {

// gdi32 exports these on every supported Windows; the declarations match the
// WDK's d3dkmthk.h and are resolved at runtime so no WDK is needed to build.
struct KmtOpenAdapterFromGdiDisplayName {
    WCHAR DeviceName[32];
    UINT hAdapter;
    LUID AdapterLuid;
    UINT VidPnSourceId;
};
struct KmtGetScanLine {
    UINT hAdapter;
    UINT VidPnSourceId;
    BOOLEAN InVerticalBlank;
    UINT ScanLine;
};
struct KmtCloseAdapter {
    UINT hAdapter;
};
using OpenAdapterFn = LONG(APIENTRY*)(KmtOpenAdapterFromGdiDisplayName*);
using GetScanLineFn = LONG(APIENTRY*)(KmtGetScanLine*);
using CloseAdapterFn = LONG(APIENTRY*)(const KmtCloseAdapter*);

struct Kmt {
    OpenAdapterFn open = nullptr;
    GetScanLineFn scan = nullptr;
    CloseAdapterFn close = nullptr;
};

const Kmt& kmt() noexcept
{
    static const Kmt functions = [] {
        Kmt result;
        HMODULE gdi = LoadLibraryExW(L"gdi32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!gdi) return result;
        result.open = reinterpret_cast<OpenAdapterFn>(GetProcAddress(gdi, "D3DKMTOpenAdapterFromGdiDisplayName"));
        result.scan = reinterpret_cast<GetScanLineFn>(GetProcAddress(gdi, "D3DKMTGetScanLine"));
        result.close = reinterpret_cast<CloseAdapterFn>(GetProcAddress(gdi, "D3DKMTCloseAdapter"));
        return result;
    }();
    return functions;
}

uint64_t frequency() noexcept
{
    static const uint64_t value = [] {
        LARGE_INTEGER f{};
        return QueryPerformanceFrequency(&f) ? static_cast<uint64_t>(f.QuadPart) : uint64_t{1};
    }();
    return value;
}

double now_seconds() noexcept
{
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return ticks_to_seconds(static_cast<uint64_t>(counter.QuadPart), frequency());
}

std::string ms(double seconds)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.2f", seconds * 1000.0);
    return buffer;
}

constexpr DWORD kGpuTimeoutMs = 5000;

// One present queue per device for the life of the process, never released.
// Overlays that hook Present (RTSS's, at least) remember the queue a swapchain
// presents on and use it again for later swapchains; destroying it with the
// wrapper crashed the next swapchain's Present inside RTSS in the offline check.
HRESULT shared_present_queue(ID3D12Device* device, Microsoft::WRL::ComPtr<ID3D12CommandQueue>& out) noexcept try
{
    struct Entry {
        ID3D12Device* device;
        ID3D12CommandQueue* queue;
    };
    static std::mutex mutex;
    static auto* entries = new std::vector<Entry>; // intentionally leaked with the queues
    std::lock_guard lock(mutex);
    for (const auto& entry : *entries) {
        if (entry.device == device) { out = entry.queue; return S_OK; }
    }
    D3D12_COMMAND_QUEUE_DESC desc{};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
    ID3D12CommandQueue* queue = nullptr;
    HRESULT hr = device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue));
    if (FAILED(hr)) {
        // A high-priority queue is a preference, not a requirement.
        desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        hr = device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue));
    }
    if (FAILED(hr)) return hr;
    (void)queue->SetName(L"Sedici FG present queue");
    // The entry keeps the queue's reference, and the queue keeps its device
    // alive, so the device address can never be reused by another device.
    entries->push_back({device, queue});
    out = queue;
    return S_OK;
}
catch (...) { return E_OUTOFMEMORY; }

} // namespace

std::unique_ptr<Presenter> Presenter::create(ID3D12Device* device, TearGuard guard,
                                             std::string& why) noexcept try
{
    std::unique_ptr<Presenter> p(new (std::nothrow) Presenter);
    if (!p) { why = "out of memory"; return nullptr; }
    p->m_device = device;
    p->m_guard = guard;

    HRESULT hr = shared_present_queue(device, p->m_queue);
    for (int i = 0; i < 2 && SUCCEEDED(hr); ++i) {
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&p->m_allocator[i]));
        if (SUCCEEDED(hr)) hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            p->m_allocator[i].Get(), nullptr, IID_PPV_ARGS(&p->m_list[i]));
        if (SUCCEEDED(hr)) hr = p->m_list[i]->Close();
    }
    if (SUCCEEDED(hr)) hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&p->m_fence));
    if (SUCCEEDED(hr)) {
        p->m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!p->m_event) hr = HRESULT_FROM_WIN32(GetLastError());
    }
    if (FAILED(hr)) { why = "present queue setup failed hr=" + std::to_string(hr); return nullptr; }

    p->m_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!p->m_timer) p->m_timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    {
        wchar_t lead[16]{};
        if (GetEnvironmentVariableW(L"SEDICI_FG_SCANLINE_LEAD_US", lead, 16) > 0) {
            const double us = std::wcstod(lead, nullptr);
            if (us >= 0.0 && us <= 5000.0) p->m_lead_seconds = us / 1e6;
        }
    }
    // Started now, while failing is still harmless; it idles until a job arrives.
    Presenter* self = p.get();
    p->m_thread = std::thread([self] { self->run(); });
    return p;
}
catch (...) { why = "exception during presenter setup"; return nullptr; }

HRESULT Presenter::adopt(Microsoft::WRL::ComPtr<IDXGISwapChain4>& native, ULONG owned,
                         ID3D12CommandQueue* gameQueue, std::string& why) noexcept try
{
    // Everything needed to rebuild the chain, read while it is still intact.
    DXGI_SWAP_CHAIN_DESC1 desc{};
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreenDesc{};
    HWND window = nullptr;
    Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
    Microsoft::WRL::ComPtr<IDXGIOutput> restrictTo, fullscreenOutput;
    BOOL fullscreen = FALSE;
    HRESULT hr = native->GetDesc1(&desc);
    if (SUCCEEDED(hr)) hr = native->GetFullscreenDesc(&fullscreenDesc);
    if (SUCCEEDED(hr)) hr = native->GetHwnd(&window);
    if (SUCCEEDED(hr)) hr = native->GetParent(IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) hr = native->GetFullscreenState(&fullscreen, &fullscreenOutput);
    if (FAILED(hr)) { why = "could not read the chain to recreate it hr=" + std::to_string(hr); return hr; }
    (void)native->GetRestrictToOutput(&restrictTo);
    UINT latency = 0;
    const bool waitable = (desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) != 0;
    if (waitable) (void)native->GetMaximumFrameLatency(&latency);
    DXGI_RGBA background{};
    const bool hasBackground = SUCCEEDED(native->GetBackgroundColor(&background));

    // Destroying the chain is only safe when the references we were handed are
    // all there are, on the chain and on every one of its buffers. Anything
    // else means someone still uses it: leave it alone.
    native->AddRef();
    const ULONG refs = native->Release();
    if (refs != owned) {
        why = "the chain has " + std::to_string(refs) + " references, expected " + std::to_string(owned);
        return E_ACCESSDENIED;
    }
    for (UINT i = 0; i < desc.BufferCount; ++i) {
        Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
        if (FAILED(native->GetBuffer(i, IID_PPV_ARGS(&buffer)))) continue;
        buffer->AddRef();
        if (buffer->Release() != 1) {
            why = "native backbuffer " + std::to_string(i) + " is still referenced";
            return E_ACCESSDENIED;
        }
    }

    // The old chain's buffers may still be in use by work on the game queue.
    if (FAILED(gameQueue->Signal(m_fence.Get(), ++m_fence_value)) || FAILED(wait_fence(m_fence_value))) {
        why = "the game queue did not go idle";
        return DXGI_ERROR_DEVICE_HUNG;
    }

    // Point of no return: from here the caller's chain is gone.
    if (fullscreen) (void)native->SetFullscreenState(FALSE, nullptr);
    IDXGISwapChain4* raw = native.Get();
    for (ULONG i = 1; i < owned; ++i) raw->Release();
    native.Reset();

    fullscreenDesc.Windowed = TRUE; // enter fullscreen after creation, as DXGI recommends
    const auto build = [&](ID3D12CommandQueue* queue, Microsoft::WRL::ComPtr<IDXGISwapChain4>& out) {
        Microsoft::WRL::ComPtr<IDXGISwapChain1> created;
        HRESULT result = factory->CreateSwapChainForHwnd(queue, window, &desc, &fullscreenDesc,
                                                         restrictTo.Get(), &created);
        if (SUCCEEDED(result)) result = created.As(&out);
        if (FAILED(result)) return result;
        if (waitable && latency) (void)out->SetMaximumFrameLatency(latency);
        if (hasBackground) (void)out->SetBackgroundColor(&background);
        if (fullscreen) (void)out->SetFullscreenState(TRUE, fullscreenOutput.Get());
        return S_OK;
    };
    hr = build(m_queue.Get(), native);
    if (FAILED(hr)) {
        why = "CreateSwapChainForHwnd on the present queue failed hr=" + std::to_string(hr);
        // Give the game a working chain on its own queue rather than none.
        const HRESULT back = build(gameQueue, native);
        if (FAILED(back)) {
            native.Reset();
            why += "; recreating it on the game queue failed too hr=" + std::to_string(back);
        }
        return hr;
    }

    m_native = native;
    refresh_output();
    log_event(1, std::string("fg: vrr presenter ready guard=") + tear_guard_name(m_guard) +
        " refresh_ms=" + ms(m_period) + " active_lines=" + std::to_string(m_active_lines) +
        " lead_lines=" + std::to_string(m_lead_lines) +
        " scanline=" + (m_scanline_ok ? "yes" : "no") +
        " buffers=" + std::to_string(desc.BufferCount) +
        " max_latency=" + std::to_string(latency) +
        " fullscreen=" + (fullscreen ? "yes" : "no"));
    return S_OK;
}
catch (...) { why = "exception while recreating the chain"; return E_FAIL; }

Presenter::~Presenter()
{
    {
        std::lock_guard lock(m_mutex);
        m_stop = true;
    }
    m_wake.notify_all();
    if (m_thread.joinable()) m_thread.join();
    if (m_window.jobs) log_window(); // the partial window, so short sessions report too
    (void)drain_gpu();
    close_adapter();
    if (m_event) CloseHandle(m_event);
    if (m_timer) CloseHandle(m_timer);
}

void Presenter::request_stop() noexcept
{
    m_cancel.store(true, std::memory_order_release);
    {
        std::lock_guard lock(m_mutex);
        m_stop = true;
        m_hooks = {};
    }
    m_wake.notify_all();
}

size_t Presenter::outstanding() const noexcept
{
    std::lock_guard lock(m_mutex);
    return static_cast<size_t>(m_submitted - m_completed);
}

bool Presenter::wait_outstanding(size_t max, DWORD timeoutMs) noexcept
{
    std::unique_lock lock(m_mutex);
    return m_done.wait_for(lock, std::chrono::milliseconds(timeoutMs),
        [&] { return m_submitted - m_completed <= max; });
}

HRESULT Presenter::submit(PresentJob&& job) noexcept try
{
    if (consume_bookkeeping_fault(BookkeepingFault::PresenterSubmit)) return E_OUTOFMEMORY;
    {
        std::lock_guard lock(m_mutex);
        if (m_stop) return DXGI_ERROR_INVALID_CALL;
        // The swapchain admits at most kPresenterQueueCapacity outstanding
        // jobs, including the worker's job. This fixed ring never allocates.
        if (!m_jobs.push_back(std::move(job))) return DXGI_ERROR_WAS_STILL_DRAWING;
        ++m_submitted;
    }
    m_wake.notify_one();
    return S_OK;
}
catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
catch (...) { return E_FAIL; }

void Presenter::set_output_hooks(const OutputHooks& hooks) noexcept
{
    std::lock_guard lock(m_mutex);
    m_hooks = hooks;
}

HRESULT Presenter::last_result() const noexcept
{
    std::lock_guard lock(m_mutex);
    return m_last;
}

HRESULT Presenter::wait_fence(uint64_t value) noexcept
{
    auto progress = classify_fence(m_fence->GetCompletedValue(), value);
    if (progress == FenceProgress::DeviceRemoved)
        return removed_device_error(m_device->GetDeviceRemovedReason());
    if (progress == FenceProgress::Complete) return S_OK;
    const HRESULT hr = m_fence->SetEventOnCompletion(value, m_event);
    if (FAILED(hr)) return hr;
    const DWORD wait = WaitForSingleObject(m_event, kGpuTimeoutMs);
    if (wait != WAIT_OBJECT_0)
        return wait == WAIT_TIMEOUT ? DXGI_ERROR_DEVICE_HUNG : HRESULT_FROM_WIN32(GetLastError());
    progress = classify_fence(m_fence->GetCompletedValue(), value);
    if (progress == FenceProgress::DeviceRemoved)
        return removed_device_error(m_device->GetDeviceRemovedReason());
    return progress == FenceProgress::Complete ? S_OK : E_FAIL;
}

HRESULT Presenter::drain_gpu() noexcept
{
    if (!m_fence || !m_queue) return S_OK;
    // Covers the native Present work DXGI queued after the last copy, too.
    return signal_then_wait(
        [&]() noexcept { return m_queue->Signal(m_fence.Get(), ++m_fence_value); },
        [&]() noexcept { return wait_fence(m_fence_value); });
}

HRESULT Presenter::resize_native(UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags) noexcept try
{
    if (count == 0) {
        DXGI_SWAP_CHAIN_DESC1 desc{};
        const HRESULT hr = m_native->GetDesc1(&desc);
        if (FAILED(hr)) return hr;
        count = desc.BufferCount;
    }
    std::vector<UINT> masks(count, 1u);
    std::vector<IUnknown*> queues(count, m_queue.Get());
    const HRESULT hr = m_native->ResizeBuffers1(count, width, height, format, flags, masks.data(), queues.data());
    if (SUCCEEDED(hr)) refresh_output();
    return hr;
}
catch (...) { return E_OUTOFMEMORY; }

void Presenter::close_adapter() noexcept
{
    if (m_adapter && kmt().close) {
        KmtCloseAdapter close{m_adapter};
        kmt().close(&close);
    }
    m_adapter = 0;
    m_scanline_ok = false;
}

void Presenter::refresh_output() noexcept
{
    Microsoft::WRL::ComPtr<IDXGIOutput> output;
    if (FAILED(m_native->GetContainingOutput(&output)) || !output) return;
    DXGI_OUTPUT_DESC desc{};
    if (FAILED(output->GetDesc(&desc))) return;

    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsW(desc.DeviceName, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1)
        m_period = 1.0 / static_cast<double>(mode.dmDisplayFrequency);

    const LONG width = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
    const LONG height = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
    const bool rotated = desc.Rotation == DXGI_MODE_ROTATION_ROTATE90 || desc.Rotation == DXGI_MODE_ROTATION_ROTATE270;
    // The scan runs along the panel's native rows, which is the desktop width
    // when the output is rotated by a quarter turn.
    m_active_lines = static_cast<UINT>(std::max<LONG>(1, rotated ? width : height));
    m_lead_lines = static_cast<UINT>(std::lround(m_active_lines * (m_lead_seconds / m_period)));

    close_adapter();
    const Kmt& k = kmt();
    if (!k.open || !k.scan) return;
    KmtOpenAdapterFromGdiDisplayName open{};
    wcsncpy_s(open.DeviceName, desc.DeviceName, _TRUNCATE);
    if (k.open(&open) < 0) return;
    m_adapter = open.hAdapter;
    m_source = open.VidPnSourceId;
    bool blank = false;
    UINT line = 0;
    m_scanline_ok = true;
    m_scanline_ok = read_scanline(blank, line);
}

bool Presenter::read_scanline(bool& inBlank, UINT& line) noexcept
{
    if (!m_scanline_ok || !kmt().scan) return false;
    KmtGetScanLine scan{m_adapter, m_source, FALSE, 0};
    if (kmt().scan(&scan) < 0) return false;
    inBlank = scan.InVerticalBlank != FALSE;
    line = scan.ScanLine;
    return true;
}

void Presenter::wait_until(double deadline) noexcept
{
    double remaining = deadline - now_seconds();
    if (!(remaining > 0.0)) return;
    remaining = std::min(remaining, 0.1); // a broken interval must never park the thread
    const double target = now_seconds() + remaining;
    constexpr double spin = 0.0005;
    if (remaining > spin && m_timer) {
        LARGE_INTEGER due{};
        due.QuadPart = -std::max<LONGLONG>(1, static_cast<LONGLONG>((remaining - spin) * 1e7));
        if (SetWaitableTimer(m_timer, &due, 0, nullptr, nullptr, FALSE))
            WaitForSingleObject(m_timer, static_cast<DWORD>(remaining * 1000.0) + 5);
    }
    while (now_seconds() < target) YieldProcessor();
}

void Presenter::hold(double notBefore) noexcept
{
    const auto record = [&](double waited) {
        if (waited <= 0.0001) return;
        ++m_window.guardHolds;
        m_window.guardSum += waited;
        m_window.guardMax = std::max(m_window.guardMax, waited);
    };

    // The panel scans for one refresh after a flip. The timer guard never starts
    // another flip before that, plus a small margin for flip-time jitter. It is
    // also the fallback when scan lines cannot be read.
    const bool timerGuard = m_guard == TearGuard::Timer || (m_guard == TearGuard::Scanline && !m_scanline_ok);
    const bool scanGuard = m_guard == TearGuard::Scanline && m_scanline_ok;
    // The scan-line guard needs a floor too. Right after Present returns the
    // flip has not latched yet, so the scan line still reports the previous
    // blank and the gate would open for a second flip ~1 ms later: that frame
    // then shows for a millisecond and the next for ~20 (finding 009, adaptive
    // runs). Hold until just before the previous image's scan can have ended;
    // the scan line then decides the exact moment.
    double floor = 0.0;
    if (m_last_flip > 0.0) {
        if (timerGuard) floor = m_last_flip + m_period + 0.0002;
        else if (scanGuard) floor = m_last_flip + m_period - m_lead_seconds - 0.0005;
    }
    const double scheduled = std::max(notBefore, now_seconds());
    // On a fixed-refresh panel the scan-line guard can only flip at a blank. A
    // target just after one would otherwise wait a whole refresh, and at ~36 fps
    // on 75 Hz that is every real frame: the pair then needs more time than the
    // game's interval and midpoints are dropped. Accept a blank up to `early`
    // before the target instead. On VRR this moves a flip by at most that much.
    const double early = scanGuard && notBefore > 0.0 ? std::min(0.0015, m_period * 0.12) : 0.0;
    wait_until(std::max(notBefore - early, floor));
    if (floor > scheduled) record(now_seconds() - scheduled);

    if (!scanGuard) return;
    // Scan-line guard: flip only while the panel is in its vertical blank, or
    // within `lead` of it so the flip has time to be latched.
    const double holdStart = now_seconds();
    const double giveUp = holdStart + m_period + 0.002;
    const UINT open = m_active_lines > m_lead_lines ? m_active_lines - m_lead_lines : 0;
    for (;;) {
        bool blank = false;
        UINT line = 0;
        if (!read_scanline(blank, line)) {
            m_scanline_ok = false;
            log_event(1, "fg: scan-line read failed; tear guard falls back to \"timer\"");
            break;
        }
        if (blank || line >= open) break;
        const double now = now_seconds();
        if (now > giveUp) { ++m_window.guardTimeouts; break; }
        // Sleep through most of the remaining scan, then poll.
        const double eta = (static_cast<double>(open - line) / m_active_lines) * m_period;
        if (eta > 0.002) wait_until(now + eta - 0.001);
        else YieldProcessor();
    }
    record(now_seconds() - holdStart);
}

HRESULT Presenter::copy_to_native(ID3D12Resource* source, D3D12_RESOURCE_STATES state, bool generated) noexcept
{
    Microsoft::WRL::ComPtr<ID3D12Resource> destination;
    HRESULT hr = m_native->GetBuffer(m_native->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&destination));
    if (FAILED(hr)) return hr;
    const uint32_t slot = m_next_list;
    m_next_list ^= 1u;
    hr = m_allocator[slot]->Reset();
    if (SUCCEEDED(hr)) hr = m_list[slot]->Reset(m_allocator[slot].Get(), nullptr);
    if (FAILED(hr)) return hr;
    auto* list = m_list[slot].Get();

    D3D12_RESOURCE_BARRIER barriers[2]{};
    UINT count = 0;
    const auto add = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        if (before == after) return;
        auto& barrier = barriers[count++];
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.pResource = resource;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
    };
    add(source, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    add(destination.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    if (count) list->ResourceBarrier(count, barriers);
    list->CopyResource(destination.Get(), source);
    count = 0;
    add(destination.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    add(source, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    if (count) list->ResourceBarrier(count, barriers);
    // The overlay draws on the final image, as it does from AMD's callback.
    if (!m_cancel.load(std::memory_order_acquire) && m_job_hooks.draw) m_job_hooks.draw(m_device.Get(), list, destination.Get(), generated);
    hr = list->Close();
    if (FAILED(hr)) return hr;
    ID3D12CommandList* lists[]{list};
    m_queue->ExecuteCommandLists(1, lists);
    return m_queue->Signal(m_fence.Get(), ++m_fence_value);
}

HRESULT Presenter::native_present(const PresentJob& job, bool generated) noexcept
{
    const double before = now_seconds();
    {
        bool blank = false;
        UINT line = 0;
        if (read_scanline(blank, line)) {
            const UINT quarter = std::min<UINT>(3, static_cast<UINT>((4ull * line) / std::max<UINT>(1, m_active_lines)));
            ++m_window.scan[blank ? 0 : 1 + quarter];
        } else {
            ++m_window.scan[5];
        }
    }
    if (m_last_flip > 0.0) {
        const double gap = before - m_last_flip;
        if (gap < m_period - 0.0001) ++m_window.shortGaps;
        if (gap < 0.002) { ++m_window.backToBack; ++m_back_to_back; }
        if (!m_window.flips || gap < m_window.gapMin) m_window.gapMin = gap;
    }
    ++m_window.flips;

    HRESULT hr;
    if (job.hasParams) {
        DXGI_PRESENT_PARAMETERS params{};
        params.DirtyRectsCount = static_cast<UINT>(job.dirty.size());
        params.pDirtyRects = job.dirty.empty() ? nullptr : const_cast<RECT*>(job.dirty.data());
        params.pScrollRect = job.hasScroll ? const_cast<RECT*>(&job.scroll) : nullptr;
        params.pScrollOffset = job.hasOffset ? const_cast<POINT*>(&job.offset) : nullptr;
        hr = m_native->Present1(job.sync, job.flags, &params);
    } else {
        hr = m_native->Present(job.sync, job.flags);
    }
    m_last_flip = before;
    if (SUCCEEDED(hr) && !m_cancel.load(std::memory_order_acquire) && m_job_hooks.presented) m_job_hooks.presented(generated);
    const double call = now_seconds() - before;
    m_window.callSum += call;
    m_window.callMax = std::max(m_window.callMax, call);
    return hr;
}

HRESULT Presenter::present_job(PresentJob& job) noexcept
{
    // Whether the frame was already finished on the GPU before the presenter got
    // to it. Then its readiness time is unknown (only that it was earlier), and
    // using the time we got here would count queueing behind earlier frames as
    // GPU latency, which feeds back into later flips and a longer queue.
    const bool alreadyReady = job.ready->GetCompletedValue() >= job.readyValue;
    const double dequeued = now_seconds();
    // Nothing for this frame may start on the present queue before the game's
    // frame and its generation work are done.
    HRESULT hr = m_queue->Wait(job.ready.Get(), job.readyValue);
    if (FAILED(hr)) return hr;

    // VRR evidence: after more than one refresh without a flip, a VRR panel sits
    // in its blank waiting; a fixed-refresh panel is in blank ~5% of the time.
    if (m_last_flip > 0.0 && now_seconds() - m_last_flip > m_period + 0.0005) {
        bool blank = false;
        UINT line = 0;
        if (read_scanline(blank, line)) {
            ++m_window.idleSamples;
            if (blank) ++m_window.idleBlank;
        }
    }

    for (uint32_t i = 0; i < job.count; ++i) {
        const JobOutput& output = job.outputs[i];
        ID3D12Resource* source = output.real ? job.real.Get() : output.image.Get();
        const D3D12_RESOURCE_STATES state = output.real ? D3D12_RESOURCE_STATE_PRESENT
                                                        : D3D12_RESOURCE_STATE_COPY_SOURCE;
        const double gpuStart = now_seconds();
        hr = copy_to_native(source, state, !output.real);
        if (SUCCEEDED(hr)) hr = wait_fence(m_fence_value);
        if (FAILED(hr)) return hr;
        const double ready = now_seconds();
        if (i == 0) {
            const double gpuWait = ready - gpuStart;
            ++m_window.gpuWaits;
            m_window.gpuWaitSum += gpuWait;
            m_window.gpuWaitMax = std::max(m_window.gpuWaitMax, gpuWait);
            // The earliest output of a frame is the one that bounds latency: it
            // can only be shown once the frame after it has been rendered.
            if (alreadyReady) track_latency(job.entry - output.content, dequeued - job.entry, false);
            else track_latency(job.entry - output.content, ready - job.entry, true);
        }

        // Each output keeps its place on the content clock, so on-screen
        // spacing follows the game's frame times and the schedule's grid.
        const double target = output.content + m_latency;
        if (later_output_due(job, i, ready)) {
            // A later output is already due: showing this one now would put the
            // whole schedule behind. Its copy is overwritten by the next one.
            if (output.real) ++m_window.realsDropped;
            else { ++m_generated_dropped; ++m_window.generatedDropped; }
            continue;
        }
        hold(target);
        const double flip = now_seconds();
        if (flip > target + 0.001) ++m_window.late;
        hr = native_present(job, !output.real);
        if (FAILED(hr)) return hr;
        if (output.real) ++m_window.realsShown;
        else { ++m_generated_shown; ++m_window.generatedShown; }
    }

    // Retirement point for drain_gpu; also covers DXGI's own present work.
    const HRESULT signaled = m_queue->Signal(m_fence.Get(), ++m_fence_value);
    if (FAILED(signaled)) return signaled;
    return hr;
}

void Presenter::track_latency(double structural, double gpu, bool measured) noexcept
{
    // Both parts are short windows rather than slow-decay averages: a single
    // hitch must not hold the latency up for long, or each frame's flips take
    // longer, the game blocks on the presenter, its frames get longer and the
    // latency stays up (finding 009, second session).
    //
    // Structural: the schedule's own offset, the largest of the last 8 frames,
    // which covers the alternating pattern of every fixed ratio.
    if (std::isfinite(structural)) {
        m_structural_ring[m_structural_next] = std::clamp(structural, 0.0, 0.25);
        m_structural_next = (m_structural_next + 1) % kStructuralWindow;
        m_structural = *std::max_element(std::begin(m_structural_ring), std::end(m_structural_ring));
    }
    // GPU: the median of the last 16 measurements. A slower-than-usual frame is
    // then simply late (flipped as soon as it is ready) instead of raising the
    // latency of the next 16; the second-highest estimate stacked with the
    // structural maximum reached 45-60 ms at 40 real fps (finding 009, adaptive).
    // A frame that was already finished when the presenter got to it only
    // bounds the GPU time from above: it is recorded only when it lowers the
    // estimate, never to raise it, which would count queueing as latency.
    if (std::isfinite(gpu)) {
        gpu = std::clamp(gpu, 0.0, 0.1);
        if (measured || gpu < m_gpu_latency) {
            m_gpu_ring[m_gpu_next] = gpu;
            m_gpu_next = (m_gpu_next + 1) % kGpuWindow;
            double sorted[kGpuWindow];
            std::copy(std::begin(m_gpu_ring), std::end(m_gpu_ring), sorted);
            std::nth_element(sorted, sorted + kGpuWindow / 2, sorted + kGpuWindow);
            m_gpu_latency = sorted[kGpuWindow / 2];
        }
    }
    m_latency = m_structural + m_gpu_latency + 0.001;
    m_window.latencySum += m_latency;
    ++m_window.latencySamples;
}

bool Presenter::later_output_due(const PresentJob& job, uint32_t index, double now) noexcept
{
    if (index + 1 < job.count)
        return later_output_is_due(true, job.outputs[index + 1].content, m_latency, now);
    std::lock_guard lock(m_mutex);
    if (m_jobs.empty() || m_jobs.front().count == 0)
        return later_output_is_due(false, 0.0, m_latency, now);
    return later_output_is_due(true, m_jobs.front().outputs[0].content, m_latency, now);
}

void Presenter::run() noexcept
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    SetThreadDescription(GetCurrentThread(), L"Sedici FG presenter");
    for (;;) {
        PresentJob job;
        {
            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, [&] { return m_stop || !m_jobs.empty(); });
            if (m_cancel.load(std::memory_order_acquire)) {
                // Cancelled (the swapchain is being parked or torn down): jobs that
                // never started are discarded and counted as completed. Leaving them
                // outstanding would keep outstanding() above zero forever, so a
                // parked swapchain could never be reaped (K48).
                while (!m_jobs.empty()) {
                    m_jobs.pop_front();
                    ++m_completed;
                }
                lock.unlock();
                m_done.notify_all();
                return;
            }
            if (m_jobs.empty()) return;
            job = std::move(m_jobs.front());
            m_jobs.pop_front();
            m_job_hooks = m_hooks;
        }
        HRESULT hr = E_FAIL;
        try { hr = present_job(job); } catch (...) { hr = E_FAIL; }
        // Drop the buffer references before reporting completion: resize checks
        // that nobody but the wrapper holds a backbuffer once we are idle.
        job = PresentJob{};
        if (++m_window.jobs >= 300) log_window();
        {
            std::lock_guard lock(m_mutex);
            m_last = hr;
            ++m_completed;
        }
        m_done.notify_all();
    }
}

void Presenter::log_window() noexcept try
{
    auto& w = m_window;
    const auto avg = [](double sum, uint32_t n) { return n ? sum / n : 0.0; };
    log_event(1, "fg: vrr present jobs=" + std::to_string(w.jobs) +
        " generated{shown=" + std::to_string(w.generatedShown) + " dropped=" + std::to_string(w.generatedDropped) + "}" +
        " real{shown=" + std::to_string(w.realsShown) + " dropped=" + std::to_string(w.realsDropped) + "}" +
        " late=" + std::to_string(w.late) +
        " latency_ms=" + ms(avg(w.latencySum, w.latencySamples)) +
        " short_gaps=" + std::to_string(w.shortGaps) + "/" + std::to_string(w.flips) +
        " back_to_back=" + std::to_string(w.backToBack) +
        " gap_min_ms=" + ms(w.gapMin) +
        " gpu_wait_ms{avg=" + ms(avg(w.gpuWaitSum, w.gpuWaits)) + " max=" + ms(w.gpuWaitMax) + "}" +
        " guard{holds=" + std::to_string(w.guardHolds) + " avg_ms=" + ms(avg(w.guardSum, w.guardHolds)) +
        " max_ms=" + ms(w.guardMax) + " timeouts=" + std::to_string(w.guardTimeouts) + "}" +
        " present_call_ms{avg=" + ms(avg(w.callSum, w.flips)) + " max=" + ms(w.callMax) + "}" +
        " scan_at_flip{blank=" + std::to_string(w.scan[0]) + " q1=" + std::to_string(w.scan[1]) +
        " q2=" + std::to_string(w.scan[2]) + " q3=" + std::to_string(w.scan[3]) +
        " q4=" + std::to_string(w.scan[4]) + " unread=" + std::to_string(w.scan[5]) + "}" +
        " idle_in_blank=" + std::to_string(w.idleBlank) + "/" + std::to_string(w.idleSamples));
    w = Window{};
}
catch (...) { m_window = Window{}; }

} // namespace sedici::fg
