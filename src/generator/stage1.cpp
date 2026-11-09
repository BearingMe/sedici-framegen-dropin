#include "generator/stage1.h"

#include <d3dcompiler.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

#include "generator/shaders.h"
#include "generator/lut_data.h"
#include "diagnostics/log.h"

namespace sedici::fg {

namespace {

constexpr uint32_t kRootConstants = sizeof(Constants) / sizeof(uint32_t);
constexpr uint32_t kSrvSlots = 8;
constexpr uint32_t kUavSlots = 4;
// Blocks 0-1: prepare. Then five blocks (splat clear, splat, fill, warp,
// composite) per generated phase, so several phases can be recorded into one
// command list without overwriting each other's descriptors.
constexpr uint32_t kPrepareBlocks = 2;
constexpr uint32_t kPhaseBlocks = 5;
constexpr uint32_t kBlocks = kPrepareBlocks + kPhaseBlocks * kMaxPhases;
constexpr uint32_t kBlockSlots = kSrvSlots + kUavSlots;
constexpr uint32_t kHeapSlots = kBlocks * kBlockSlots;
constexpr uint32_t kGroupSize = 8;

uint32_t groups(uint32_t size) noexcept
{
    return (size + kGroupSize - 1) / kGroupSize;
}

std::string hresult_text(HRESULT result)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "0x%08lX", static_cast<unsigned long>(result));
    return buffer;
}

void log_failure(const char* what, HRESULT result)
{
    log_error(std::string("frame generation: ") + what + " failed (" + hresult_text(result) + ")");
}

D3D12_RESOURCE_BARRIER transition_barrier(ID3D12Resource* resource,
                                          D3D12_RESOURCE_STATES before,
                                          D3D12_RESOURCE_STATES after) noexcept
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

D3D12_RESOURCE_BARRIER uav_barrier_desc(ID3D12Resource* resource) noexcept
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resource;
    return barrier;
}

} // namespace

bool Stage1::initialize(ID3D12Device* device) noexcept try
{
    if (!device) return false;
    m_device = device;

    D3D12_ROOT_PARAMETER parameters[3]{};

    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.ShaderRegister = 0;
    parameters[0].Constants.RegisterSpace = 0;
    parameters[0].Constants.Num32BitValues = kRootConstants;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = kSrvSlots;
    srvRange.BaseShaderRegister = 0;
    srvRange.RegisterSpace = 0;
    srvRange.OffsetInDescriptorsFromTableStart = 0;

    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    parameters[1].DescriptorTable.pDescriptorRanges = &srvRange;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = kUavSlots;
    uavRange.BaseShaderRegister = 0;
    uavRange.RegisterSpace = 0;
    uavRange.OffsetInDescriptorsFromTableStart = 0;

    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[2].DescriptorTable.NumDescriptorRanges = 1;
    parameters[2].DescriptorTable.pDescriptorRanges = &uavRange;
    parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC description{};
    description.NumParameters = 3;
    description.pParameters = parameters;
    description.NumStaticSamplers = 0;
    description.pStaticSamplers = nullptr;
    description.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    Microsoft::WRL::ComPtr<ID3DBlob> serialized;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    HRESULT result = D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1,
                                                 serialized.GetAddressOf(), errors.GetAddressOf());
    if (FAILED(result)) {
        log_failure("root signature serialization", result);
        return false;
    }

    result = device->CreateRootSignature(0, serialized->GetBufferPointer(),
                                         serialized->GetBufferSize(),
                                         IID_PPV_ARGS(m_root_signature.GetAddressOf()));
    if (FAILED(result)) {
        log_failure("root signature creation", result);
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = kHeapSlots;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    result = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(m_heap.GetAddressOf()));
    if (FAILED(result)) {
        log_failure("descriptor heap creation", result);
        return false;
    }
    m_increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_HEAP_PROPERTIES upload{};
    upload.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = sizeof(kLutBits);
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    result = device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &buffer,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(m_lut.GetAddressOf()));
    if (FAILED(result)) return false;
    void* mapped = nullptr;
    D3D12_RANGE noRead{0, 0};
    if (FAILED(m_lut->Map(0, &noRead, &mapped))) return false;
    std::memcpy(mapped, kLutBits, sizeof(kLutBits));
    m_lut->Unmap(0, nullptr);

    m_pipelines_ready = compile_pipeline(shaders::kPrepare, "prepare", m_prepare.GetAddressOf()) &&
                        compile_pipeline(shaders::kClear, "clear", m_clear.GetAddressOf()) &&
                        compile_pipeline(shaders::kReproject, "reproject", m_reproject.GetAddressOf()) &&
                        compile_pipeline(shaders::kFill, "fill", m_fill.GetAddressOf()) &&
                        compile_pipeline(shaders::kWarp, "warp", m_warp.GetAddressOf()) &&
                        compile_pipeline(shaders::kComposite, "composite", m_composite.GetAddressOf());
    return m_pipelines_ready;
}
catch (...) { return false; }

