#pragma once

#include <dxgi1_6.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "settings/config.h"
#include "presentation/pacing.h"
#include "presentation/presenter.h"
#include "generator/stage1.h"

namespace sedici::fg {

// Records the midpoint for the previous/current real-frame pair.
//
// The wrapper owns the command list and the output resource; the hook only
// records. `output` is display resolution, RGBA8, UAV-capable, and is presented
// in the same cycle, before the current real frame and before any presentation
// wait. A midpoint that misses its deadline is dropped, never kept for next time.
//
// Lifetime contract (P2.4): `user` is owned by whoever installed the sink and must
// outlive it. The wrapper calls the sink only inside present(), under the same
// mutex set_generator() takes, so once set_generator() returns, no callback is
// running and none will start. An owner therefore detaches (installs an empty sink)
// before it is destroyed and needs no other synchronisation. Callbacks must not
// call back into the wrapper.
//
// Exception (K103): a generation whose submission was not registered is kept in
// m_untracked_sink, with its owner lease, until a queue fence is accepted. That
// path calls `submitted`/`abandoned` after detach, on purpose, so the owner can
// still register retirement (K78). It is not cleared by detach and is unbounded
// while retirement keeps failing (K77).
struct GenerateSink {
    void* user = nullptr;
    // `outputState` is the state `output` is in when recording starts; the hook
    // must restore it before returning, because the wrapper copies from it.
    bool (*record)(void* user, ID3D12GraphicsCommandList* list, ID3D12Resource* real,
                   ID3D12Resource* output, D3D12_RESOURCE_STATES outputState) = nullptr;
    // "vrr" schedules: records every phase of one real frame into one list.
    // Without it, `record` is called once per phase (enough for count-only
    // sinks; a real generator must advance its history once per real frame).
    bool (*recordPhases)(void* user, ID3D12GraphicsCommandList* list, ID3D12Resource* real,
                         const GeneratePhase* phases, uint32_t count) = nullptr;
    // Called after the recorded work was submitted and its fence signalled, so
    // the owner can register retirement (fg::Context::track_submission).
    bool (*submitted)(void* user, ID3D12Fence* fence, uint64_t value) = nullptr;
    // A recorded generation did not become a usable output; discard history
    // but keep any borrowed prepare protected until its queue fence is known.
    void (*abandoned)(void* user) = nullptr;
    // Optional owner lease, required for deferred retirement callbacks after
    // detachment. Offline stack-owned sinks may leave it empty.
    std::shared_ptr<void> lifetime;
    bool (*needsSubmission)(void* user) = nullptr;
};

// Wraps the existing native swapchain without replacing it or changing its
// buffer count. The game renders into private backbuffers and only this object
// copies into native presentation buffers, presenting a generated midpoint
// before the current real frame when a temporal pair is available.
// How the wrapper presents. `wrap` reads it from settings unless one is given.
struct PresentChoice {
    FrameGenerationSettings::Present present = FrameGenerationSettings::Present::Vsync;
    FrameGenerationSettings::TearGuard guard = FrameGenerationSettings::TearGuard::Scanline;
    FrameGenerationSettings::Multiplier multiplier = FrameGenerationSettings::Multiplier::X2;
    double targetFps = 0.0; // adaptive; 0 = three below the refresh rate
};

class Swapchain final : public IDXGISwapChain4 {
public:
    // All allocation happens before success. Neither the caller's pointer nor
    // the underlying swapchain is mutated on allocation failure.
    //
    // `handedRefs` is how many references to `native` the caller gives up to
    // the wrapper (the FFX wrap desc hands over the game's one). With "vrr" and
    // a handed-over reference, the native chain is recreated on Sedici's present
    // queue and the old one is destroyed, as AMD's swapchain does; if that is
    // not possible the wrapper falls back to "vsync" and still succeeds.
    static HRESULT wrap(IDXGISwapChain4* native, ID3D12CommandQueue* queue,
                        Swapchain** result, const PresentChoice* choice = nullptr,
                        ULONG handedRefs = 0) noexcept;

    // K47: a swapchain whose final Release could not drain stays parked, with every
    // GPU object intact, until its fence retires (or the device is removed); it is
    // then destroyed normally. Called from Present, Release and wrap. Never blocks.
    static void reap_retained() noexcept;
    static unsigned retained_now() noexcept;

    // Presenter jobs admitted but not yet completed (0 without a presenter).
    size_t presenter_outstanding() const noexcept { return m_presenter ? m_presenter->outstanding() : 0; }

    // What parking does to the presenter, exposed so the offline host can cancel it
    // at a chosen moment instead of racing the 5 s drain bound.
    void cancel_presenter() noexcept { if (m_presenter) m_presenter->request_stop(); }

    // True when "vrr" is active (presenter thread and queue in use).
    bool async_present() const noexcept { return m_presenter != nullptr; }
    // "vrr": flips issued within 2 ms of the previous one (0 without a presenter).
    uint64_t back_to_back_flips() const noexcept { return m_presenter ? m_presenter->back_to_back_flips() : 0; }

