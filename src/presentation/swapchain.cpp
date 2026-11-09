#include "presentation/swapchain.h"
#include "diagnostics/bookkeeping_faults.h"
#include "gpu/retirement_policy.h"
#include "gpu/slot_policy.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <new>
#include <thread>

#include "diagnostics/log.h"
#include "settings/config.h"
#include "settings/live.h"
#include "settings/target_rate.h"
#include "presentation/limiter.h"
#include "diagnostics/stats.h"

namespace sedici::fg {

namespace {

uint64_t ticks_per_second() noexcept
{
    static const uint64_t frequency = [] {
        LARGE_INTEGER value{};
        return QueryPerformanceFrequency(&value) ? static_cast<uint64_t>(value.QuadPart) : uint64_t{0};
    }();
    return frequency;
}

double now_seconds() noexcept
{
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return ticks_to_seconds(static_cast<uint64_t>(counter.QuadPart), ticks_per_second());
}

uint64_t now_ticks() noexcept
{
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return static_cast<uint64_t>(counter.QuadPart);
}

bool supported_color_format(DXGI_FORMAT format) noexcept
{
    return format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_R10G10B10A2_UNORM;
}

} // namespace

HRESULT Swapchain::wrap(IDXGISwapChain4* native, ID3D12CommandQueue* queue,
                        Swapchain** result, const PresentChoice* choice, ULONG handedRefs) noexcept
{
    if (!result) return E_POINTER;
    *result = nullptr;
    reap_retained();
    if (!native || !queue || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return E_INVALIDARG;
    auto* object = new (std::nothrow) Swapchain;
    if (!object) return E_OUTOFMEMORY;
    object->m_native = native;
    object->m_queue = queue;
    HRESULT hr = queue->GetDevice(IID_PPV_ARGS(&object->m_device));
    if (SUCCEEDED(hr)) {
        Microsoft::WRL::ComPtr<ID3D12Device> nativeDevice;
        hr = native->GetDevice(IID_PPV_ARGS(&nativeDevice));
        if (SUCCEEDED(hr) && nativeDevice.Get() != object->m_device.Get()) {
            // Alan Wake 2 reports a different device object for the chain than for its
            // queue (K130). The same adapter is enough: every resource is created on, and
            // every list is submitted through, the queue's device. A different adapter is
            // refused. The decision is logged either way.
            const LUID chainAdapter = nativeDevice->GetAdapterLuid();
            const LUID queueAdapter = object->m_device->GetAdapterLuid();
            const bool sameAdapter = chainAdapter.LowPart == queueAdapter.LowPart &&
                                     chainAdapter.HighPart == queueAdapter.HighPart;
            char text[200];
            std::snprintf(text, sizeof(text),
                "fg: swapchain wrap: chain device %p differs from queue device %p; %s",
                static_cast<void*>(nativeDevice.Get()), static_cast<void*>(object->m_device.Get()),
                sameAdapter ? "same adapter, using the queue's device" : "different adapters, refused");
            log_error(text);
            if (!sameAdapter) hr = E_INVALIDARG;
        }
    }
    if (SUCCEEDED(hr)) hr = object->m_device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&object->m_allocator[0]));
    if (SUCCEEDED(hr)) hr = object->m_device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&object->m_allocator[1]));
    if (SUCCEEDED(hr)) hr = object->m_device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&object->m_generate_allocator));
    if (SUCCEEDED(hr)) hr = object->m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        object->m_allocator[0].Get(), nullptr, IID_PPV_ARGS(&object->m_list[0]));
    if (SUCCEEDED(hr)) hr = object->m_list[0]->Close();
    if (SUCCEEDED(hr)) hr = object->m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        object->m_allocator[1].Get(), nullptr, IID_PPV_ARGS(&object->m_list[1]));
    if (SUCCEEDED(hr)) hr = object->m_list[1]->Close();
    if (SUCCEEDED(hr)) hr = object->m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        object->m_generate_allocator.Get(), nullptr, IID_PPV_ARGS(&object->m_generate_list));
    if (SUCCEEDED(hr)) hr = object->m_generate_list->Close();
    if (SUCCEEDED(hr)) hr = object->m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
        IID_PPV_ARGS(&object->m_fence));
    if (SUCCEEDED(hr)) {
        object->m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!object->m_event) hr = HRESULT_FROM_WIN32(GetLastError());
    }
    if (FAILED(hr)) { delete object; return hr; }

    using Present = sedici::FrameGenerationSettings::Present;
    PresentChoice chosen;
    if (choice) {
        chosen = *choice;
    } else {
        chosen.present = sedici::settings().fg.present;
        chosen.guard = sedici::settings().fg.tear_guard;
        chosen.multiplier = sedici::settings().fg.multiplier;
        chosen.targetFps = sedici::settings().fg.target_fps;
    }
    std::string why;
    if (chosen.present == Present::Vrr && GetModuleHandleW(L"RTSSHooks64.dll") &&
        GetEnvironmentVariableW(L"SEDICI_FG_ALLOW_RTSS", nullptr, 0) == 0) {
        // RTSS draws its overlay into the back buffer from whichever queue it
        // last saw, not the one the chain presents on. With a separate present
        // queue that removed the device in the offline check. Do not risk it.
        why = "RTSS is injected and would write the chain from the wrong queue; "
              "close RTSS or set SEDICI_FG_ALLOW_RTSS=1";
        chosen.present = Present::Vsync;
    }
    if (chosen.present == Present::Vrr) {
        object->m_presenter = Presenter::create(object->m_device.Get(), chosen.guard, why);
        if (!object->m_presenter) chosen.present = Present::Vsync;
        else if (handedRefs == 0) {
            why = "the caller keeps its swapchain reference, so the chain cannot be recreated";
            object->m_presenter.reset();
            chosen.present = Present::Vsync;
        }
    }
    // Everything fallible happens before the chain is recreated. The buffers
    // only depend on the description, which recreation keeps.
    hr = object->initialize_buffers();
    if (FAILED(hr)) { delete object; return hr; }
    bool originalKept = true; // still presenting through the chain the caller handed over
    if (object->m_presenter) {
        IDXGISwapChain4* const original = object->m_native.Get();
        hr = object->m_presenter->adopt(object->m_native, handedRefs + 1, queue, why);
        if (SUCCEEDED(hr)) originalKept = false;
        if (FAILED(hr)) {
            object->m_presenter.reset();
            for (auto& set : object->m_phase_outputs)
                for (auto& output : set) output.Reset();
            chosen.present = Present::Vsync;
            if (!object->m_native) {
                // The original chain is gone and nothing could replace it.
                log_error("fg: \"vrr\" swapchain recreation failed (" + why + ")");
                object->m_buffers.clear();
                delete object;
                return hr;
            }
            if (object->m_native.Get() != original) {
                originalKept = false;
                log_error("fg: \"vrr\" chain recreated on the game queue instead (" + why + ")");
            }
        }
    }
    object->configure_schedule(chosen);
    stats_on_swapchain_event("create");
    if (chosen.present == Present::Vsync && !why.empty()) {
        // Never fail the game's wrap over this: present the safe way.
        log_error("fg: \"vrr\" present unavailable (" + why + "); falling back to \"vsync\"");
    }
    // `[fg] present = "vsync"` takes one vertical blank per output,
    // which is what stopped mid-scan tearing in the game session (finding 008).
    // Legacy A/B switch: `SEDICI_FG_SYNC=0/1` overrides "vsync"/"game".
    object->m_force_sync = chosen.present == Present::Vsync;
    {
        wchar_t syncEnv[8]{};
        if (!object->m_presenter && GetEnvironmentVariableW(L"SEDICI_FG_SYNC", syncEnv, 8) > 0)
            object->m_force_sync = syncEnv[0] == L'1';
        wchar_t alignEnv[8]{};
        if (GetEnvironmentVariableW(L"SEDICI_FG_ALIGN", alignEnv, 8) > 0 && alignEnv[0] == L'1')
            object->m_align_vblank = true;
    }
    // Record the native chain as the game created it. Two presents per cycle are
    // driven into this chain, so its depth and latency setting bound how many can
    // be in flight; the values are logged once and never used to change behaviour.
    try {
        DXGI_SWAP_CHAIN_DESC1 desc{};
        UINT latency = 0;
        if (SUCCEEDED(object->m_native->GetDesc1(&desc))) {
            (void)object->m_native->GetMaximumFrameLatency(&latency);
            const bool waitable = object->m_native->GetFrameLatencyWaitableObject() != nullptr;
            log_event(1, "fg: native chain buffers=" + std::to_string(desc.BufferCount) +
                " size=" + std::to_string(desc.Width) + "x" + std::to_string(desc.Height) +
                " flags=" + std::to_string(desc.Flags) +
                " max_latency=" + std::to_string(latency) +
                " waitable=" + (waitable ? "yes" : "no") +
                " present=" + (object->m_presenter ? "vrr" : object->m_force_sync ? "vsync" : "game") +
                " align_vblank=" + (object->m_align_vblank ? "yes" : "no"));
        }
    } catch (...) {}
    // Timer availability affects precision, never whether wrapping succeeds.
    object->m_pacing_timer = CreateWaitableTimerExW(nullptr, nullptr,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!object->m_pacing_timer)
        object->m_pacing_timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    // K12: the caller handed `handedRefs` references to this wrapper. While the
    // original chain is still the one presented through (vsync, game, or a vrr
    // fallback), m_native holds the wrapper's own reference, so take over the
    // caller's: without this the native chain outlived the wrapper and a new chain
    // could not be created on the same window. A recreated chain has no such
    // references (the original and its handed references are already gone).
    if (originalKept)
        for (ULONG i = 0; i < handedRefs; ++i) native->Release();
    *result = object;
    return S_OK;
}

