// Offline acceptance host for stage 1 of the frame generator.
//
// Feeds synthetic scenes through the production pipeline (the same Stage1 the
// DLL uses), then compares each generated midpoint against the scene evaluated
// at the midpoint: PSNR, SSIM and GPU time. No engine, no game and no captured
// data are required, so the analytical stage can be checked and tuned before the
// first deploy.
//
// Scenes are a static gradient background at the far plane with a moving bar in
// front. Motion vectors follow the convention the shaders use: the displacement
// of content over one frame, previous to current, in pixels.

#include <d3d12.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <chrono>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <DirectXPackedVector.h>

#include "fg_context.h"
#include "config.h"
#include "fg_integration.h"
#include "fg_pacing.h"
#include "fg_params.h"
#include "fg_route.h"
#include "fg_bookkeeping_faults.h"
#include "fg_stage1.h"
#include "fg_swapchain.h"
#include "log.h"

#include "ffx_api_framegeneration_dx12.h"
#include "ffx_framegeneration.h"

// forward.cpp reports the marker; the DLL defines it in dllmain.cpp, hosts here.
extern "C" const char g_sedici_proxy_marker[] = "SEDICI_PROXY_V1";

using Microsoft::WRL::ComPtr;

#pragma warning(push)
#pragma warning(disable : 4996) // Exercise the desc Cyberpunk actually uses.