bool Stage1::compile_pipeline(const char* body, const char* name,
                              ID3D12PipelineState** out) noexcept try
{
    const std::string source = std::string(shaders::kCommon) + body;

    Microsoft::WRL::ComPtr<ID3DBlob> bytecode;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    const HRESULT result = D3DCompile(source.data(), source.size(), "stage1.hlsl", nullptr, nullptr,
                                      "CSMain", "cs_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                      bytecode.GetAddressOf(), errors.GetAddressOf());
    if (FAILED(result)) {
        const char* message = errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no message";
        log_error(std::string("frame generation: shader ") + name + " failed: " + message);
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC description{};
    description.pRootSignature = m_root_signature.Get();
    description.CS.pShaderBytecode = bytecode->GetBufferPointer();
    description.CS.BytecodeLength = bytecode->GetBufferSize();

    const HRESULT created = m_device->CreateComputePipelineState(
        &description, IID_PPV_ARGS(out));
    if (FAILED(created)) {
        log_failure(name, created);
        return false;
    }
    return true;
}
catch (...) { return false; }

bool Stage1::create_target(Target& target, DXGI_FORMAT format, uint32_t width, uint32_t height,
                           D3D12_RESOURCE_FLAGS flags) noexcept
{
    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;

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

    const HRESULT result = m_device->CreateCommittedResource(
        &heapProperties, D3D12_HEAP_FLAG_NONE, &description,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(target.resource.GetAddressOf()));
    if (FAILED(result)) {
        log_failure("frame generation target creation", result);
        return false;
    }
    target.state = D3D12_RESOURCE_STATE_COMMON;
    return true;
}

void Stage1::release_targets() noexcept
{
    for (auto& target : m_depth) target = Target{};
    for (auto& target : m_motion) target = Target{};
    for (auto& target : m_color) target = Target{};
    m_reprojection = Target{};
    m_filled = Target{};
    m_configured = false;
    m_history_valid = false;
    m_prepared = false;
}

bool Stage1::create_targets() noexcept
{
    release_targets();

    const auto uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const auto readOnly = D3D12_RESOURCE_FLAG_NONE;

    for (int index = 0; index < 2; ++index) {
        if (!create_target(m_depth[index], DXGI_FORMAT_R32_FLOAT, m_params.renderWidth,
                           m_params.renderHeight, uav)) return false;
        if (!create_target(m_motion[index], DXGI_FORMAT_R16G16_FLOAT, m_params.renderWidth,
                           m_params.renderHeight, uav)) return false;
        if (!create_target(m_color[index], DXGI_FORMAT_R8G8B8A8_UNORM, m_params.displayWidth,
                           m_params.displayHeight, readOnly)) return false;
    }

    if (!create_target(m_reprojection, DXGI_FORMAT_R32_UINT, m_params.renderWidth,
                       m_params.renderHeight, uav)) return false;
    if (!create_target(m_filled, DXGI_FORMAT_R32_UINT, m_params.renderWidth,
                       m_params.renderHeight, uav)) return false;

    // The warp writes the midpoint here and the composite pass finishes it, so
    // HUD composition never has to run inside the fusion pass.
    if (!create_target(m_midpoint, DXGI_FORMAT_R8G8B8A8_UNORM, m_params.displayWidth,
                       m_params.displayHeight, uav)) return false;

    return true;
}

bool Stage1::configure(const FrameParams& params) noexcept
{
    if (!ready()) return false;
    if (m_prepared) return false; // Do not replace targets for an unconsumed snapshot.
    if (!valid(params)) {
        log_error("frame generation: invalid frame parameters");
        return false;
    }

    const bool resize = !m_configured ||
                        params.renderWidth != m_params.renderWidth ||
                        params.renderHeight != m_params.renderHeight ||
                        params.displayWidth != m_params.displayWidth ||
                        params.displayHeight != m_params.displayHeight;

    m_params = params;
    if (resize) {
        if (!create_targets()) return false;
        m_current = 0;
    }
    m_configured = true;
    return true;
}

void Stage1::transition(ID3D12GraphicsCommandList* list, Target& target,
                        D3D12_RESOURCE_STATES after) noexcept
{
    if (!target.get() || target.state == after) return;
    const auto barrier = transition_barrier(target.get(), target.state, after);
    list->ResourceBarrier(1, &barrier);
    target.state = after;
}

void Stage1::transition_external(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                                 D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) noexcept
{
    if (!resource || before == after) return;
    const auto barrier = transition_barrier(resource, before, after);
    list->ResourceBarrier(1, &barrier);
}

void Stage1::uav_barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource) noexcept
{
    const auto barrier = uav_barrier_desc(resource);
    list->ResourceBarrier(1, &barrier);
}

void Stage1::begin_block(uint32_t index) noexcept
{
    m_block = index;
}

void Stage1::write_srv(uint32_t slot, ID3D12Resource* resource, DXGI_FORMAT format) noexcept
{
    D3D12_SHADER_RESOURCE_VIEW_DESC description{};
    description.Format = format;
    description.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    description.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    description.Texture2D.MipLevels = 1;

    const auto handle = m_heap->GetCPUDescriptorHandleForHeapStart();
    const SIZE_T index = static_cast<SIZE_T>(m_block) * kBlockSlots + slot;
    m_device->CreateShaderResourceView(resource, &description,
                                       {handle.ptr + index * m_increment});
}

void Stage1::write_uav(uint32_t slot, ID3D12Resource* resource, DXGI_FORMAT format) noexcept
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC description{};
    description.Format = format;
    description.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

    const auto base = m_heap->GetCPUDescriptorHandleForHeapStart();
    const SIZE_T index = static_cast<SIZE_T>(m_block) * kBlockSlots + kSrvSlots + slot;
    m_device->CreateUnorderedAccessView(resource, nullptr, &description,
                                        {base.ptr + index * m_increment});
}

