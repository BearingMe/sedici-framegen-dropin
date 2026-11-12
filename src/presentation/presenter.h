#pragma once

#include <atomic>

#include <dxgi1_6.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "settings/config.h"
#include "gpu/queue_policy.h"
#include "presentation/pacing.h"

// `[fg] present = "vrr"`: presentation off the game's thread and queue.
//
// Finding 008 showed that pacing the two Present calls in CPU time does not pace
// the flips: a flip happens when the GPU work queued before it completes, so the
// midpoint landed 3-7 ms late and the following real frame early. Half of all
// flips arrived sooner than one refresh after the previous one and tore even on
// a VRR display.
//
// The presenter fixes the anchor. Every output carries a content time on the
// game's Present clock (OutputSchedule). It is flipped at content + latency,
// where latency is learned from how long after its content time the first image
// of each frame is actually finished on the GPU. At 2x that is the midpoint as
// soon as it is ready and the real frame half an interval later; at other
// ratios it keeps the schedule's even grid on screen. A tear guard can hold any
// flip until the panel is not scanning out. Because the game's queue already
// holds the next frame's work while this happens, the native chain is moved to a
// dedicated present queue with ResizeBuffers1; a flip on the game queue would
// otherwise wait for that work too.

namespace sedici::fg {

// What AMD's present callback gives the overlay, for Sedici's own swapchain:
// `draw` records onto a native backbuffer (PRESENT state, returned to it) on
// the list that is executed right before that buffer is presented; `presented`
// follows each successful native Present. Both are optional and may be called
// from the presenter thread.
struct OutputHooks {
    void (*draw)(ID3D12Device* device, ID3D12GraphicsCommandList* list, ID3D12Resource* backbuffer,
                 bool generated) = nullptr;
    void (*presented)(bool generated) = nullptr;
};

// One image to show: the real frame or a generated one, in content-time order.
struct JobOutput {
    Microsoft::WRL::ComPtr<ID3D12Resource> image; // generated, COPY_SOURCE; null when real
    bool real = true;
    double content = 0.0; // seconds on the game's Present clock
};

// One application frame's worth of output. A frame may show nothing of its own
// (adaptive target below the real rate), only generated frames, or both.
struct PresentJob {
    // Private game backbuffer for this frame, in PRESENT state.
    Microsoft::WRL::ComPtr<ID3D12Resource> real;
    JobOutput outputs[3];
    uint32_t count = 0;
    double entry = 0.0; // the game's Present for this frame, on the content clock
    // Signalled on the game queue after the frame and its generation work.
    Microsoft::WRL::ComPtr<ID3D12Fence> ready;
    uint64_t readyValue = 0;
    UINT sync = 0;
    UINT flags = 0;
    bool hasParams = false;
    std::vector<RECT> dirty;
    bool hasScroll = false;
    RECT scroll{};
    bool hasOffset = false;
    POINT offset{};
};

// Number of devices that currently have a shared present queue (K140: it must fall back to the
// level it had once the last presenter of a device is gone, unless a Present-hooking overlay is loaded).
size_t shared_present_queue_count() noexcept;

class Presenter {
public:
    using TearGuard = FrameGenerationSettings::TearGuard;

    // Creates everything that can fail (queue, lists, fence, thread) without
    // touching any swapchain. Returns null, with `why` set, on failure.
    static std::unique_ptr<Presenter> create(ID3D12Device* device, TearGuard guard, std::string& why) noexcept;

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
    HRESULT adopt(Microsoft::WRL::ComPtr<IDXGISwapChain4>& native, ULONG owned, ID3D12CommandQueue* gameQueue,
                  std::string& why) noexcept;
    ~Presenter();
    Presenter(const Presenter&) = delete;
    Presenter& operator=(const Presenter&) = delete;

    // Jobs submitted and not yet finished, including the one being presented.
    size_t outstanding() const noexcept;
    // Blocks until at most `max` jobs are outstanding. False on timeout.
    bool wait_outstanding(size_t max, DWORD timeoutMs) noexcept;
    // Rejects stopped or unqueued jobs rather than silently dropping them.
    HRESULT submit(PresentJob&& job) noexcept;
    // Requests cancellation without waiting for a worker blocked in the driver.
    void request_stop() noexcept;
    void set_output_hooks(const OutputHooks& hooks) noexcept;
    // Result of the most recent native Present, or the error that stopped a job.
    HRESULT last_result() const noexcept;
    // Waits for the GPU to retire everything the presenter queued. The caller
    // must have waited for zero outstanding jobs first.
    HRESULT drain_gpu() noexcept;
    // ResizeBuffers1 on the native chain, keeping it on the present queue.
    // Only call with zero outstanding jobs and a drained GPU.
    HRESULT resize_native(UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags) noexcept;

