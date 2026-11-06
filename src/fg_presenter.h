#pragma once

#include <dxgi1_6.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "config.h"

// `[fg] present = "vrr"`: presentation off the game's thread and queue.
//
// Finding 008 showed that pacing the two Present calls in CPU time does not pace
// the flips: a flip happens when the GPU work queued before it completes, so the
// midpoint landed 3-7 ms late and the following real frame early. Half of all
// flips arrived sooner than one refresh after the previous one and tore even on
// a VRR display.
//
// The presenter fixes the anchor. For each application frame it waits until the
// image to show is finished on the GPU, then flips, and spaces the real frame
// half an interval after the midpoint's actual flip. A tear guard can hold any
// flip until the panel is not scanning out. Because the game's queue already
// holds the next frame's work while this happens, the native chain is moved to a
// dedicated present queue with ResizeBuffers1; a flip on the game queue would
// otherwise wait for that work too.

namespace sedici::fg {

// One application frame's worth of output.
struct PresentJob {
    // Private game backbuffer for this frame, in PRESENT state.
    Microsoft::WRL::ComPtr<ID3D12Resource> real;
    // Generated midpoint to show first; null for a real-frame-only cycle.
    Microsoft::WRL::ComPtr<ID3D12Resource> midpoint;
    D3D12_RESOURCE_STATES midpointState = D3D12_RESOURCE_STATE_COPY_SOURCE;
    // Signalled on the game queue after the frame and its generation work.
    Microsoft::WRL::ComPtr<ID3D12Fence> ready;
    uint64_t readyValue = 0;
    double half = 0.0; // seconds from the midpoint flip to the real flip
    UINT sync = 0;
    UINT flags = 0;
    bool hasParams = false;
    std::vector<RECT> dirty;
    bool hasScroll = false;
    RECT scroll{};
    bool hasOffset = false;
    POINT offset{};
};

class Presenter {
public:
    using TearGuard = FrameGenerationSettings::TearGuard;

    // Creates everything that can fail (queue, lists, fence, thread) without
    // touching any swapchain. Returns null, with `why` set, on failure.
    static std::unique_ptr<Presenter> create(ID3D12Device* device, TearGuard guard,
                                             std::string& why) noexcept;

    // Recreates `native` for the same window, description and fullscreen state
    // on the present queue, as AMD's swapchain replacement does. Overlays that
    // hook swapchain creation (RTSS draws its OSD on the creation queue) then
    // see the queue the chain really presents on; moving an existing chain with
    // ResizeBuffers1 made RTSS write it from the game queue and the device was
    // removed. `owned` is every reference the caller holds and gives up,
    // including `native` itself.
    //
    // S_OK: `native` is the new chain and the presenter uses it.
    // Failure before destruction (no other references allowed, reads failed):
    //   `native` is untouched.
    // Failure after destruction: `native` is a replacement on `gameQueue`, or
    //   null if even that could not be created.
    HRESULT adopt(Microsoft::WRL::ComPtr<IDXGISwapChain4>& native, ULONG owned,
                  ID3D12CommandQueue* gameQueue, std::string& why) noexcept;
    ~Presenter();
    Presenter(const Presenter&) = delete;
    Presenter& operator=(const Presenter&) = delete;

    // Jobs submitted and not yet finished, including the one being presented.
    size_t outstanding() const noexcept;
    // Blocks until at most `max` jobs are outstanding. False on timeout.
    bool wait_outstanding(size_t max, DWORD timeoutMs) noexcept;
    void submit(PresentJob&& job) noexcept;
    // Result of the most recent native Present, or the error that stopped a job.
    HRESULT last_result() const noexcept;
    // Waits for the GPU to retire everything the presenter queued. The caller
    // must have waited for zero outstanding jobs first.
    HRESULT drain_gpu() noexcept;
    // ResizeBuffers1 on the native chain, keeping it on the present queue.
    // Only call with zero outstanding jobs and a drained GPU.
    HRESULT resize_native(UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags) noexcept;

    uint64_t midpoints_shown() const noexcept { return m_midpoints_shown; }
    uint64_t midpoints_dropped() const noexcept { return m_midpoints_dropped; }

private:
    Presenter() = default;
    void run() noexcept;
    HRESULT present_job(PresentJob& job) noexcept;
    HRESULT copy_to_native(ID3D12Resource* source, D3D12_RESOURCE_STATES state) noexcept;
    HRESULT wait_fence(uint64_t value) noexcept;
    HRESULT native_present(const PresentJob& job) noexcept;
    // Holds until `notBefore`, then applies the tear guard.
    void hold(double notBefore) noexcept;
    void wait_until(double deadline) noexcept;
    // Re-reads the containing output: refresh period, active height and the
    // kernel adapter handle used for scan-line reads.
    void refresh_output() noexcept;
    void close_adapter() noexcept;
    bool read_scanline(bool& inBlank, UINT& line) noexcept;
    void log_window() noexcept;

    Microsoft::WRL::ComPtr<IDXGISwapChain4> m_native;
    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_allocator[2];
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_list[2];
    uint32_t m_next_list = 0;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
    uint64_t m_fence_value = 0;
    HANDLE m_event = nullptr;
    HANDLE m_timer = nullptr;
    TearGuard m_guard = TearGuard::Scanline;

    // Job queue. `m_submitted - m_completed` is the outstanding count.
    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::condition_variable m_done;
    std::deque<PresentJob> m_jobs;
    uint64_t m_submitted = 0;
    uint64_t m_completed = 0;
    HRESULT m_last = S_OK;
    bool m_stop = false;
    std::thread m_thread;

    // Display. Only the presenter thread touches these after create().
    double m_period = 1.0 / 60.0; // seconds per refresh at the output's current mode
    UINT m_active_lines = 0;
    UINT m_lead_lines = 0;
    double m_lead_seconds = 0.00025;
    UINT m_adapter = 0; // D3DKMT_HANDLE
    UINT m_source = 0;  // VidPnSourceId
    bool m_scanline_ok = false;
    double m_last_flip = 0.0;

    uint64_t m_midpoints_shown = 0;
    uint64_t m_midpoints_dropped = 0;

    // Diagnostics, reported every 300 jobs and then reset.
    struct Window {
        uint32_t jobs = 0;
        uint32_t mids = 0;
        uint32_t dropped = 0;
        uint32_t flips = 0;
        uint32_t shortGaps = 0;    // flip sooner than one refresh after the previous
        double gapMin = 0.0;
        double gpuWaitSum = 0.0;   // waiting for the image to finish on the GPU
        double gpuWaitMax = 0.0;
        uint32_t guardHolds = 0;   // the tear guard actually delayed a flip
        double guardSum = 0.0;
        double guardMax = 0.0;
        uint32_t guardTimeouts = 0;
        double callSum = 0.0;      // native Present call duration
        double callMax = 0.0;
        uint32_t scan[6]{};        // at the flip: blank, 0-25, 25-50, 50-75, 75-100 %, unread
        uint32_t idleBlank = 0;    // > one refresh since the last flip and the panel in blank
        uint32_t idleSamples = 0;
    } m_window;
};

} // namespace sedici::fg