namespace {
std::atomic<unsigned> g_debug_errors{0};
bool g_debug_callback_installed = false;

constexpr uint32_t kDisplayWidth = 480;
constexpr uint32_t kDisplayHeight = 270;
constexpr uint32_t kRenderWidth = 320;
constexpr uint32_t kRenderHeight = 180;
constexpr uint32_t kIterations = 40;

struct SceneConfig {
    const char* name;
    float travelX;      // screen widths per frame
    float travelY;      // screen heights per frame
    float barWidth;     // fraction of the width
    float startX;
    float startY;
};

constexpr SceneConfig kScenes[] = {
    {"rightward pan", 0.10f, 0.0f, 0.12f, 0.35f, 0.50f},
    {"leftward pan", -0.05f, 0.0f, 0.20f, 0.70f, 0.50f},
    {"diagonal thin bar", 0.06f, 0.04f, 0.03f, 0.30f, 0.40f},
};

struct Image {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<float> color;  // rgba
    std::vector<float> depth;  // one channel, reversed-Z (near = 1)
    std::vector<float> motion; // two channels, pixels
};

float smooth_step(float edge0, float edge1, float value)
{
    const float t = std::clamp((value - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Renders the scene at time `t` in [0, 1] (0 = previous frame, 1 = current).
Image make_scene(const SceneConfig& config, float t, uint32_t width, uint32_t height)
{
    Image image;
    image.width = width;
    image.height = height;
    image.color.resize(static_cast<size_t>(width) * height * 4);
    image.depth.resize(static_cast<size_t>(width) * height);
    image.motion.resize(static_cast<size_t>(width) * height * 2);

    const float widthf = static_cast<float>(width);
    const float heightf = static_cast<float>(height);

    const float centreX = (config.startX + config.travelX * t) * widthf;
    const float centreY = (config.startY + config.travelY * t) * heightf;
    const float halfWidth = 0.5f * config.barWidth * widthf;
    const float travelPixelsX = config.travelX * widthf;
    const float travelPixelsY = config.travelY * heightf;
    const float feather = 1.25f;

    for (uint32_t y = 0; y < height; ++y) {
        const float v = (static_cast<float>(y) + 0.5f) / heightf;
        for (uint32_t x = 0; x < width; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / widthf;
            const float pixelX = static_cast<float>(x) + 0.5f;
            const float pixelY = static_cast<float>(y) + 0.5f;

            const float horizontal = smooth_step(halfWidth, halfWidth - feather,
                                                 std::abs(pixelX - centreX));
            // Full height for horizontal scenes, a box when the scene also
            // moves vertically (that is where disocclusion shows up).
            const float halfHeight = config.travelY != 0.0f ? 0.15f * heightf : 0.5f * heightf;
            const float vertical = smooth_step(halfHeight, halfHeight - feather,
                                               std::abs(pixelY - centreY));
            const float inside = horizontal * vertical;

            const size_t index = static_cast<size_t>(y) * width + x;

            const float backgroundR = 0.10f + 0.25f * v;
            const float backgroundG = 0.14f + 0.30f * v;
            const float backgroundB = 0.22f + 0.20f * u;
            constexpr float barR = 0.90f, barG = 0.55f, barB = 0.15f;

            image.color[index * 4 + 0] = backgroundR + (barR - backgroundR) * inside;
            image.color[index * 4 + 1] = backgroundG + (barG - backgroundG) * inside;
            image.color[index * 4 + 2] = backgroundB + (barB - backgroundB) * inside;
            image.color[index * 4 + 3] = 1.0f;

            image.depth[index] = inside > 0.5f ? 1.0f : 0.0f;

            image.motion[index * 2 + 0] = inside > 0.5f ? travelPixelsX : 0.0f;
            image.motion[index * 2 + 1] = inside > 0.5f ? travelPixelsY : 0.0f;
        }
    }

    return image;
}

uint32_t align_to(uint32_t value, uint32_t alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

std::vector<uint8_t> quantise(const std::vector<float>& values)
{
    std::vector<uint8_t> result(values.size());
    for (size_t i = 0; i < values.size(); ++i) {
        if (i % 4 == 3) {
            result[i] = 255;
            continue;
        }
        result[i] = static_cast<uint8_t>(std::lround(std::clamp(values[i], 0.0f, 1.0f) * 255.0f));
    }
    return result;
}

std::vector<uint16_t> pack_ffx_motion(const std::vector<float>& values)
{
    std::vector<uint16_t> result(values.size() * 2, 0);
    for (size_t i = 0; i < values.size() / 2; ++i) {
        // Cyberpunk-shaped RGBA16F: FFX current->previous UV vectors.
        result[i * 4] = DirectX::PackedVector::XMConvertFloatToHalf(-values[i * 2] / kRenderWidth);
        result[i * 4 + 1] = DirectX::PackedVector::XMConvertFloatToHalf(-values[i * 2 + 1] / kRenderHeight);
    }
    return result;
}

ComPtr<ID3D12Resource> create_buffer(ID3D12Device* device, uint64_t size,
                                     D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = heap;

    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = size;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> resource;
    if (FAILED(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &description,
                                               state, nullptr, IID_PPV_ARGS(&resource)))) {
        return nullptr;
    }
    return resource;
}

ComPtr<ID3D12Resource> create_texture(ID3D12Device* device, DXGI_FORMAT format,
                                       uint32_t width, uint32_t height,
                                       D3D12_RESOURCE_FLAGS flags,
                                       const D3D12_CLEAR_VALUE* clear = nullptr)
{
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = width;
    description.Height = height;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = format;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    description.Flags = flags;

    ComPtr<ID3D12Resource> resource;
    if (FAILED(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &description,
                                               D3D12_RESOURCE_STATE_COMMON, clear,
                                               IID_PPV_ARGS(&resource)))) {
        return nullptr;
    }
    return resource;
}

void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    if (before == after) return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    list->ResourceBarrier(1, &barrier);
}

void upload(ID3D12GraphicsCommandList* list, ID3D12Resource* texture, ID3D12Resource* staging,
            const void* data, uint32_t width, uint32_t height, uint32_t pixelBytes)
{
    const uint32_t rowPitch = align_to(width * pixelBytes, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    void* mapped = nullptr;
    if (FAILED(staging->Map(0, nullptr, &mapped))) return;

    const auto* source = static_cast<const uint8_t*>(data);
    auto* destination = static_cast<uint8_t*>(mapped);
    for (uint32_t y = 0; y < height; ++y) {
        std::memcpy(destination + static_cast<size_t>(y) * rowPitch,
                    source + static_cast<size_t>(y) * width * pixelBytes,
                    static_cast<size_t>(width) * pixelBytes);
    }
    staging->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION from{};
    from.pResource = staging;
    from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from.PlacedFootprint.Footprint.Format = texture->GetDesc().Format;
    from.PlacedFootprint.Footprint.Width = width;
    from.PlacedFootprint.Footprint.Height = height;
    from.PlacedFootprint.Footprint.Depth = 1;
    from.PlacedFootprint.Footprint.RowPitch = rowPitch;

    D3D12_TEXTURE_COPY_LOCATION to{};
    to.pResource = texture;
    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.SubresourceIndex = 0;

    transition(list, texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    transition(list, texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
}

double psnr(const std::vector<uint8_t>& reference, const std::vector<uint8_t>& candidate)
{
    if (reference.size() != candidate.size() || reference.empty()) return 0.0;

    double sum = 0.0;
    size_t samples = 0;
    for (size_t i = 0; i < reference.size(); ++i) {
        if (i % 4 == 3) continue;
        const double difference = static_cast<double>(reference[i]) - static_cast<double>(candidate[i]);
        sum += difference * difference;
        ++samples;
    }
    if (samples == 0) return 0.0;
    const double mse = sum / static_cast<double>(samples);
    if (mse <= 0.0) return 99.0;
    return 10.0 * std::log10((255.0 * 255.0) / mse);
}

double ssim(const std::vector<uint8_t>& reference, const std::vector<uint8_t>& candidate,
            uint32_t width, uint32_t height)
{
    constexpr double c1 = 6.5025;
    constexpr double c2 = 58.5225;
    constexpr uint32_t window = 8;
    constexpr uint32_t stride = 4;

    auto luma = [width](const std::vector<uint8_t>& pixels, uint32_t x, uint32_t y) {
        const size_t index = (static_cast<size_t>(y) * width + x) * 4;
        return 0.299 * pixels[index] + 0.587 * pixels[index + 1] + 0.114 * pixels[index + 2];
    };

    double total = 0.0;
    uint32_t windows = 0;
    for (uint32_t y = 0; y + window <= height; y += stride) {
        for (uint32_t x = 0; x + window <= width; x += stride) {
            double sumA = 0.0, sumB = 0.0, sumAA = 0.0, sumBB = 0.0, sumAB = 0.0;
            for (uint32_t dy = 0; dy < window; ++dy) {
                for (uint32_t dx = 0; dx < window; ++dx) {
                    const double a = luma(reference, x + dx, y + dy);
                    const double b = luma(candidate, x + dx, y + dy);
                    sumA += a;
                    sumB += b;
                    sumAA += a * a;
                    sumBB += b * b;
                    sumAB += a * b;
                }
            }
            const double n = static_cast<double>(window) * window;
            const double meanA = sumA / n, meanB = sumB / n;
            const double varA = sumAA / n - meanA * meanA;
            const double varB = sumBB / n - meanB * meanB;
            const double covariance = sumAB / n - meanA * meanB;
            const double numerator = (2.0 * meanA * meanB + c1) * (2.0 * covariance + c2);
            const double denominator = (meanA * meanA + meanB * meanB + c1) * (varA + varB + c2);
            total += denominator != 0.0 ? numerator / denominator : 0.0;
            ++windows;
        }
    }
    return windows ? total / static_cast<double>(windows) : 0.0;
}

bool save_ppm(const char* path, const std::vector<uint8_t>& pixels, uint32_t width, uint32_t height)
{
    FILE* file = nullptr;
    if (fopen_s(&file, path, "wb") != 0 || !file) return false;
    std::fprintf(file, "P6\n%u %u\n255\n", width, height);
    for (size_t i = 0; i < pixels.size(); i += 4) {
        std::fwrite(&pixels[i], 1, 3, file);
    }
    std::fclose(file);
    return true;
}

struct Device {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    uint64_t fenceValue = 0;

    bool wait()
    {
        if (FAILED(queue->Signal(fence.Get(), ++fenceValue))) return false;
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) return false;
        const HRESULT hr = fence->SetEventOnCompletion(fenceValue, event);
        const DWORD result = SUCCEEDED(hr) ? WaitForSingleObject(event, 5000) : WAIT_FAILED;
        CloseHandle(event);
        return result == WAIT_OBJECT_0 && SUCCEEDED(device->GetDeviceRemovedReason());
    }

    bool submit()
    {
        if (FAILED(list->Close())) return false;
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        return wait();
    }
};

// The chronology check uses FLIP_SEQUENTIAL so previously presented native
// buffers retain their contents. Read the actual copies, not a callback count.
std::vector<uint8_t> read_presented(Device& state, ID3D12Resource* texture)
{
    const auto desc = texture->GetDesc();
    const auto width = static_cast<uint32_t>(desc.Width);
    const auto pitch = align_to(width * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    auto buffer = create_buffer(state.device.Get(), static_cast<uint64_t>(pitch) * desc.Height,
        D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!buffer || FAILED(state.allocator->Reset()) ||
        FAILED(state.list->Reset(state.allocator.Get(), nullptr))) return {};
    transition(state.list.Get(), texture, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION from{};
    from.pResource = texture;
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION to{};
    to.pResource = buffer.Get();
    to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, width, desc.Height, 1, pitch};
    state.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    transition(state.list.Get(), texture, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    if (!state.submit()) return {};
    void* mapped = nullptr;
    if (FAILED(buffer->Map(0, nullptr, &mapped))) return {};
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * desc.Height * 4);
    for (uint32_t y = 0; y < desc.Height; ++y)
        std::memcpy(pixels.data() + static_cast<size_t>(y) * width * 4,
                    static_cast<const uint8_t*>(mapped) + static_cast<size_t>(y) * pitch, width * 4);
    buffer->Unmap(0, nullptr);
    return pixels;
}

struct FrameTextures {
    ComPtr<ID3D12Resource> color;
    ComPtr<ID3D12Resource> depth;
    ComPtr<ID3D12Resource> motion;
};

bool run_case(Device& state, const SceneConfig& config, bool keepImages)
{
    const auto previousDisplay = make_scene(config, 0.0f, kDisplayWidth, kDisplayHeight);
    const auto currentDisplay = make_scene(config, 1.0f, kDisplayWidth, kDisplayHeight);
    const auto truth = make_scene(config, 0.5f, kDisplayWidth, kDisplayHeight);

    const auto previousRender = make_scene(config, 0.0f, kRenderWidth, kRenderHeight);
    const auto currentRender = make_scene(config, 1.0f, kRenderWidth, kRenderHeight);

    const std::vector<uint8_t> previousColor = quantise(previousDisplay.color);
    const std::vector<uint8_t> currentColor = quantise(currentDisplay.color);
    const std::vector<uint16_t> previousMotion = pack_ffx_motion(previousRender.motion);
    const std::vector<uint16_t> currentMotion = pack_ffx_motion(currentRender.motion);

    FrameTextures frames[2];
    for (int index = 0; index < 2; ++index) {
        frames[index].color = create_texture(state.device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM,
                                             kDisplayWidth, kDisplayHeight,
                                             D3D12_RESOURCE_FLAG_NONE);
        // Match the game's depth-target allocation; FFX describes it as R32_FLOAT.
        frames[index].depth = create_texture(state.device.Get(), DXGI_FORMAT_D32_FLOAT,
                                              kRenderWidth, kRenderHeight,
                                              D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        frames[index].motion = create_texture(state.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
                                              kRenderWidth, kRenderHeight,
                                              D3D12_RESOURCE_FLAG_NONE);
        if (!frames[index].color || !frames[index].depth || !frames[index].motion) {
            std::fprintf(stderr, "input textures failed\n");
            return false;
        }
    }

    const auto output = create_texture(state.device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM,
                                       kDisplayWidth, kDisplayHeight,
                                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (!output) {
        std::fprintf(stderr, "output texture failed\n");
        return false;
    }

    const uint32_t colorRowPitch = align_to(kDisplayWidth * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    const uint32_t renderRowPitch = align_to(kRenderWidth * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    const uint64_t colorBytes = static_cast<uint64_t>(colorRowPitch) * kDisplayHeight;
    const uint64_t renderBytes = static_cast<uint64_t>(renderRowPitch) * kRenderHeight;
    const uint64_t motionBytes = static_cast<uint64_t>(align_to(kRenderWidth * 8,
        D3D12_TEXTURE_DATA_PITCH_ALIGNMENT)) * kRenderHeight;

    for (int index = 0; index < 2; ++index) {
        const auto& color = index == 0 ? previousColor : currentColor;
        const auto& motion = index == 0 ? previousMotion : currentMotion;
        const auto& render = index == 0 ? previousRender : currentRender;

        auto colorStaging = create_buffer(state.device.Get(), colorBytes, D3D12_HEAP_TYPE_UPLOAD,
                                          D3D12_RESOURCE_STATE_GENERIC_READ);
        auto depthStaging = create_buffer(state.device.Get(), renderBytes, D3D12_HEAP_TYPE_UPLOAD,
                                          D3D12_RESOURCE_STATE_GENERIC_READ);
        auto motionStaging = create_buffer(state.device.Get(), motionBytes, D3D12_HEAP_TYPE_UPLOAD,
                                           D3D12_RESOURCE_STATE_GENERIC_READ);

        state.allocator->Reset();
        state.list->Reset(state.allocator.Get(), nullptr);
        upload(state.list.Get(), frames[index].color.Get(), colorStaging.Get(), color.data(),
               kDisplayWidth, kDisplayHeight, 4);
        upload(state.list.Get(), frames[index].depth.Get(), depthStaging.Get(), render.depth.data(),
               kRenderWidth, kRenderHeight, 4);
        upload(state.list.Get(), frames[index].motion.Get(), motionStaging.Get(), motion.data(),
               kRenderWidth, kRenderHeight, 8);
        if (!state.submit()) {
            std::fprintf(stderr, "upload failed\n");
            return false;
        }

    }

    sedici::fg::Context context;
    ffxCreateBackendDX12Desc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = state.device.Get();
    ffxCreateContextDescFrameGeneration create{};
    create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    create.header.pNext = &backend.header;
    create.flags = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED;
    create.displaySize = {kDisplayWidth, kDisplayHeight};
    create.maxRenderSize = {kRenderWidth, kRenderHeight};
    create.backBufferFormat = FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
    if (!context.create(&create.header)) {
        std::fprintf(stderr, "context initialization failed\n");
        return false;
    }

    D3D12_QUERY_HEAP_DESC queryDescription{};
    queryDescription.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    queryDescription.Count = 2;
    ComPtr<ID3D12QueryHeap> queryHeap;
    if (FAILED(state.device->CreateQueryHeap(&queryDescription, IID_PPV_ARGS(&queryHeap)))) {
        std::fprintf(stderr, "query heap failed\n");
        return false;
    }
    auto timings = create_buffer(state.device.Get(), 2 * sizeof(uint64_t), D3D12_HEAP_TYPE_READBACK,
                                 D3D12_RESOURCE_STATE_COPY_DEST);

    const uint32_t outputRowPitch = align_to(kDisplayWidth * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    auto readback = create_buffer(state.device.Get(),
                                  static_cast<uint64_t>(outputRowPitch) * kDisplayHeight,
                                  D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);

    D3D12_RESOURCE_STATES outputState = D3D12_RESOURCE_STATE_COMMON;
    uint64_t ticks = 0;
    uint32_t measured = 0;

    for (uint32_t iteration = 0; iteration < kIterations; ++iteration) {
        const uint32_t source = iteration % 2;
        if (FAILED(state.allocator->Reset()) || FAILED(state.list->Reset(state.allocator.Get(), nullptr))) return false;
        state.list->EndQuery(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        ffxConfigureDescFrameGeneration configure{};
        configure.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        configure.frameGenerationEnabled = true;
        configure.HUDLessColor = ffxApiGetResourceDX12(frames[source].color.Get(), FFX_API_RESOURCE_STATE_COMMON);
        configure.frameID = iteration;
        ffxDispatchDescFrameGenerationPrepare prepare{};
        prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
        prepare.frameID = iteration;
        prepare.commandList = state.list.Get();
        prepare.renderSize = {kRenderWidth, kRenderHeight};
        prepare.motionVectorScale = {static_cast<float>(kRenderWidth), static_cast<float>(kRenderHeight)};
        prepare.depth = ffxApiGetResourceDX12(frames[source].depth.Get(), FFX_API_RESOURCE_STATE_COMMON);
        prepare.motionVectors = ffxApiGetResourceDX12(frames[source].motion.Get(), FFX_API_RESOURCE_STATE_COMMON);
        // Repeating pairs, not a physically incorrect backwards step with
        // forward-pointing motion. Each pair begins with a history reset.
        prepare.unused_reset = source == 0;
        if (!context.configure(&configure.header) || !context.prepare(&prepare.header)) return false;

        if (!context.generate(state.list.Get(), output.Get(), outputState)) {
            std::fprintf(stderr, "stage 1 rejected the frame\n");
            return false;
        }
        state.list->EndQuery(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        state.list->ResolveQueryData(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2,
                                     timings.Get(), 0);

        transition(state.list.Get(), output.Get(), outputState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        outputState = D3D12_RESOURCE_STATE_COPY_SOURCE;

        D3D12_TEXTURE_COPY_LOCATION from{};
        from.pResource = output.Get();
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

        D3D12_TEXTURE_COPY_LOCATION to{};
        to.pResource = readback.Get();
        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        to.PlacedFootprint.Footprint.Width = kDisplayWidth;
        to.PlacedFootprint.Footprint.Height = kDisplayHeight;
        to.PlacedFootprint.Footprint.Depth = 1;
        to.PlacedFootprint.Footprint.RowPitch = outputRowPitch;

        state.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        if (!state.submit()) {
            std::fprintf(stderr, "dispatch failed\n");
            return false;
        }
        if (!context.track_submission(state.fence.Get(), state.fenceValue)) return false;

        void* mapped = nullptr;
        if (SUCCEEDED(timings->Map(0, nullptr, &mapped))) {
            const auto* values = static_cast<const uint64_t*>(mapped);
            if (source == 1 && values[1] > values[0]) {
                ticks += values[1] - values[0];
                ++measured;
            }
            timings->Unmap(0, nullptr);
        }
    }

    uint64_t frequency = 0;
    state.queue->GetTimestampFrequency(&frequency);

    std::vector<uint8_t> generated(static_cast<size_t>(kDisplayWidth) * kDisplayHeight * 4);
    void* mapped = nullptr;
    if (FAILED(readback->Map(0, nullptr, &mapped))) {
        std::fprintf(stderr, "readback failed\n");
        return false;
    }
    for (uint32_t y = 0; y < kDisplayHeight; ++y) {
        std::memcpy(generated.data() + static_cast<size_t>(y) * kDisplayWidth * 4,
                    static_cast<const uint8_t*>(mapped) + static_cast<size_t>(y) * outputRowPitch,
                    static_cast<size_t>(kDisplayWidth) * 4);
    }
    readback->Unmap(0, nullptr);

    const auto reference = quantise(truth.color);
    const double psnrValue = psnr(reference, generated);
    const double ssimValue = ssim(reference, generated, kDisplayWidth, kDisplayHeight);
    const double milliseconds = measured && frequency
        ? static_cast<double>(ticks) / measured / static_cast<double>(frequency) * 1000.0
        : 0.0;

    std::printf("%-18s psnr %6.2f dB  ssim %.4f  stage1 %.3f ms\n",
                config.name, psnrValue, ssimValue, milliseconds);

    if (keepImages) {
        char name[128];
        std::snprintf(name, sizeof(name), "fgcheck-%s-reference.ppm", config.name);
        save_ppm(name, reference, kDisplayWidth, kDisplayHeight);
        std::snprintf(name, sizeof(name), "fgcheck-%s-generated.ppm", config.name);
        save_ppm(name, generated, kDisplayWidth, kDisplayHeight);
    }

    // Phases: two generated frames from one prepare, at 1/3 and 2/3, into two
    // outputs recorded in one command list (the "vrr" 1.5x/1.75x/adaptive path).
    // Each must be closer to the true scene at its time than simply showing the
    // nearer real frame, which a wrong or shared alpha would not be.
    {
        const auto second = create_texture(state.device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM,
                                           kDisplayWidth, kDisplayHeight,
                                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (!second) return false;
        D3D12_RESOURCE_STATES secondState = D3D12_RESOURCE_STATE_COMMON;
        for (uint32_t step = 0; step < 2; ++step) {
            if (FAILED(state.allocator->Reset()) || FAILED(state.list->Reset(state.allocator.Get(), nullptr))) return false;
            ffxConfigureDescFrameGeneration configure{};
            configure.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
            configure.frameGenerationEnabled = true;
            configure.HUDLessColor = ffxApiGetResourceDX12(frames[step].color.Get(), FFX_API_RESOURCE_STATE_COMMON);
            configure.frameID = 1000 + step;
            ffxDispatchDescFrameGenerationPrepare prepare{};
            prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
            prepare.frameID = 1000 + step;
            prepare.commandList = state.list.Get();
            prepare.renderSize = {kRenderWidth, kRenderHeight};
            prepare.motionVectorScale = {static_cast<float>(kRenderWidth), static_cast<float>(kRenderHeight)};
            prepare.depth = ffxApiGetResourceDX12(frames[step].depth.Get(), FFX_API_RESOURCE_STATE_COMMON);
            prepare.motionVectors = ffxApiGetResourceDX12(frames[step].motion.Get(), FFX_API_RESOURCE_STATE_COMMON);
            prepare.unused_reset = step == 0;
            if (!context.configure(&configure.header) || !context.prepare(&prepare.header)) return false;
            const sedici::fg::GeneratePhase phases[2]{{output.Get(), outputState, 1.0f / 3.0f},
                                                     {second.Get(), secondState, 2.0f / 3.0f}};
            if (!context.generate(state.list.Get(), phases, 2)) {
                std::fprintf(stderr, "phases: stage 1 rejected the frame\n");
                return false;
            }
            if (!state.submit() || !context.track_submission(state.fence.Get(), state.fenceValue)) return false;
        }
        const auto readTexture = [&](ID3D12Resource* texture, D3D12_RESOURCE_STATES& textureState,
                                     std::vector<uint8_t>& pixels) {
            if (FAILED(state.allocator->Reset()) || FAILED(state.list->Reset(state.allocator.Get(), nullptr))) return false;
            transition(state.list.Get(), texture, textureState, D3D12_RESOURCE_STATE_COPY_SOURCE);
            textureState = D3D12_RESOURCE_STATE_COPY_SOURCE;
            D3D12_TEXTURE_COPY_LOCATION from{};
            from.pResource = texture;
            from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION to{};
            to.pResource = readback.Get();
            to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            to.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, kDisplayWidth, kDisplayHeight, 1, outputRowPitch};
            state.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            if (!state.submit()) return false;
            void* data = nullptr;
            if (FAILED(readback->Map(0, nullptr, &data))) return false;
            pixels.resize(static_cast<size_t>(kDisplayWidth) * kDisplayHeight * 4);
            for (uint32_t y = 0; y < kDisplayHeight; ++y) {
                std::memcpy(pixels.data() + static_cast<size_t>(y) * kDisplayWidth * 4,
                            static_cast<const uint8_t*>(data) + static_cast<size_t>(y) * outputRowPitch,
                            static_cast<size_t>(kDisplayWidth) * 4);
            }
            readback->Unmap(0, nullptr);
            return true;
        };
        std::vector<uint8_t> third, twoThirds;
        if (!readTexture(output.Get(), outputState, third) || !readTexture(second.Get(), secondState, twoThirds)) return false;
        const auto truthThird = quantise(make_scene(config, 1.0f / 3.0f, kDisplayWidth, kDisplayHeight).color);
        const auto truthTwoThirds = quantise(make_scene(config, 2.0f / 3.0f, kDisplayWidth, kDisplayHeight).color);
        const double generatedThird = psnr(truthThird, third);
        const double generatedTwoThirds = psnr(truthTwoThirds, twoThirds);
        const double nearestThird = psnr(truthThird, previousColor);
        const double nearestTwoThirds = psnr(truthTwoThirds, currentColor);
        const bool phasesOk = generatedThird > nearestThird && generatedTwoThirds > nearestTwoThirds;
        std::printf("%s %-18s phases in one list: 1/3 psnr %6.2f dB (nearest real %6.2f), "
                    "2/3 psnr %6.2f dB (nearest real %6.2f)\n",
                    phasesOk ? "PASS" : "FAIL", config.name, generatedThird, nearestThird,
                    generatedTwoThirds, nearestTwoThirds);
        if (!phasesOk) return false;
    }

    // HUD preservation: the game's backbuffer carries a HUD the HUD-less input
    // does not. A known white block over a background-only corner must survive
    // the composite; it is a corner the synthetic scenes never reach.
    {
        const uint32_t x0 = 4, y0 = 4, x1 = 60, y1 = 40;
        auto hudColor = currentDisplay.color;
        for (uint32_t y = y0; y < y1; ++y) {
            for (uint32_t x = x0; x < x1; ++x) {
                const size_t index = (static_cast<size_t>(y) * kDisplayWidth + x) * 4;
                hudColor[index + 0] = 1.0f;
                hudColor[index + 1] = 1.0f;
                hudColor[index + 2] = 1.0f;
            }
        }
        const auto hudBytes = quantise(hudColor);
        auto hudTexture = create_texture(state.device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM,
                                         kDisplayWidth, kDisplayHeight, D3D12_RESOURCE_FLAG_NONE);
        auto hudStaging = create_buffer(state.device.Get(), outputRowPitch * kDisplayHeight,
                                        D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!hudTexture || !hudStaging) return false;
        if (FAILED(state.allocator->Reset()) ||
            FAILED(state.list->Reset(state.allocator.Get(), nullptr))) return false;

        upload(state.list.Get(), hudTexture.Get(), hudStaging.Get(), hudBytes.data(),
               kDisplayWidth, kDisplayHeight, 4);

        ffxConfigureDescFrameGeneration hudConfigure{};
        hudConfigure.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        hudConfigure.frameGenerationEnabled = true;
        hudConfigure.HUDLessColor = ffxApiGetResourceDX12(frames[1].color.Get(), FFX_API_RESOURCE_STATE_COMMON);
        hudConfigure.frameID = kIterations;

        ffxDispatchDescFrameGenerationPrepare hudPrepare{};
        hudPrepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
        hudPrepare.frameID = kIterations;
        hudPrepare.commandList = state.list.Get();
        hudPrepare.renderSize = {kRenderWidth, kRenderHeight};
        hudPrepare.motionVectorScale = {static_cast<float>(kRenderWidth), static_cast<float>(kRenderHeight)};
        hudPrepare.depth = ffxApiGetResourceDX12(frames[1].depth.Get(), FFX_API_RESOURCE_STATE_COMMON);
        hudPrepare.motionVectors = ffxApiGetResourceDX12(frames[1].motion.Get(), FFX_API_RESOURCE_STATE_COMMON);
        if (!context.configure(&hudConfigure.header) || !context.prepare(&hudPrepare.header)) return false;

        sedici::fg::HudSource hud{hudTexture.Get(), D3D12_RESOURCE_STATE_COMMON};
        if (!context.generate(state.list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, &hud)) {
            std::fprintf(stderr, "HUD composite generation failed\n");
            return false;
        }

        D3D12_TEXTURE_COPY_LOCATION from{};
        from.pResource = output.Get();
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION to{};
        to.pResource = readback.Get();
        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        to.PlacedFootprint.Footprint.Width = kDisplayWidth;
        to.PlacedFootprint.Footprint.Height = kDisplayHeight;
        to.PlacedFootprint.Footprint.Depth = 1;
        to.PlacedFootprint.Footprint.RowPitch = outputRowPitch;
        state.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        if (!state.submit() || !context.track_submission(state.fence.Get(), state.fenceValue)) return false;

        std::vector<uint8_t> composite(static_cast<size_t>(kDisplayWidth) * kDisplayHeight * 4);
        void* hudMapped = nullptr;
        if (FAILED(readback->Map(0, nullptr, &hudMapped))) return false;
        for (uint32_t y = 0; y < kDisplayHeight; ++y) {
            std::memcpy(composite.data() + static_cast<size_t>(y) * kDisplayWidth * 4,
                        static_cast<const uint8_t*>(hudMapped) + static_cast<size_t>(y) * outputRowPitch,
                        static_cast<size_t>(kDisplayWidth) * 4);
        }
        readback->Unmap(0, nullptr);

        size_t inside = 0, insideWhite = 0;
        for (uint32_t y = y0 + 1; y < y1 - 1; ++y) {
            for (uint32_t x = x0 + 1; x < x1 - 1; ++x) {
                const size_t index = (static_cast<size_t>(y) * kDisplayWidth + x) * 4;
                ++inside;
                if (composite[index] > 216 && composite[index + 1] > 216 && composite[index + 2] > 216) {
                    ++insideWhite;
                }
            }
        }
        const double preserved = inside ? static_cast<double>(insideWhite) / inside : 0.0;
        const bool ok = preserved > 0.95;
        std::printf("%-18s hud preservation %.1f%%\n", config.name, preserved * 100.0);
        if (!ok) std::fprintf(stderr, "HUD layer did not survive compositing\n");
        if (!ok) return false;

        // K53: a reset seed emits the real frame, not a HUD-less phase. With a
        // HUD source the output is the game's own frame; without one, the
        // HUD-less colour.
        const auto seedOutput = [&](const sedici::fg::HudSource* source, uint64_t frameId,
                                    std::vector<uint8_t>& pixels) {
            if (FAILED(state.allocator->Reset()) ||
                FAILED(state.list->Reset(state.allocator.Get(), nullptr))) return false;
            hudConfigure.frameID = frameId;
            hudPrepare.frameID = frameId;
            hudPrepare.unused_reset = true;
            if (!context.configure(&hudConfigure.header) || !context.prepare(&hudPrepare.header)) return false;
            if (!context.generate(state.list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, source))
                return false;
            state.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            if (!state.submit() || !context.track_submission(state.fence.Get(), state.fenceValue)) return false;
            void* data = nullptr;
            if (FAILED(readback->Map(0, nullptr, &data))) return false;
            pixels.resize(static_cast<size_t>(kDisplayWidth) * kDisplayHeight * 4);
            for (uint32_t y = 0; y < kDisplayHeight; ++y) {
                std::memcpy(pixels.data() + static_cast<size_t>(y) * kDisplayWidth * 4,
                            static_cast<const uint8_t*>(data) + static_cast<size_t>(y) * outputRowPitch,
                            static_cast<size_t>(kDisplayWidth) * 4);
            }
            readback->Unmap(0, nullptr);
            return true;
        };
        std::vector<uint8_t> seededWithHud, seededWithout;
        const bool seeded = seedOutput(&hud, kIterations + 1, seededWithHud) &&
                            seedOutput(nullptr, kIterations + 2, seededWithout);
        const bool seedOk = seeded && seededWithHud == hudBytes &&
                            seededWithout == quantise(currentDisplay.color);
        std::printf("%s %-18s reset seed: the game frame with a HUD source, the HUD-less colour without\n",
                    seedOk ? "PASS" : "FAIL", config.name);
        return seedOk;
    }

    return true;
}


// Contract checks for the FFX-facing context: the descs Cyberpunk sends must be
// accepted, and anything the generator cannot honour must be refused so the
// caller can fall back.
int check_contract(Device& state)
{
    int failures = 0;
    auto expect = [&failures](bool condition, const char* what) {
        std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
        if (!condition) ++failures;
    };

    const auto color = create_texture(state.device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM,
                                      64, 36, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const auto depth = create_texture(state.device.Get(), DXGI_FORMAT_R32_FLOAT,
                                      32, 18, D3D12_RESOURCE_FLAG_NONE);
    const auto motion = create_texture(state.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
                                       32, 18, D3D12_RESOURCE_FLAG_NONE);
    const auto output = create_texture(state.device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM,
                                       64, 36, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (!color || !depth || !motion || !output) {
        std::printf("FAIL contract textures\n");
        return 1;
    }

    ffxCreateBackendDX12Desc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = state.device.Get();

    ffxCreateContextDescFrameGeneration create{};
    create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    create.header.pNext = &backend.header;
    create.flags = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED;
    create.displaySize = {64, 36};
    create.maxRenderSize = {64, 36};
    create.backBufferFormat = FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;

    sedici::fg::Context context;
    expect(context.create(&create.header), "create accepts Cyberpunk's create desc");

    ffxConfigureDescFrameGeneration configure{};
    configure.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    configure.frameGenerationEnabled = true;
    configure.HUDLessColor.resource = color.Get();
    configure.HUDLessColor.state = FFX_API_RESOURCE_STATE_COMMON;
    configure.frameID = 7;
    expect(context.configure(&configure.header), "configure accepts the FrameGenerationConfig desc");

    ffxDispatchDescFrameGenerationPrepare prepare{};
    prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
    prepare.frameID = configure.frameID;
    state.allocator->Reset();
    state.list->Reset(state.allocator.Get(), nullptr);
    prepare.commandList = state.list.Get();
    prepare.renderSize = {32, 18};
    prepare.motionVectorScale = {32.0f, 18.0f};
    prepare.depth.resource = depth.Get();
    prepare.depth.state = FFX_API_RESOURCE_STATE_COMMON;
    prepare.motionVectors.resource = motion.Get();
    prepare.motionVectors.state = FFX_API_RESOURCE_STATE_COMMON;
    expect(context.prepare(&prepare.header), "prepare accepts the legacy prepare desc");
    expect(!context.retired() && !context.destroy(),
           "recorded prepare prevents premature teardown");

    // Queue ordering alone is not proof that the borrowed prepare retired.
    // A generation rejection must leave its resources protected (K45).
    context.note_ordered_submission();
    expect(!context.generate(state.list.Get(), static_cast<ID3D12Resource*>(nullptr),
                             D3D12_RESOURCE_STATE_COMMON) && !context.retired() && !context.destroy(),
           "rejected ordered generation keeps the prepare unretired");

    const bool generated = context.generate(state.list.Get(), output.Get(),
                                            D3D12_RESOURCE_STATE_COMMON);
    expect(generated, "generate produces a frame");
    expect(state.submit() && context.track_submission(state.fence.Get(), state.fenceValue),
           "the frame command list executes and registers retirement");
    expect(!context.generate(state.list.Get(), output.Get(), D3D12_RESOURCE_STATE_COMMON),
           "a second generate without a new prepare is refused");

    // The FFX description hides the native depth format. Exercise the actual
    // D32 and two-plane depth/stencil allocations through Context, not only
    // Stage1 with a hand-selected view. Each must use a typed depth-plane SRV.
    for (const auto format : {DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_R32_TYPELESS,
                              DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_D32_FLOAT_S8X24_UINT}) {
        const bool stencil = format == DXGI_FORMAT_R32G8X24_TYPELESS ||
                             format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        const DXGI_FORMAT dsvFormat = stencil ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D32_FLOAT;
        D3D12_CLEAR_VALUE clear{};
        clear.Format = dsvFormat;
        clear.DepthStencil.Depth = 0.75f;
        clear.DepthStencil.Stencil = 37;
        const auto nativeDepth = create_texture(state.device.Get(), format, 32, 18,
                                                D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, &clear);
        if (!nativeDepth) { expect(false, "native depth texture creation"); continue; }
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        heapDesc.NumDescriptors = 1;
        ComPtr<ID3D12DescriptorHeap> heap;
        if (FAILED(state.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap)))) return failures + 1;
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
        dsv.Format = dsvFormat;
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        const auto target = heap->GetCPUDescriptorHandleForHeapStart();
        state.device->CreateDepthStencilView(nativeDepth.Get(), &dsv, target);
        if (FAILED(state.allocator->Reset()) ||
            FAILED(state.list->Reset(state.allocator.Get(), nullptr))) return failures + 1;
        transition(state.list.Get(), nativeDepth.Get(), D3D12_RESOURCE_STATE_COMMON,
                   D3D12_RESOURCE_STATE_DEPTH_WRITE);
        state.list->ClearDepthStencilView(target, stencil
            ? D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL : D3D12_CLEAR_FLAG_DEPTH,
            0.75f, 37, 0, nullptr);
        transition(state.list.Get(), nativeDepth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        prepare.depth = ffxApiGetResourceDX12(nativeDepth.Get(),
            FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        const bool accepted = context.prepare(&prepare.header);
        const char* label = format == DXGI_FORMAT_D32_FLOAT
            ? "prepare samples native D32_FLOAT using an R32_FLOAT view"
            : format == DXGI_FORMAT_R32_TYPELESS
            ? "prepare samples native R32_TYPELESS using an R32_FLOAT view"
            : format == DXGI_FORMAT_R32G8X24_TYPELESS
            ? "prepare samples Cyberpunk's R32G8X24_TYPELESS depth plane"
            : "prepare samples native D32_FLOAT_S8X24_UINT depth plane";
        expect(accepted, label);
        const bool generatedDepth = accepted && context.generate(state.list.Get(), output.Get(),
                                                                  D3D12_RESOURCE_STATE_COMMON);
        expect(generatedDepth, "native depth snapshot generates a frame");
        const bool submitted = state.submit();
        expect(submitted && (!accepted || context.track_submission(state.fence.Get(), state.fenceValue)),
               "native depth work retires with input states restored");
    }
    prepare.depth = ffxApiGetResourceDX12(depth.Get(), FFX_API_RESOURCE_STATE_COMMON);

    prepare.depth.resource = color.Get();
    expect(!context.prepare(&prepare.header) && !context.pending(),
           "incompatible native depth format is still refused");
    prepare.depth.resource = depth.Get();

    prepare.motionVectors.resource = color.Get();
    expect(!context.prepare(&prepare.header) && !context.pending(),
           "a non-float motion resource is refused and clears pending preparation");
    prepare.motionVectors.resource = motion.Get();

    configure.frameGenerationEnabled = false;
    expect(context.configure(&configure.header), "configure accepts frame generation off");
    expect(!context.generate(state.list.Get(), output.Get(), D3D12_RESOURCE_STATE_COMMON),
           "generate is refused while frame generation is off");

    // One deterministic lifetime check, not a timing race: hold a prepare
    // submission behind a gate, invalidate it, then recover after retirement.
    ComPtr<ID3D12CommandQueue> prepareQueue;
    ComPtr<ID3D12CommandAllocator> prepareAllocator;
    ComPtr<ID3D12GraphicsCommandList> prepareList;
    ComPtr<ID3D12Fence> prepareFence, gate;
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    if (FAILED(state.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&prepareQueue))) ||
        FAILED(state.device->CreateCommandAllocator(queueDesc.Type, IID_PPV_ARGS(&prepareAllocator))) ||
        FAILED(state.device->CreateCommandList(0, queueDesc.Type, prepareAllocator.Get(), nullptr,
                                               IID_PPV_ARGS(&prepareList))) ||
        FAILED(state.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&prepareFence))) ||
        FAILED(state.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)))) return failures + 1;

    configure.frameGenerationEnabled = true;
    configure.frameID = 8;
    prepare.frameID = 8;
    prepare.commandList = prepareList.Get();
    expect(context.configure(&configure.header) && context.prepare(&prepare.header),
           "next prepare records on a separate compute queue");
    configure.frameGenerationEnabled = false;
    expect(context.configure(&configure.header) && !context.pending(),
           "FG off invalidates the recorded snapshot without freeing it");
    if (FAILED(prepareList->Close())) return failures + 1;
    // Only this test gate is CPU-signalled; retirement is queue-signalled.
    if (FAILED(prepareQueue->Wait(gate.Get(), 1))) return failures + 1;
    ID3D12CommandList* prepareLists[]{prepareList.Get()};
    prepareQueue->ExecuteCommandLists(1, prepareLists);
    const HRESULT signaled = prepareQueue->Signal(prepareFence.Get(), 1);
    const bool tracked = SUCCEEDED(signaled) && context.track_submission(prepareFence.Get(), 1);
    expect(tracked && !context.retired(), "submitted prepare remains occupied behind its GPU fence");
    expect(!context.wait_for_retirement(2) && !context.retired(),
           "a bounded retirement timeout leaves gated GPU work unretired");
    expect(!context.prepare(&prepare.header) && !context.destroy(),
           "in-flight snapshot refuses reuse and teardown");
    // Always open the gate before any return or blocking wait.
    if (FAILED(gate->Signal(1))) return failures + 1;
    if (FAILED(state.queue->Wait(prepareFence.Get(), 1)) || !state.wait()) return failures + 1;
    expect(context.retired(), "prepare becomes reusable after GPU retirement");

    if (FAILED(prepareAllocator->Reset()) ||
        FAILED(prepareList->Reset(prepareAllocator.Get(), nullptr))) return failures + 1;
    configure.frameGenerationEnabled = true;
    configure.frameID = 9;
    prepare.frameID = 9;
    expect(context.configure(&configure.header) && context.prepare(&prepare.header),
           "FG re-enable recovers an abandoned retired snapshot");
    if (FAILED(prepareList->Close())) return failures + 1;
    prepareQueue->ExecuteCommandLists(1, prepareLists);
    if (FAILED(prepareQueue->Signal(prepareFence.Get(), 2))) return failures + 1;
    if (FAILED(state.allocator->Reset()) ||
        FAILED(state.list->Reset(state.allocator.Get(), nullptr))) return failures + 1;
    expect(context.track_submission(prepareFence.Get(), 2), "split prepare registers its completion fence");
    expect(!context.generate(state.list.Get(), output.Get(), D3D12_RESOURCE_STATE_COMMON),
           "split generation without an explicit queue dependency is refused");
    expect(context.wait_prepare(state.queue.Get()),
           "generation queue waits for the registered compute prepare");
    expect(context.generate(state.list.Get(), output.Get(), D3D12_RESOURCE_STATE_COMMON),
           "split-queue generation consumes the recovered snapshot");
    expect(state.submit() && context.track_submission(state.fence.Get(), state.fenceValue),
           "generation retirement covers the cross-queue dependency");

    expect(context.destroy(), "retired context destroys safely");
    return failures;
}

// Output pacing is a pure decision, so it is checked without a clock.
bool check_pacing()
{
    int failures = 0;
    auto expect = [&failures](bool condition, const char* what) {
        std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
        if (!condition) ++failures;
    };

    expect(sedici::fg::ticks_to_seconds(30000000, 10000000) == 3.0,
           "QPC counts divide by frequency to produce seconds");
    const double uptime = sedici::fg::ticks_to_seconds(987654321000000ULL, 10000000);
    const double later = sedici::fg::ticks_to_seconds(987654321277778ULL, 10000000);
    expect(std::abs((later - uptime) - 1.0 / 36.0) < 1e-7,
           "QPC conversion preserves a 36 fps interval after long uptime");
    expect(sedici::fg::ticks_to_seconds(1, 0) == 0.0,
           "missing QPC frequency does not divide by zero");

    sedici::fg::PacingInput input;
    input.cycleStarted = false;
    input.generatedReady = true;
    auto plan = sedici::fg::plan_present(input);
    expect(!plan.presentGenerated && plan.firstWait == 0.0 && plan.secondWait == 0.0,
           "the first cycle presents only the real seed without waiting");

    input.cycleStarted = true;
    input.interval = 1.0 / 60.0;
    input.lastCycle = 10.0;
    input.now = 10.0 + input.interval;
    plan = sedici::fg::plan_present(input);
    expect(plan.presentGenerated, "an on-time cycle presents the midpoint before the real frame");
    expect(plan.firstWait == 0.0, "an on-time cycle does not wait");
    expect(std::abs(plan.secondWait - input.interval * 0.5) < 1e-9,
           "the real frame follows the midpoint by half an interval");

    input.now = 10.0;
    plan = sedici::fg::plan_present(input);
    expect(plan.firstWait == 0.0,
           "an early cycle never waits before the current midpoint");

    input.now = 10.0 + 3.0 * input.interval;
    plan = sedici::fg::plan_present(input);
    expect(plan.firstWait == 0.0, "a late cycle presents without waiting");
    expect(!plan.presentGenerated && plan.secondWait == 0.0,
           "a midpoint past its slot is dropped without delaying the real frame");

    input.generatedReady = false;
    input.now = input.lastCycle;
    plan = sedici::fg::plan_present(input);
    expect(!plan.presentGenerated && plan.firstWait == 0.0 && plan.secondWait == 0.0,
           "without a midpoint the real frame is never held");

    input.interval = 0.0;
    input.now = input.lastCycle + 1.0 / 60.0;
    input.generatedReady = true;
    plan = sedici::fg::plan_present(input);
    expect(plan.secondWait > 0.0 && plan.secondWait <= 0.5,
           "an invalid interval falls back to a bounded pacing value");

    // Feed actual elapsed wrapper work back into the next entry. One-shot
    // decisions cannot expose a self-amplifying scheduler. Simulate over nine
    // minutes, including FG off/on and a 200 ms source stall, with no real sleeps.
    sedici::fg::PacingState pacing;
    constexpr double period = 1.0 / 36.0;
    double entry = 1000000.0;
    bool bounded = true, recovered = true;
    uint32_t outputs = 0;
    for (uint32_t cycle = 0; cycle < 20000; ++cycle) {
        const bool enabled = cycle < 5000 || cycle >= 5100;
        if (cycle == 5000 || cycle == 5100) pacing = {};
        if (cycle == 10000) entry += 0.2;
        const auto next = pacing.plan(entry, enabled);
        // CPU recording, native Present and copy work vary independently. They
        // consume the entry-based budget instead of extending its deadline.
        const double work = cycle % 2 ? 0.002 : 0.004;
        const double real = std::max(entry + work, next.realDeadline);
        const double followingEntry = std::max(entry + period, real + 0.004);
        bounded &= next.firstWait == 0.0 && next.realDeadline <= entry + pacing.interval * 0.5 + 1e-8;
        if (!enabled) bounded &= !next.presentGenerated && next.realDeadline == entry;
        if (cycle > 10500) recovered &= std::abs(pacing.interval - period) < 1e-7;
        outputs += next.presentGenerated ? 2 : 1;
        entry = followingEntry;
    }
    expect(bounded && recovered && std::abs(pacing.interval - period) < 1e-7 && outputs > 39800,
           "20000-cycle closed loop stays at 36 real / 72 output and recovers from off/on and a stall");

    {
        sedici::fg::OutputSchedule schedule;
        schedule.set_ratio(UINT32_MAX, UINT32_MAX);
        expect(schedule.numerator() == UINT32_MAX && schedule.denominator() == UINT32_MAX,
               "ratio validation handles large denominators without overflow");
        schedule.set_ratio(UINT32_MAX, 0);
        expect(schedule.numerator() == 2 && schedule.denominator() == 1,
               "ratio validation rejects a zero denominator");
    }

    // Current presenter policy, characterized without changing real-drop semantics.
    expect(!sedici::fg::later_output_is_due(false, 1.0, 0.1, 9.0) &&
           !sedici::fg::later_output_is_due(true, 1.0, 0.125, 1.124) &&
           sedici::fg::later_output_is_due(true, 1.0, 0.125, 1.125) &&
           sedici::fg::later_output_is_due(true, 1.0, 0.125, 1.126),
           "presenter stale-output policy uses the later deadline including equality");

    {
        // Characterize the production stale-output predicate after a hitch:
        // overdue outputs are discarded in order and the newest remains eligible.
        const double content[] = {1.0, 1.01, 1.02, 1.03};
        const double now = 1.035;
        unsigned dropped = 0;
        unsigned shown = 0;
        double lastShown = 0.0;
        for (size_t i = 0; i < std::size(content); ++i) {
            const bool hasLater = i + 1 < std::size(content);
            const double nextContent = hasLater ? content[i + 1] : 0.0;
            if (sedici::fg::later_output_is_due(hasLater, nextContent, 0.0, now)) {
                ++dropped;
            } else {
                ++shown;
                lastShown = content[i];
            }
        }
        const double recovery = 1.04;
        expect(dropped == 3 && shown == 1 && lastShown == 1.03 && recovery > lastShown,
               "stale-output policy drains a hitch backlog chronologically");
    }

    // Output schedule: every output lands on an even grid in content time.
    // For each ratio, count outputs over many real frames, check the pattern
    // never needs more than two generated frames per real frame, and that the
    // spacing between consecutive outputs is constant.
    struct Ratio { uint32_t p, q; const char* name; };
    for (const Ratio ratio : {Ratio{2, 1, "2x"}, Ratio{7, 4, "1.75x"}, Ratio{3, 2, "1.5x"}, Ratio{5, 4, "1.25x"}}) {
        sedici::fg::OutputSchedule schedule;
        schedule.set_ratio(ratio.p, ratio.q);
        const double interval = 1.0 / 48.0;
        double previousContent = -1.0, minGap = 1.0, maxGap = 0.0;
        uint32_t produced = 0, reals = 0, maxGenerated = 0;
        const uint32_t frames = 1 + 400 * ratio.q;
        for (uint32_t frame = 0; frame < frames; ++frame) {
            const auto step = schedule.plan(10.0 + frame * interval);
            maxGenerated = std::max(maxGenerated, step.generated());
            for (uint32_t i = 0; i < step.count; ++i) {
                const auto& out = step.outputs[i];
                if (frame > 0) {
                    ++produced;
                    reals += out.real ? 1u : 0u;
                    const double gap = out.content - previousContent;
                    minGap = std::min(minGap, gap);
                    maxGap = std::max(maxGap, gap);
                }
                previousContent = out.content;
            }
        }
        const double expectedGap = interval * ratio.q / ratio.p;
        char what[160];
        std::snprintf(what, sizeof(what),
                      "%s schedule: %u outputs per %u real frames, %u real, <=2 generated, even %.3f ms grid",
                      ratio.name, produced, frames - 1, reals, expectedGap * 1000.0);
        expect(produced == 400 * ratio.p && reals == 400 && maxGenerated <= 2 &&
               std::abs(minGap - expectedGap) < 1e-6 && std::abs(maxGap - expectedGap) < 1e-6, what);
    }
    {
        // A hitch is a discontinuity: the frame after a 3x interval shows only
        // itself, and the next normal frame resumes generating.
        sedici::fg::OutputSchedule schedule;
        schedule.set_ratio(3, 2);
        double t = 10.0;
        for (int frame = 0; frame < 20; ++frame) (void)schedule.plan(t += 1.0 / 48.0);
        const auto afterHitch = schedule.plan(t += 3.0 / 48.0);
        const auto resumed = schedule.plan(t += 1.0 / 48.0);
        expect(afterHitch.count == 1 && afterHitch.outputs[0].real && resumed.generated() >= 1,
               "1.5x schedule: a 3x hitch shows only the real frame, then generation resumes");
    }
    {
        // Adaptive: 48 real fps towards 72 output fps averages 1.5 outputs per
        // real frame on an even 1/72 s grid, snapping to real frames nearby.
        sedici::fg::OutputSchedule schedule;
        schedule.set_target(72.0);
        const double interval = 1.0 / 48.0;
        double previousContent = -1.0, maxGapError = 0.0;
        uint32_t produced = 0, maxGenerated = 0;
        for (uint32_t frame = 0; frame < 4801; ++frame) {
            const auto step = schedule.plan(10.0 + frame * interval);
            maxGenerated = std::max(maxGenerated, step.generated());
            for (uint32_t i = 0; i < step.count; ++i) {
                if (frame > 1) {
                    ++produced;
                    maxGapError = std::max(maxGapError, std::abs(step.outputs[i].content - previousContent - 1.0 / 72.0));
                }
                previousContent = step.outputs[i].content;
            }
        }
        expect(produced >= 7190 && produced <= 7210 && maxGenerated <= 2 && maxGapError <= 0.1 / 72.0 + 1e-9,
               "adaptive schedule: 48 real -> ~72 output fps, <=2 generated, within a tenth of a slot of the grid");
        // A pause resets to the real frame alone, and a slower real rate than the
        // target still never asks for more than two generated frames.
        const auto paused = schedule.plan(20.0);
        expect(paused.count == 1 && paused.outputs[0].real, "adaptive schedule: a 0.25 s gap shows only the real frame");
        // Below 20 real fps the schedule stops generating.
        uint32_t slowGenerated = 0;
        for (uint32_t frame = 0; frame < 40; ++frame) slowGenerated += schedule.plan(21.0 + frame * 0.055).generated();
        expect(slowGenerated == 0, "adaptive schedule: below 20 real fps shows only real frames");
        // K95: jitter around 20 real fps must not halve the output. Alternate
        // 46 ms and 54 ms (average 50 ms) with the fixed 2x ratio.
        {
            sedici::fg::OutputSchedule jitter;
            jitter.set_ratio(2, 1);
            double t = 40.0;
            uint32_t frames = 0, generated = 0;
            for (uint32_t frame = 0; frame < 400; ++frame) {
                t += (frame % 2 == 0) ? 0.046 : 0.054;
                const auto step = jitter.plan(t);
                if (frame >= 20) { ++frames; generated += step.generated() > 0 ? 1u : 0u; }
            }
            expect(generated >= frames * 95 / 100,
                   "2x schedule: jitter averaging 20 real fps still generates for almost every frame");
            // Steady 60 ms stays real-only, and recovery to 40 ms generates again.
            jitter.reset();
            uint32_t slowOnly = 0, recoveredCount = 0;
            for (uint32_t frame = 0; frame < 100; ++frame) slowOnly += jitter.plan(50.0 + frame * 0.06).generated();
            for (uint32_t frame = 0; frame < 100; ++frame) recoveredCount += jitter.plan(60.0 + frame * 0.04).generated();
            expect(slowOnly == 0 && recoveredCount > 90,
                   "2x schedule: a steady 60 ms interval is real-only and recovery to 40 ms resumes generation");
        }
        schedule.set_target(144.0);
        uint32_t worst = 0;
        for (uint32_t frame = 0; frame < 200; ++frame) worst = std::max(worst, schedule.plan(30.0 + frame / 40.0).generated());
        expect(worst <= 2, "adaptive schedule: 40 real fps towards 144 caps at two generated frames");
    }
    return failures == 0;
}

struct TestGenerator {
    uint32_t calls = 0;
    IDXGISwapChain4* native = nullptr;
    bool before_present = true;
};

// Stand-in for the real generator. Presentation is what is under test here, so
// the sink only has to be a well-formed, empty recording; the real host records
// fg::Context into the same output resource.
bool record_solid(void* user, ID3D12GraphicsCommandList* list, ID3D12Resource* real,
                  ID3D12Resource* output, D3D12_RESOURCE_STATES state)
{
    (void)state;
    auto* generator = static_cast<TestGenerator*>(user);
    if (!generator || !list || !output || !real) return false;
    UINT count = 0;
    const UINT expected = generator->calls ? generator->calls * 2 - 1 : 0;
    generator->before_present &= generator->native &&
        SUCCEEDED(generator->native->GetLastPresentCount(&count)) && count == expected;
    ++generator->calls;
    return true;
}

bool record_counted(void* user, ID3D12GraphicsCommandList* list, ID3D12Resource* real,
                    ID3D12Resource* output, D3D12_RESOURCE_STATES)
{
    auto* calls = static_cast<uint32_t*>(user);
    if (!calls || !list || !output || !real) return false;
    ++*calls;
    return true;
}

ID3D12Fence* recoveryTestGate = nullptr;
std::atomic<bool> recoveryTestReleased{false};
void release_recovery_test_gate() noexcept
{
    if (recoveryTestGate && SUCCEEDED(recoveryTestGate->Signal(1)))
        recoveryTestReleased = true;
}

// `[fg] present = "vrr"`: the native chain moves to Sedici's present queue and
// both outputs come from the presenter thread with the game's tearing flags.
// Every midpoint is either shown or dropped as stale, never lost; resize keeps
// the chain on the present queue; teardown drains the thread.
bool check_vrr_window(Device& state, sedici::FrameGenerationSettings::Multiplier multiplier,
                      const char* label, DWORD frameMs = 27, bool signalFault = false,
                      bool closeFault = false, bool resetFault = false, bool liveRecovery = false,
                      bool paramsFault = false)
{
    // Below ~20 ms per real frame at 1.5x the plan asks for more frames than a
    // 75 Hz panel can show. The game may then be held to the panel's pace, as
    // with vsync, but never slower: finding 009's game session spiralled to
    // 16 real fps because queueing was counted as latency.
    const bool overload = frameMs < 13;
    if (GetModuleHandleW(L"RTSSHooks64.dll") && GetEnvironmentVariableW(L"SEDICI_FG_ALLOW_RTSS", nullptr, 0) == 0) {
        // The wrapper refuses "vrr" under RTSS (see Swapchain::wrap); not a pass.
        std::printf("SKIP vrr presentation: RTSS is injected; close it or set SEDICI_FG_ALLOW_RTSS=1\n");
        return true;
    }
    const wchar_t* className = L"SediciFgVrr";
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = className;
    if (!RegisterClassW(&wc)) { std::printf("vrr: step failed at line %d\n", __LINE__); return false; }
    HWND window = CreateWindowW(className, L"Sedici vrr presentation", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 500, 320, nullptr, nullptr, wc.hInstance, nullptr);
    if (!window) { UnregisterClassW(className, wc.hInstance); return false; }

    bool ok = [&] {
        ComPtr<IDXGIFactory5> factory;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) { std::printf("vrr: step failed at line %d\n", __LINE__); return false; }
        BOOL tearing = FALSE;
        const bool canTear = SUCCEEDED(factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
            &tearing, sizeof(tearing))) && tearing;
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = 320;
        desc.Height = 180;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 3;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        // Shaped like the game's chain: tearing allowed, latency waitable.
        desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
            (canTear ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u);
        ComPtr<IDXGISwapChain1> created;
        if (FAILED(factory->CreateSwapChainForHwnd(state.queue.Get(), window, &desc,
            nullptr, nullptr, &created))) { std::printf("vrr: step failed at line %d\n", __LINE__); return false; }
        ComPtr<IDXGISwapChain4> native;
        if (FAILED(created.As(&native))) { std::printf("vrr: step failed at line %d\n", __LINE__); return false; }
        if (FAILED(native->SetMaximumFrameLatency(1))) { std::printf("vrr: step failed at line %d\n", __LINE__); return false; }

        sedici::fg::PresentChoice choice;
        choice.present = sedici::FrameGenerationSettings::Present::Vrr;
        choice.guard = sedici::FrameGenerationSettings::TearGuard::Scanline;
        choice.multiplier = multiplier;
        const bool doubled = multiplier == sedici::FrameGenerationSettings::Multiplier::X2;
        ComPtr<sedici::fg::Swapchain> wrapped;
        // Hand the only reference over, as the FFX wrap desc does for the game:
        // "vrr" destroys this chain and recreates it on the present queue.
        created.Reset();
        if (FAILED(sedici::fg::Swapchain::wrap(native.Get(), state.queue.Get(), &wrapped, &choice, 1))) { std::printf("vrr: step failed at line %d\n", __LINE__); return false; }
        native.Detach();
        if (!wrapped->async_present()) {
            std::printf("vrr: presenter did not start\n");
            return false;
        }
        uint32_t calls = 0;
        wrapped->set_generator(sedici::fg::GenerateSink{&calls, &record_counted, nullptr});

        ShowWindow(window, SW_SHOWNOACTIVATE);
        const UINT presentFlags = canTear ? DXGI_PRESENT_ALLOW_TEARING : 0u;
        if (paramsFault) {
            RECT dirty{0, 0, 16, 16};
            DXGI_PRESENT_PARAMETERS parameters{};
            parameters.DirtyRectsCount = 1;
            parameters.pDirtyRects = &dirty;
            const UINT beforeIndex = wrapped->GetCurrentBackBufferIndex();
            sedici::fg::arm_bookkeeping_fault(sedici::fg::BookkeepingFault::PresentParametersAllocation);
            const HRESULT first = wrapped->Present1(0, presentFlags, &parameters);
            const bool reached = !sedici::fg::consume_bookkeeping_fault(
                sedici::fg::BookkeepingFault::PresentParametersAllocation);
            sedici::fg::arm_bookkeeping_fault(sedici::fg::BookkeepingFault::None);
            if (!reached || first != E_OUTOFMEMORY || calls != 0 ||
                wrapped->GetCurrentBackBufferIndex() != beforeIndex) {
                std::puts("FAIL parameter-allocation failure changed frame admission");
                return false;
            }
            const HRESULT retry = wrapped->Present1(0, presentFlags, &parameters);
            const bool retried = SUCCEEDED(retry) && calls == 1;
            std::puts(retried ? "PASS Present1 retry with the same metadata is accepted"
                              : "FAIL Present1 retry after allocation error");
            if (!retried) return false;
            // K105: a refused presenter job leaves ownership unadvanced and a retry works.
            const UINT indexBefore = wrapped->GetCurrentBackBufferIndex();
            sedici::fg::arm_bookkeeping_fault(sedici::fg::BookkeepingFault::PresenterSubmit);
            const HRESULT refused = wrapped->Present(0, presentFlags);
            const bool hit = !sedici::fg::consume_bookkeeping_fault(sedici::fg::BookkeepingFault::PresenterSubmit);
            sedici::fg::arm_bookkeeping_fault(sedici::fg::BookkeepingFault::None);
            if (!hit || refused != E_OUTOFMEMORY || wrapped->GetCurrentBackBufferIndex() != indexBefore) {
                std::puts("FAIL a refused presenter job advanced buffer ownership");
                return false;
            }
            const HRESULT again = wrapped->Present(0, presentFlags);
            const bool accepted = SUCCEEDED(again) && wrapped->GetCurrentBackBufferIndex() != indexBefore &&
                                  g_debug_errors.load() == 0;
            std::puts(accepted ? "PASS K105: a refused presenter job leaves ownership unchanged and a retry is accepted"
                               : "FAIL retry after a refused presenter job");
            return accepted;
        }
        if (signalFault || closeFault || resetFault) {
            ComPtr<ID3D12Fence> recoveryGate;
            if (liveRecovery) {
                if (FAILED(state.device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                                     IID_PPV_ARGS(&recoveryGate))) ||
                    FAILED(state.queue->Wait(recoveryGate.Get(), 1))) return false;
            }
            const auto fault = resetFault ? sedici::fg::BookkeepingFault::GenerationReset
                             : closeFault ? sedici::fg::BookkeepingFault::GenerationClose
                                          : sedici::fg::BookkeepingFault::GenerationSignal;
            sedici::fg::arm_bookkeeping_fault(fault);
            const HRESULT first = wrapped->Present(0, presentFlags);
            const bool reachedInjection = !sedici::fg::consume_bookkeeping_fault(fault);
            sedici::fg::arm_bookkeeping_fault(sedici::fg::BookkeepingFault::None);
            const HRESULT retry = wrapped->Present(0, presentFlags);
            const bool stopped = reachedInjection && first == E_FAIL && retry == E_FAIL &&
                                 calls == (resetFault ? 0u : 1u) && g_debug_errors.load() == 0;
            if (!g_debug_callback_installed) std::puts("INFO DX12 debug callback unavailable");
            if (liveRecovery) {
                if (!stopped) {
                    (void)recoveryGate->Signal(1);
                    return false;
                }
                // The generation list is queued behind recoveryGate. Release it
                // only once teardown has queued the replacement retirement fence.
                recoveryTestGate = recoveryGate.Get();
                recoveryTestReleased = false;
                const unsigned retainedBefore = sedici::fg::swapchain_retention_count();
                sedici::fg::arm_queue_recovery_hook(&release_recovery_test_gate);
                const auto before = std::chrono::steady_clock::now();
                auto* finalReference = wrapped.Detach();
                const ULONG remaining = finalReference->Release();
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - before).count();
                sedici::fg::arm_queue_recovery_hook(nullptr);
                recoveryTestGate = nullptr;
                (void)recoveryGate->Signal(1); // Unblock even if recovery failed.
                const bool recovered = remaining == 0 && recoveryTestReleased.load() && elapsed < 3000 &&
                                       sedici::fg::swapchain_retention_count() == retainedBefore &&
                                       SUCCEEDED(state.device->GetDeviceRemovedReason()) &&
                                       g_debug_errors.load() == 0;
                std::puts(recovered ? "PASS wrapper teardown recovered gated GPU work on a live device"
                                    : "FAIL wrapper teardown did not recover gated GPU work");
                return recovered;
            }
            ComPtr<ID3D12Device5> removable;
            if (FAILED(state.device.As(&removable))) return false;
            removable->RemoveDevice();
            std::puts(stopped ? "PASS VRR injected failure stops later admission"
                              : "FAIL VRR injected failure did not stop admission");
            return stopped;
        }
        const UINT cycles = 40;
        uint64_t skippedAtTen = 0;
        HANDLE latency = wrapped->GetFrameLatencyWaitableObject();
        LARGE_INTEGER loopStart{}, loopEnd{}, frequency{};
        LONGLONG waitableTicks = 0, presentTicks = 0;
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&loopStart);
        for (UINT cycle = 0; cycle < cycles; ++cycle) {
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            LARGE_INTEGER waitStart{}, waitEnd{};
            QueryPerformanceCounter(&waitStart);
            if (latency) WaitForSingleObjectEx(latency, 1000, TRUE);
            QueryPerformanceCounter(&waitEnd);
            waitableTicks += waitEnd.QuadPart - waitStart.QuadPart;
            Sleep(frameMs); // 27 ms: ~35 fps real rate, under half of a 75 Hz refresh like the game cap
            ComPtr<ID3D12Resource> buffer;
            if (FAILED(wrapped->GetBuffer(wrapped->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&buffer)))) { std::printf("vrr: step failed at line %d\n", __LINE__); return false; }
            if (FAILED(state.allocator->Reset()) || FAILED(state.list->Reset(state.allocator.Get(), nullptr))) { std::printf("vrr: step failed at line %d\n", __LINE__); return false; }
            transition(state.list.Get(), buffer.Get(), D3D12_RESOURCE_STATE_PRESENT,
                       D3D12_RESOURCE_STATE_COPY_DEST);
            transition(state.list.Get(), buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_PRESENT);
            if (!state.submit()) { std::printf("vrr: step failed at line %d\n", __LINE__); return false; }
            LARGE_INTEGER presentStart{}, presentEnd{};
            QueryPerformanceCounter(&presentStart);
            const HRESULT presentResult = wrapped->Present(0, presentFlags);
            QueryPerformanceCounter(&presentEnd);
            presentTicks += presentEnd.QuadPart - presentStart.QuadPart;
            if (const HRESULT hr = presentResult; FAILED(hr)) {
                std::printf("vrr: Present failed at cycle %u hr=0x%08lx\n", cycle, static_cast<unsigned long>(hr));
                return false;
            }
            if (wrapped->GetCurrentBackBufferIndex() != (cycle + 1) % 3) { std::printf("vrr: index\n"); return false; }
            if (cycle == 9) skippedAtTen = wrapped->skipped_generates();
        }
        QueryPerformanceCounter(&loopEnd);
        const double perFrameMs = 1000.0 * static_cast<double>(loopEnd.QuadPart - loopStart.QuadPart) /
                                  static_cast<double>(frequency.QuadPart) / cycles;
        // Resize drains the presenter, so every output has been issued after it.
        if (const HRESULT hr = wrapped->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, desc.Flags); FAILED(hr)) {
            std::printf("vrr: resize failed hr=0x%08lx\n", static_cast<unsigned long>(hr));
            return false;
        }
        UINT presented = 0;
        if (FAILED(wrapped->GetLastPresentCount(&presented))) { std::printf("vrr: step failed at line %d\n", __LINE__); return false; }
        const uint64_t shown = wrapped->generated_presents();
        const uint64_t skipped = wrapped->skipped_generates();
        std::printf("vrr presentation %s: %u recordings, %llu shown, %llu skipped, %u native presents, tearing=%s\n", label,
                    calls, static_cast<unsigned long long>(shown), static_cast<unsigned long long>(skipped),
                    presented, canTear ? "yes" : "no");
        const double toMs = 1000.0 / static_cast<double>(frequency.QuadPart) / cycles;
        std::printf("vrr: skipped in the first 10 cycles: %llu, game loop %.2f ms per frame (sleep %lu, "
                    "latency waitable %.2f ms, Present %.2f ms)\n",
                    static_cast<unsigned long long>(skippedAtTen), perFrameMs, frameMs,
                    waitableTicks * toMs, presentTicks * toMs);
        // Time the test game spent inside Sedici per frame. The loop time itself is
        // dominated by Sleep's 15.6 ms granularity (Sleep(21) and Sleep(27) both
        // take ~31 ms), so it says nothing about blocking.
        const double sediciMs = (waitableTicks + presentTicks) * toMs;
        if (overload) {
            // More frames are planned than the panel can show: the presenter
            // drops outputs and the game waits at most about one panel pace per
            // frame, and nothing beyond the plan is presented.
            const UINT planned = 1 + (cycles - 1) * 3 / 2;
            DEVMODEW mode{};
            mode.dmSize = sizeof(mode);
            const double refreshMs = EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) &&
                mode.dmDisplayFrequency > 1 ? 1000.0 / mode.dmDisplayFrequency : 1000.0 / 60.0;
            const double panelPace = 1.5 * refreshMs; // 1.5 flips per real frame
            if (sediciMs > panelPace) {
                std::printf("vrr: game waited %.2f ms per frame in Sedici (panel pace %.2f ms)\n",
                            sediciMs, panelPace);
                return false;
            }
            if (presented > planned) { std::printf("vrr: step failed at line %d\n", __LINE__); return false; }
        } else if (doubled) {
            if (calls != cycles) { std::printf("vrr: step failed at line %d\n", __LINE__); return false; }
            if (shown + skipped != cycles - 1) { std::printf("vrr: step failed at line %d\n", __LINE__); return false; }
            // Every real frame plus every shown midpoint, less at most two outputs
            // the presenter dropped because a later one was already due (timing
            // dependent; seen once under CTest). Never more than planned.
            const UINT planned = static_cast<UINT>(cycles + shown);
            if (presented > planned || presented + 2 < planned) {
                std::printf("vrr: presented %u of %u planned\n", presented, planned);
                return false;
            }
        } else if (multiplier == sedici::FrameGenerationSettings::Multiplier::Adaptive) {
            // ~40 real fps towards 72: about 1.8 outputs per real frame, and the
            // game is not held back by the presenter (two frames may be in flight).
            const double expected = cycles * 72.0 * perFrameMs / 1000.0;
            if (sediciMs > 2.0) {
                std::printf("vrr: adaptive game waited %.2f ms per frame in Sedici\n", sediciMs);
                return false;
            }
            if (presented < 0.8 * expected || presented > expected + 4.0) {
                std::printf("vrr: adaptive presented %u, expected about %.0f\n", presented, expected);
                return false;
            }
        } else {
            // 1.5x: 3 outputs per 2 real frames after the first, two of them
            // generated. Allow a few start-up drops, never more than planned.
            const UINT planned = 1 + (cycles - 1) * 3 / 2;
            if (calls < cycles) { std::printf("vrr: step failed at line %d\n", __LINE__); return false; }
            if (presented > planned || presented + 6 < planned) {
                std::printf("vrr: step failed at line %d (presented %u of %u)\n", __LINE__, presented, planned);
                return false;
            }
            if (shown + 4 < (planned - 1) * 2 / 3) { std::printf("vrr: step failed at line %d\n", __LINE__); return false; }
        }
        // No flip may follow the previous one within 2 ms (finding 009: the
        // scan-line guard opened before the previous flip had latched).
        if (const uint64_t backToBack = wrapped->back_to_back_flips(); backToBack != 0) {
            std::printf("vrr: %llu back-to-back flips\n", static_cast<unsigned long long>(backToBack));
            return false;
        }
        // Presentation continues after the resize, still from the presenter.
        for (UINT cycle = 0; cycle < 3; ++cycle) {
            if (FAILED(wrapped->Present(0, presentFlags))) { std::printf("vrr: step failed at line %d\n", __LINE__); return false; }
        }
        wrapped->set_generator(sedici::fg::GenerateSink{});
        return true;
    }();

    DestroyWindow(window);
    UnregisterClassW(className, wc.hInstance);
    std::printf("%s vrr presentation %s: %s\n", ok ? "PASS" : "FAIL", label,
                paramsFault ? "parameter allocation failure and retry" :
                (signalFault || closeFault || resetFault) ? "injected failure stops admission" :
                "presenter thread, queue, resize, teardown");
    return ok;
}

// One generated frame is shown per cycle starting with the second: the midpoint
// for a pair cannot be shown before the pair exists.
bool check_generated_window(Device& state, bool* knownSignature = nullptr)
{
    if (knownSignature) *knownSignature = false;
    const wchar_t* className = L"SediciFgGenerated";
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = className;
    if (!RegisterClassW(&wc)) return false;
    HWND window = CreateWindowW(className, L"Sedici generated presentation", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 500, 320, nullptr, nullptr, wc.hInstance, nullptr);
    if (!window) { UnregisterClassW(className, wc.hInstance); return false; }

    bool ok = [&] {
        ComPtr<IDXGIFactory4> factory;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return false;
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = 320;
        desc.Height = 180;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 3;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<IDXGISwapChain1> created;
        if (FAILED(factory->CreateSwapChainForHwnd(state.queue.Get(), window, &desc,
            nullptr, nullptr, &created))) return false;
        ComPtr<IDXGISwapChain4> native;
        if (FAILED(created.As(&native))) return false;
        ComPtr<sedici::fg::Swapchain> wrapped;
        if (FAILED(sedici::fg::Swapchain::wrap(native.Get(), state.queue.Get(), &wrapped))) return false;

        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = 1;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ComPtr<ID3D12DescriptorHeap> uavHeap;
        if (FAILED(state.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&uavHeap)))) return false;
        TestGenerator generator{0, native.Get()};
        sedici::fg::GenerateSink sink{&generator, &record_solid, nullptr};
        wrapped->set_generator(sink);