Swapchain::~Swapchain()
{
    const HRESULT drained = drain();
    if (FAILED(drained) && m_device && SUCCEEDED(m_device->GetDeviceRemovedReason())) {
        // The device is alive but its work did not retire within the bounded wait.
        // A timeout is not permission to free memory the GPU may still be using:
        // retain every GPU object (and the presenter, which owns the native
        // chain) for the life of the process and say so (K39). On a removed
        // device nothing more can execute, so the normal release below is safe.
        retain_gpu_state();
        return;
    }
    m_presenter.reset();
    if (m_event) CloseHandle(m_event);
    if (m_pacing_timer) CloseHandle(m_pacing_timer);
}

namespace {
std::mutex g_retained_mutex;
Swapchain* g_retained_head = nullptr; // intrusive: parking never allocates
std::atomic<unsigned> g_retained_count{0};
}

bool Swapchain::retirable() const noexcept
{
    if (!m_device || FAILED(m_device->GetDeviceRemovedReason())) return true; // nothing can run any more
    if (FAILED(m_submission_failure)) return false; // untracked work: no token covers it
    if (m_presenter && m_presenter->outstanding() != 0) return false;
    return !m_fence || m_fence->GetCompletedValue() >= m_fence_value;
}

void Swapchain::park() noexcept
{
    note_swapchain_retention();
    if (m_presenter) m_presenter->request_stop();
    {
        std::lock_guard lock(g_retained_mutex);
        m_retained_next = g_retained_head;
        g_retained_head = this;
    }
    g_retained_count.fetch_add(1, std::memory_order_release);
    try { log_event(1, "fg: swapchain parked after a failed drain; it is released once its GPU work retires"); } catch (...) {}
}

unsigned Swapchain::retained_now() noexcept { return g_retained_count.load(std::memory_order_acquire); }

void Swapchain::reap_retained() noexcept
{
    if (!g_retained_count.load(std::memory_order_acquire)) return;
    for (;;) {
        Swapchain* ready = nullptr;
        {
            std::lock_guard lock(g_retained_mutex);
            for (Swapchain** link = &g_retained_head; *link; link = &(*link)->m_retained_next) {
                if ((*link)->retirable()) {
                    ready = *link;
                    *link = ready->m_retained_next;
                    ready->m_retained_next = nullptr;
                    break;
                }
            }
        }
        if (!ready) return;
        g_retained_count.fetch_sub(1, std::memory_order_release);
        note_swapchain_release();
        delete ready; // its fence has retired, so the destructor's drain returns at once
        try { log_event(1, "fg: a parked swapchain was released after its GPU work retired"); } catch (...) {}
    }
}

void Swapchain::retain_gpu_state() noexcept
{
    note_swapchain_retention();
    // Transfer each existing COM reference without allocating. Leaking a
    // reference is preferable to releasing GPU-visible memory after a timeout.
    // This is still unbounded retention, not a completed P2.5 recovery design.
    const auto hold = [](auto& pointer) { (void)pointer.Detach(); };
    if (m_presenter) m_presenter->request_stop();
    hold(m_native);
    hold(m_queue);
    hold(m_device);
    for (auto& v : m_allocator) hold(v);
    for (auto& v : m_list) hold(v);
    hold(m_generate_allocator);
    hold(m_generate_list);
    hold(m_fence);
    for (auto& v : m_buffers) hold(v);
    hold(m_generated);
    for (auto& set : m_phase_outputs) for (auto& v : set) hold(v);
    for (auto& v : m_snapshots) hold(v);
    for (auto& v : m_snapshot_allocators) hold(v);
    for (auto& v : m_snapshot_lists) hold(v);
    for (auto& v : m_slot_generate_allocators) hold(v);
    for (auto& v : m_slot_generate_lists) hold(v);
    (void)m_presenter.release(); // worker may be inside a driver call
    // A pending fence completion may still signal m_event; do not close it.
    if (m_pacing_timer) CloseHandle(m_pacing_timer);
    try { log_event(1, "fg: swapchain GPU state retained after failed drain"); } catch (...) {}

}

HRESULT Swapchain::interpolation_list(ID3D12GraphicsCommandList** list) noexcept try
{
    if (!list) return E_POINTER;
    std::lock_guard lock(m_scratch_mutex);
    if (!m_scratch_list) {
        HRESULT hr = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                      IID_PPV_ARGS(&m_scratch_allocator));
        if (SUCCEEDED(hr)) hr = m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            m_scratch_allocator.Get(), nullptr, IID_PPV_ARGS(&m_scratch_list));
        if (FAILED(hr)) { m_scratch_list.Reset(); m_scratch_allocator.Reset(); return hr; }
        *list = m_scratch_list.Get(); // created open
        return S_OK;
    }
    // The previous recording was never executed; start it over.
    (void)m_scratch_list->Close();
    HRESULT hr = m_scratch_allocator->Reset();
    if (SUCCEEDED(hr)) hr = m_scratch_list->Reset(m_scratch_allocator.Get(), nullptr);
    if (FAILED(hr)) return hr;
    *list = m_scratch_list.Get();
    return S_OK;
}
catch (...) { return E_OUTOFMEMORY; }

HRESULT Swapchain::interpolation_texture(ID3D12Resource** texture) noexcept try
{
    if (!texture) return E_POINTER;
    std::lock_guard lock(m_scratch_mutex);
    if (m_scratch_format == DXGI_FORMAT_UNKNOWN) return DXGI_ERROR_NOT_FOUND; // buffers not created yet
    if (m_scratch_texture) {
        const D3D12_RESOURCE_DESC have = m_scratch_texture->GetDesc();
        if (have.Width == m_scratch_width && have.Height == m_scratch_height && have.Format == m_scratch_format) {
            *texture = m_scratch_texture.Get();
            return S_OK;
        }
        // A resize changed the chain. The game may still hold the old pointer for a frame.
        m_scratch_retired.push_back(std::move(m_scratch_texture));
    }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC texDesc{};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = m_scratch_width;
    texDesc.Height = m_scratch_height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = m_scratch_format;
    texDesc.SampleDesc.Count = 1;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const HRESULT hr = m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_scratch_texture));
    if (FAILED(hr)) return hr;
    *texture = m_scratch_texture.Get();
    return S_OK;
}
catch (...) { return E_OUTOFMEMORY; }