void Stage1::copy_target(ID3D12GraphicsCommandList* list, Target& source,
                         Target& destination) noexcept
{
    transition(list, source, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(list, destination, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyResource(destination.get(), source.get());
    transition(list, destination, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(list, source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

namespace {
std::atomic<PassMarkFn> g_pass_mark{nullptr};
std::atomic<void*> g_pass_user{nullptr};
} // namespace

void stage1_set_pass_mark(PassMarkFn fn, void* user) noexcept
{
    g_pass_user.store(user, std::memory_order_relaxed);
    g_pass_mark.store(fn, std::memory_order_release);
}

bool Stage1::dispatch(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pipeline,
                      uint32_t groupX, uint32_t groupY) noexcept
{
    if (!pipeline) return false;

    FrameParams params = m_params;
    params.alpha = m_alpha;
    const Constants constants = make_constants(params);

    ID3D12DescriptorHeap* heaps[] = {m_heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(m_root_signature.Get());
    list->SetPipelineState(pipeline);

    D3D12_GPU_DESCRIPTOR_HANDLE srv = m_heap->GetGPUDescriptorHandleForHeapStart();
    srv.ptr += static_cast<UINT64>(m_block) * kBlockSlots * m_increment;
    D3D12_GPU_DESCRIPTOR_HANDLE uav = srv;
    uav.ptr += static_cast<UINT64>(kSrvSlots) * m_increment;

    Constants withWeight = constants;
    withWeight.padding[0] = m_composite_weight;
    list->SetComputeRoot32BitConstants(0, kRootConstants, &withWeight, 0);
    list->SetComputeRootDescriptorTable(1, srv);
    list->SetComputeRootDescriptorTable(2, uav);
    list->Dispatch(groupX, groupY, 1);
    if (const PassMarkFn mark = g_pass_mark.load(std::memory_order_acquire)) {
        const char* name = pipeline == m_prepare.Get() ? "prepare"
            : pipeline == m_clear.Get() ? "clear" : pipeline == m_reproject.Get() ? "reproject"
            : pipeline == m_fill.Get() ? "fill" : pipeline == m_warp.Get() ? "warp"
            : pipeline == m_composite.Get() ? "composite" : "pass";
        mark(g_pass_user.load(std::memory_order_relaxed), list, name);
    }
    return true;
}

void Stage1::discard_prepare() noexcept
{
    m_prepared = false;
    m_history_valid = false;
}

bool Stage1::capture_prepare(ID3D12GraphicsCommandList* list, const InputFrame& frame) noexcept
{
    if (!m_configured || !m_pipelines_ready || !list) return false;
    if (!frame.depth || !frame.motion || m_prepared) return false;
    for (auto* resource : {frame.depth, frame.motion}) {
        const auto d = resource->GetDesc();
        if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            d.Width != m_params.renderWidth || d.Height != m_params.renderHeight ||
            d.MipLevels != 1 || d.DepthOrArraySize != 1 || d.SampleDesc.Count != 1) return false;
    }
    const uint32_t current = m_current;
    transition_external(list, frame.depth, frame.depthState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition_external(list, frame.motion, frame.motionState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(list, m_depth[current], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(list, m_motion[current], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    begin_block(0);
    write_srv(0, frame.depth, frame.depthFormat);
    write_srv(1, frame.motion, frame.motionFormat);
    write_uav(0, m_depth[current].get(), DXGI_FORMAT_R32_FLOAT);
    write_uav(1, m_motion[current].get(), DXGI_FORMAT_R16G16_FLOAT);
    dispatch(list, m_prepare.Get(), groups(m_params.renderWidth), groups(m_params.renderHeight));
    uav_barrier(list, m_depth[current].get());
    uav_barrier(list, m_motion[current].get());
    transition(list, m_depth[current], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(list, m_motion[current], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition_external(list, frame.depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, frame.depthState);
    transition_external(list, frame.motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, frame.motionState);
    m_prepared = true;
    return true;
}

bool Stage1::generate(ID3D12GraphicsCommandList* list, const InputFrame& frame,
                      ID3D12Resource* output, bool resetHistory,
                      D3D12_RESOURCE_STATES outputState, const HudSource* hud) noexcept
{
    const GeneratePhase phase{output, outputState, m_params.alpha};
    return generate(list, frame, &phase, 1, resetHistory, hud);
}

bool Stage1::generate(ID3D12GraphicsCommandList* list, const InputFrame& frame,
                      const GeneratePhase* phases, uint32_t count, bool resetHistory,
                      const HudSource* hud) noexcept
{
    if (!m_configured || !m_pipelines_ready || !list || !frame.color) return false;
    if (!phases || count == 0 || count > kMaxPhases) return false;
    const auto matches = [](ID3D12Resource* resource, uint32_t width, uint32_t height) {
        const auto desc = resource->GetDesc();
        return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
               desc.Width == width && desc.Height == height &&
               desc.DepthOrArraySize == 1 && desc.MipLevels == 1 && desc.SampleDesc.Count == 1;
    };
    if (!matches(frame.color, m_params.displayWidth, m_params.displayHeight)) return false;
    if (frame.color->GetDesc().Format != DXGI_FORMAT_R8G8B8A8_UNORM) return false;
    for (uint32_t i = 0; i < count; ++i) {
        ID3D12Resource* output = phases[i].output;
        if (!output || !matches(output, m_params.displayWidth, m_params.displayHeight)) return false;
        if (output->GetDesc().Format != DXGI_FORMAT_R8G8B8A8_UNORM ||
            !(output->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) return false;
        if (output == frame.color || output == frame.depth || output == frame.motion) return false;
        if (hud && hud->real == output) return false;
        if (!(phases[i].alpha > 0.0f && phases[i].alpha < 1.0f)) return false;
        for (uint32_t j = 0; j < i; ++j) {
            if (phases[j].output == output) return false;
        }
    }
    if (hud && hud->real) {
        const auto desc = hud->real->GetDesc();
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            desc.Width != m_params.displayWidth || desc.Height != m_params.displayHeight ||
            desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM || desc.MipLevels != 1 ||
            desc.DepthOrArraySize != 1 || desc.SampleDesc.Count != 1) return false;
    }
    if (!m_prepared && !capture_prepare(list, frame)) return false;

    const uint32_t current = m_current;
    const uint32_t previous = current ^ 1u;

    // 1. Keep a private copy of the HUD-less color: the game's resource is
    //    recycled as soon as the frame has been consumed.
    transition_external(list, frame.color, frame.colorState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(list, m_color[current], D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyResource(m_color[current].get(), frame.color);
    transition(list, m_color[current], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition_external(list, frame.color, D3D12_RESOURCE_STATE_COPY_SOURCE, frame.colorState);

    if (resetHistory || !m_history_valid) {
        copy_target(list, m_depth[current], m_depth[previous]);
        copy_target(list, m_motion[current], m_motion[previous]);
        copy_target(list, m_color[current], m_color[previous]);
        // There is no temporal pair yet. Seed history and emit the real frame;
        // warping two copies of it would manufacture motion on a reset. With a
        // HUD source the game's own frame is the exact frame to show, so the
        // seed is not HUD-less.
        transition(list, m_color[current], D3D12_RESOURCE_STATE_COPY_SOURCE);
        for (uint32_t i = 0; i < count; ++i) {
            ID3D12Resource* output = phases[i].output;
            transition_external(list, output, phases[i].outputState, D3D12_RESOURCE_STATE_COPY_DEST);
            if (hud && hud->real) {
                transition_external(list, hud->real, hud->realState, D3D12_RESOURCE_STATE_COPY_SOURCE);
                list->CopyResource(output, hud->real);
                transition_external(list, hud->real, D3D12_RESOURCE_STATE_COPY_SOURCE, hud->realState);
            } else {
                list->CopyResource(output, m_color[current].get());
            }
            transition_external(list, output, D3D12_RESOURCE_STATE_COPY_DEST, phases[i].outputState);
        }
        transition(list, m_color[current], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        m_current = previous;
        m_history_valid = true;
        m_prepared = false;
        return true;
    }

    // 2. One synthesis per phase. Each owns its descriptor blocks and its alpha
    //    in the root constants; the scratch targets are reused in order, which
    //    the transitions inside serialise on the GPU.
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t block = kPrepareBlocks + kPhaseBlocks * i;
        synthesize(list, current, previous, phases[i].alpha, block);
        composite_hud(list, current, phases[i], hud, block + 4);
    }
    m_alpha = m_params.alpha;
    m_current = previous;
    m_history_valid = true;
    m_prepared = false;
    return true;
}

// Produce one HUD-less analytical phase in m_midpoint. HUD composition is a
// separate recording step so a future correction stage can run at this boundary.
void Stage1::synthesize(ID3D12GraphicsCommandList* list, uint32_t current, uint32_t previous,
                        float alpha, uint32_t block) noexcept
{
    m_alpha = alpha;

    // 3. Clear, splat, fill.
    transition(list, m_reprojection, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    begin_block(block);
    write_uav(0, m_reprojection.get(), DXGI_FORMAT_R32_UINT);
    dispatch(list, m_clear.Get(), groups(m_params.renderWidth), groups(m_params.renderHeight));
    uav_barrier(list, m_reprojection.get());

    begin_block(block + 1);
    write_srv(0, m_depth[current].get(), DXGI_FORMAT_R32_FLOAT);
    write_srv(1, m_motion[current].get(), DXGI_FORMAT_R16G16_FLOAT);
    write_srv(2, m_motion[previous].get(), DXGI_FORMAT_R16G16_FLOAT);
    write_uav(0, m_reprojection.get(), DXGI_FORMAT_R32_UINT);
    dispatch(list, m_reproject.Get(), groups(m_params.renderWidth), groups(m_params.renderHeight));
    uav_barrier(list, m_reprojection.get());
    transition(list, m_reprojection, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    transition(list, m_filled, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    begin_block(block + 2);
    write_srv(0, m_reprojection.get(), DXGI_FORMAT_R32_UINT);
    write_uav(0, m_filled.get(), DXGI_FORMAT_R32_UINT);
    dispatch(list, m_fill.Get(), groups(m_params.renderWidth), groups(m_params.renderHeight));
    uav_barrier(list, m_filled.get());
    transition(list, m_filled, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // 4. Warp both real frames to the midpoint and fuse into a private target.
    transition(list, m_midpoint, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    begin_block(block + 3);
    D3D12_SHADER_RESOURCE_VIEW_DESC lutView{};
    lutView.Format = DXGI_FORMAT_R32_FLOAT;
    lutView.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    lutView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    lutView.Buffer.NumElements = 128 * 128;
    const auto lutHandle = m_heap->GetCPUDescriptorHandleForHeapStart();
    m_device->CreateShaderResourceView(m_lut.Get(), &lutView,
        {lutHandle.ptr + static_cast<SIZE_T>((block + 3) * kBlockSlots + 7) * m_increment});
    write_srv(0, m_filled.get(), DXGI_FORMAT_R32_UINT);
    write_srv(1, m_color[current].get(), DXGI_FORMAT_R8G8B8A8_UNORM);
    write_srv(2, m_color[previous].get(), DXGI_FORMAT_R8G8B8A8_UNORM);
    write_srv(3, m_depth[current].get(), DXGI_FORMAT_R32_FLOAT);
    write_srv(4, m_depth[previous].get(), DXGI_FORMAT_R32_FLOAT);
    write_srv(5, m_motion[current].get(), DXGI_FORMAT_R16G16_FLOAT);
    write_srv(6, m_motion[previous].get(), DXGI_FORMAT_R16G16_FLOAT);
    write_uav(0, m_midpoint.get(), DXGI_FORMAT_R8G8B8A8_UNORM);
    dispatch(list, m_warp.Get(), groups(m_params.displayWidth), groups(m_params.displayHeight));

    uav_barrier(list, m_midpoint.get());
    transition(list, m_midpoint, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

}

// Compose only after analytical generation. Without a HUD source this is a copy.
void Stage1::composite_hud(ID3D12GraphicsCommandList* list, uint32_t current,
                           const GeneratePhase& phase, const HudSource* hud, uint32_t block) noexcept
{
    ID3D12Resource* const output = phase.output;
    const D3D12_RESOURCE_STATES outputState = phase.outputState;
    const bool hasHud = hud && hud->real;
    m_composite_weight = hasHud ? 1.0f : 0.0f;
    if (hasHud) {
        transition_external(list, hud->real, hud->realState,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    transition_external(list, output, outputState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    begin_block(block);
    write_srv(0, m_midpoint.get(), DXGI_FORMAT_R8G8B8A8_UNORM);
    write_srv(1, hasHud ? hud->real : m_midpoint.get(), DXGI_FORMAT_R8G8B8A8_UNORM);
    write_srv(2, m_color[current].get(), DXGI_FORMAT_R8G8B8A8_UNORM);
    write_uav(0, output, DXGI_FORMAT_R8G8B8A8_UNORM);
    dispatch(list, m_composite.Get(), groups(m_params.displayWidth), groups(m_params.displayHeight));
    transition_external(list, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, outputState);
    if (hasHud) {
        transition_external(list, hud->real, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                            hud->realState);
    }
    m_composite_weight = 0.0f;
}

} // namespace sedici::fg
