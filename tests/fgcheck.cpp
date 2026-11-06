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
#include <cstring>
#include <string>
#include <vector>

#include <DirectXPackedVector.h>

#include "fg_context.h"
#include "fg_params.h"
#include "fg_stage1.h"
#include "fg_swapchain.h"

#include "ffx_api_framegeneration_dx12.h"
#include "ffx_framegeneration.h"

using Microsoft::WRL::ComPtr;

#pragma warning(push)
#pragma warning(disable : 4996) // Exercise the desc Cyberpunk actually uses.

namespace {

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
                                      D3D12_RESOURCE_FLAGS flags)
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
                                               D3D12_RESOURCE_STATE_COMMON, nullptr,
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
        frames[index].depth = create_texture(state.device.Get(), DXGI_FORMAT_R32_FLOAT,
                                             kRenderWidth, kRenderHeight,
                                             D3D12_RESOURCE_FLAG_NONE);
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

    const bool generated = context.generate(state.list.Get(), output.Get(),
                                            D3D12_RESOURCE_STATE_COMMON);
    expect(generated, "generate produces a frame");
    expect(state.submit() && context.track_submission(state.fence.Get(), state.fenceValue),
           "the frame command list executes and registers retirement");
    expect(!context.generate(state.list.Get(), output.Get(), D3D12_RESOURCE_STATE_COMMON),
           "a second generate without a new prepare is refused");

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

} // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    bool keepImages = false, present = false;
    for (int i = 1; i < argc; ++i) {
        keepImages |= std::string(argv[i]) == "--images";
        present |= std::string(argv[i]) == "--present";
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
        messages->RegisterMessageCallback([](D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity,
            D3D12_MESSAGE_ID, LPCSTR description, void*) {
                if (severity <= D3D12_MESSAGE_SEVERITY_ERROR) std::fprintf(stderr, "DX12: %s\n", description);
            }, D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &callbackCookie);
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
    for (const auto& scene : kScenes) {
        ok = run_case(state, scene, keepImages) && ok;
    }

    const int contractFailures = check_contract(state);
    if (contractFailures != 0) ok = false;
    if (present) ok = check_window(state) && ok;

    if (ComPtr<ID3D12InfoQueue> info; SUCCEEDED(state.device.As(&info))) {
        const UINT64 count = info->GetNumStoredMessages();
        if (count > 0) {
            std::printf("debug layer reported %llu message(s):\n",
                        static_cast<unsigned long long>(count));
            for (UINT64 index = 0; index < count && index < 8; ++index) {
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