bool Swapchain::set_generator(const GenerateSink& sink) noexcept
{
    std::lock_guard lock(m_mutex);
    if (m_generator_closed) return false;
    if (sink.record != m_sink.record || sink.user != m_sink.user) {
        m_generated_ready = false;
        m_pacing = {};
    }
    m_sink = sink;
    return true;
}

bool Swapchain::clear_generator_if_owner(const void* owner) noexcept
{
    std::lock_guard lock(m_mutex);
    if (m_generator_closed || !owner || m_sink.user != owner) return false;
    m_sink = {};
    m_generated_ready = false;
    m_pacing = {};
    return true;
}

void Swapchain::close_generator() noexcept
{
    std::lock_guard lock(m_mutex);
    m_generator_closed = true;
    m_sink = {};
    m_generated_ready = false;
    m_pacing = {};
}

void Swapchain::set_present_hook(void (*hook)(uint64_t)) noexcept
{
    std::lock_guard lock(m_mutex);
    m_present_hook = hook;
}

void Swapchain::set_output_hooks(const OutputHooks& hooks) noexcept
{
    std::lock_guard lock(m_mutex);
    m_output_hooks = hooks;
    if (m_presenter) m_presenter->set_output_hooks(hooks);
}

HRESULT Swapchain::initialize_buffers() noexcept try
{
    DXGI_SWAP_CHAIN_DESC1 desc{};
    HRESULT hr = m_native->GetDesc1(&desc);
    if (FAILED(hr)) return hr;
    if (desc.SampleDesc.Count != 1 || desc.BufferCount == 0 || desc.Stereo ||
        !supported_color_format(desc.Format)) return DXGI_ERROR_UNSUPPORTED;

    {
        std::lock_guard scratch(m_scratch_mutex);
        m_scratch_format = desc.Format;
        m_scratch_width = desc.Width;
        m_scratch_height = desc.Height;
    }
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> buffers(desc.BufferCount);
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC texture{};
    texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width = desc.Width;
    texture.Height = desc.Height;
    texture.DepthOrArraySize = 1;
    texture.MipLevels = 1;
    texture.Format = desc.Format;
    texture.SampleDesc = desc.SampleDesc;
    texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = desc.Format;
    for (auto& buffer : buffers) {
        hr = m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &texture,
            D3D12_RESOURCE_STATE_PRESENT, &clear, IID_PPV_ARGS(&buffer));
        if (FAILED(hr)) return hr;
    }

    // The generated image is written by a compute pass and copied to a native
    // backbuffer, so it needs UAV access and never needs to be a render target.
    D3D12_RESOURCE_DESC generated = texture;
    generated.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    Microsoft::WRL::ComPtr<ID3D12Resource> midpoint;
    Microsoft::WRL::ComPtr<ID3D12Resource> phases[kSlots][kMaxPhases];
    Microsoft::WRL::ComPtr<ID3D12Resource> snapshots[kSlots];
    if (m_presenter) {
        for (auto& set : phases) {
            for (auto& output : set) {
                hr = m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &generated,
                    D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, IID_PPV_ARGS(&output));
                if (FAILED(hr)) return hr;
            }
        }
        // Copies of the game's frame for the presenter: copy source/dest only,
        // kept in PRESENT like the game's buffers so the presenter treats both alike.
        D3D12_RESOURCE_DESC snapshot = texture;
        snapshot.Flags = D3D12_RESOURCE_FLAG_NONE;
        for (auto& image : snapshots) {
            hr = m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &snapshot,
                D3D12_RESOURCE_STATE_PRESENT, nullptr, IID_PPV_ARGS(&image));
            if (FAILED(hr)) return hr;
        }
        const auto makeList = [&](Microsoft::WRL::ComPtr<ID3D12CommandAllocator>& allocator,
                                  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList>& list) {
            if (list) return S_OK;
            HRESULT result = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                              IID_PPV_ARGS(&allocator));
            if (SUCCEEDED(result)) result = m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                allocator.Get(), nullptr, IID_PPV_ARGS(&list));
            if (SUCCEEDED(result)) result = list->Close();
            return result;
        };
        for (uint32_t i = 0; i < kSlots; ++i) {
            hr = makeList(m_snapshot_allocators[i], m_snapshot_lists[i]);
            if (SUCCEEDED(hr)) hr = makeList(m_slot_generate_allocators[i], m_slot_generate_lists[i]);
            if (FAILED(hr)) return hr;
        }
    } else {
        hr = m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &generated,
            D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, IID_PPV_ARGS(&midpoint));
        if (FAILED(hr)) return hr;
    }

    m_buffers = std::move(buffers);
    m_generated = midpoint;
    for (uint32_t set = 0; set < kSlots; ++set) {
        for (uint32_t i = 0; i < kMaxPhases; ++i) m_phase_outputs[set][i] = phases[set][i];
        m_snapshots[set] = snapshots[set];
    }
    m_phase_set = 0;
    m_schedule.reset();
    m_generated_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
    m_generated_ready = false;
    m_game_index = 0;
    return S_OK;
}
catch (...) { return E_OUTOFMEMORY; }

void Swapchain::hold_untracked_generator() noexcept
{
    if (!m_sink.user || !m_sink.submitted || !m_sink.lifetime || !m_sink.needsSubmission ||
        !m_sink.needsSubmission(m_sink.user)) return;
    // Admission is serialized by m_mutex; an unresolved owner is never replaced.
    if (m_untracked_sink.user) return;
    m_untracked_sink = m_sink;
    if (m_untracked_sink.abandoned) m_untracked_sink.abandoned(m_untracked_sink.user);
}

bool Swapchain::resolve_untracked_generator() noexcept
{
    if (!m_untracked_sink.user || !m_untracked_sink.submitted) return true;
    if (!m_untracked_sink.submitted(m_untracked_sink.user, m_fence.Get(), m_fence_value))
        return false; // Keep the owner lease; no retirement was acknowledged.
    m_untracked_sink = {};
    return true;
}

HRESULT Swapchain::drain() noexcept
{
    if (m_presenter) {
        if (!m_presenter->wait_outstanding(0, 5000)) return DXGI_ERROR_DEVICE_HUNG;
        const HRESULT hr = m_presenter->drain_gpu();
        if (FAILED(hr)) return hr;
    }
    if (!m_fence || !m_queue) return S_OK;
    if (FAILED(m_submission_failure)) {
        // A failed signal left submitted wrapper work without a retirement
        // token. A later queue-signalled fence can cover that work. Keep frame
        // admission closed even if this bounded teardown attempt succeeds; a
        // Session with its own untracked work is not declared retired here.
        const HRESULT recovery = m_queue->Signal(m_fence.Get(), ++m_fence_value);
        if (FAILED(recovery)) return recovery;
        if (!resolve_untracked_generator()) return E_FAIL;
        run_queue_recovery_hook(); // Unarmed outside the offline test host.
    }
    if (m_fence_value == 0) return S_OK;
    auto progress = classify_fence(m_fence->GetCompletedValue(), m_fence_value);
    if (progress == FenceProgress::DeviceRemoved)
        return removed_device_error(m_device->GetDeviceRemovedReason());
    if (progress == FenceProgress::Complete) return S_OK;
    const HRESULT hr = m_fence->SetEventOnCompletion(m_fence_value, m_event);
    if (FAILED(hr)) return hr;
    // Bounded wait: a removed/hung device must not trap teardown forever.
    const DWORD wait = WaitForSingleObject(m_event, 5000);
    if (wait != WAIT_OBJECT_0)
        return wait == WAIT_TIMEOUT ? DXGI_ERROR_DEVICE_HUNG : HRESULT_FROM_WIN32(GetLastError());
    progress = classify_fence(m_fence->GetCompletedValue(), m_fence_value);
    if (progress == FenceProgress::DeviceRemoved)
        return removed_device_error(m_device->GetDeviceRemovedReason());
    return progress == FenceProgress::Complete ? S_OK : E_FAIL;
}