    // Without a sink the wrapper presents real frames only. Safe to change any
    // time; a pending midpoint is discarded when the sink is replaced or cleared.
    // False after close_generator(): a destroyed wrapper context may not be
    // reattached, even if the game still holds a COM reference to the wrapper.
    bool set_generator(const GenerateSink& sink) noexcept;
    // Clear only the sink installed by this owner; a stale Session must not
    // detach a newer Session's sink.
    bool clear_generator_if_owner(const void* owner) noexcept;
    // Atomically detach and forbid later attachment; waits for an in-flight
    // Present callback under the same wrapper mutex.
    void close_generator() noexcept;

    // Called once per application Present, before any output is produced, with
    // the QPC timestamp taken on entry. The real-frame limiter attaches here
    // because this is the one place that sees a game frame rather than an output
    // frame. Left unset, the wrapper does no pacing of its own.
    void set_present_hook(void (*hook)(uint64_t now_qpc_ticks)) noexcept;

    // Overlay drawing and output timing, on whichever path presents.
    void set_output_hooks(const OutputHooks& hooks) noexcept;

    // Generated midpoints presented since wrap, and midpoint deadline misses
    // that fell back to real-frame-only presentation.
    uint64_t generated_presents() const noexcept
    { return m_generated_presents + (m_presenter ? m_presenter->midpoints_shown() : 0); }
    uint64_t skipped_generates() const noexcept
    { return m_skipped_generates + (m_presenter ? m_presenter->midpoints_dropped() : 0); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID name, UINT size, const void* data) override;
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID name, const IUnknown* value) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID name, UINT* size, void* data) override;
    HRESULT STDMETHODCALLTYPE GetParent(REFIID iid, void** result) override;
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID iid, void** result) override;
    HRESULT STDMETHODCALLTYPE Present(UINT sync, UINT flags) override;
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT index, REFIID iid, void** result) override;
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL fullscreen, IDXGIOutput* target) override;
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* fullscreen, IDXGIOutput** target) override;
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* desc) override;
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags) override;
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* desc) override;
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** output) override;
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* stats) override;
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* count) override;
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* desc) override;
    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* desc) override;
    HRESULT STDMETHODCALLTYPE GetHwnd(HWND* hwnd) override;
    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID iid, void** result) override;
    HRESULT STDMETHODCALLTYPE Present1(UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* params) override;
    BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override;
    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** output) override;
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA* color) override;
    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* color) override;
    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION rotation) override;
    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* rotation) override;
    HRESULT STDMETHODCALLTYPE SetSourceSize(UINT width, UINT height) override;
    HRESULT STDMETHODCALLTYPE GetSourceSize(UINT* width, UINT* height) override;
    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT latency) override;
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* latency) override;
    HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override;
    HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F* matrix) override;
    HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F* matrix) override;
    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override;
    HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE color, UINT* support) override;
    HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE color) override;
    HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT count, UINT width, UINT height, DXGI_FORMAT format,
        UINT flags, const UINT* masks, IUnknown* const* queues) override;
    HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE type, UINT size, void* data) override;

