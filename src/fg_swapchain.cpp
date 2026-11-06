#include "fg_swapchain.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <new>
#include <thread>

#include "log.h"
#include "config.h"

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

} // namespace

HRESULT Swapchain::wrap(IDXGISwapChain4* native, ID3D12CommandQueue* queue,
                        Swapchain** result, const PresentChoice* choice, ULONG handedRefs) noexcept
{
    if (!result) return E_POINTER;
    *result = nullptr;
    if (!native || !queue || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return E_INVALIDARG;
    auto* object = new (std::nothrow) Swapchain;
    if (!object) return E_OUTOFMEMORY;
    object->m_native = native;
    object->m_queue = queue;
    HRESULT hr = queue->GetDevice(IID_PPV_ARGS(&object->m_device));
    if (SUCCEEDED(hr)) {
        Microsoft::WRL::ComPtr<ID3D12Device> nativeDevice;
        hr = native->GetDevice(IID_PPV_ARGS(&nativeDevice));
        if (SUCCEEDED(hr) && nativeDevice.Get() != object->m_device.Get()) hr = E_INVALIDARG;
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
    if (object->m_presenter) {
        IDXGISwapChain4* const original = object->m_native.Get();
        hr = object->m_presenter->adopt(object->m_native, handedRefs + 1, queue, why);
        if (FAILED(hr)) {
            object->m_presenter.reset();
            object->m_generated_spare.Reset();
            chosen.present = Present::Vsync;
            if (!object->m_native) {
                // The original chain is gone and nothing could replace it.
                log_error("fg: \"vrr\" swapchain recreation failed (" + why + ")");
                object->m_buffers.clear();
                delete object;
                return hr;
            }
            if (object->m_native.Get() != original)
                log_error("fg: \"vrr\" chain recreated on the game queue instead (" + why + ")");
        }
    }
    if (chosen.present == Present::Vsync && !why.empty()) {
        // Never fail the game's wrap over this: present the safe way.
        log_error("fg: \"vrr\" present unavailable (" + why + "); falling back to \"vsync\"");
    }
    // `[fg] present = "vsync"` (default) takes one vertical blank per output,
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
    *result = object;
    return S_OK;
}

Swapchain::~Swapchain()
{
    (void)drain();
    m_presenter.reset();
    if (m_event) CloseHandle(m_event);
    if (m_pacing_timer) CloseHandle(m_pacing_timer);
}

void Swapchain::set_generator(const GenerateSink& sink) noexcept
{
    std::lock_guard lock(m_mutex);
    if (sink.record != m_sink.record || sink.user != m_sink.user) {
        m_generated_ready = false;
        m_pacing = {};
    }
    m_sink = sink;
}

void Swapchain::set_present_hook(void (*hook)(uint64_t)) noexcept
{
    std::lock_guard lock(m_mutex);
    m_present_hook = hook;
}

HRESULT Swapchain::initialize_buffers() noexcept try
{
    DXGI_SWAP_CHAIN_DESC1 desc{};
    HRESULT hr = m_native->GetDesc1(&desc);
    if (FAILED(hr)) return hr;
    if (desc.SampleDesc.Count != 1 || desc.BufferCount == 0 || desc.Stereo ||
        desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM) return DXGI_ERROR_UNSUPPORTED;

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
    Microsoft::WRL::ComPtr<ID3D12Resource> midpoint, spare;
    hr = m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &generated,
        D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, IID_PPV_ARGS(&midpoint));
    if (FAILED(hr)) return hr;
    if (m_presenter) {
        hr = m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &generated,
            D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, IID_PPV_ARGS(&spare));
        if (FAILED(hr)) return hr;
    }

    m_buffers = std::move(buffers);
    m_generated = midpoint;
    m_generated_spare = spare;
    m_generated_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
    m_generated_ready = false;
    m_game_index = 0;
    return S_OK;
}
catch (...) { return E_OUTOFMEMORY; }

HRESULT Swapchain::drain() noexcept
{
    if (m_presenter) {
        if (!m_presenter->wait_outstanding(0, 5000)) return DXGI_ERROR_DEVICE_HUNG;
        const HRESULT hr = m_presenter->drain_gpu();
        if (FAILED(hr)) return hr;
    }
    if (!m_fence || m_fence_value == 0) return S_OK;
    const UINT64 completed = m_fence->GetCompletedValue();
    if (completed == UINT64_MAX) return m_device->GetDeviceRemovedReason();
    if (completed >= m_fence_value) return S_OK;
    HRESULT hr = m_fence->SetEventOnCompletion(m_fence_value, m_event);
    if (FAILED(hr)) return hr;
    // Bounded wait: a removed/hung device must not trap teardown forever.
    const DWORD wait = WaitForSingleObject(m_event, 5000);
    if (wait == WAIT_OBJECT_0) return m_device->GetDeviceRemovedReason();
    return wait == WAIT_TIMEOUT ? DXGI_ERROR_DEVICE_HUNG : HRESULT_FROM_WIN32(GetLastError());
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

HRESULT Swapchain::blit(ID3D12Resource* source, D3D12_RESOURCE_STATES* sourceState) noexcept
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

    hr = list->Close();
    if (FAILED(hr)) return hr;
    ID3D12CommandList* lists[]{list};
    m_queue->ExecuteCommandLists(1, lists);
    return S_OK;
}