void Swapchain::wait_until(double deadline, double maxWait) noexcept{
    const double now = now_seconds();
    if (!std::isfinite(deadline) || !(maxWait > 0.0)) return;
    const double remaining = std::clamp(deadline - now, 0.0, std::min(maxWait, 0.05));
    if (!(remaining > 0.0)) return;
    // Sleep most of the bounded wait, then finish against the absolute QPC
    // deadline. A relative sleep after Present would add its API time to the gap.
    constexpr double spin = 0.00025;
    const double target = now + remaining;
    if (!ticks_per_second()) {
        std::this_thread::sleep_for(std::chrono::duration<double>(remaining));
        return;
    }
    if (remaining > spin) {
        LARGE_INTEGER due{};
        due.QuadPart = -std::max<LONGLONG>(1, static_cast<LONGLONG>((remaining - spin) * 1e7));
        if (m_pacing_timer && SetWaitableTimer(m_pacing_timer, &due, 0, nullptr, nullptr, FALSE)) {
            if (WaitForSingleObject(m_pacing_timer, static_cast<DWORD>(remaining * 1000.0) + 5)
                != WAIT_OBJECT_0) return;
        } else {
            std::this_thread::sleep_for(std::chrono::duration<double>(remaining - spin));
        }
    }
    while (now_seconds() < target) YieldProcessor();
}

double Swapchain::vblank_deadline(double now) noexcept
{
    if (!m_align_vblank || !ticks_per_second()) return now;
    DXGI_FRAME_STATISTICS stats{};
    if (SUCCEEDED(m_native->GetFrameStatistics(&stats))) {
        const double stamp = ticks_to_seconds(static_cast<uint64_t>(stats.SyncQPCTime.QuadPart), ticks_per_second());
        if (!m_have_vblank) {
            m_have_vblank = true;
            m_vblank_count = stats.SyncRefreshCount;
            m_vblank_qpc = static_cast<uint64_t>(stats.SyncQPCTime.QuadPart);
        } else if (stats.SyncRefreshCount != m_vblank_count) {
            const uint32_t steps = stats.SyncRefreshCount - m_vblank_count;
            const double epoch = ticks_to_seconds(m_vblank_qpc, ticks_per_second());
            if (steps && stamp > epoch) {
                const double period = (stamp - epoch) / steps;
                if (period > 0.004 && period < 0.05) m_vblank_period = period;
            }
            m_vblank_count = stats.SyncRefreshCount;
            m_vblank_qpc = static_cast<uint64_t>(stats.SyncQPCTime.QuadPart);
        }
    }
    if (!m_have_vblank) return now;
    const double epoch = ticks_to_seconds(m_vblank_qpc, ticks_per_second());
    const double period = m_vblank_period;
    // Arm shortly before the next blank so the flip lands at the scan start.
    constexpr double margin = 0.0012;
    const double lead = now + margin;
    double target = epoch;
    if (lead >= epoch) target = epoch + std::ceil((lead - epoch) / period) * period;
    return target - margin;
}