    // Generated frames shown, and generated frames dropped because a later
    // output was already due.
    uint64_t midpoints_shown() const noexcept { return m_generated_shown; }
    uint64_t midpoints_dropped() const noexcept { return m_generated_dropped; }
    // Flips issued within 2 ms of the previous one since the presenter started.
    uint64_t back_to_back_flips() const noexcept { return m_back_to_back; }
    // The output's refresh rate when the chain was adopted or last resized.
    double refresh_hz() const noexcept { return m_period > 0.0 ? 1.0 / m_period : 60.0; }

private:
    Presenter() = default;
    void run() noexcept;
    HRESULT present_job(PresentJob& job) noexcept;
    // Latency = structural + GPU. `structural`: how far before the game's
    // Present the frame's earliest output sits on the content clock (exact).
    // `gpu`: seconds from that Present to the image being ready; with
    // `measured` false it is only an upper bound and can only lower the estimate.
    void track_latency(double structural, double gpu, bool measured) noexcept;
    bool later_output_due(const PresentJob& job, uint32_t index, double now) noexcept;
    HRESULT copy_to_native(ID3D12Resource* source, D3D12_RESOURCE_STATES state, bool generated) noexcept;
    HRESULT wait_fence(uint64_t value) noexcept;
    HRESULT native_present(const PresentJob& job, bool generated) noexcept;
    // What hold() decided, for the level-2 trace: the flip floor (0 = none) and
    // when the scan-line gate started polling (0 = no gate).
    struct HoldInfo {
        double floor = 0.0;
        double scanStart = 0.0;
    };
    // Holds until `notBefore`, then applies the tear guard.
    void hold(double notBefore, HoldInfo* info = nullptr) noexcept;
    void wait_until(double deadline) noexcept;
    // Re-reads the containing output: refresh period, active height and the
    // kernel adapter handle used for scan-line reads.
    void refresh_output() noexcept;
    void close_adapter() noexcept;
    bool read_scanline(bool& inBlank, UINT& line) noexcept;
    void log_window() noexcept;
    // Level 2: the latency parts and the window's per-output trace (#37).
    void log_trace() noexcept;

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
    FixedQueue<PresentJob, kPresenterQueueCapacity> m_jobs;
    OutputHooks m_hooks{};     // guarded by m_mutex
    OutputHooks m_job_hooks{}; // presenter thread's copy for the current job
    uint64_t m_submitted = 0;
    uint64_t m_completed = 0;
    HRESULT m_last = S_OK;
    bool m_stop = false;
    std::atomic<bool> m_cancel{false};
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
    // Last native_present: scan line read just before the flip and the call time.
    UINT m_flip_line = 0;
    int m_flip_blank = -1; // 1 blank, 0 scanning, -1 unread
    double m_flip_call = 0.0;

    uint64_t m_generated_shown = 0;
    uint64_t m_generated_dropped = 0;
    uint64_t m_back_to_back = 0;
    LatencyEstimator m_estimator;
    double m_latency = 0.0; // seconds from content time to flip, m_estimator.latency()

    // One output as the presenter handled it, for the level-2 trace (#37). Times
    // are seconds relative to the job's entry; 0 where a step did not happen.
    struct TraceRecord {
        uint32_t job = 0;
        uint8_t index = 0;
        bool real = false;
        bool alreadyReady = false;
        char drop = '-'; // '-' shown, 'i' later output in this job due, 'n' next job's output due
        float dequeued = 0.0f;
        float ready = 0.0f;
        float target = 0.0f;
        float floor = 0.0f;
        float scanStart = 0.0f;
        float flip = 0.0f;
        float call = 0.0f; // native Present duration, seconds
        float latency = 0.0f;
        UINT line = 0;
        int blank = -1;
    };
    static constexpr uint32_t kTraceRecords = 48;
    static constexpr uint32_t kTraceStartJob = 20; // into each 300-job window
    uint32_t m_job_serial = 0;

    // Diagnostics, reported every 300 jobs and then reset.
    struct Window {
        uint32_t jobs = 0;
        uint32_t generatedShown = 0;
        uint32_t generatedDropped = 0;
        uint32_t realsShown = 0;
        uint32_t realsDropped = 0;
        uint32_t late = 0; // flipped more than 1 ms after its target
        // `late` by cause (#37): the frame's first output was not finished on the
        // GPU by its target; a later output (or a job dequeued behind earlier
        // flips) was not ready by its target; ready in time but the floor or
        // scan-line gate held the flip past its target.
        uint32_t lateGpu = 0;
        uint32_t lateCarry = 0;
        uint32_t lateGuard = 0;
        TraceRecord trace[kTraceRecords]{};
        uint32_t traceCount = 0;
        double latencySum = 0.0;
        uint32_t latencySamples = 0;
        uint32_t gpuWaits = 0;
        uint32_t flips = 0;
        uint32_t shortGaps = 0;  // flip sooner than one refresh after the previous
        uint32_t backToBack = 0; // flip within 2 ms of the previous
        double gapMin = 0.0;
        double gpuWaitSum = 0.0; // waiting for the image to finish on the GPU
        double gpuWaitMax = 0.0;
        uint32_t guardHolds = 0; // the tear guard actually delayed a flip
        double guardSum = 0.0;
        double guardMax = 0.0;
        uint32_t guardTimeouts = 0;
        double callSum = 0.0; // native Present call duration
        double callMax = 0.0;
        uint32_t scan[6]{};     // at the flip: blank, 0-25, 25-50, 50-75, 75-100 %, unread
        uint32_t idleBlank = 0; // > one refresh since the last flip and the panel in blank
        uint32_t idleSamples = 0;
    } m_window;
};

} // namespace sedici::fg
