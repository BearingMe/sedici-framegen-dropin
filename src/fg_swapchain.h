#pragma once

#include <dxgi1_6.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "fg_pacing.h"

namespace sedici::fg {

// Records the midpoint for the real frame the game just finished.
//
// The wrapper owns the command list and the output resource; the hook only
// records. `output` is display resolution, RGBA8, UAV-capable, and is presented
// on the *next* cycle, so a midpoint that misses its deadline is dropped instead
// of delaying the real frame.
struct GenerateSink {
    void* user = nullptr;
    // `outputState` is the state `output` is in when recording starts; the hook
    // must restore it before returning, because the wrapper copies from it.
    bool (*record)(void* user, ID3D12GraphicsCommandList* list, ID3D12Resource* real,
                   ID3D12Resource* output, D3D12_RESOURCE_STATES outputState) = nullptr;
    // Called after the recorded work was submitted and its fence signalled, so
    // the owner can register retirement (fg::Context::track_submission).
    void (*submitted)(void* user, ID3D12Fence* fence, uint64_t value) = nullptr;
};

// Wraps the existing native swapchain without replacing it or changing its
// buffer count. The game renders into private backbuffers and only this object
// copies into native presentation buffers, presenting a generated midpoint
// before the real frame when one is available. One cycle of generated-frame
// latency is inherent: a midpoint between frames N-1 and N cannot precede N.
class Swapchain final : public IDXGISwapChain4 {
public:
    // All allocation happens before success. Neither the caller's pointer nor
    // the underlying swapchain is mutated on allocation failure.
    static HRESULT wrap(IDXGISwapChain4* native, ID3D12CommandQueue* queue,
                        Swapchain** result) noexcept;

    // Without a sink the wrapper presents real frames only. Safe to change any
    // time; a pending midpoint is discarded when the sink is replaced or cleared.
    void set_generator(const GenerateSink& sink) noexcept;

    // Generated midpoints presented since wrap, and midpoint deadline misses
    // that fell back to real-frame-only presentation.
    uint64_t generated_presents() const noexcept { return m_generated_presents; }
    uint64_t skipped_generates() const noexcept { return m_skipped_generates; }

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
    HRESULT blit(ID3D12Resource* source, D3D12_RESOURCE_STATES* sourceState) noexcept;
    HRESULT present(UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* params) noexcept;
    void record_generated(double now) noexcept;
    HRESULT resize(UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags,
                   const UINT* masks, IUnknown* const* queues, bool version1) noexcept;

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

    // One-cycle-deep generated frame. Bounded by construction: a new midpoint is
    // never recorded while an unshown one exists.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_generated;
    D3D12_RESOURCE_STATES m_generated_state = D3D12_RESOURCE_STATE_COMMON;
    GenerateSink m_sink{};
    bool m_generated_ready = false;
    uint64_t m_generated_presents = 0;
    uint64_t m_skipped_generates = 0;

    UINT m_game_index = 0;   // game-facing buffer, advances once per cycle
    UINT m_native_index = 0;
    UINT64 m_fence_value = 0;
    HANDLE m_event = nullptr;

    double m_last_cycle = 0.0;
    double m_last_game_present = 0.0;
    double m_interval = 1.0 / 60.0;
    bool m_cycle_started = false;
};

} // namespace sedici::fg