private:
    Swapchain() = default;
    ~Swapchain();
    HRESULT initialize_buffers() noexcept;
    HRESULT drain() noexcept;
    void hold_untracked_generator() noexcept;
    bool resolve_untracked_generator() noexcept;
    void wait_until(double deadline, double maxWait) noexcept;
    HRESULT blit(ID3D12Resource* source, D3D12_RESOURCE_STATES* sourceState, bool generated) noexcept;
    // Absolute QPC deadline for arming a flip just before the next vertical
    // blank. Returns `now` when frame statistics are unavailable.
    double vblank_deadline(double now) noexcept;
    HRESULT present(UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* params) noexcept;
    // "vrr": generate on the game thread, hand both outputs to the presenter.
    HRESULT present_async(UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* params) noexcept;
    HRESULT record_generated() noexcept;
    // "vrr": records `count` phases into `outputs`; false when nothing usable
    // was submitted (no sink, or the generator refused the frame).
    bool record_phases(const GeneratePhase* phases, uint32_t count, uint32_t slot,
                       HRESULT& submissionFailure) noexcept;
    // "vrr": waits until the frame that last used `slot` has finished on the GPU.
    HRESULT wait_slot(uint32_t slot) noexcept;
    void configure_schedule(const PresentChoice& choice) noexcept;
    // Tells the settings layer the output ratio and the monitor's refresh rate
    // so the one target fps resolves to a real-frame cap (FR-05, K19). Called on
    // configure, on resize and every 300 cycles, so a monitor change is picked up.
    void publish_output_context() noexcept;
    double display_refresh_hz() const noexcept;
    double adaptive_target() const noexcept;
    // "vrr": copies the game's backbuffer into snapshot `slot` on the game queue.
    HRESULT snapshot_real(uint32_t slot) noexcept;
    HRESULT resize(UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags,
                   const UINT* masks, IUnknown* const* queues, bool version1) noexcept;
    // Keeps every GPU object alive after a drain that timed out on a live device.
    void retain_gpu_state() noexcept;
    // True when no GPU work or presenter job can still touch this object. Never
    // blocks and never takes m_mutex, so a stalled Present cannot hold it up.
    bool retirable() const noexcept;
    void park() noexcept;
    Swapchain* m_retained_next = nullptr;

    std::atomic<ULONG> m_refs{1};
    std::mutex m_mutex;
    Microsoft::WRL::ComPtr<IDXGISwapChain4> m_native;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue;
    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_allocator[2];
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_list[2];
    uint32_t m_next_list = 0;
    // Generation never shares an allocator with the presentation copies: it is
    // recorded after they were executed and their allocators are not yet free.
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_generate_allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_generate_list;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> m_buffers;

    // One in-flight midpoint, consumed or discarded in its own cycle.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_generated;
    // "vrr" only: the presenter may still copy last cycle's midpoint while this
    // cycle's is generated, so the two alternate.
    // "vrr" only. Up to kInFlight older frames may still be presenting while
    // the game renders the next one, so everything a job reads comes from a
    // ring of kInFlight + 1 slots that the game never touches: a snapshot of
    // the real frame (copied on the game queue right after generation, which
    // releases the game's own backbuffer in queue order) and the generated
    // images. With one frame in flight the game blocked on the presenter on most
    // frames at ~40 fps in adaptive (finding 009).
    static constexpr uint32_t kInFlight = static_cast<uint32_t>(kPresenterQueueCapacity - 1);
    static constexpr uint32_t kSlots = kInFlight + 1;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_phase_outputs[kSlots][kMaxPhases];
    Microsoft::WRL::ComPtr<ID3D12Resource> m_snapshots[kSlots];
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_snapshot_allocators[kSlots];
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_snapshot_lists[kSlots];
    // Generation lists per slot: with frames in flight the previous frame's
    // generation may still be executing when the next one is recorded.
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_slot_generate_allocators[kSlots];
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_slot_generate_lists[kSlots];
    uint64_t m_snapshot_fences[kSlots]{};
    uint32_t m_phase_set = 0;
    OutputSchedule m_schedule;
    // Per 300 game frames: when the window started and how many outputs were
    // planned, to report the real and output rates against the refresh rate.
    double m_rate_window_start = 0.0;
    uint32_t m_rate_window_outputs = 0;
    uint32_t m_rate_window_generated = 0;
    std::unique_ptr<Presenter> m_presenter;
    // Game-thread time spent waiting for the presenter to catch up.
    uint32_t m_backpressure_samples = 0;
    uint32_t m_backpressure_waits = 0;
    double m_backpressure_sum = 0.0;
    double m_backpressure_max = 0.0;
    D3D12_RESOURCE_STATES m_generated_state = D3D12_RESOURCE_STATE_COMMON;
    GenerateSink m_sink{};
    GenerateSink m_untracked_sink{}; // owner lease until a queue fence is registered
    bool m_generator_closed = false; // guarded by m_mutex
    void (*m_present_hook)(uint64_t) = nullptr;
    OutputHooks m_output_hooks{};
    // Test-only A/B (`SEDICI_FG_SYNC=1`): present with SyncInterval=1 and drop
    // DXGI_PRESENT_ALLOW_TEARING, to prove whether tearing is flip-phase
    // misalignment. Off by default; the game's own flags are used otherwise.
    bool m_force_sync = false;
    // Opt-in (`SEDICI_FG_ALIGN=1`): arm each Present shortly before a vertical
    // blank using DXGI_FRAME_STATISTICS, so the flip takes effect at the start
    // of a scan instead of partway through one. Keeps SyncInterval=0.
    bool m_align_vblank = false;
    bool m_have_vblank = false;
    uint32_t m_vblank_count = 0;
    uint64_t m_vblank_qpc = 0;
    double m_vblank_period = 1.0 / 75.0;
    uint32_t m_align_samples = 0;
    uint32_t m_align_misses = 0;
    bool m_generated_ready = false;
    uint64_t m_generated_presents = 0;
    uint64_t m_skipped_generates = 0;

    UINT m_game_index = 0;   // game-facing buffer, advances once per cycle
    UINT64 m_fence_value = 0;
    // Once submission retirement cannot be established, do not admit further
    // frames or reuse any slot until the wrapper is replaced (K37).
    HRESULT m_submission_failure = S_OK;
    HANDLE m_event = nullptr;
    HANDLE m_pacing_timer = nullptr;

    PacingState m_pacing;
    uint64_t m_present_cycles = 0;
    uint32_t m_gap_samples = 0;
    double m_gap_sum = 0.0;
    double m_gap_min = 0.0;
    double m_gap_max = 0.0;

    // Diagnostic-only evidence, never used to decide what to present:    // - whether the GPU finished the midpoint copy before the real frame was
    //   presented, which distinguishes "queued by DXGI" from "not rendered yet";
    // - how often a cycle had to wait for the previous real copy to retire.
    uint64_t m_mid_checks = 0;
    uint64_t m_mid_not_ready = 0;
    uint32_t m_drain_samples = 0;
    uint64_t m_drain_waits = 0;
    double m_drain_wait_sum = 0.0;
    double m_drain_wait_max = 0.0;
};

} // namespace sedici::fg