        ShowWindow(window, SW_SHOWNOACTIVATE);
        const UINT cycles = 6;
        for (UINT cycle = 0; cycle < cycles; ++cycle) {
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            ComPtr<ID3D12Resource> buffer;
            if (FAILED(wrapped->GetBuffer(wrapped->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&buffer)))) return false;
            if (FAILED(state.allocator->Reset()) || FAILED(state.list->Reset(state.allocator.Get(), nullptr))) return false;
            transition(state.list.Get(), buffer.Get(), D3D12_RESOURCE_STATE_PRESENT,
                       D3D12_RESOURCE_STATE_COPY_DEST);
            transition(state.list.Get(), buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_PRESENT);
            if (!state.submit()) return false;
            if (FAILED(wrapped->Present(1, 0))) return false;
            if (wrapped->GetCurrentBackBufferIndex() != (cycle + 1) % 3) return false;
        }

        std::printf("generated presentation: %u recordings, %llu shown, %llu skipped\n",
                    generator.calls,
                    static_cast<unsigned long long>(wrapped->generated_presents()),
                    static_cast<unsigned long long>(wrapped->skipped_generates()));
        if (generator.calls != cycles) return false;
        if (!generator.before_present || wrapped->generated_presents() != cycles - 1) {
            // The historical 2-shown/3-skipped result also causes the
            // before-present count check to fail after the first skipped frame.
            if (knownSignature && wrapped->generated_presents() == 2 && wrapped->skipped_generates() == 3)
                *knownSignature = true;
            return false;
        }
        if (wrapped->skipped_generates() != 0) return false;
        UINT presented = 0;
        if (FAILED(wrapped->GetLastPresentCount(&presented))) return false;
        if (presented != 2 * cycles - 1) return false;

        // Clearing the sink must stop generated presentation without stalling
        // real frames.
        wrapped->set_generator(sedici::fg::GenerateSink{});
        for (UINT cycle = 0; cycle < 2; ++cycle) {
            if (FAILED(wrapped->Present(1, 0))) return false;
        }
        if (FAILED(wrapped->GetLastPresentCount(&presented)) || presented != 2 * cycles + 1) return false;

        wrapped->set_generator(sedici::fg::GenerateSink{});
        return true;
    }();

    DestroyWindow(window);
    UnregisterClassW(className, wc.hInstance);
    std::printf("%s generated presentation: one midpoint per cycle, bounded, sink removable\n",
                ok ? "PASS" : "FAIL");
    return ok;
}

