#pragma once

// Shared pieces of the offline frame generation acceptance host (P4.5: split from the
// single fgcheck.cpp). The check groups live in fgcheck_quality.cpp, fgcheck_pacing.cpp,
// fgcheck_present.cpp, fgcheck_routing.cpp and fgcheck_retention.cpp; main() is in fgcheck.cpp.

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

#include "runtime/context.h"
#include "settings/config.h"
#include "status/diag.h"
#include "runtime/integration.h"
#include "presentation/pacing.h"
#include "generator/params.h"
#include "abi/route.h"
#include "diagnostics/bookkeeping_faults.h"
#include "generator/stage1.h"
#include "presentation/swapchain.h"
#include "settings/live.h"
#include "diagnostics/log.h"

#include "ffx_api_framegeneration_dx12.h"
#include "ffx_framegeneration.h"

// forward.cpp reports the marker; the DLL defines it in dllmain.cpp, hosts here.

using Microsoft::WRL::ComPtr;

#pragma warning(push)
#pragma warning(disable : 4996) // Exercise the desc Cyberpunk actually uses.


namespace fgcheck {

inline std::atomic<unsigned> g_debug_errors{0};
inline bool g_debug_callback_installed = false;

// Test-sized by default; `--resolution WxH` runs the quality scenes at a production
// size (render size = display size, as FSR 3.1 frame generation is driven).
inline uint32_t kDisplayWidth = 480;
inline uint32_t kDisplayHeight = 270;
inline uint32_t kRenderWidth = 320;
inline uint32_t kRenderHeight = 180;
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
    // K120: a quarter of the screen per frame. The bar uncovers a large area of
    // background between the two real frames, which is the disocclusion the owner
    // saw at high speed. The score is recorded; there is no threshold to meet.
    {"fast bar", 0.25f, 0.0f, 0.12f, 0.20f, 0.50f},
};

struct Image {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<float> color;  // rgba
    std::vector<float> depth;  // one channel, reversed-Z (near = 1)
    std::vector<float> motion; // two channels, pixels
};

inline float smooth_step(float edge0, float edge1, float value)
{
    const float t = std::clamp((value - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Renders the scene at time `t` in [0, 1] (0 = previous frame, 1 = current).
inline Image make_scene(const SceneConfig& config, float t, uint32_t width, uint32_t height)
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

inline uint32_t align_to(uint32_t value, uint32_t alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

inline std::vector<uint8_t> quantise(const std::vector<float>& values)
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

inline std::vector<uint16_t> pack_ffx_motion(const std::vector<float>& values)
{
    std::vector<uint16_t> result(values.size() * 2, 0);
    for (size_t i = 0; i < values.size() / 2; ++i) {
        // Cyberpunk-shaped RGBA16F: FFX current->previous UV vectors.
        result[i * 4] = DirectX::PackedVector::XMConvertFloatToHalf(-values[i * 2] / kRenderWidth);
        result[i * 4 + 1] = DirectX::PackedVector::XMConvertFloatToHalf(-values[i * 2 + 1] / kRenderHeight);
    }
    return result;
}

inline ComPtr<ID3D12Resource> create_buffer(ID3D12Device* device, uint64_t size,
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

inline ComPtr<ID3D12Resource> create_texture(ID3D12Device* device, DXGI_FORMAT format,
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

inline void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
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

inline void upload(ID3D12GraphicsCommandList* list, ID3D12Resource* texture, ID3D12Resource* staging,
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

inline double psnr(const std::vector<uint8_t>& reference, const std::vector<uint8_t>& candidate)
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

inline double ssim(const std::vector<uint8_t>& reference, const std::vector<uint8_t>& candidate,
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

inline bool save_ppm(const char* path, const std::vector<uint8_t>& pixels, uint32_t width, uint32_t height)
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
inline std::vector<uint8_t> read_presented(Device& state, ID3D12Resource* texture)
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

// P4.4 (K117): per-pass GPU time. With `--profile` a timestamp is written after each
// Stage 1 pass and the deltas are printed beside the total.
inline bool g_profile = false;
struct PassProfile {
    ID3D12QueryHeap* heap = nullptr;
    static constexpr uint32_t kMaxMarks = 32;
    uint32_t next = 2; // 0 and 1 are the frame start and end
    std::vector<const char*> names;
};
inline void pass_mark(void* user, ID3D12GraphicsCommandList* list, const char* pass)
{
    auto* profile = static_cast<PassProfile*>(user);
    if (!profile->heap || profile->next >= 2 + PassProfile::kMaxMarks) return;
    list->EndQuery(profile->heap, D3D12_QUERY_TYPE_TIMESTAMP, profile->next++);
    profile->names.push_back(pass);
}


struct TestGenerator {
    uint32_t calls = 0;
    IDXGISwapChain4* native = nullptr;
    bool before_present = true;
};

// Stand-in for the real generator. Presentation is what is under test here, so
// the sink only has to be a well-formed, empty recording; the real host records
// fg::Context into the same output resource.
inline bool record_solid(void* user, ID3D12GraphicsCommandList* list, ID3D12Resource* real,
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

inline bool record_counted(void* user, ID3D12GraphicsCommandList* list, ID3D12Resource* real,
                    ID3D12Resource* output, D3D12_RESOURCE_STATES)
{
    auto* calls = static_cast<uint32_t*>(user);
    if (!calls || !list || !output || !real) return false;
    ++*calls;
    return true;
}

inline ID3D12Fence* recoveryTestGate = nullptr;
inline std::atomic<bool> recoveryTestReleased{false};
inline void release_recovery_test_gate() noexcept
{
    if (recoveryTestGate && SUCCEEDED(recoveryTestGate->Signal(1)))
        recoveryTestReleased = true;
}


// Entry points of the check groups.
bool run_case(Device& state, const SceneConfig& config, bool keepImages);
int check_contract(Device& state);
bool check_ten_bit(Device& state);
bool check_pacing();
bool check_vrr_window(Device& state, sedici::FrameGenerationSettings::Multiplier multiplier,
                      const char* label, DWORD frameMs = 27, bool signalFault = false,
                      bool closeFault = false, bool resetFault = false, bool liveRecovery = false,
                      bool paramsFault = false);
bool check_generated_window(Device& state, bool* knownSignature = nullptr);
bool check_session_window(Device& state, bool loseDevice = false, bool signalFault = false,
                          bool closeFault = false, bool resetFault = false,
                          bool contextRace = false, bool sessionRecovery = false,
                          bool registrationFault = false, bool* knownSignature = nullptr);
bool check_routing(Device& state, bool wrapFirst = false);
bool check_routing_faults(Device& state);
bool check_window(Device& state);
bool check_retention_timeout(Device& state);
bool check_consecutive_retention(Device& state);
bool check_presenter_retention(Device& state);

} // namespace fgcheck
