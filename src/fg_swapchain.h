#pragma once

#include <dxgi1_6.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <atomic>
#include <mutex>
#include <vector>

namespace sedici::fg {

// Wraps the existing native swapchain without replacing it or changing its
// buffer count. The game renders into private backbuffers; only this object
// copies into native presentation buffers. Not routed from exports yet.
class Swapchain final : public IDXGISwapChain4 {
public:
    // All allocation happens before success. Neither the caller's pointer nor
    // the underlying swapchain is mutated on allocation failure.
    static HRESULT wrap(IDXGISwapChain4* native, ID3D12CommandQueue* queue,
                        Swapchain** result) noexcept;

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
    HRESULT present(UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* params) noexcept;
    HRESULT resize(UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags,
                   const UINT* masks, IUnknown* const* queues, bool version1) noexcept;

    std::atomic<ULONG> m_refs{1};
    std::mutex m_mutex;
    Microsoft::WRL::ComPtr<IDXGISwapChain4> m_native;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue;
    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_list;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> m_buffers;
    UINT m_current = 0;
    UINT64 m_fence_value = 0;
    HANDLE m_event = nullptr;
};

} // namespace sedici::fg