// The production path end to end: a wrapped swapchain, an FFX-shaped frame
// generation session bound to it, and generated frames coming out of Sedici's
// own presentation schedule. Native buffer readback verifies temporal order,
// current-frame HUD labels, and the actual midpoint image rather than counts.
HANDLE contextRecordArrived = nullptr;
HANDLE contextRecordResume = nullptr;
void pause_context_record() noexcept
{
    SetEvent(contextRecordArrived);
    (void)WaitForSingleObject(contextRecordResume, 5000);
}

bool check_session_window(Device& state, bool loseDevice = false, bool signalFault = false,
                          bool closeFault = false, bool resetFault = false,
                          bool contextRace = false, bool sessionRecovery = false,
                          bool registrationFault = false, bool* knownSignature = nullptr)
{
    if (knownSignature) *knownSignature = false;
    const wchar_t* className = L"SediciFgSession";
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = className;
    if (!RegisterClassW(&wc)) return false;
    HWND window = CreateWindowW(className, L"Sedici session", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 500, 320, nullptr, nullptr, wc.hInstance, nullptr);
    if (!window) { UnregisterClassW(className, wc.hInstance); return false; }

    bool ok = [&] {
        ComPtr<IDXGIFactory4> factory;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return false;
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = kDisplayWidth;
        desc.Height = kDisplayHeight;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 3;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        ComPtr<IDXGISwapChain1> created;
        if (FAILED(factory->CreateSwapChainForHwnd(state.queue.Get(), window, &desc,
            nullptr, nullptr, &created))) return false;
        ComPtr<IDXGISwapChain4> native;
        if (FAILED(created.As(&native))) return false;

        sedici::fg::WrapHandle handle;
        if (!handle.create(native.Get(), state.queue.Get()) || !handle.swapchain()) {
            std::printf("session: wrap failed\n");
            return false;
        }

        sedici::fg::Session stackSession;
        std::shared_ptr<sedici::fg::Session> ownedSession;
        if (sessionRecovery) {
            ownedSession = std::make_shared<sedici::fg::Session>();
            ownedSession->set_callback_lifetime(ownedSession);
        }
        sedici::fg::Session& session = ownedSession ? *ownedSession : stackSession;
        ffxCreateBackendDX12Desc backend{};
        backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        backend.device = state.device.Get();
        ffxCreateContextDescFrameGeneration create{};
        create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
        create.header.pNext = &backend.header;
        create.flags = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED;
        create.displaySize = {kDisplayWidth, kDisplayHeight};
        create.maxRenderSize = {kRenderWidth, kRenderHeight};
        create.backBufferFormat = FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
        if (session.state() != sedici::fg::SessionState::Empty ||
            session.configure(&create.header) || session.prepare(&create.header)) return false; // refused before create
        if (!session.create(&create.header)) {
            std::printf("session: create failed: %s\n", session.context().last_error());
            return false;
        }
        if (session.state() != sedici::fg::SessionState::Ready) return false;
        ffxConfigureDescFrameGeneration stateProbe{};
        stateProbe.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        stateProbe.frameGenerationEnabled = true;
        if (!session.configure(&stateProbe.header) ||
            session.state() != sedici::fg::SessionState::Degraded) return false;
        stateProbe.frameGenerationEnabled = false;
        if (!session.configure(&stateProbe.header) ||
            session.state() != sedici::fg::SessionState::Suspended) return false;

        // Five consecutive moving frames, not repeating/resetting one pair.
        constexpr uint32_t cycles = 5;
        FrameTextures frames[cycles];
        for (uint32_t index = 0; index < cycles; ++index) {
            const auto display = make_scene(kScenes[0], static_cast<float>(index),
                                            kDisplayWidth, kDisplayHeight);
            const auto render = make_scene(kScenes[0], static_cast<float>(index),
                                           kRenderWidth, kRenderHeight);
            frames[index].color = create_texture(state.device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM,
                                                 kDisplayWidth, kDisplayHeight, D3D12_RESOURCE_FLAG_NONE);
            frames[index].depth = create_texture(state.device.Get(), DXGI_FORMAT_R32_FLOAT,
                                                 kRenderWidth, kRenderHeight, D3D12_RESOURCE_FLAG_NONE);
            frames[index].motion = create_texture(state.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
                                                  kRenderWidth, kRenderHeight, D3D12_RESOURCE_FLAG_NONE);
            if (!frames[index].color || !frames[index].depth || !frames[index].motion) return false;

            const auto colorBytes = quantise(display.color);
            const auto motionBytes = pack_ffx_motion(render.motion);
            const auto colorPitch = align_to(kDisplayWidth * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
            const auto renderPitch = align_to(kRenderWidth * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
            const auto motionPitch = align_to(kRenderWidth * 8, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);

            auto colorStaging = create_buffer(state.device.Get(), colorPitch * kDisplayHeight,
                                              D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
            auto depthStaging = create_buffer(state.device.Get(), renderPitch * kRenderHeight,
                                              D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
            auto motionStaging = create_buffer(state.device.Get(), motionPitch * kRenderHeight,
                                               D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
            if (!colorStaging || !depthStaging || !motionStaging) return false;

            if (FAILED(state.allocator->Reset()) ||
                FAILED(state.list->Reset(state.allocator.Get(), nullptr))) return false;
            upload(state.list.Get(), frames[index].color.Get(), colorStaging.Get(), colorBytes.data(),
                   kDisplayWidth, kDisplayHeight, 4);
            upload(state.list.Get(), frames[index].depth.Get(), depthStaging.Get(), render.depth.data(),
                   kRenderWidth, kRenderHeight, 4);
            upload(state.list.Get(), frames[index].motion.Get(), motionStaging.Get(), motionBytes.data(),
                   kRenderWidth, kRenderHeight, 8);
            if (!state.submit()) return false;
        }

        const uint32_t hudPitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
        auto hudUpload = create_buffer(state.device.Get(), hudPitch * 8,
            D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!hudUpload) return false;
        ShowWindow(window, SW_SHOWNOACTIVATE);
        for (uint32_t cycle = 0; cycle < cycles; ++cycle) {
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            const uint32_t source = cycle;
            if (FAILED(state.allocator->Reset()) ||
                FAILED(state.list->Reset(state.allocator.Get(), nullptr))) return false;

            ffxConfigureDescFrameGeneration configure{};
            configure.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
            configure.frameGenerationEnabled = true;
            configure.swapChain = handle.swapchain();
            configure.HUDLessColor = ffxApiGetResourceDX12(frames[source].color.Get(),
                                                           FFX_API_RESOURCE_STATE_COMMON);
            configure.frameID = cycle;

            ffxDispatchDescFrameGenerationPrepare prepare{};
            prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
            prepare.frameID = cycle;
            prepare.commandList = state.list.Get();
            prepare.renderSize = {kRenderWidth, kRenderHeight};
            prepare.motionVectorScale = {static_cast<float>(kRenderWidth),
                                         static_cast<float>(kRenderHeight)};
            prepare.depth = ffxApiGetResourceDX12(frames[source].depth.Get(),
                                                  FFX_API_RESOURCE_STATE_COMMON);
            prepare.motionVectors = ffxApiGetResourceDX12(frames[source].motion.Get(),
                                                          FFX_API_RESOURCE_STATE_COMMON);
            prepare.unused_reset = source == 0;
            if (!session.configure(&configure.header)) {
                std::printf("session: configure failed: %s\n", session.context().last_error());
                return false;
            }
            if (cycle == 0 && session.state() != sedici::fg::SessionState::Warmup) return false;
            if (!session.prepare(&prepare.header)) {
                std::printf("session: prepare failed: %s\n", session.context().last_error());
                return false;
            }

            // Stand in for the game's own rendering of this frame.
            ComPtr<ID3D12Resource> buffer;
            if (FAILED(handle.swapchain()->GetBuffer(handle.swapchain()->GetCurrentBackBufferIndex(),
                                                     IID_PPV_ARGS(&buffer)))) return false;
            transition(state.list.Get(), frames[source].color.Get(), D3D12_RESOURCE_STATE_COMMON,
                       D3D12_RESOURCE_STATE_COPY_SOURCE);
            transition(state.list.Get(), buffer.Get(), D3D12_RESOURCE_STATE_PRESENT,
                       D3D12_RESOURCE_STATE_COPY_DEST);
            state.list->CopyResource(buffer.Get(), frames[source].color.Get());
            transition(state.list.Get(), frames[source].color.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                       D3D12_RESOURCE_STATE_COMMON);
            const float marker[] = {static_cast<float>(cycle + 1) / 8.0f, 1.0f, 0.0f, 1.0f};
            void* mappedHud = nullptr;
            if (FAILED(hudUpload->Map(0, nullptr, &mappedHud))) return false;
            for (uint32_t y = 0; y < 8; ++y) {
                for (uint32_t x = 0; x < 8; ++x) {
                    auto* pixel = static_cast<uint8_t*>(mappedHud) + y * hudPitch + x * 4;
                    pixel[0] = static_cast<uint8_t>(std::lround(marker[0] * 255.0f));
                    pixel[1] = pixel[3] = 255;
                    pixel[2] = 0;
                }
            }
            hudUpload->Unmap(0, nullptr);
            D3D12_TEXTURE_COPY_LOCATION hudFrom{};
            hudFrom.pResource = hudUpload.Get();
            hudFrom.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            hudFrom.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, 8, 8, 1, hudPitch};
            D3D12_TEXTURE_COPY_LOCATION hudTo{};
            hudTo.pResource = buffer.Get();
            hudTo.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            state.list->CopyTextureRegion(&hudTo, 0, kDisplayHeight - 8, 0, &hudFrom, nullptr);
            transition(state.list.Get(), buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_PRESENT);
            if (!state.submit()) return false;

            if (contextRace && cycle == 0) {
                contextRecordArrived = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                contextRecordResume = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                HANDLE configureStarted = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                HANDLE configureDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                HANDLE prepareStarted = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                HANDLE prepareDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                if (!contextRecordArrived || !contextRecordResume || !configureStarted || !configureDone ||
                    !prepareStarted || !prepareDone) {
                    if (contextRecordArrived) CloseHandle(contextRecordArrived);
                    if (contextRecordResume) CloseHandle(contextRecordResume);
                    if (configureStarted) CloseHandle(configureStarted);
                    if (configureDone) CloseHandle(configureDone);
                    if (prepareStarted) CloseHandle(prepareStarted);
                    if (prepareDone) CloseHandle(prepareDone);
                    contextRecordArrived = contextRecordResume = nullptr;
                    return false;
                }
                sedici::fg::arm_context_record_hook(&pause_context_record);
                HRESULT presentResult = E_FAIL;
                std::thread presenting([&] { presentResult = handle.swapchain()->Present(1, 0); });
                const bool arrived = WaitForSingleObject(contextRecordArrived, 5000) == WAIT_OBJECT_0;
                bool configured = false;
                bool prepareAccepted = true;
                bool blocked = false;
                std::thread configuring;
                std::thread preparing;
                if (arrived) {
                    auto configureAgain = configure;
                    configuring = std::thread([&, configureAgain]() mutable {
                        SetEvent(configureStarted);
                        configured = session.configure(&configureAgain.header);
                        SetEvent(configureDone);
                    });
                    preparing = std::thread([&] {
                        ffxDispatchDescFrameGenerationPrepare invalid{};
                        invalid.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
                        SetEvent(prepareStarted);
                        prepareAccepted = session.prepare(&invalid.header);
                        SetEvent(prepareDone);
                    });
                    const bool started = WaitForSingleObject(configureStarted, 5000) == WAIT_OBJECT_0 &&
                                         WaitForSingleObject(prepareStarted, 5000) == WAIT_OBJECT_0;
                    blocked = started && WaitForSingleObject(configureDone, 50) == WAIT_TIMEOUT &&
                              WaitForSingleObject(prepareDone, 50) == WAIT_TIMEOUT;
                }
                SetEvent(contextRecordResume);
                presenting.join();
                if (configuring.joinable()) configuring.join();
                if (preparing.joinable()) preparing.join();
                sedici::fg::arm_context_record_hook(nullptr);
                CloseHandle(contextRecordArrived);
                CloseHandle(contextRecordResume);
                CloseHandle(configureStarted);
                CloseHandle(configureDone);
                CloseHandle(prepareStarted);
                CloseHandle(prepareDone);
                contextRecordArrived = contextRecordResume = nullptr;
                const bool serialized = arrived && blocked && SUCCEEDED(presentResult) && configured &&
                                        !prepareAccepted && g_debug_errors.load() == 0;
                std::puts(serialized ? "PASS configure and prepare wait for an active context callback"
                                     : "FAIL configure/context callback synchronization");
                return serialized;
            }

            ComPtr<ID3D12Fence> sessionRecoveryGate;
            if (sessionRecovery && cycle == 0) {
                if (FAILED(state.device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                                     IID_PPV_ARGS(&sessionRecoveryGate))) ||
                    FAILED(state.queue->Wait(sessionRecoveryGate.Get(), 1))) return false;
            }
            const UINT firstNative = native->GetCurrentBackBufferIndex();
            const auto fault = registrationFault ? sedici::fg::BookkeepingFault::SubmissionRegistration
                             : resetFault ? sedici::fg::BookkeepingFault::GenerationReset
                             : closeFault ? sedici::fg::BookkeepingFault::GenerationClose
                                          : sedici::fg::BookkeepingFault::GenerationSignal;
            if ((signalFault || closeFault || resetFault || sessionRecovery) && cycle == 0)
                sedici::fg::arm_bookkeeping_fault(fault);
            const HRESULT presentResult = handle.swapchain()->Present(1, 0);
            if ((signalFault || closeFault || resetFault || sessionRecovery) && cycle == 0) {
                const bool reachedInjection = !sedici::fg::consume_bookkeeping_fault(fault);
                sedici::fg::arm_bookkeeping_fault(sedici::fg::BookkeepingFault::None);
                const HRESULT retry = handle.swapchain()->Present(1, 0);
                const bool stopped = reachedInjection && presentResult == E_FAIL && retry == E_FAIL &&
                                     !session.retired() && g_debug_errors.load() == 0;
                if (!g_debug_callback_installed) std::puts("INFO DX12 debug callback unavailable");
                if (sessionRecovery) {
                    if (!stopped) {
                        (void)sessionRecoveryGate->Signal(1);
                        return false;
                    }
                    // K104: while destroy polls its bounded window, other callers of
                    // the context mutex must not be held for that whole window.
                    bool destroyRefused = false;
                    std::atomic<bool> destroyStarted{false};
                    std::thread destroying([&] {
                        destroyStarted = true;
                        destroyRefused = !session.destroy();
                    });
                    while (!destroyStarted.load()) std::this_thread::yield();
                    std::this_thread::sleep_for(std::chrono::milliseconds(40));
                    const auto probeStart = std::chrono::steady_clock::now();
                    (void)session.ready();
                    const auto probeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - probeStart).count();
                    destroying.join();
                    const bool lockFree = probeMs < 60;
                    std::printf("%s K104: a context call during the destroy wait is not blocked (%lld ms)%c",
                                lockFree ? "PASS" : "FAIL", static_cast<long long>(probeMs), 10);
                    const bool firstDestroyRefused = lockFree && destroyRefused &&
                        session.state() == sedici::fg::SessionState::Draining;
                    recoveryTestGate = sessionRecoveryGate.Get();
                    recoveryTestReleased = false;
                    const unsigned retainedBefore = sedici::fg::swapchain_retention_count();
                    sedici::fg::arm_queue_recovery_hook(&release_recovery_test_gate);
                    const auto before = std::chrono::steady_clock::now();
                    handle.destroy();
                    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - before).count();
                    sedici::fg::arm_queue_recovery_hook(nullptr);
                    recoveryTestGate = nullptr;
                    (void)sessionRecoveryGate->Signal(1);
                    const bool retired = session.retired();
                    const bool destroyed = session.destroy();
                    const bool recovered = firstDestroyRefused && recoveryTestReleased.load() &&
                        elapsed < 3000 && sedici::fg::swapchain_retention_count() == retainedBefore &&
                        retired && destroyed && session.state() == sedici::fg::SessionState::Stopped &&
                        SUCCEEDED(state.device->GetDeviceRemovedReason()) && g_debug_errors.load() == 0;
                    std::puts(recovered ? "PASS session retires after a replacement queue fence"
                                        : "FAIL session retirement after signal failure");
                    return recovered;
                }
                // Remove only this isolated test process's device before teardown.
                ComPtr<ID3D12Device5> removable;
                if (FAILED(state.device.As(&removable))) return false;
                removable->RemoveDevice();
                std::puts(stopped ? "PASS injected generation failure stops later admission"
                                  : "FAIL injected generation failure did not stop admission");
                return stopped;
            }
            if (FAILED(presentResult)) {
                std::puts("session: present failed");
                return false;
            }
            if (cycle == 0 && session.state() != sedici::fg::SessionState::Active) return false;
            std::printf("session: cycle %u generated %llu shown %llu skipped %llu\n", cycle,
                        static_cast<unsigned long long>(session.context().frames_generated()),
                        static_cast<unsigned long long>(handle.wrapper()->generated_presents()),
                         static_cast<unsigned long long>(handle.wrapper()->skipped_generates()));

            if (loseDevice && cycle == 1) {
                // P2.5 device loss after generation; this does not claim that
                // the earlier GPU work is still in flight at removal time.
                // Present must fail and teardown must remain bounded (K38/K73).
                ComPtr<ID3D12Device5> device5;
                if (FAILED(state.device.As(&device5))) {
                    std::printf("SKIP device loss: ID3D12Device5 unavailable\n");
                    return true;
                }
                device5->RemoveDevice();
                if (session.state() != sedici::fg::SessionState::DeviceLost || session.ready() ||
                    session.configure(&configure.header) || session.prepare(&prepare.header)) return false;
                const HRESULT afterLoss = handle.swapchain()->Present(1, 0);
                std::printf("session: present after device removal -> 0x%08lx\n",
                            static_cast<unsigned long>(afterLoss));
                if (SUCCEEDED(afterLoss)) return false;
                const auto begin = std::chrono::steady_clock::now();
                if (!session.destroy()) {
                    std::printf("session: destroy refused after device removal: %s\n",
                                session.context().last_error());
                    return false;
                }
                if (session.state() != sedici::fg::SessionState::Stopped) return false;
                handle.destroy();
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - begin).count();
                std::printf("session: teardown after device removal took %lld ms\n",
                            static_cast<long long>(ms));
                return ms < 3000;
            }

            // Each native image must belong to this cycle. FLIP_SEQUENTIAL
            // preserves these buffers after Present. Exclude the top-left OSD
            // area from the scene comparison, but verify the HUD label below it.
            for (UINT image = 0; image < (cycle ? 2u : 1u); ++image) {
                ComPtr<ID3D12Resource> presented;
                if (FAILED(native->GetBuffer((firstNative + image) % desc.BufferCount,
                                             IID_PPV_ARGS(&presented)))) return false;
                const auto actual = read_presented(state, presented.Get());
                if (actual.empty()) return false;
                const bool midpoint = cycle && image == 0;
                const float time = static_cast<float>(cycle) - (midpoint ? 0.5f : 0.0f);
                const auto expected = quantise(make_scene(kScenes[0], time,
                                                          kDisplayWidth, kDisplayHeight).color);
                std::vector<uint8_t> sceneActual, sceneExpected;
                for (uint32_t y = 80; y < kDisplayHeight - 16; ++y) {
                    const size_t begin = (static_cast<size_t>(y) * kDisplayWidth + 160) * 4;
                    const size_t end = static_cast<size_t>(y + 1) * kDisplayWidth * 4;
                    sceneActual.insert(sceneActual.end(), actual.begin() + begin, actual.begin() + end);
                    sceneExpected.insert(sceneExpected.end(), expected.begin() + begin, expected.begin() + end);
                }
                const double score = psnr(sceneExpected, sceneActual);
                const size_t label = (static_cast<size_t>(kDisplayHeight - 4) * kDisplayWidth + 4) * 4;
                const int red = static_cast<int>(std::lround(marker[0] * 255.0f));
                const bool matchingHud = std::abs(static_cast<int>(actual[label]) - red) <= 1 &&
                    actual[label + 1] == 255 && actual[label + 2] == 0;
                std::printf("session: frame %u %s time=%.1f psnr=%.2f HUD=%s\n", cycle,
                    midpoint ? "midpoint" : "real", time, score, matchingHud ? "current" : "WRONG");
                if (!matchingHud || (midpoint ? score < 35.0 : sceneActual != sceneExpected)) {
                    if (knownSignature && cycle == 2 && midpoint && matchingHud &&
                        score >= 15.5 && score <= 16.5 &&
                        handle.wrapper()->generated_presents() == 1 &&
                        handle.wrapper()->skipped_generates() == 1)
                        *knownSignature = true;
                    return false;
                }
            }
        }

        if (session.context().frames_generated() != cycles) {
            std::printf("session: generated %llu of %u\n",
                        static_cast<unsigned long long>(session.context().frames_generated()), cycles);
            std::printf("session: context last error '%s'\n", session.context().last_error());
            return false;
        }
        if (handle.wrapper()->generated_presents() != cycles - 1) {
            std::printf("session: presented %llu of %u, skipped %llu\n",
                        static_cast<unsigned long long>(handle.wrapper()->generated_presents()),
                        cycles - 1,
                        static_cast<unsigned long long>(handle.wrapper()->skipped_generates()));
            return false;
        }
        if (handle.wrapper()->skipped_generates() != 0) return false;

        // Turning frame generation off must stop generation without breaking
        // presentation, and destruction must respect GPU retirement.
        ffxConfigureDescFrameGeneration off{};
        off.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        off.frameGenerationEnabled = false;
        off.swapChain = handle.swapchain();
        off.frameID = cycles;
        if (!session.configure(&off.header)) return false;
        if (session.state() != sedici::fg::SessionState::Suspended) return false;
        if (FAILED(handle.swapchain()->Present(1, 0))) return false;
        if (handle.wrapper()->generated_presents() != cycles - 1) return false;

        if (!session.destroy()) return false;
        if (session.state() != sedici::fg::SessionState::Stopped) return false;
        // Detached: the wrapper keeps presenting and never calls the destroyed owner.
        if (FAILED(handle.swapchain()->Present(1, 0))) return false;
        if (handle.wrapper()->generated_presents() != cycles - 1) return false;
        if (session.configure(&off.header) || session.prepare(&off.header)) return false; // refused after stop
        if (!session.destroy()) return false; // idempotent
        handle.destroy();
        return true;
    }();

    DestroyWindow(window);
    UnregisterClassW(className, wc.hInstance);
    std::printf("%s session: %s\n", ok ? "PASS" : "FAIL",
                contextRace ? "context callback synchronization" :
                sessionRecovery ? "session retirement recovery" :
                (signalFault || closeFault || resetFault) ? "injected failure stops admission" :
                "describe, prepare, generate and present through the wrapper");
    return ok;
}