HRESULT Swapchain::blit(ID3D12Resource* source, D3D12_RESOURCE_STATES* sourceState, bool generated) noexcept
{
    if (!source || !sourceState) return E_INVALIDARG;
    Microsoft::WRL::ComPtr<ID3D12Resource> destination;
    HRESULT hr = m_native->GetBuffer(m_native->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&destination));
    if (FAILED(hr)) return hr;
    // Alternating allocators: both copies of a cycle execute before either can be
    // reset, so they must not share one.
    const uint32_t slot = m_next_list;
    m_next_list ^= 1u;
    auto* allocator = m_allocator[slot].Get();
    auto* list = m_list[slot].Get();
    hr = allocator->Reset();
    if (SUCCEEDED(hr)) hr = list->Reset(allocator, nullptr);
    if (FAILED(hr)) return hr;

    const D3D12_RESOURCE_STATES restore = *sourceState;
    const bool sourceTransition = restore != D3D12_RESOURCE_STATE_COPY_SOURCE;

    D3D12_RESOURCE_BARRIER barriers[2]{};
    UINT count = 0;
    const auto add = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                         D3D12_RESOURCE_STATES after) {
        if (before == after) return;
        auto& barrier = barriers[count++];
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.pResource = resource;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
    };
    add(source, restore, D3D12_RESOURCE_STATE_COPY_SOURCE);
    add(destination.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    if (count) list->ResourceBarrier(count, barriers);
    list->CopyResource(destination.Get(), source);

    count = 0;
    add(destination.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    if (sourceTransition) add(source, D3D12_RESOURCE_STATE_COPY_SOURCE, restore);
    if (count) list->ResourceBarrier(count, barriers);
    // The overlay draws on the final image, as it does from AMD's callback.
    if (m_output_hooks.draw) m_output_hooks.draw(m_device.Get(), list, destination.Get(), generated);

    hr = list->Close();
    if (FAILED(hr)) return hr;
    ID3D12CommandList* lists[]{list};
    m_queue->ExecuteCommandLists(1, lists);
    return S_OK;
}

double Swapchain::adaptive_target() const noexcept
{
    // Adaptive needs a rate to aim for: the one target, or auto when it is
    // uncapped (0). Without a known refresh rate keep the old safe fallback.
    const sedici::LiveValues live = sedici::live_snapshot();
    const double hz = display_refresh_hz();
    double target = sedici::resolve_target_fps(live.target_fps, live.target_auto, hz);
    if (target <= 0.0) target = sedici::auto_target_fps(hz);
    if (target <= 0.0) target = std::max(1.0, hz - 3.0);
    return target;
}

double Swapchain::display_refresh_hz() const noexcept
{
    double hz = 0.0;
    try {
        Microsoft::WRL::ComPtr<IDXGIOutput> output;
        DXGI_OUTPUT_DESC desc{};
        if (m_native && SUCCEEDED(m_native->GetContainingOutput(&output)) && output &&
            SUCCEEDED(output->GetDesc(&desc))) {
            DEVMODEW mode{};
            mode.dmSize = sizeof(mode);
            if (EnumDisplaySettingsW(desc.DeviceName, ENUM_CURRENT_SETTINGS, &mode) &&
                mode.dmDisplayFrequency > 1)
                hz = static_cast<double>(mode.dmDisplayFrequency);
        }
    } catch (...) {}
    if (hz <= 0.0 && m_presenter) hz = m_presenter->refresh_hz();
    return hz;
}

void Swapchain::publish_output_context() noexcept
{
    const double ratio = !m_presenter ? 2.0
        : m_schedule.adaptive() ? 1.0
        : static_cast<double>(m_schedule.numerator()) / static_cast<double>(m_schedule.denominator());
    const double hz = display_refresh_hz();
    if (m_presenter && m_schedule.adaptive()) {
        const double target = adaptive_target();
        if (std::fabs(target - m_schedule.target()) > 1e-9) m_schedule.set_target(target);
    }
    if (sedici::live_set_output_context(ratio, hz)) limiter_on_config_changed();
    try {
        log_event(2, "fg: output context ratio=" + std::to_string(ratio) + " refresh_hz=" + std::to_string(hz) +
                         " real_cap=" + std::to_string(sedici::live_limiter_fps()));
    } catch (...) {}
}

void Swapchain::configure_schedule(const PresentChoice& choice) noexcept
{
    using Multiplier = sedici::FrameGenerationSettings::Multiplier;
    if (!m_presenter) {
        if (choice.multiplier != Multiplier::X2)
            log_error(std::string("fg: multiplier \"") + sedici::multiplier_name(choice.multiplier) +
                      "\" needs present = \"vrr\"; presenting 2x");
        publish_output_context();
        return;
    }
    switch (choice.multiplier) {
    case Multiplier::X1_75: m_schedule.set_ratio(7, 4); break;
    case Multiplier::X1_5: m_schedule.set_ratio(3, 2); break;
    case Multiplier::X1_25: m_schedule.set_ratio(5, 4); break;
    case Multiplier::Adaptive:
        m_schedule.set_target(adaptive_target());
        break;
    case Multiplier::X2: m_schedule.set_ratio(2, 1); break;
    }
    try {
        log_event(1, std::string("fg: schedule multiplier=") + sedici::multiplier_name(choice.multiplier) +
            (m_schedule.adaptive() ? " target_fps=" + std::to_string(m_schedule.target())
                                   : " ratio=" + std::to_string(m_schedule.numerator()) + "/" +
                                         std::to_string(m_schedule.denominator())));
    } catch (...) {}
    publish_output_context();
}

HRESULT Swapchain::wait_slot(uint32_t slot) noexcept
{
    // The slot was last used kSlots frames ago; its lists and images are free
    // once that frame's ready fence (signalled after its generation and
    // snapshot) has completed. Normally it has, because the presenter finished
    // that frame before this one was allowed in.
    if (!m_fence || slot >= kSlots) return E_INVALIDARG;
    const UINT64 target = m_snapshot_fences[slot];
    auto progress = classify_fence(m_fence->GetCompletedValue(), target);
    if (progress == FenceProgress::DeviceRemoved)
        return removed_device_error(m_device->GetDeviceRemovedReason());
    if (progress == FenceProgress::Complete) return S_OK;
    const HRESULT hr = m_fence->SetEventOnCompletion(target, m_event);
    if (FAILED(hr)) return hr;
    const DWORD wait = WaitForSingleObject(m_event, 5000);
    if (wait != WAIT_OBJECT_0)
        return wait == WAIT_TIMEOUT ? DXGI_ERROR_DEVICE_HUNG : HRESULT_FROM_WIN32(GetLastError());
    progress = classify_fence(m_fence->GetCompletedValue(), target);
    if (progress == FenceProgress::DeviceRemoved)
        return removed_device_error(m_device->GetDeviceRemovedReason());
    return progress == FenceProgress::Complete ? S_OK : E_FAIL;
}

HRESULT Swapchain::snapshot_real(uint32_t slot) noexcept
{
    auto* allocator = m_snapshot_allocators[slot].Get();
    auto* list = m_snapshot_lists[slot].Get();
    HRESULT hr = allocator->Reset();
    if (SUCCEEDED(hr)) hr = list->Reset(allocator, nullptr);
    if (FAILED(hr)) return hr;
    ID3D12Resource* source = m_buffers[m_game_index].Get();
    ID3D12Resource* destination = m_snapshots[slot].Get();
    D3D12_RESOURCE_BARRIER barriers[2]{};
    const auto transition = [&](D3D12_RESOURCE_BARRIER& barrier, ID3D12Resource* resource,
                                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.pResource = resource;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
    };
    transition(barriers[0], source, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(barriers[1], destination, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    list->ResourceBarrier(2, barriers);
    list->CopyResource(destination, source);
    transition(barriers[0], source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    transition(barriers[1], destination, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    list->ResourceBarrier(2, barriers);
    hr = list->Close();
    if (FAILED(hr)) return hr;
    ID3D12CommandList* lists[]{list};
    m_queue->ExecuteCommandLists(1, lists);
    return S_OK;
}

bool Swapchain::record_phases(const GeneratePhase* phases, uint32_t count, uint32_t slot,
                              HRESULT& submissionFailure) noexcept
{
    submissionFailure = S_OK;
    if ((!m_sink.recordPhases && !m_sink.record) || count == 0 || slot >= kSlots) return false;
    auto* allocator = m_slot_generate_allocators[slot].Get();
    auto* list = m_slot_generate_lists[slot].Get();
    if (!allocator || !list) {
        submissionFailure = m_submission_failure = E_POINTER;
        return false;
    }
    HRESULT reset = consume_bookkeeping_fault(BookkeepingFault::GenerationReset)
        ? E_FAIL : allocator->Reset();
    if (SUCCEEDED(reset)) reset = list->Reset(allocator, nullptr);
    if (FAILED(reset)) {
        submissionFailure = m_submission_failure = reset;
        return false;
    }
    auto* real = m_buffers[m_game_index].Get();
    bool recorded = true;
    if (m_sink.recordPhases) {
        recorded = m_sink.recordPhases(m_sink.user, list, real, phases, count);
    } else {
        for (uint32_t i = 0; i < count && recorded; ++i)
            recorded = m_sink.record(m_sink.user, list, real, phases[i].output, phases[i].outputState);
    }
    // Even a generator refusal may leave a partially recorded list. A Close
    // failure is a submission-path error, not an ordinary skipped frame.
    const HRESULT closeResult = list->Close();
    const HRESULT closed = consume_bookkeeping_fault(BookkeepingFault::GenerationClose)
        ? E_FAIL : closeResult;
    if (FAILED(closed)) {
        hold_untracked_generator();
        submissionFailure = m_submission_failure = closed;
        return false;
    }
    if (!recorded) {
        hold_untracked_generator();
        return false;
    }
    ID3D12CommandList* lists[]{list};
    m_queue->ExecuteCommandLists(1, lists);
    ++m_fence_value;
    const HRESULT signaled = consume_bookkeeping_fault(BookkeepingFault::GenerationSignal)
        ? E_FAIL : m_queue->Signal(m_fence.Get(), m_fence_value);
    if (FAILED(signaled)) {
        hold_untracked_generator();
        submissionFailure = m_submission_failure = signaled;
        return false;
    }
    if (m_sink.submitted && !m_sink.submitted(m_sink.user, m_fence.Get(), m_fence_value)) {
        hold_untracked_generator();
        submissionFailure = m_submission_failure = E_FAIL;
        return false;
    }
    return true;
}

HRESULT Swapchain::record_generated() noexcept
{
    if (!m_sink.record || !m_generated || m_generated_ready) return S_OK;
    auto* allocator = m_generate_allocator.Get();
    auto* list = m_generate_list.Get();
    if (!allocator || !list) return m_submission_failure = E_POINTER;
    HRESULT reset = consume_bookkeeping_fault(BookkeepingFault::GenerationReset)
        ? E_FAIL : allocator->Reset();
    if (SUCCEEDED(reset)) reset = list->Reset(allocator, nullptr);
    if (FAILED(reset)) return m_submission_failure = reset;
    auto* real = m_buffers[m_game_index].Get();
    const bool recorded = m_sink.record(m_sink.user, list, real, m_generated.Get(), m_generated_state);
    const HRESULT closeResult = list->Close();
    const HRESULT closed = consume_bookkeeping_fault(BookkeepingFault::GenerationClose)
        ? E_FAIL : closeResult;
    if (FAILED(closed)) {
        hold_untracked_generator();
        ++m_skipped_generates;
        return m_submission_failure = closed;
    }
    if (!recorded) {
        hold_untracked_generator();
        ++m_skipped_generates;
        return S_OK;
    }
    ID3D12CommandList* lists[]{list};
    m_queue->ExecuteCommandLists(1, lists);
    ++m_fence_value;
    const HRESULT signaled = consume_bookkeeping_fault(BookkeepingFault::GenerationSignal)
        ? E_FAIL : m_queue->Signal(m_fence.Get(), m_fence_value);
    if (FAILED(signaled)) {
        hold_untracked_generator();
        ++m_skipped_generates;
        m_submission_failure = signaled;
        return signaled;
    }
    // The sink restores the state it was given; do not invent a new state here.
    if (m_sink.submitted && !m_sink.submitted(m_sink.user, m_fence.Get(), m_fence_value)) {
        hold_untracked_generator();
        return m_submission_failure = E_FAIL;
    }
    m_generated_ready = true;
    return S_OK;
}

HRESULT Swapchain::present(UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* params) noexcept
{
    reap_retained(); // one atomic load unless a timed-out swapchain is parked (K47)
    // K138: say when Present waits for the swapchain lock or runs long, so a stalled game thread
    // can be told apart from a deadlock. A call that never returns logs nothing, so the
    // periodic lines going quiet while prepares continue is the other half of the evidence.
    struct PresentTimer {
        double start = now_seconds();
        double lockedAt = 0.0;
        ~PresentTimer() noexcept {
            try {
                const double total = (now_seconds() - start) * 1000.0;
                if (total >= 250.0) log_event(1, "fg: slow present took " + std::to_string(total) + " ms" +
                    (lockedAt - start >= 0.1 ? " (waited " + std::to_string((lockedAt - start) * 1000.0) +
                     " ms for the swapchain lock)" : std::string()));
            } catch (...) {}
        }
    } timer;
    std::lock_guard lock(m_mutex);
    timer.lockedAt = now_seconds();
    if (flags & DXGI_PRESENT_TEST) return params ? m_native->Present1(sync, flags, params) : m_native->Present(sync, flags);
    if (FAILED(m_submission_failure)) return m_submission_failure;
    if (m_buffers.empty()) return DXGI_ERROR_INVALID_CALL;
    if (m_presenter) return present_async(sync, flags, params);
    if ((flags & DXGI_PRESENT_DO_NOT_WAIT) && m_fence->GetCompletedValue() < m_fence_value)
        return DXGI_ERROR_WAS_STILL_DRAWING;
    const double drainStart = now_seconds();
    HRESULT hr = drain();
    ++m_drain_samples;
    const double drainMs = (now_seconds() - drainStart) * 1000.0;
    if (drainMs > 0.25) {
        ++m_drain_waits;
        m_drain_wait_sum += drainMs;
        m_drain_wait_max = std::max(m_drain_wait_max, drainMs);
    }
    if (FAILED(hr)) return hr;

    // The limiter caps the game's real frames. This is the one place that sees
    // exactly one call per application frame, so it runs here and not per output
    // frame; the generated midpoint follows whatever the real frame does.
    // A DO_NOT_WAIT present must not block (K52); the limiter's cap is a sleep, so
    // it is skipped for such calls and the frame is simply not capped.
    if (m_present_hook && !(flags & DXGI_PRESENT_DO_NOT_WAIT)) m_present_hook(now_ticks());

    const double now = now_seconds();
    // Test-only A/B: SyncInterval=1 cannot be combined with ALLOW_TEARING.
    const UINT outSync = m_force_sync ? 1u : sync;
    const UINT outFlags = m_force_sync ? (flags & ~static_cast<UINT>(DXGI_PRESENT_ALLOW_TEARING)) : flags;
    // Snapshot colour/HUD and generate from THIS prepare before any native
    // Present or sleep can let the application recycle its inputs. The preceding
    // real frame is already on screen; the next outputs must be midpoint(N-1,N)
    // and real N, not last cycle's midpoint followed by real N.
    m_generated_ready = false;
    hr = record_generated();
    if (FAILED(hr)) return hr;
    const bool hadHistory = m_pacing.cycleStarted;
    const PacingOutput plan = m_pacing.plan(now, m_generated_ready);

    if (m_generated_ready && !plan.presentGenerated) {
        // The midpoint missed its slot. Drop it rather than delaying the frame
        // the game has already finished; one cycle of backlog is never kept.
        m_generated_ready = false;
        if (hadHistory) ++m_skipped_generates;
    }

    double cycleStartedAt = 0.0;
    bool presentedGenerated = false;
    uint64_t midCopyFence = 0;

    if (plan.presentGenerated) {
        hr = blit(m_generated.Get(), &m_generated_state, true);
        if (SUCCEEDED(hr)) {
            // Diagnostic signal: marks the midpoint copy, so the sample below can
            // say whether the GPU had produced that image before the real Present.
            if (SUCCEEDED(m_queue->Signal(m_fence.Get(), ++m_fence_value))) midCopyFence = m_fence_value;
            if (m_align_vblank) {
                const double target = vblank_deadline(now_seconds());
                wait_until(target, 0.05);
                ++m_align_samples;
                if (now_seconds() > target + 0.0005) ++m_align_misses;
            }
            cycleStartedAt = now_seconds();
            hr = params ? m_native->Present1(outSync, outFlags, params) : m_native->Present(outSync, outFlags);
            if (FAILED(hr)) return hr;
            if (m_output_hooks.presented) m_output_hooks.presented(true);
            m_generated_ready = false;
            ++m_generated_presents;
            presentedGenerated = true;
        } else {
            m_generated_ready = false;
            ++m_skipped_generates;
        }
        if (presentedGenerated)
            wait_until(plan.realDeadline, m_pacing.interval * 0.5);
    }

    if (midCopyFence) {
        ++m_mid_checks;
        if (m_fence->GetCompletedValue() < midCopyFence) ++m_mid_not_ready;
    }

    auto* real = m_buffers[m_game_index].Get();
    D3D12_RESOURCE_STATES realState = D3D12_RESOURCE_STATE_PRESENT;
    hr = blit(real, &realState, false);
    if (FAILED(hr)) return hr;
    if (m_align_vblank) {
        const double target = vblank_deadline(now_seconds());
        wait_until(target, 0.05);
        ++m_align_samples;
        if (now_seconds() > target + 0.0005) ++m_align_misses;
    }
    const double realSubmittedAt = now_seconds();
    hr = params ? m_native->Present1(outSync, outFlags, params) : m_native->Present(outSync, outFlags);
    if (SUCCEEDED(hr) && m_output_hooks.presented) m_output_hooks.presented(false);

    // Retire our copy AND any work DXGI queues for Present. A fence before
    // Present is insufficient for releasing native buffers on resize.
    const HRESULT signaled = m_queue->Signal(m_fence.Get(), ++m_fence_value);
    if (FAILED(signaled)) return m_submission_failure = signaled;
    if (!resolve_untracked_generator()) return m_submission_failure = E_FAIL;

    if (SUCCEEDED(hr)) {
        if (presentedGenerated) {
            const double gap = (realSubmittedAt - cycleStartedAt) * 1000.0;
            if (!m_gap_samples || gap < m_gap_min) m_gap_min = gap;
            m_gap_max = std::max(m_gap_max, gap);
            m_gap_sum += gap;
            ++m_gap_samples;
        }
        if (++m_present_cycles % 300 == 0) {
            try {
                log_event(1, "fg: present pacing cycles=" + std::to_string(m_present_cycles) +
                    " generated=" + std::to_string(m_generated_presents) +
                    " skipped=" + std::to_string(m_skipped_generates) +
                    " interval_ms=" + std::to_string(m_pacing.interval * 1000.0) +
                    " cpu_gap_ms{avg=" + std::to_string(m_gap_samples ? m_gap_sum / m_gap_samples : 0.0) +
                    " min=" + std::to_string(m_gap_min) + " max=" + std::to_string(m_gap_max) + "}" +
                    " mid_not_ready=" + std::to_string(m_mid_not_ready) + "/" + std::to_string(m_mid_checks) +
                    " drain_waits=" + std::to_string(m_drain_waits) + "/" + std::to_string(m_drain_samples) +
                    " drain_wait_ms{avg=" + std::to_string(m_drain_waits ? m_drain_wait_sum / m_drain_waits : 0.0) +
                    " max=" + std::to_string(m_drain_wait_max) + "}" +
                    " align_misses=" + std::to_string(m_align_misses) + "/" + std::to_string(m_align_samples));
            } catch (...) {}
            m_gap_samples = 0;
            m_gap_sum = m_gap_min = m_gap_max = 0.0;
            m_mid_checks = m_mid_not_ready = 0;
            m_drain_samples = 0;
            m_drain_waits = 0;
            m_drain_wait_sum = m_drain_wait_max = 0.0;
            m_align_samples = 0;
            m_align_misses = 0;
        }
        m_generated_ready = false; // Never carry a midpoint past its own real frame.
        m_game_index = (m_game_index + 1) % static_cast<UINT>(m_buffers.size());
    }
    return hr;
}

HRESULT Swapchain::present_async(UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* params) noexcept try
{
    // Up to kInFlight older frames may still be presenting. Each job reads only
    // its own slot (real-frame snapshot and generated images), which a frame
    // kSlots ago used and has finished with once at most kInFlight are left.
    const size_t allowed = kInFlight;
    if ((flags & DXGI_PRESENT_DO_NOT_WAIT) && m_presenter->outstanding() > allowed)
        return DXGI_ERROR_WAS_STILL_DRAWING;
    const double waitStart = now_seconds();
    if (!m_presenter->wait_outstanding(allowed, 5000)) return DXGI_ERROR_DEVICE_HUNG;
    const double waited = (now_seconds() - waitStart) * 1000.0;
    ++m_backpressure_samples;
    if (waited > 0.25) {
        ++m_backpressure_waits;
        m_backpressure_sum += waited;
        m_backpressure_max = std::max(m_backpressure_max, waited);
    }
    // Presentation is asynchronous, so the game sees the outcome one frame
    // later: device loss, occlusion and other status codes all surface here.
    const HRESULT previous = m_presenter->last_result();
    if (FAILED(previous)) return previous;

    // Own variable Present1 parameters before advancing the schedule or
    // recording GPU work. Allocation failure is still a game-visible error
    // (K76), but it must not leave partial GPU/history ownership behind.
    PresentJob job;
    if (params) {
        job.hasParams = true;
        if (params->DirtyRectsCount && params->pDirtyRects) {
            if (consume_bookkeeping_fault(BookkeepingFault::PresentParametersAllocation))
                throw std::bad_alloc();
            job.dirty.assign(params->pDirtyRects, params->pDirtyRects + params->DirtyRectsCount);
        }
        if (params->pScrollRect) { job.hasScroll = true; job.scroll = *params->pScrollRect; }
        if (params->pScrollOffset) { job.hasOffset = true; job.offset = *params->pScrollOffset; }
    }

    if (m_present_hook && !(flags & DXGI_PRESENT_DO_NOT_WAIT)) m_present_hook(now_ticks());

    const double now = now_seconds();
    uint32_t slot = 0;
    const HRESULT slotHr = acquire_frame_slot(m_phase_set, kSlots, slot,
                                              [&](uint32_t candidate) noexcept { return wait_slot(candidate); });
    if (FAILED(slotHr)) return slotHr;
    // Planning follows admission so a failed slot wait cannot consume a grid
    // point. `now` remains anchored before the wait, as in the existing path.
    const OutputSchedule scheduleBefore = m_schedule; // restored if the job is not queued (K105)
    OutputPlan plan = m_schedule.plan(now);
    // Generate every planned phase in one submission. The generator advances
    // its history once per real frame, so a frame with nothing generated to
    // show (adaptive target below the real rate) still runs one phase.
    auto& set = m_phase_outputs[slot];
    GeneratePhase phases[kMaxPhases]{};
    uint32_t phaseCount = 0;
    for (uint32_t i = 0; i < plan.count && phaseCount < kMaxPhases; ++i) {
        if (plan.outputs[i].real) continue;
        phases[phaseCount] = GeneratePhase{set[phaseCount].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                                           plan.outputs[i].alpha};
        ++phaseCount;
    }
    const bool showGenerated = phaseCount > 0;
    if (!showGenerated)
        phases[phaseCount++] = GeneratePhase{set[0].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, kMidpointAlpha};
    HRESULT generationFailure = S_OK;
    const bool generated = record_phases(phases, phaseCount, slot, generationFailure);
    if (FAILED(generationFailure)) return generationFailure;
    if (!generated) {
        // Nothing usable was generated: show the real frame on its own.
        if (showGenerated && m_sink.record) ++m_skipped_generates;
        plan.count = 1;
        plan.outputs[0] = PlannedOutput{1.0f, true, now};
    }

    // The presenter reads the snapshot, so the game's own buffer is free again
    // once this copy has run, which queue order guarantees before its next use.
    HRESULT hr = snapshot_real(slot);
    if (FAILED(hr)) return m_submission_failure = hr;

    // Everything the game and the generator queued for this frame.
    hr = m_queue->Signal(m_fence.Get(), ++m_fence_value);
    if (FAILED(hr)) return m_submission_failure = hr;
    if (!resolve_untracked_generator()) return m_submission_failure = E_FAIL;
    m_snapshot_fences[slot] = m_fence_value;

    job.real = m_snapshots[slot];
    uint32_t nextPhase = 0;
    for (uint32_t i = 0; i < plan.count; ++i) {
        JobOutput& output = job.outputs[job.count++];
        output.real = plan.outputs[i].real;
        output.content = plan.outputs[i].content;
        if (!output.real) output.image = set[nextPhase++];
    }
    job.ready = m_fence;
    job.readyValue = m_fence_value;
    job.entry = now;
    job.sync = sync;
    // DO_NOT_WAIT was answered above; the presenter itself must be allowed to wait.
    job.flags = flags & ~static_cast<UINT>(DXGI_PRESENT_DO_NOT_WAIT);
    hr = m_presenter->submit(std::move(job));
    if (FAILED(hr)) {
        // Do not advance ownership for an unqueued job (K51); the planned content
        // time is returned to the schedule so a retry does not skip a slot (K105).
        m_schedule = scheduleBefore;
        return hr;
    }
    if (m_rate_window_start == 0.0) m_rate_window_start = now;
    m_rate_window_outputs += plan.count;
    m_rate_window_generated += plan.generated();
    m_game_index = (m_game_index + 1) % static_cast<UINT>(m_buffers.size());

    if (++m_present_cycles % 300 == 0) {
        publish_output_context();
        try {
            const double elapsed = now - m_rate_window_start;
            const double realFps = elapsed > 0.0 ? 300.0 / elapsed : 0.0;
            const double outputFps = elapsed > 0.0 ? m_rate_window_outputs / elapsed : 0.0;
            const double refresh = m_presenter->refresh_hz();
            std::string rates = " real_fps=" + std::to_string(realFps) + " planned_output_fps=" +
                std::to_string(outputFps) + " refresh_hz=" + std::to_string(refresh);
            // Only meaningful while frames are generated: with frame generation
            // off (menus) every real frame is shown as is.
            if (outputFps > refresh * 1.02 && m_rate_window_generated > 0) {
                // The panel cannot show more than one frame per refresh; the
                // presenter has to drop outputs and the game waits for it.
                const double ratio = static_cast<double>(m_rate_window_outputs) / 300.0;
                rates += " OVER_REFRESH: cap the real rate at " +
                         std::to_string(sedici::auto_target_fps(refresh) / ratio) +
                         " fps (set [fg] target_fps = \"auto\")";
                if (!m_schedule.adaptive()) rates += " or use multiplier = \"adaptive\"";
            }
            log_event(1, "fg: vrr game thread cycles=" + std::to_string(m_present_cycles) + rates +
                " skipped=" + std::to_string(m_skipped_generates) +
                " backpressure_waits=" + std::to_string(m_backpressure_waits) + "/" +
                std::to_string(m_backpressure_samples) +
                " backpressure_ms{avg=" + std::to_string(m_backpressure_waits ? m_backpressure_sum / m_backpressure_waits : 0.0) +
                " max=" + std::to_string(m_backpressure_max) + "}");
        } catch (...) {}
        m_backpressure_samples = m_backpressure_waits = 0;
        m_backpressure_sum = m_backpressure_max = 0.0;
        m_rate_window_start = now;
        m_rate_window_outputs = 0;
        m_rate_window_generated = 0;
    }
    return previous;
}
catch (...) { return E_OUTOFMEMORY; }

HRESULT STDMETHODCALLTYPE Swapchain::QueryInterface(REFIID iid, void** result)
{
    if (!result) return E_POINTER;
    *result = nullptr;
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IDXGIObject) || iid == __uuidof(IDXGIDeviceSubObject) ||
        iid == __uuidof(IDXGISwapChain) || iid == __uuidof(IDXGISwapChain1) ||
        iid == __uuidof(IDXGISwapChain2) || iid == __uuidof(IDXGISwapChain3) || iid == __uuidof(IDXGISwapChain4)) {
        *result = static_cast<IDXGISwapChain4*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE; // Never leak native identity through QueryInterface.
}
ULONG STDMETHODCALLTYPE Swapchain::AddRef() { return ++m_refs; }
ULONG STDMETHODCALLTYPE Swapchain::Release()
{
    const ULONG refs = --m_refs;
    if (refs == 0) {
        const HRESULT drained = drain();
        if (FAILED(drained) && m_device && SUCCEEDED(m_device->GetDeviceRemovedReason()))
            park(); // live device, work not retired: keep everything until it is (K47)
        else
            delete this;
        reap_retained();
    }
    return refs;
}

HRESULT STDMETHODCALLTYPE Swapchain::Present(UINT sync, UINT flags) { return present(sync, flags, nullptr); }
HRESULT STDMETHODCALLTYPE Swapchain::Present1(UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* params)
{
    if (!params) return DXGI_ERROR_INVALID_CALL;
    return present(sync, flags, params);
}
HRESULT STDMETHODCALLTYPE Swapchain::GetBuffer(UINT index, REFIID iid, void** result)
{
    if (!result) return E_POINTER;
    *result = nullptr;
    std::lock_guard lock(m_mutex);
    if (index >= m_buffers.size()) return DXGI_ERROR_INVALID_CALL;
    return m_buffers[index]->QueryInterface(iid, result);
}
UINT STDMETHODCALLTYPE Swapchain::GetCurrentBackBufferIndex() { std::lock_guard lock(m_mutex); return m_game_index; }

HRESULT Swapchain::resize(UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags,
                          const UINT* masks, IUnknown* const* queues, bool version1) noexcept
{
    std::lock_guard lock(m_mutex);
    if (format != DXGI_FORMAT_UNKNOWN && !supported_color_format(format)) return DXGI_ERROR_UNSUPPORTED;
    if (version1) {
        const UINT buffers = count ? count : static_cast<UINT>(m_buffers.size());
        if (!masks || !queues) return E_INVALIDARG;
        for (UINT i = 0; i < buffers; ++i) {
            // This initial implementation uses one direct queue and one node.
            if (masks[i] > 1 || queues[i] != m_queue.Get()) return DXGI_ERROR_UNSUPPORTED;
        }
    }
    // A job still on the presenter holds its own buffer reference; let it finish
    // so only the application's references are counted below.
    if (m_presenter && !m_presenter->wait_outstanding(0, 5000)) return DXGI_ERROR_DEVICE_HUNG;
    // Resize must fail while the application owns a backbuffer reference, just
    // as native DXGI does. Do not destroy buffers behind the application's back.
    for (auto& buffer : m_buffers) {
        buffer->AddRef();
        if (buffer->Release() != 1) return DXGI_ERROR_INVALID_CALL;
    }
    HRESULT hr = drain();
    if (FAILED(hr)) return hr;
    if (m_presenter) {
        // The game names its own queue; the chain stays on Sedici's present queue.
        hr = m_presenter->resize_native(count, width, height, format, flags);
    } else {
        hr = version1 ? m_native->ResizeBuffers1(count, width, height, format, flags, masks, queues)
                      : m_native->ResizeBuffers(count, width, height, format, flags);
    }
    if (FAILED(hr)) return hr;
    m_generated_ready = false;
    m_pacing = {};
    hr = initialize_buffers();
    publish_output_context();
    stats_on_swapchain_event("resize"); // K99: lets a stall report coincide with a resize
    return hr;
}
HRESULT STDMETHODCALLTYPE Swapchain::ResizeBuffers(UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags)
{ return resize(n, w, h, f, flags, nullptr, nullptr, false); }
HRESULT STDMETHODCALLTYPE Swapchain::ResizeBuffers1(UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags,
    const UINT* masks, IUnknown* const* queues) { return resize(n, w, h, f, flags, masks, queues, true); }

#define FORWARD_HR(name, args, call) HRESULT STDMETHODCALLTYPE Swapchain::name args { return m_native->name call; }
FORWARD_HR(SetPrivateData, (REFGUID n, UINT s, const void* d), (n, s, d))
FORWARD_HR(SetPrivateDataInterface, (REFGUID n, const IUnknown* v), (n, v))
FORWARD_HR(GetPrivateData, (REFGUID n, UINT* s, void* d), (n, s, d))
FORWARD_HR(GetParent, (REFIID i, void** r), (i, r))
FORWARD_HR(GetDevice, (REFIID i, void** r), (i, r))
FORWARD_HR(GetFullscreenState, (BOOL* f, IDXGIOutput** t), (f, t))
FORWARD_HR(GetDesc, (DXGI_SWAP_CHAIN_DESC* d), (d))
FORWARD_HR(ResizeTarget, (const DXGI_MODE_DESC* d), (d))
FORWARD_HR(GetContainingOutput, (IDXGIOutput** o), (o))
FORWARD_HR(GetFrameStatistics, (DXGI_FRAME_STATISTICS* s), (s))
FORWARD_HR(GetLastPresentCount, (UINT* c), (c))
FORWARD_HR(GetDesc1, (DXGI_SWAP_CHAIN_DESC1* d), (d))
FORWARD_HR(GetFullscreenDesc, (DXGI_SWAP_CHAIN_FULLSCREEN_DESC* d), (d))
FORWARD_HR(GetHwnd, (HWND* h), (h))
FORWARD_HR(GetCoreWindow, (REFIID i, void** r), (i, r))
FORWARD_HR(GetRestrictToOutput, (IDXGIOutput** o), (o))
FORWARD_HR(SetBackgroundColor, (const DXGI_RGBA* c), (c))
FORWARD_HR(GetBackgroundColor, (DXGI_RGBA* c), (c))
FORWARD_HR(SetRotation, (DXGI_MODE_ROTATION r), (r))
FORWARD_HR(GetRotation, (DXGI_MODE_ROTATION* r), (r))
FORWARD_HR(SetSourceSize, (UINT w, UINT h), (w, h))
FORWARD_HR(GetSourceSize, (UINT* w, UINT* h), (w, h))
FORWARD_HR(SetMaximumFrameLatency, (UINT l), (l))
FORWARD_HR(GetMaximumFrameLatency, (UINT* l), (l))
FORWARD_HR(SetMatrixTransform, (const DXGI_MATRIX_3X2_F* m), (m))
FORWARD_HR(GetMatrixTransform, (DXGI_MATRIX_3X2_F* m), (m))
FORWARD_HR(CheckColorSpaceSupport, (DXGI_COLOR_SPACE_TYPE c, UINT* s), (c, s))
FORWARD_HR(SetColorSpace1, (DXGI_COLOR_SPACE_TYPE c), (c))
FORWARD_HR(SetHDRMetaData, (DXGI_HDR_METADATA_TYPE t, UINT s, void* d), (t, s, d))
#undef FORWARD_HR
HRESULT STDMETHODCALLTYPE Swapchain::SetFullscreenState(BOOL fullscreen, IDXGIOutput* target)
{
    // A mode change must not race a flip issued from the presenter thread.
    std::lock_guard lock(m_mutex);
    if (m_presenter) (void)drain();
    return m_native->SetFullscreenState(fullscreen, target);
}
BOOL STDMETHODCALLTYPE Swapchain::IsTemporaryMonoSupported() { return m_native->IsTemporaryMonoSupported(); }
HANDLE STDMETHODCALLTYPE Swapchain::GetFrameLatencyWaitableObject() { return m_native->GetFrameLatencyWaitableObject(); }

} // namespace sedici::fg
