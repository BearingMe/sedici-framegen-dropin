#include "fg_swapchain.h"

#include <new>

namespace sedici::fg {

HRESULT Swapchain::wrap(IDXGISwapChain4* native, ID3D12CommandQueue* queue,
                        Swapchain** result) noexcept
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
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&object->m_allocator));
    if (SUCCEEDED(hr)) hr = object->m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        object->m_allocator.Get(), nullptr, IID_PPV_ARGS(&object->m_list));
    if (SUCCEEDED(hr)) hr = object->m_list->Close();
    if (SUCCEEDED(hr)) hr = object->m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
        IID_PPV_ARGS(&object->m_fence));
    if (SUCCEEDED(hr)) {
        object->m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!object->m_event) hr = HRESULT_FROM_WIN32(GetLastError());
    }
    if (SUCCEEDED(hr)) hr = object->initialize_buffers();
    if (FAILED(hr)) { delete object; return hr; }
    *result = object;
    return S_OK;
}

Swapchain::~Swapchain()
{
    (void)drain();
    if (m_event) CloseHandle(m_event);
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
    m_buffers = std::move(buffers);
    m_current = 0;
    return S_OK;
}
catch (...) { return E_OUTOFMEMORY; }

HRESULT Swapchain::drain() noexcept
{
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

HRESULT Swapchain::present(UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* params) noexcept
{
    std::lock_guard lock(m_mutex);
    if (flags & DXGI_PRESENT_TEST) return params ? m_native->Present1(sync, flags, params) : m_native->Present(sync, flags);
    if (m_buffers.empty()) return DXGI_ERROR_INVALID_CALL;
    if ((flags & DXGI_PRESENT_DO_NOT_WAIT) && m_fence->GetCompletedValue() < m_fence_value)
        return DXGI_ERROR_WAS_STILL_DRAWING;
    HRESULT hr = drain();
    if (FAILED(hr)) return hr;
    Microsoft::WRL::ComPtr<ID3D12Resource> destination;
    hr = m_native->GetBuffer(m_native->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&destination));
    if (FAILED(hr)) return hr;
    hr = m_allocator->Reset();
    if (SUCCEEDED(hr)) hr = m_list->Reset(m_allocator.Get(), nullptr);
    if (FAILED(hr)) return hr;
    auto* source = m_buffers[m_current].Get();
    D3D12_RESOURCE_BARRIER barriers[2]{};
    for (auto& barrier : barriers) {
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    }
    barriers[0].Transition.pResource = source;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriers[1].Transition.pResource = destination.Get();
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    m_list->ResourceBarrier(2, barriers);
    m_list->CopyResource(destination.Get(), source);
    for (auto& barrier : barriers) std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    m_list->ResourceBarrier(2, barriers);
    hr = m_list->Close();
    if (FAILED(hr)) return hr;
    ID3D12CommandList* lists[]{m_list.Get()};
    m_queue->ExecuteCommandLists(1, lists);
    hr = params ? m_native->Present1(sync, flags, params) : m_native->Present(sync, flags);
    // Retire both our copy AND any work DXGI adds to the queue for Present.
    // A fence before Present is insufficient for releasing native buffers on resize.
    const HRESULT signaled = m_queue->Signal(m_fence.Get(), ++m_fence_value);
    if (FAILED(signaled)) return signaled;
    if (SUCCEEDED(hr) && hr != DXGI_STATUS_OCCLUDED) m_current = (m_current + 1) % static_cast<UINT>(m_buffers.size());
    return hr;
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
UINT STDMETHODCALLTYPE Swapchain::GetCurrentBackBufferIndex() { std::lock_guard lock(m_mutex); return m_current; }

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
    // Resize must fail while the application owns a backbuffer reference, just
    // as native DXGI does. Do not destroy buffers behind the application's back.
    for (auto& buffer : m_buffers) {
        buffer->AddRef();
        if (buffer->Release() != 1) return DXGI_ERROR_INVALID_CALL;
    }
    HRESULT hr = drain();
    if (FAILED(hr)) return hr;
    hr = version1 ? m_native->ResizeBuffers1(count, width, height, format, flags, masks, queues)
                  : m_native->ResizeBuffers(count, width, height, format, flags);
    if (FAILED(hr)) return hr;
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
FORWARD_HR(SetFullscreenState, (BOOL f, IDXGIOutput* t), (f, t))
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
BOOL STDMETHODCALLTYPE Swapchain::IsTemporaryMonoSupported() { return m_native->IsTemporaryMonoSupported(); }
HANDLE STDMETHODCALLTYPE Swapchain::GetFrameLatencyWaitableObject() { return m_native->GetFrameLatencyWaitableObject(); }

} // namespace sedici::fg