HANDLE routingLookupArrived = nullptr;
HANDLE routingLookupResume = nullptr;
void pause_routing_after_lookup() noexcept
{
    SetEvent(routingLookupArrived);
    (void)WaitForSingleObject(routingLookupResume, 5000);
}

std::atomic<unsigned> routingHookA{0};
std::atomic<unsigned> routingHookB{0};
void routing_hook_a(uint64_t) { ++routingHookA; }
void routing_hook_b(uint64_t) { ++routingHookB; }

bool check_routing(Device& state, bool wrapFirst = false)
{
    const wchar_t* className = L"SediciFgRouting";
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = className;
    if (!RegisterClassW(&wc)) return false;
    HWND window = CreateWindowW(className, L"Sedici routing", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 500, 320, nullptr, nullptr, wc.hInstance, nullptr);
    if (!window) { UnregisterClassW(className, wc.hInstance); return false; }

    bool ok = [&] {
        ComPtr<IDXGIFactory4> factory;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return false;
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = kDisplayWidth;
        desc.Height = kDisplayHeight;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 3;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<IDXGISwapChain1> created;
        if (FAILED(factory->CreateSwapChainForHwnd(state.queue.Get(), window, &desc,
            nullptr, nullptr, &created))) return false;
        ComPtr<IDXGISwapChain4> native, caller;
        if (FAILED(created.As(&native))) return false;

        struct Contexts {
            ffxContext session = nullptr;
            ffxContext wrap = nullptr;
            ~Contexts() {
                ffxReturnCode_t result{};
                if (session) (void)sedici::fg::route_destroy(&session, &result);
                if (wrap) (void)sedici::fg::route_destroy(&wrap, &result);
            }
        } contexts;
        ffxReturnCode_t result = FFX_API_RETURN_ERROR;
        ffxCreateBackendDX12Desc backend{};
        backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        backend.device = state.device.Get();
        ffxCreateContextDescFrameGeneration create{};
        create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
        create.header.pNext = &backend.header;
        create.flags = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED;
        create.displaySize = {kDisplayWidth, kDisplayHeight};
        create.maxRenderSize = {kRenderWidth, kRenderHeight};
        create.backBufferFormat = FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
        IDXGISwapChain4* replacement = native.Get();
        ffxCreateContextDescFrameGenerationSwapChainWrapDX12 wrap{};
        wrap.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WRAP_DX12;
        wrap.header.pNext = &backend.header;
        wrap.swapchain = &replacement;
        wrap.gameQueue = state.queue.Get();
        const auto createSession = [&] {
            return sedici::fg::route_create(&contexts.session, &create.header, &result) &&
                   result == FFX_API_RETURN_OK && contexts.session;
        };
        const auto createWrap = [&] {
            // Each injected bookkeeping failure must reject locally before the
            // native chain is touched. Normal wrapping immediately afterwards
            // exercises rollback and proves the chain remains usable.
            for (const auto point : {sedici::fg::BookkeepingFault::OwnerAllocation,
                                     sedici::fg::BookkeepingFault::RouteRegistryAllocation,
                                     sedici::fg::BookkeepingFault::WrapperRegistryAllocation}) {
                IDXGISwapChain4* untouched = native.Get();
                ffxContext failedContext = nullptr;
                auto failedWrap = wrap;
                failedWrap.swapchain = &untouched;
                ffxReturnCode_t failedResult = FFX_API_RETURN_OK;
                sedici::fg::arm_bookkeeping_fault(point);
                const bool answered = sedici::fg::route_create(&failedContext, &failedWrap.header,
                                                                &failedResult);
                const bool reachedInjection = !sedici::fg::consume_bookkeeping_fault(point);
                sedici::fg::arm_bookkeeping_fault(sedici::fg::BookkeepingFault::None);
                if (!reachedInjection || !answered || failedResult == FFX_API_RETURN_OK ||
                    failedContext || untouched != native.Get()) return false;
            }
            std::puts("PASS injected create-bookkeeping failures leave the native swapchain untouched");
            if (!sedici::fg::route_create(&contexts.wrap, &wrap.header, &result) ||
                result != FFX_API_RETURN_OK || !contexts.wrap || replacement == native.Get()) return false;
            caller.Attach(replacement); // Adopt the reference returned to the caller.
            return true;
        };
        // Exercise hook updates concurrently with both creation orders. This
        // is stress coverage; it does not force every possible interleaving.
        std::atomic<bool> stopHooks{false};
        std::atomic<bool> hooksStarted{false};
        std::thread hookUpdater([&] {
            hooksStarted = true;
            while (!stopHooks.load()) {
                sedici::fg::route_set_present_hook(&routing_hook_a);
                sedici::fg::route_set_present_hook(&routing_hook_b);
                std::this_thread::yield();
            }
        });
        while (!hooksStarted.load()) std::this_thread::yield();
        const bool creationOk = wrapFirst ? (createWrap() && createSession())
                                         : (createSession() && createWrap());
        stopHooks = true;
        hookUpdater.join();
        if (!creationOk || contexts.wrap == contexts.session) {
            sedici::fg::route_set_present_hook(nullptr);
            return false;
        }
        routingHookA = 0;
        routingHookB = 0;
        sedici::fg::route_set_present_hook(&routing_hook_b);
        const HRESULT hookedPresent = caller->Present(0, 0);
        sedici::fg::route_set_present_hook(nullptr);
        if (FAILED(hookedPresent) || routingHookA != 0 || routingHookB != 1) return false;
        // K52: a DO_NOT_WAIT present never enters the (sleeping) limiter hook.
        routingHookB = 0;
        sedici::fg::route_set_present_hook(&routing_hook_b);
        const HRESULT nonBlocking = caller->Present(0, DXGI_PRESENT_DO_NOT_WAIT);
        const bool hookSkipped = routingHookB == 0;
        const HRESULT waiting = caller->Present(0, 0);
        sedici::fg::route_set_present_hook(nullptr);
        if ((FAILED(nonBlocking) && nonBlocking != DXGI_ERROR_WAS_STILL_DRAWING) || !hookSkipped ||
            FAILED(waiting) || routingHookB != 1) return false;
        std::puts("PASS K52: a DO_NOT_WAIT present skips the limiter hook and a waiting one runs it");

        ffxConfigureDescFrameGeneration configure{};
        configure.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        configure.swapChain = caller.Get();
        configure.frameGenerationEnabled = false;
        if (!sedici::fg::route_configure(&contexts.session, &configure.header, &result) ||
            result != FFX_API_RETURN_OK) return false;

        // A different generator may replace this Session's sink. Destroying the
        // old Session must not clear that newer owner (K79).
        struct ExtraContext {
            ffxContext value = nullptr;
            ~ExtraContext() {
                if (value) { ffxReturnCode_t ignored{}; (void)sedici::fg::route_destroy(&value, &ignored); }
            }
        } extra;
        if (!sedici::fg::route_create(&extra.value, &create.header, &result) ||
            result != FFX_API_RETURN_OK || !extra.value) return false;
        auto enabledConfigure = configure;
        enabledConfigure.frameGenerationEnabled = true;
        if (!sedici::fg::route_configure(&extra.value, &enabledConfigure.header, &result) ||
            result != FFX_API_RETURN_OK) return false;
        auto* wrapperObject = static_cast<sedici::fg::Swapchain*>(caller.Get());
        uint32_t replacementCalls = 0;
        const sedici::fg::GenerateSink replacementSink{&replacementCalls, &record_counted, nullptr};
        if (!wrapperObject->set_generator(replacementSink)) return false;
        auto disabledConfigure = enabledConfigure;
        disabledConfigure.frameGenerationEnabled = false;
        if (!sedici::fg::route_configure(&extra.value, &disabledConfigure.header, &result) ||
            result != FFX_API_RETURN_OK || FAILED(caller->Present(0, 0)) || replacementCalls != 1) return false;
        if (!sedici::fg::route_configure(&extra.value, &enabledConfigure.header, &result) ||
            result != FFX_API_RETURN_OK || !wrapperObject->set_generator(replacementSink)) return false;
        if (!sedici::fg::route_destroy(&extra.value, &result) || result != FFX_API_RETURN_OK) return false;
        extra.value = nullptr;
        if (FAILED(caller->Present(0, 0)) || replacementCalls != 2) return false;
        std::puts("PASS disabling or destroying an old session preserves a replacement generator");
        wrapperObject->set_generator(sedici::fg::GenerateSink{});

        // K41: a destroy refused because GPU work has not retired parks the owner. The
        // game's token is stale at once; the owner is freed on a later call after the
        // work retires, and nothing is freed or double-parked in between.
        {
            ffxApiHeader parkProbe{};
            parkProbe.type = UINT64_MAX;
            ffxContext parkedContexts[3] = {};
            bool created = true;
            for (auto& parkedContext : parkedContexts)
                created = created && sedici::fg::route_create(&parkedContext, &create.header, &result) &&
                          result == FFX_API_RETURN_OK && parkedContext;
            const unsigned parkedBefore = sedici::fg::route_parked_count();
            sedici::fg::set_retirement_hold(true);
            bool refused = created;
            for (auto& parkedContext : parkedContexts) {
                ffxContext token = parkedContext;
                refused = refused && sedici::fg::route_destroy(&token, &result) &&
                          result == FFX_API_RETURN_ERROR;
            }
            const unsigned parkedHeld = sedici::fg::route_parked_count();
            // A repeated destroy and a configure of a stale token are rejected and
            // must not park the owner a second time.
            ffxContext repeated = parkedContexts[0];
            const bool secondDestroy = sedici::fg::route_destroy(&repeated, &result) && result == FFX_API_RETURN_ERROR;
            const bool staleConfigure = sedici::fg::route_configure(&repeated, &disabledConfigure.header, &result) &&
                                        result == FFX_API_RETURN_ERROR;
            sedici::fg::route_query(&contexts.session, &parkProbe, &result); // reaps; still held
            const unsigned parkedStillHeld = sedici::fg::route_parked_count();
            sedici::fg::set_retirement_hold(false);
            sedici::fg::route_query(&contexts.session, &parkProbe, &result); // reaps
            const unsigned parkedAfter = sedici::fg::route_parked_count();
            const bool parking = created && refused && secondDestroy && staleConfigure &&
                                 parkedHeld == parkedBefore + 3 && parkedStillHeld == parkedHeld &&
                                 parkedAfter == parkedBefore;
            std::printf("%s K41: refused destroys park their owners, a repeat does not double-park, and they are released once work retires (%u -> %u -> %u)%c",
                        parking ? "PASS" : "FAIL", parkedBefore, parkedHeld, parkedAfter, 10);
            if (!parking) return false;
        }

        ffxApiHeader unsupported{};
        unsupported.type = UINT64_MAX;
        for (auto* context : {&contexts.session, &contexts.wrap}) {
            if (!sedici::fg::route_query(context, &unsupported, &result) ||
                result != FFX_API_RETURN_ERROR) return false;
            if (!sedici::fg::route_dispatch(context, &unsupported, &result) ||
                result != FFX_API_RETURN_ERROR) return false;
        }
        int unrelated = 0;
        ffxContext foreign = &unrelated;
        if (sedici::fg::route_query(&foreign, &unsupported, &result)) return false;

        if (!wrapFirst) {
            // Force the K68 window: configure has an owning wrapper reference,
            // then wrapper-context destroy completes, then configure resumes.
            routingLookupArrived = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            routingLookupResume = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!routingLookupArrived || !routingLookupResume) {
                if (routingLookupArrived) CloseHandle(routingLookupArrived);
                if (routingLookupResume) CloseHandle(routingLookupResume);
                routingLookupArrived = routingLookupResume = nullptr;
                return false;
            }
            auto raceConfigure = configure;
            raceConfigure.frameGenerationEnabled = true;
            ffxContext session = contexts.session;
            ffxReturnCode_t configureResult = FFX_API_RETURN_ERROR;
            bool configureAnswered = false;
            sedici::fg::arm_wrapper_lookup_hook(&pause_routing_after_lookup);
            std::thread configuring([&] {
                configureAnswered = sedici::fg::route_configure(
                    &session, &raceConfigure.header, &configureResult);
            });
            const bool arrived = WaitForSingleObject(routingLookupArrived, 5000) == WAIT_OBJECT_0;
            ffxReturnCode_t destroyResult = FFX_API_RETURN_ERROR;
            const bool destroyed = arrived && sedici::fg::route_destroy(&contexts.wrap, &destroyResult) &&
                                   destroyResult == FFX_API_RETURN_OK;
            if (destroyed) contexts.wrap = nullptr;
            SetEvent(routingLookupResume);
            configuring.join();
            sedici::fg::arm_wrapper_lookup_hook(nullptr);
            CloseHandle(routingLookupArrived);
            CloseHandle(routingLookupResume);
            routingLookupArrived = routingLookupResume = nullptr;
            if (!arrived || !destroyed || !configureAnswered || configureResult != FFX_API_RETURN_OK)
                return false;
            std::puts("PASS configure paused after lookup cannot reattach after wrapper destroy");
        }

        // Calls racing a destroy of the same handle: each either runs on the live
        // owner or is refused as a stale issued token; none may reach a torn-down
        // owner (P2.4 lease contract). The registry lock is not held across these calls.
        {
            const ffxContext racing = contexts.session;
            std::atomic<bool> go{false};
            std::atomic<int> answered{0};
            std::vector<std::thread> threads;
            for (int t = 0; t < 4; ++t) {
                threads.emplace_back([&, t] {
                    while (!go.load()) std::this_thread::yield();
                    for (int i = 0; i < 400; ++i) {
                        ffxContext handle = racing;
                        ffxReturnCode_t local = FFX_API_RETURN_OK;
                        const bool routed = (t & 1)
                            ? sedici::fg::route_configure(&handle, &configure.header, &local)
                            : sedici::fg::route_dispatch(&handle, &unsupported, &local);
                        if (routed) answered.fetch_add(1);
                    }
                });
            }
            go = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            bool wrapDestroyed = true;
            if (wrapFirst) {
                wrapDestroyed = sedici::fg::route_destroy(&contexts.wrap, &result) &&
                                result == FFX_API_RETURN_OK;
                if (wrapDestroyed) contexts.wrap = nullptr;
            }
            const bool sessionDestroyed = sedici::fg::route_destroy(&contexts.session, &result) &&
                                          result == FFX_API_RETURN_OK;
            // Join on every path: returning with joinable threads terminates the host.
            for (auto& thread : threads) thread.join();
            if (!wrapDestroyed || !sessionDestroyed) return false;
            ffxContext stale = racing;
            if (!sedici::fg::route_configure(&stale, &configure.header, &result) ||
                result == FFX_API_RETURN_OK) return false;
            if (!sedici::fg::route_destroy(&stale, &result) ||
                result == FFX_API_RETURN_OK) return false; // no double destroy
            contexts.session = nullptr;
        }
        // Hold a wrapper reference before destroying its FFX context, then
        // force an attempted attachment only after destruction returns. This
        // deterministically exercises the late-attachment gate (K68).
        auto* survivingWrapper = static_cast<sedici::fg::Swapchain*>(caller.Get());
        uint32_t lateCalls = 0;
        const sedici::fg::GenerateSink lateSink{&lateCalls, &record_counted, nullptr};
        if (contexts.wrap) {
            ComPtr<sedici::fg::Swapchain> held = survivingWrapper;
            std::atomic<bool> heldReady{false}, tryAttach{false}, attached{false};
            std::thread late([&] {
                heldReady = true;
                while (!tryAttach.load()) std::this_thread::yield();
                attached = held->set_generator(lateSink);
            });
            while (!heldReady.load()) std::this_thread::yield();
            const bool destroyed = sedici::fg::route_destroy(&contexts.wrap, &result) &&
                                   result == FFX_API_RETURN_OK;
            tryAttach = true;
            late.join();
            if (!destroyed || attached.load()) return false;
            contexts.wrap = nullptr;
        }
        // The caller's COM reference survives FFX wrapper-context destruction,
        // but the closed generator gate must reject any late reattachment.
        if (survivingWrapper->set_generator(lateSink)) return false;
        if (FAILED(caller->Present(0, 0)) || lateCalls != 0) return false;
        DXGI_SWAP_CHAIN_DESC1 after{};
        return SUCCEEDED(caller->GetDesc1(&after)) && after.Width == kDisplayWidth;
    }();
    DestroyWindow(window);
    UnregisterClassW(className, wc.hInstance);
    std::printf("%s routing: two live handles (%s), configure/query/dispatch, foreign handle, racing destroy\n",
                ok ? "PASS" : "FAIL", wrapFirst ? "wrap-first" : "session-first");
    return ok;
}

