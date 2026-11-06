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

#include "ffx_api_framegeneration_dx12.h"
#include "ffx_framegeneration.h"

using Microsoft::WRL::ComPtr;

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

std::vector<uint16_t> pack_half2(const std::vector<float>& values)
{
    std::vector<uint16_t> result(values.size());
    for (size_t i = 0; i < values.size(); ++i) {
        result[i] = DirectX::PackedVector::XMConvertFloatToHalf(values[i]);
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
        queue->Signal(fence.Get(), ++fenceValue);
        while (fence->GetCompletedValue() < fenceValue) {
            Sleep(0);
        }
        return true;
    }

    bool submit()
    {
        list->Close();
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
    const std::vector<uint16_t> previousMotion = pack_half2(previousRender.motion);
    const std::vector<uint16_t> currentMotion = pack_half2(currentRender.motion);

    FrameTextures frames[2];
    for (int index = 0; index < 2; ++index) {
        frames[index].color = create_texture(state.device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM,
                                             kDisplayWidth, kDisplayHeight,
                                             D3D12_RESOURCE_FLAG_NONE);
        frames[index].depth = create_texture(state.device.Get(), DXGI_FORMAT_R32_FLOAT,
                                             kRenderWidth, kRenderHeight,
                                             D3D12_RESOURCE_FLAG_NONE);
        frames[index].motion = create_texture(state.device.Get(), DXGI_FORMAT_R16G16_FLOAT,
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

    for (int index = 0; index < 2; ++index) {
        const auto& color = index == 0 ? previousColor : currentColor;
        const auto& motion = index == 0 ? previousMotion : currentMotion;
        const auto& render = index == 0 ? previousRender : currentRender;

        auto colorStaging = create_buffer(state.device.Get(), colorBytes, D3D12_HEAP_TYPE_UPLOAD,
                                          D3D12_RESOURCE_STATE_GENERIC_READ);
        auto depthStaging = create_buffer(state.device.Get(), renderBytes, D3D12_HEAP_TYPE_UPLOAD,
                                          D3D12_RESOURCE_STATE_GENERIC_READ);
        auto motionStaging = create_buffer(state.device.Get(), renderBytes, D3D12_HEAP_TYPE_UPLOAD,
                                           D3D12_RESOURCE_STATE_GENERIC_READ);

        state.allocator->Reset();
        state.list->Reset(state.allocator.Get(), nullptr);
        upload(state.list.Get(), frames[index].color.Get(), colorStaging.Get(), color.data(),
               kDisplayWidth, kDisplayHeight, 4);
        upload(state.list.Get(), frames[index].depth.Get(), depthStaging.Get(), render.depth.data(),
               kRenderWidth, kRenderHeight, 4);
        upload(state.list.Get(), frames[index].motion.Get(), motionStaging.Get(), motion.data(),
               kRenderWidth, kRenderHeight, 4);
        if (!state.submit()) {
            std::fprintf(stderr, "upload failed\n");
            return false;
        }
    }

    sedici::fg::Stage1 stage;
    if (!stage.initialize(state.device.Get())) {
        std::fprintf(stderr, "stage 1 initialization failed\n");
        return false;
    }

    sedici::fg::FrameParams params{};
    params.renderWidth = kRenderWidth;
    params.renderHeight = kRenderHeight;
    params.displayWidth = kDisplayWidth;
    params.displayHeight = kDisplayHeight;
    params.depthInverted = true;
    params.motionVectorScaleX = static_cast<float>(kRenderWidth);
    params.motionVectorScaleY = static_cast<float>(kRenderHeight);
    if (!stage.configure(params)) {
        std::fprintf(stderr, "stage 1 configuration failed\n");
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
        const bool first = iteration == 0;
        const uint32_t source = iteration % 2;

        sedici::fg::InputFrame frame{};
        frame.color = frames[source].color.Get();
        frame.depth = frames[source].depth.Get();
        frame.motion = frames[source].motion.Get();
        frame.colorState = D3D12_RESOURCE_STATE_COMMON;
        frame.depthState = D3D12_RESOURCE_STATE_COMMON;
        frame.motionState = D3D12_RESOURCE_STATE_COMMON;
        frame.depthFormat = DXGI_FORMAT_R32_FLOAT;
        frame.motionFormat = DXGI_FORMAT_R16G16_FLOAT;

        state.allocator->Reset();
        state.list->Reset(state.allocator.Get(), nullptr);
        state.list->EndQuery(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        if (!stage.generate(state.list.Get(), frame, output.Get(), first, outputState)) {
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

        void* mapped = nullptr;
        if (SUCCEEDED(timings->Map(0, nullptr, &mapped))) {
            const auto* values = static_cast<const uint64_t*>(mapped);
            if (iteration > 0 && values[1] > values[0]) {
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
    configure.HUDLessColor.state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    configure.frameID = 7;
    expect(context.configure(&configure.header), "configure accepts the FrameGenerationConfig desc");

    ffxDispatchDescFrameGenerationPrepare prepare{};
    prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
    prepare.renderSize = {32, 18};
    prepare.motionVectorScale = {32.0f, 18.0f};
    prepare.depth.resource = depth.Get();
    prepare.depth.state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    prepare.motionVectors.resource = motion.Get();
    prepare.motionVectors.state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    expect(context.prepare(&prepare.header), "prepare accepts the legacy prepare desc");

    state.allocator->Reset();
    state.list->Reset(state.allocator.Get(), nullptr);
    const bool generated = context.generate(state.list.Get(), output.Get(),
                                            D3D12_RESOURCE_STATE_COMMON);
    expect(generated, "generate produces a frame");
    expect(state.submit(), "the frame command list executes");
    expect(!context.generate(state.list.Get(), output.Get(), D3D12_RESOURCE_STATE_COMMON),
           "a second generate without a new prepare is refused");

    prepare.motionVectors.resource = color.Get();
    expect(!context.prepare(&prepare.header), "a non-float motion vector resource is refused");
    prepare.motionVectors.resource = motion.Get();

    configure.frameGenerationEnabled = false;
    expect(context.configure(&configure.header), "configure accepts frame generation off");
    expect(!context.generate(state.list.Get(), output.Get(), D3D12_RESOURCE_STATE_COMMON),
           "generate is refused while frame generation is off");

    context.destroy();
    return failures;
}

} // namespace

int main(int argc, char** argv)
{
    const bool keepImages = argc > 1 && std::string(argv[1]) == "--images";

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