void Swapchain::record_generated() noexcept
{
    if (!m_sink.record || !m_generated || m_generated_ready) return;
    auto* allocator = m_generate_allocator.Get();
    auto* list = m_generate_list.Get();
    if (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator, nullptr))) return;
    auto* real = m_buffers[m_game_index].Get();
    const bool recorded = m_sink.record(m_sink.user, list, real, m_generated.Get(), m_generated_state);
    if (!recorded) {
        ++m_skipped_generates;
        // The list may hold partial work; close it so the next reset is legal.
        list->Close();
        return;
    }
    if (FAILED(list->Close())) {
        ++m_skipped_generates;
        return;
    }
    ID3D12CommandList* lists[]{list};
    m_queue->ExecuteCommandLists(1, lists);
    if (FAILED(m_queue->Signal(m_fence.Get(), ++m_fence_value))) {
        ++m_skipped_generates;
        return;
    }
    // The sink restores the state it was given; do not invent a new state here.
    m_generated_ready = true;
    if (m_sink.submitted) m_sink.submitted(m_sink.user, m_fence.Get(), m_fence_value);
}

HRESULT Swapchain::present(UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* params) noexcept
{
    std::lock_guard lock(m_mutex);
    if (flags & DXGI_PRESENT_TEST) return params ? m_native->Present1(sync, flags, params) : m_native->Present(sync, flags);
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
    if (m_present_hook) m_present_hook(now_ticks());

    const double now = now_seconds();
    // Test-only A/B: SyncInterval=1 cannot be combined with ALLOW_TEARING.
    const UINT outSync = m_force_sync ? 1u : sync;
    const UINT outFlags = m_force_sync ? (flags & ~static_cast<UINT>(DXGI_PRESENT_ALLOW_TEARING)) : flags;
    // Snapshot colour/HUD and generate from THIS prepare before any native
    // Present or sleep can let the application recycle its inputs. The preceding
    // real frame is already on screen; the next outputs must be midpoint(N-1,N)
    // and real N, not last cycle's midpoint followed by real N.
    m_generated_ready = false;
    record_generated();
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
        hr = blit(m_generated.Get(), &m_generated_state);
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
    hr = blit(real, &realState);
    if (FAILED(hr)) return hr;
    if (m_align_vblank) {
        const double target = vblank_deadline(now_seconds());
        wait_until(target, 0.05);
        ++m_align_samples;
        if (now_seconds() > target + 0.0005) ++m_align_misses;
    }
    const double realSubmittedAt = now_seconds();
    hr = params ? m_native->Present1(outSync, outFlags, params) : m_native->Present(outSync, outFlags);

    // Retire our copy AND any work DXGI queues for Present. A fence before
    // Present is insufficient for releasing native buffers on resize.
    const HRESULT signaled = m_queue->Signal(m_fence.Get(), ++m_fence_value);
    if (FAILED(signaled)) return signaled;

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
    // The presenter may still be showing the previous frame. Generation writes
    // the other midpoint slot and the game renders into a buffer the presenter
    // finished with, as long as at most one older job is still outstanding.
    const size_t allowed = m_buffers.size() > 2 ? 1 : 0;
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

    if (m_present_hook) m_present_hook(now_ticks());

    const double now = now_seconds();
    std::swap(m_generated, m_generated_spare);
    m_generated_ready = false;
    record_generated();
    const bool hadHistory = m_pacing.cycleStarted;
    const PacingOutput plan = m_pacing.plan(now, m_generated_ready);
    const bool showMidpoint = m_generated_ready && plan.presentGenerated;
    if (m_generated_ready && !showMidpoint && hadHistory) ++m_skipped_generates;
    m_generated_ready = false;

    // Everything the game and the generator queued for this frame.
    HRESULT hr = m_queue->Signal(m_fence.Get(), ++m_fence_value);
    if (FAILED(hr)) return hr;

    PresentJob job;
    job.real = m_buffers[m_game_index];
    if (showMidpoint) {
        job.midpoint = m_generated;
        job.midpointState = m_generated_state;
    }
    job.ready = m_fence;
    job.readyValue = m_fence_value;
    job.half = 0.5 * m_pacing.interval;
    job.sync = sync;
    // DO_NOT_WAIT was answered above; the presenter itself must be allowed to wait.
    job.flags = flags & ~static_cast<UINT>(DXGI_PRESENT_DO_NOT_WAIT);
    if (params) {
        job.hasParams = true;
        if (params->DirtyRectsCount && params->pDirtyRects)
            job.dirty.assign(params->pDirtyRects, params->pDirtyRects + params->DirtyRectsCount);
        if (params->pScrollRect) { job.hasScroll = true; job.scroll = *params->pScrollRect; }
        if (params->pScrollOffset) { job.hasOffset = true; job.offset = *params->pScrollOffset; }
    }
    m_presenter->submit(std::move(job));
    m_game_index = (m_game_index + 1) % static_cast<UINT>(m_buffers.size());

    if (++m_present_cycles % 300 == 0) {
        try {
            log_event(1, "fg: vrr game thread cycles=" + std::to_string(m_present_cycles) +
                " interval_ms=" + std::to_string(m_pacing.interval * 1000.0) +
                " skipped=" + std::to_string(m_skipped_generates) +
                " backpressure_waits=" + std::to_string(m_backpressure_waits) + "/" +
                std::to_string(m_backpressure_samples) +
                " backpressure_ms{avg=" + std::to_string(m_backpressure_waits ? m_backpressure_sum / m_backpressure_waits : 0.0) +
                " max=" + std::to_string(m_backpressure_max) + "}");
        } catch (...) {}
        m_backpressure_samples = m_backpressure_waits = 0;
        m_backpressure_sum = m_backpressure_max = 0.0;
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
    if (refs == 0) delete this;
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
    if (format != DXGI_FORMAT_UNKNOWN && format != DXGI_FORMAT_R8G8B8A8_UNORM) return DXGI_ERROR_UNSUPPORTED;
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
    return initialize_buffers();
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