// P2.4 creation-path failures and hook propagation during creation:
//   K70  unsupported back buffer format -> local error, no native change
//   K29  handle-namespace reservation failure -> local error, retry succeeds
//   K63  backend allocation failure before creation -> local error, chain untouched
//   K60  hook change racing wrap creation -> the newest hook is installed
HANDLE routingCreationArrived = nullptr;
HANDLE routingCreationResume = nullptr;
void pause_creation_hook() noexcept
{
    SetEvent(routingCreationArrived);
    (void)WaitForSingleObject(routingCreationResume, 5000);
}

bool check_routing_faults(Device& state)
{
    const wchar_t* className = L"SediciFgCreation";
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = className;
    if (!RegisterClassW(&wc)) return false;
    HWND window = CreateWindowW(className, L"Sedici creation", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 500, 320, nullptr, nullptr, wc.hInstance, nullptr);
    if (!window) { UnregisterClassW(className, wc.hInstance); return false; }

    bool ok = [&] {
        ComPtr<IDXGIFactory4> factory;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return false;
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = kDisplayWidth;
        desc.Height = kDisplayHeight;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 3;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<IDXGISwapChain1> created;
        if (FAILED(factory->CreateSwapChainForHwnd(state.queue.Get(), window, &desc,
            nullptr, nullptr, &created))) return false;
        ComPtr<IDXGISwapChain4> native;
        if (FAILED(created.As(&native))) return false;
        ComPtr<IDXGISwapChain4> caller;

        struct Release {
            ffxContext* context;
            ~Release() {
                if (*context) { ffxReturnCode_t ignored{}; (void)sedici::fg::route_destroy(context, &ignored); }
            }
        };
        ffxReturnCode_t result = FFX_API_RETURN_ERROR;
        ffxCreateBackendDX12Desc backend{};
        backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        backend.device = state.device.Get();
        ffxCreateContextDescFrameGeneration create{};
        create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
        create.header.pNext = &backend.header;
        create.flags = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED;
        create.displaySize = {kDisplayWidth, kDisplayHeight};
        create.maxRenderSize = {kRenderWidth, kRenderHeight};
        create.backBufferFormat = FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;

        // K70: an unsupported back buffer format is a local, logged error.
        {
            auto unsupported = create;
            unsupported.backBufferFormat = FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM;
            ffxContext context = nullptr;
            const bool answered = sedici::fg::route_create(&context, &unsupported.header, &result);
            if (!answered || result == FFX_API_RETURN_OK || context) return false;
            std::puts("PASS K70: unsupported back buffer format is a local error");
        }

        // K29: a refused handle reservation issues no token; a retry succeeds.
        {
            ffxContext denied = nullptr;
            sedici::fg::arm_bookkeeping_fault(sedici::fg::BookkeepingFault::TokenReservation);
            const bool answered = sedici::fg::route_create(&denied, &create.header, &result);
            const bool reached = !sedici::fg::consume_bookkeeping_fault(sedici::fg::BookkeepingFault::TokenReservation);
            sedici::fg::arm_bookkeeping_fault(sedici::fg::BookkeepingFault::None);
            if (!answered || !reached || result == FFX_API_RETURN_OK || denied) return false;
            ffxContext session = nullptr;
            Release releaseSession{&session};
            if (!sedici::fg::route_create(&session, &create.header, &result) ||
                result != FFX_API_RETURN_OK || !session) return false;
            std::puts("PASS K29: a refused handle reservation issues no token and a retry succeeds");
        }

        // K63: backend allocation failure before creation rejects locally.
        {
            ffxContext denied = nullptr;
            sedici::fg::arm_bookkeeping_fault(sedici::fg::BookkeepingFault::BackendCreation);
            const bool answered = sedici::fg::route_create(&denied, &create.header, &result);
            const bool reached = !sedici::fg::consume_bookkeeping_fault(sedici::fg::BookkeepingFault::BackendCreation);
            sedici::fg::arm_bookkeeping_fault(sedici::fg::BookkeepingFault::None);
            if (!answered || !reached || result == FFX_API_RETURN_OK || denied) return false;
        }
        IDXGISwapChain4* nativePointer = native.Get();
        {
            IDXGISwapChain4* replacement = nativePointer;
            ffxCreateContextDescFrameGenerationSwapChainWrapDX12 wrap{};
            wrap.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WRAP_DX12;
            wrap.header.pNext = &backend.header;
            wrap.swapchain = &replacement;
            wrap.gameQueue = state.queue.Get();
            ffxContext denied = nullptr;
            sedici::fg::arm_bookkeeping_fault(sedici::fg::BookkeepingFault::BackendCreation);
            const bool answered = sedici::fg::route_create(&denied, &wrap.header, &result);
            const bool reached = !sedici::fg::consume_bookkeeping_fault(sedici::fg::BookkeepingFault::BackendCreation);
            sedici::fg::arm_bookkeeping_fault(sedici::fg::BookkeepingFault::None);
            if (!answered || !reached || result == FFX_API_RETURN_OK || denied ||
                replacement != nativePointer) return false;
        }
        std::puts("PASS K63: backend allocation failure rejects creation before the native chain is touched");

        // K102: a chain naming both a wrap and frame generation is refused, not half-created.
        {
            IDXGISwapChain4* replacement = nativePointer;
            auto both = create; // frame generation desc, chained behind the wrap desc
            ffxCreateContextDescFrameGenerationSwapChainWrapDX12 wrap{};
            wrap.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WRAP_DX12;
            wrap.header.pNext = &both.header;
            wrap.swapchain = &replacement;
            wrap.gameQueue = state.queue.Get();
            ffxContext refused = nullptr;
            const bool answered = sedici::fg::route_create(&refused, &wrap.header, &result);
            if (!answered || result == FFX_API_RETURN_OK || refused || replacement != nativePointer)
                return false;
            std::puts("PASS K102: a create naming both a wrap and frame generation is refused");
        }

        // K60: a hook change while wrap creation is paused must end on the newest hook.
        {
            routingCreationArrived = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            routingCreationResume = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!routingCreationArrived || !routingCreationResume) {
                if (routingCreationArrived) CloseHandle(routingCreationArrived);
                if (routingCreationResume) CloseHandle(routingCreationResume);
                routingCreationArrived = routingCreationResume = nullptr;
                return false;
            }
            IDXGISwapChain4* replacement = nativePointer;
            ffxCreateContextDescFrameGenerationSwapChainWrapDX12 wrap{};
            wrap.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WRAP_DX12;
            wrap.header.pNext = &backend.header;
            wrap.swapchain = &replacement;
            wrap.gameQueue = state.queue.Get();
            ffxContext wrapContext = nullptr;
            ffxReturnCode_t wrapResult = FFX_API_RETURN_ERROR;
            bool wrapAnswered = false;
            sedici::fg::route_set_present_hook(&routing_hook_a);
            sedici::fg::arm_creation_hook(&pause_creation_hook);
            std::thread creating([&] {
                wrapAnswered = sedici::fg::route_create(&wrapContext, &wrap.header, &wrapResult);
            });
            const bool arrived = WaitForSingleObject(routingCreationArrived, 5000) == WAIT_OBJECT_0;
            // Give the change 200 ms to finish while creation is paused. Correct code
            // blocks it on the paused owner's call lock, so the wait times out. If the
            // change completes here, creation then installs the stale hook and fails.
            std::atomic<bool> changed{false};
            std::thread changing([&] {
                sedici::fg::route_set_present_hook(&routing_hook_b);
                changed = true;
            });
            for (int waited = 0; waited < 200 && !changed.load(); ++waited) Sleep(1);
            // Correct code must still be holding the change back here.
            const bool heldBack = !changed.load();
            SetEvent(routingCreationResume);
            creating.join();
            changing.join();
            sedici::fg::arm_creation_hook(nullptr);
            CloseHandle(routingCreationArrived);
            CloseHandle(routingCreationResume);
            routingCreationArrived = routingCreationResume = nullptr;
            Release releaseWrap{&wrapContext};
            if (!arrived || !heldBack || !wrapAnswered || wrapResult != FFX_API_RETURN_OK ||
                !wrapContext || replacement == nativePointer) return false;
            caller.Attach(replacement); // adopt the reference the wrap returned
            routingHookA = 0;
            routingHookB = 0;
            const HRESULT presented = caller->Present(0, 0);
            sedici::fg::route_set_present_hook(nullptr);
            if (FAILED(presented) || routingHookA != 0 || routingHookB != 1) return false;
        }
        std::puts("PASS K60: a hook change during wrap creation ends on the newest hook");
        return true;
    }();
    DestroyWindow(window);
    UnregisterClassW(className, wc.hInstance);
    std::printf("%s routing faults: K70, K29, K63 and K60\n", ok ? "PASS" : "FAIL");
    return ok;
}

bool check_window(Device& state)
{
    std::printf("window host: creating window\n");
    const wchar_t* className = L"SediciFgAcceptance";
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = className;
    if (!RegisterClassW(&wc)) return false;
    HWND window = CreateWindowW(className, L"Sedici offline swapchain", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 500, 320, nullptr, nullptr, wc.hInstance, nullptr);
    if (!window) { UnregisterClassW(className, wc.hInstance); return false; }
    bool ok = [&] {
        std::printf("window host: creating native swapchain\n");
        ComPtr<IDXGIFactory4> factory;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return false;
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = 480;
        desc.Height = 270;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 3;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<IDXGISwapChain1> created;
        if (FAILED(factory->CreateSwapChainForHwnd(state.queue.Get(), window, &desc,
            nullptr, nullptr, &created))) return false;
        ComPtr<IDXGISwapChain4> native;
        if (FAILED(created.As(&native))) return false;
        ComPtr<sedici::fg::Swapchain> wrapped;
        std::printf("window host: allocating wrapper\n");
        if (FAILED(sedici::fg::Swapchain::wrap(native.Get(), state.queue.Get(), &wrapped))) return false;
        ComPtr<IUnknown> identity, identityAgain;
        ComPtr<IDXGISwapChain1> older;
        if (FAILED(wrapped.As(&identity)) || FAILED(wrapped.As(&older)) ||
            FAILED(older.As(&identityAgain)) || identity.Get() != identityAgain.Get()) return false;
        ComPtr<ID3D12DescriptorHeap> rtv;
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap.NumDescriptors = 1;
        if (FAILED(state.device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&rtv)))) return false;
        ShowWindow(window, SW_SHOWNOACTIVATE);
        std::printf("window host: presenting\n");
        for (UINT frame = 0; frame < 8; ++frame) {
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            const UINT index = wrapped->GetCurrentBackBufferIndex();
            if (FAILED(wrapped->Present(0, DXGI_PRESENT_TEST)) ||
                index != wrapped->GetCurrentBackBufferIndex()) return false;
            ComPtr<ID3D12Resource> buffer;
            if (FAILED(wrapped->GetBuffer(index, IID_PPV_ARGS(&buffer)))) return false;
            const auto target = rtv->GetCPUDescriptorHandleForHeapStart();
            state.device->CreateRenderTargetView(buffer.Get(), nullptr, target);
            if (FAILED(state.allocator->Reset()) || FAILED(state.list->Reset(state.allocator.Get(), nullptr))) return false;
            transition(state.list.Get(), buffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
            const float color[4]{}; // Matches the wrapper's optimized clear value.
            state.list->ClearRenderTargetView(target, color, 0, nullptr);
            transition(state.list.Get(), buffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
            if (!state.submit()) return false;
            if (frame % 2) {
                DXGI_PRESENT_PARAMETERS parameters{};
                if (FAILED(wrapped->Present1(1, 0, &parameters))) return false;
            } else if (FAILED(wrapped->Present(1, 0))) return false;
            if (wrapped->GetCurrentBackBufferIndex() != (index + 1) % 3) return false;
        }
        ComPtr<ID3D12Resource> held;
        std::printf("window host: resizing\n");
        if (FAILED(wrapped->GetBuffer(0, IID_PPV_ARGS(&held)))) return false;
        if (wrapped->ResizeBuffers(3, 320, 180, desc.Format, 0) != DXGI_ERROR_INVALID_CALL) return false;
        std::printf("window host: outstanding-reference rejection passed\n");
        held.Reset();
        if (FAILED(wrapped->ResizeBuffers(3, 320, 180, desc.Format, 0))) return false;
        std::printf("window host: resize completed\n");
        if (FAILED(wrapped->GetBuffer(0, IID_PPV_ARGS(&held))) || held->GetDesc().Width != 320) return false;
        held.Reset();
        std::printf("window host: releasing swapchain\n");
        // Wrapper teardown drains its own copy submission before releasing resources.
        return true;
    }();
    DestroyWindow(window);
    UnregisterClassW(className, wc.hInstance);
    std::printf("%s windowed real-frame wrapper: identity, Present/Present1, TEST, resize, teardown\n", ok ? "PASS" : "FAIL");
    return ok;
}


// K41: a live GPU stalled behind an unsignalled queue wait makes the bounded
// drain time out. Teardown must then retain every GPU object (K39), return
// within the wait bound, and leave the device healthy once the stall clears.
bool check_retention_timeout(Device& state)
{
    const wchar_t* className = L"SediciFgRetention";
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = className;
    if (!RegisterClassW(&wc)) return false;
    HWND window = CreateWindowW(className, L"Sedici retention", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 500, 320, nullptr, nullptr, wc.hInstance, nullptr);
    if (!window) { UnregisterClassW(className, wc.hInstance); return false; }
    ComPtr<ID3D12Fence> gate;
    bool ok = [&] {
        ComPtr<IDXGIFactory4> factory;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return false;
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = 320;
        desc.Height = 180;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 3;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<IDXGISwapChain1> created;
        if (FAILED(factory->CreateSwapChainForHwnd(state.queue.Get(), window, &desc,
            nullptr, nullptr, &created))) return false;
        ComPtr<IDXGISwapChain4> native;
        if (FAILED(created.As(&native))) return false;
        ComPtr<sedici::fg::Swapchain> wrapped;
        created.Reset();
        if (FAILED(sedici::fg::Swapchain::wrap(native.Get(), state.queue.Get(), &wrapped))) return false;
        native.Detach();
        if (FAILED(state.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate))) ||
            FAILED(state.queue->Wait(gate.Get(), 1))) return false;
        ComPtr<ID3D12Resource> buffer;
        if (FAILED(wrapped->GetBuffer(wrapped->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&buffer)))) return false;
        if (FAILED(state.allocator->Reset()) || FAILED(state.list->Reset(state.allocator.Get(), nullptr))) return false;
        transition(state.list.Get(), buffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
        transition(state.list.Get(), buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
        if (FAILED(state.list->Close())) return false;
        ID3D12CommandList* gated[] = {state.list.Get()};
        state.queue->ExecuteCommandLists(1, gated); // queued behind the gate; do not wait
        buffer.Reset();
        if (FAILED(wrapped->Present(0, 0))) return false;
        const unsigned retainedBefore = sedici::fg::swapchain_retention_count();
        const auto before = std::chrono::steady_clock::now();
        auto* finalReference = wrapped.Detach();
        const ULONG remaining = finalReference->Release();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - before).count();
        const bool retained = sedici::fg::swapchain_retention_count() == retainedBefore + 1;
        const bool alive = SUCCEEDED(state.device->GetDeviceRemovedReason());
        const bool bounded = elapsed >= 4500 && elapsed < 15000;
        std::printf("%s K41: a timed-out drain retains GPU state within the wait bound (%lld ms)\n",
                    remaining == 0 && retained && alive && bounded ? "PASS" : "FAIL",
                    static_cast<long long>(elapsed));

        // K47: while the GPU is still stalled the parked swapchain must stay intact,
        // however often the reaper runs; once the stall clears it must be destroyed.
        const unsigned releasedBefore = sedici::fg::swapchain_release_count();
        for (int i = 0; i < 5; ++i) sedici::fg::Swapchain::reap_retained();
        const bool heldWhileStalled = sedici::fg::Swapchain::retained_now() == 1 &&
                                      sedici::fg::swapchain_release_count() == releasedBefore;
        (void)gate->Signal(1); // End the stall so the retained work can finish.
        const auto waitStart = std::chrono::steady_clock::now();
        while (sedici::fg::Swapchain::retained_now() != 0 &&
               std::chrono::steady_clock::now() - waitStart < std::chrono::seconds(5)) {
            sedici::fg::Swapchain::reap_retained();
            Sleep(5);
        }
        const bool released = sedici::fg::Swapchain::retained_now() == 0 &&
                              sedici::fg::swapchain_release_count() == releasedBefore + 1;
        std::printf("%s K47: a parked swapchain is kept while the GPU is stalled and destroyed once its work retires\n",
                    heldWhileStalled && released ? "PASS" : "FAIL");
        const bool healthy = SUCCEEDED(state.device->GetDeviceRemovedReason());
        return remaining == 0 && retained && alive && bounded && heldWhileStalled && released && healthy &&
               g_debug_errors.load() == 0;
    }();
    // The window is left alone: the retained chain still references it.
    return ok;
}

// K77: consecutive timeouts. Two swapchains released while the same stall holds the
// GPU are both parked (each after its own bounded wait), neither is freed early, and
// both are destroyed once the stall clears.
bool check_consecutive_retention(Device& state)
{
    const wchar_t* className = L"SediciFgConsecutive";
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = className;
    if (!RegisterClassW(&wc)) return false;
    HWND windows[2] = {};
    for (auto& w : windows)
        w = CreateWindowW(className, L"Sedici consecutive", WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, 500, 320, nullptr, nullptr, wc.hInstance, nullptr);
    if (!windows[0] || !windows[1]) return false;
    ComPtr<ID3D12Fence> gate;
    bool ok = [&] {
        ComPtr<IDXGIFactory4> factory;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return false;
        if (FAILED(state.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate))) ||
            FAILED(state.queue->Wait(gate.Get(), 1))) return false;
        sedici::fg::Swapchain* chains[2] = {};
        for (int i = 0; i < 2; ++i) {
            DXGI_SWAP_CHAIN_DESC1 desc{};
            desc.Width = 320;
            desc.Height = 180;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            desc.BufferCount = 3;
            desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            ComPtr<IDXGISwapChain1> created;
            if (FAILED(factory->CreateSwapChainForHwnd(state.queue.Get(), windows[i], &desc,
                nullptr, nullptr, &created))) { (void)gate->Signal(1); return false; }
            ComPtr<IDXGISwapChain4> native;
            if (FAILED(created.As(&native))) { (void)gate->Signal(1); return false; }
            ComPtr<sedici::fg::Swapchain> wrapped;
            created.Reset();
            if (FAILED(sedici::fg::Swapchain::wrap(native.Get(), state.queue.Get(), &wrapped))) {
                (void)gate->Signal(1);
                return false;
            }
            native.Detach();
            if (FAILED(wrapped->Present(0, 0))) { (void)gate->Signal(1); return false; }
            chains[i] = wrapped.Detach();
        }
        const unsigned retainedBefore = sedici::fg::swapchain_retention_count();
        const unsigned releasedBefore = sedici::fg::swapchain_release_count();
        const ULONG first = chains[0]->Release();
        const unsigned afterFirst = sedici::fg::Swapchain::retained_now();
        const ULONG second = chains[1]->Release();
        const unsigned afterSecond = sedici::fg::Swapchain::retained_now();
        for (int i = 0; i < 3; ++i) sedici::fg::Swapchain::reap_retained();
        const bool heldTogether = sedici::fg::Swapchain::retained_now() == 2 &&
                                  sedici::fg::swapchain_release_count() == releasedBefore;
        (void)gate->Signal(1);
        const auto waitStart = std::chrono::steady_clock::now();
        while (sedici::fg::Swapchain::retained_now() != 0 &&
               std::chrono::steady_clock::now() - waitStart < std::chrono::seconds(5)) {
            sedici::fg::Swapchain::reap_retained();
            Sleep(5);
        }
        const bool released = sedici::fg::Swapchain::retained_now() == 0 &&
                              sedici::fg::swapchain_release_count() == releasedBefore + 2;
        const bool parked = first == 0 && second == 0 && afterFirst == 1 && afterSecond == 2 &&
                            sedici::fg::swapchain_retention_count() == retainedBefore + 2;
        const bool healthy = SUCCEEDED(state.device->GetDeviceRemovedReason()) && g_debug_errors.load() == 0;
        std::printf("%s K77: two consecutive timeouts are both parked, kept while stalled and destroyed once it clears\n",
                    parked && heldTogether && released && healthy ? "PASS" : "FAIL");
        return parked && heldTogether && released && healthy;
    }();
    return ok;
}

// K48: a "vrr" swapchain released while the GPU is stalled has presenter jobs
// admitted but not started. Parking cancels the presenter; the jobs that never
// started must be discarded so the swapchain can be reaped once the stall clears.
bool check_presenter_retention(Device& state)
{
    const wchar_t* className = L"SediciFgPresenterRetention";
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = className;
    if (!RegisterClassW(&wc)) return false;
    HWND window = CreateWindowW(className, L"Sedici presenter retention", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 500, 320, nullptr, nullptr, wc.hInstance, nullptr);
    if (!window) { UnregisterClassW(className, wc.hInstance); return false; }
    ComPtr<ID3D12Fence> gate;
    bool ok = [&] {
        ComPtr<IDXGIFactory5> factory;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return false;
        BOOL tearing = FALSE;
        const bool canTear = SUCCEEDED(factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
            &tearing, sizeof(tearing))) && tearing;
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = 320;
        desc.Height = 180;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 3;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
            (canTear ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u);
        ComPtr<IDXGISwapChain1> created;
        if (FAILED(factory->CreateSwapChainForHwnd(state.queue.Get(), window, &desc,
            nullptr, nullptr, &created))) return false;
        ComPtr<IDXGISwapChain4> native;
        if (FAILED(created.As(&native))) return false;
        sedici::fg::PresentChoice choice;
        choice.present = sedici::FrameGenerationSettings::Present::Vrr;
        choice.guard = sedici::FrameGenerationSettings::TearGuard::Scanline;
        choice.multiplier = sedici::FrameGenerationSettings::Multiplier::X2;
        ComPtr<sedici::fg::Swapchain> wrapped;
        created.Reset();
        if (FAILED(sedici::fg::Swapchain::wrap(native.Get(), state.queue.Get(), &wrapped, &choice, 1))) return false;
        native.Detach();
        if (!wrapped->async_present()) { std::puts("FAIL K48: presenter did not start"); return false; }
        ShowWindow(window, SW_SHOWNOACTIVATE);
        const UINT presentFlags = canTear ? DXGI_PRESENT_ALLOW_TEARING : 0u;
        if (FAILED(wrapped->Present(0, presentFlags))) return false; // warm-up, completes
        if (FAILED(state.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate))) ||
            FAILED(state.queue->Wait(gate.Get(), 1))) return false;
        // Admit as many presents as the stalled GPU lets through, from a helper
        // thread so a blocked admission cannot hang the test.
        std::atomic<int> admitted{0};
        std::atomic<bool> finished{false};
        std::thread presenting([&] {
            for (int i = 0; i < 3; ++i) {
                if (FAILED(wrapped->Present(0, presentFlags | DXGI_PRESENT_DO_NOT_WAIT))) break;
                ++admitted;
            }
            finished = true;
        });
        const auto begin = std::chrono::steady_clock::now();
        while (!finished.load() && std::chrono::steady_clock::now() - begin < std::chrono::seconds(3))
            Sleep(10);
        if (!finished.load()) { (void)gate->Signal(1); presenting.join(); return false; }
        presenting.join();
        Sleep(300); // let the presenter pick up the first job and block on the gated GPU
        const size_t outstanding = wrapped->presenter_outstanding();
        std::printf("INFO K48: %d present(s) admitted, %zu presenter job(s) outstanding at release\n",
                    admitted.load(), outstanding);
        if (outstanding < 2) { (void)gate->Signal(1); return false; } // one running, one queued
        // Cancel now, while the first job is blocked on the gated GPU, so the queued
        // job is certain never to start (parking does the same after its drain bound).
        wrapped->cancel_presenter();
        const unsigned retainedBefore = sedici::fg::swapchain_retention_count();
        const unsigned releasedBefore = sedici::fg::swapchain_release_count();
        auto* finalReference = wrapped.Detach();
        const ULONG remaining = finalReference->Release();
        const bool parked = sedici::fg::swapchain_retention_count() == retainedBefore + 1 &&
                            sedici::fg::Swapchain::retained_now() == 1;
        (void)gate->Signal(1);
        const auto waitStart = std::chrono::steady_clock::now();
        while (sedici::fg::Swapchain::retained_now() != 0 &&
               std::chrono::steady_clock::now() - waitStart < std::chrono::seconds(10)) {
            sedici::fg::Swapchain::reap_retained();
            Sleep(5);
        }
        const bool released = sedici::fg::Swapchain::retained_now() == 0 &&
                              sedici::fg::swapchain_release_count() == releasedBefore + 1;
        const bool healthy = SUCCEEDED(state.device->GetDeviceRemovedReason()) && g_debug_errors.load() == 0;
        std::printf("%s K48: a presenter with unstarted jobs is parked on release and destroyed once the stall clears\n",
                    remaining == 0 && parked && released && healthy ? "PASS" : "FAIL");
        return remaining == 0 && parked && released && healthy;
    }();
    return ok;
}

} // namespace


int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    bool keepImages = false, present = false, routing = false, deviceLoss = false, signalFault = false;
    bool signalFaultVrr = false, closeFault = false, closeFaultVrr = false;
    bool resetFault = false, resetFaultVrr = false, contextRace = false, recoveryVrr = false;
    bool paramsFaultVrr = false, sessionRecovery = false, registrationRecovery = false, retention = false;
    for (int i = 1; i < argc; ++i) {
        deviceLoss |= std::string(argv[i]) == "--device-loss";
        signalFault |= std::string(argv[i]) == "--submission-fault";
        signalFaultVrr |= std::string(argv[i]) == "--submission-fault-vrr";
        closeFault |= std::string(argv[i]) == "--recording-fault";
        closeFaultVrr |= std::string(argv[i]) == "--recording-fault-vrr";
        resetFault |= std::string(argv[i]) == "--reset-fault";
        resetFaultVrr |= std::string(argv[i]) == "--reset-fault-vrr";
        recoveryVrr |= std::string(argv[i]) == "--submission-recovery-vrr";
        paramsFaultVrr |= std::string(argv[i]) == "--params-fault-vrr";
        sessionRecovery |= std::string(argv[i]) == "--session-recovery";
        registrationRecovery |= std::string(argv[i]) == "--registration-recovery";
        contextRace |= std::string(argv[i]) == "--context-race";
        keepImages |= std::string(argv[i]) == "--images";
        present |= std::string(argv[i]) == "--present";
        routing |= std::string(argv[i]) == "--routing";
        retention |= std::string(argv[i]) == "--retention-timeout";
    }

    if (routing) {
        // Use only the offline executable's directory, never the game's config.
        // Settings are read once; remove our fixture immediately afterwards.
        const auto config = dll_directory() / L"sedici.toml";
        if (std::filesystem::exists(config)) {
            std::fprintf(stderr, "routing check refuses to overwrite existing sedici.toml\n");
            return 1;
        }
        {
            std::ofstream fixture(config);
            fixture << "[fg]\nmode = \"sedici\"\npresent = \"game\"\n[log]\nlevel = 0\n";
            if (!fixture) return 1;
        }
        (void)sedici::settings();
        std::filesystem::remove(config);
    }

    {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
        }
    }

    Device state;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
                                 IID_PPV_ARGS(&state.device)))) {
        std::fprintf(stderr, "no D3D12 device\n");
        return 1;
    }
    ComPtr<ID3D12InfoQueue1> messages;
    DWORD callbackCookie = 0;
    if (SUCCEEDED(state.device.As(&messages))) {
        const HRESULT registered = messages->RegisterMessageCallback(
            [](D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity,
               D3D12_MESSAGE_ID, LPCSTR description, void*) {
                if (severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                    ++g_debug_errors;
                    std::fputs("DX12 error: ", stderr);
                    std::fputs(description, stderr);
                    std::fputc(10, stderr);
                }
            }, D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &callbackCookie);
        g_debug_callback_installed = SUCCEEDED(registered);
    }

    D3D12_COMMAND_QUEUE_DESC queueDescription{};
    queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

    if (FAILED(state.device->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&state.queue))) ||
        FAILED(state.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                    IID_PPV_ARGS(&state.allocator))) ||
        FAILED(state.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               state.allocator.Get(), nullptr,
                                               IID_PPV_ARGS(&state.list))) ||
        FAILED(state.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&state.fence)))) {
        std::fprintf(stderr, "command objects failed\n");
        return 1;
    }
    state.list->Close();

    std::printf("display %ux%u, render %ux%u, alpha 0.5, %u iterations\n",
                kDisplayWidth, kDisplayHeight, kRenderWidth, kRenderHeight, kIterations);

    bool ok = true;
    if (retention) return (check_retention_timeout(state) && check_presenter_retention(state) &&
                       check_consecutive_retention(state)) ? 0 : 1;
    if (contextRace) return check_session_window(state, false, false, false, false, true) ? 0 : 1;
    if (sessionRecovery || registrationRecovery)
        return check_session_window(state, false, false, false, false, false, true, registrationRecovery) ? 0 : 1;
    if (signalFaultVrr || closeFaultVrr || resetFaultVrr || recoveryVrr || paramsFaultVrr) {
        ComPtr<ID3D12Device5> removable;
        if (FAILED(state.device.As(&removable)) ||
            (GetModuleHandleW(L"RTSSHooks64.dll") &&
             GetEnvironmentVariableW(L"SEDICI_FG_ALLOW_RTSS", nullptr, 0) == 0)) {
            std::puts("SKIP VRR submission fault: device removal or VRR unavailable");
            return 77;
        }
        return check_vrr_window(state, sedici::FrameGenerationSettings::Multiplier::X2,
                                "submission fault", 27, signalFaultVrr || recoveryVrr,
                                closeFaultVrr, resetFaultVrr, recoveryVrr, paramsFaultVrr) ? 0 : 1;
    }
    if (signalFault || closeFault || resetFault) {
        ComPtr<ID3D12Device5> removable;
        if (FAILED(state.device.As(&removable))) {
            std::puts("SKIP submission fault: ID3D12Device5 unavailable");
            return 77;
        }
        return check_session_window(state, false, signalFault, closeFault, resetFault) ? 0 : 1;
    }
    if (deviceLoss) {
        // Own process: removing the device ends every later check on it.
        ComPtr<ID3D12Device5> removable;
        if (FAILED(state.device.As(&removable))) {
            std::puts("SKIP device loss: ID3D12Device5 unavailable");
            return 77;
        }
        const bool lost = check_session_window(state, true);
        std::printf("%s device loss: session and wrapper tear down cleanly after RemoveDevice\n",
                    lost ? "PASS" : "FAIL");
        return lost ? 0 : 1;
    }
    for (const auto& scene : kScenes) {
        ok = run_case(state, scene, keepImages) && ok;
    }

    const int contractFailures = check_contract(state);
    if (contractFailures != 0) ok = false;
    {
        std::string why;
        auto presenter = sedici::fg::Presenter::create(
            state.device.Get(), sedici::FrameGenerationSettings::TearGuard::Off, why);
        bool rejected = false;
        if (presenter) {
            presenter->request_stop();
            sedici::fg::PresentJob job;
            rejected = presenter->submit(std::move(job)) == DXGI_ERROR_INVALID_CALL &&
                       presenter->outstanding() == 0;
        }
        std::printf("%s presenter refuses a job after stop without advancing its queue\n",
                    rejected ? "PASS" : "FAIL");
        ok = rejected && ok;
    }
    ok = check_pacing() && ok;
    if (present) ok = check_window(state) && ok;
    // Known failures, tracked in docs/refactor/reliability.md ("Current coverage
    // and gaps"). They are reported as XFAIL and do not fail the run; if one
    // starts passing it is reported as XPASS and fails the run, so the record
    // is updated instead of silently turning green.
    const auto knownFailure = [](bool passed, bool knownSignature, const char* name, const char* evidence) {
        if (passed) {
            std::printf("XPASS %s: tracked failure now passes; update reliability record", name);
            std::putchar(10);
            return false;
        }
        if (!knownSignature) {
            std::printf("FAIL %s: failure did not match the tracked signature", name);
            std::putchar(10);
            return false;
        }
        std::printf("XFAIL %s (%s)", name, evidence);
        std::putchar(10);
        return true;
    };
    if (present) {
        bool generatedKnown = false;
        const bool generatedPass = check_generated_window(state, &generatedKnown);
        ok = knownFailure(generatedPass, generatedKnown, "generated presentation",
                          "vsync path skips 3 of 5 midpoints; finding 009") && ok;
        bool sessionKnown = false;
        const bool sessionPass = check_session_window(state, false, false, false, false, false, false, false, &sessionKnown);
        ok = knownFailure(sessionPass, sessionKnown, "session",
                          "vsync path frame 2 midpoint reads back at 16 dB; finding 009") && ok;
    }
    if (routing) ok = check_routing(state) && check_routing(state, true) && ok;
    if (routing) ok = check_routing_faults(state) && ok;
    // Last: overlays that hook Present (RTSS) keep using the queue of the most
    // recent swapchain, so the present queue would leak into later checks.
    if (present) ok = check_vrr_window(state, sedici::FrameGenerationSettings::Multiplier::X2, "2x") && ok;
    if (present) ok = check_vrr_window(state, sedici::FrameGenerationSettings::Multiplier::X1_5, "1.5x") && ok;
    if (present) ok = check_vrr_window(state, sedici::FrameGenerationSettings::Multiplier::X1_5, "1.5x overload", 6) && ok;
    if (present) ok = check_vrr_window(state, sedici::FrameGenerationSettings::Multiplier::Adaptive, "adaptive at ~40 fps", 21) && ok;

    if (ComPtr<ID3D12InfoQueue> info; SUCCEEDED(state.device.As(&info))) {
        const UINT64 count = info->GetNumStoredMessages();
        if (count > 0) {
            std::printf("debug layer reported %llu message(s):\n",
                        static_cast<unsigned long long>(count));
            for (UINT64 index = 0; index < count && index < 32; ++index) {
                SIZE_T length = 0;
                if (FAILED(info->GetMessage(index, nullptr, &length)) || length == 0) continue;
                std::vector<char> buffer(length);
                auto* message = reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
                if (FAILED(info->GetMessage(index, message, &length))) continue;
                std::printf("  [%d] %.*s\n", static_cast<int>(message->Severity),
                            static_cast<int>(message->DescriptionByteLength), message->pDescription);
            }
            ok = false;
        }
    }

    return ok ? 0 : 1;
}

#pragma warning(pop)
