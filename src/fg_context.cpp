#include "fg_context.h"

#include "ffx_api_framegeneration_dx12.h"
#include "ffx_framegeneration.h"
#include "fg_bookkeeping_faults.h"
#include "diagnostics/log.h"
#include "overlay_compose.h"

#include <cmath>
#include <string>

#pragma warning(push)
#pragma warning(disable : 4996) // Cyberpunk uses the legacy prepare contract.

namespace sedici::fg {

namespace {

constexpr size_t kMaxChainItems = 64;

template <typename T>
const T* find_desc(const ffxApiHeader* desc, uint64_t type) noexcept
{
    size_t visited = 0;
    for (const ffxApiHeader* item = desc; item && visited < kMaxChainItems;
         item = item->pNext, ++visited) {
        if (item->type == type) return reinterpret_cast<const T*>(item);
    }
    return nullptr;
}

D3D12_RESOURCE_STATES as_state(uint32_t value) noexcept
{
    return static_cast<D3D12_RESOURCE_STATES>(sedici::dx12_state_from_ffx(value));
}

DXGI_FORMAT format_of(ID3D12Resource* resource) noexcept
{
    return resource ? resource->GetDesc().Format : DXGI_FORMAT_UNKNOWN;
}

DXGI_FORMAT depth_srv_format(DXGI_FORMAT format) noexcept
{
    switch (format) {
    // FFX reports D32_FLOAT as R32_FLOAT. The native resource format is not
    // itself an SRV format: depth and typeless allocations need a typed view.
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT:
        return DXGI_FORMAT_R32_FLOAT;
    // Cyberpunk's native depth/stencil allocation has two planes. Sample only
    // plane 0 as float depth; the stencil plane is not an input to generation.
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R16_FLOAT:
        return DXGI_FORMAT_R16_FLOAT;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

bool is_supported_motion(DXGI_FORMAT format) noexcept
{
    switch (format) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R32G32_FLOAT:
        return true;
    default:
        return false;
    }
}

} // namespace

bool Context::fail(const char* reason) noexcept
{
    m_error = reason;
    try { log_event(2, std::string("fg: rejected: ") + reason); } catch (...) {}
    return false;
}

bool Context::retired() const noexcept
{
    // A removed device executes nothing more, so work recorded or submitted on it
    // can never be "in flight": without this, destroy was refused forever after
    // device loss (K38).
    if (m_device && FAILED(m_device->GetDeviceRemovedReason())) return true;
    if (retirement_held()) return false; // offline seam, never set at runtime
    if (m_recorded) return false;
    if (!m_retirement_fence) return true;
    const uint64_t completed = m_retirement_fence->GetCompletedValue();
    return completed != UINT64_MAX && completed >= m_retirement_value;
}

bool Context::track_submission(ID3D12Fence* fence, uint64_t value) noexcept
{
    if (!m_recorded || !fence || value == 0 || value == UINT64_MAX)
        return fail("invalid submission retirement token");
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if (FAILED(fence->GetDevice(IID_PPV_ARGS(&device))) || device.Get() != m_device)
        return fail("submission fence belongs to another device");
    if (fence == m_retirement_fence.Get() && value <= m_retirement_value)
        return fail("submission fence timeline did not advance");
    m_retirement_fence = fence;
    m_retirement_value = value;
    m_recorded = false;
    return true;
}

bool Context::wait_prepare(ID3D12CommandQueue* queue) noexcept
{
    if (!queue || !m_snapshot || m_recorded || !m_retirement_fence)
        return fail("prepare has no registered submission");
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))) || device.Get() != m_device)
        return fail("generation queue belongs to another device");
    if (m_retirement_fence->GetCompletedValue() == UINT64_MAX)
        return fail("prepare device removed");
    if (FAILED(queue->Wait(m_retirement_fence.Get(), m_retirement_value)))
        return fail("generation queue wait failed");
    m_prepare_waited = true;
    return true;
}

bool Context::wait_for_retirement(uint32_t timeoutMs) noexcept
{
    if (m_recorded) return false;      // not submitted yet: nothing to wait for
    if (!m_retirement_fence) return true;
    if (retired()) return true;

    // A timed-out SetEventOnCompletion registration can outlive the waiting
    // handle. Poll the actual queue-signalled fence instead: there is no event
    // registration to cancel or retain, and no CPU-signalled completion.
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        if (retired()) return true;
        if (GetTickCount64() - start >= timeoutMs) return fail("retirement timed out");
        Sleep(1);
    }
}

bool Context::destroy() noexcept
{
    if (!retired()) return fail("destroy before GPU retirement");
    m_stage.release_targets();
    m_retirement_fence.Reset();
    m_retirement_value = 0;
    m_snapshot = false;
    m_prepare_waited = false;
    m_prepare_list = nullptr;
    m_device = nullptr;
    m_hudless = nullptr;
    m_depth = nullptr;
    m_motion = nullptr;
    m_input_formats_logged = false;
    m_enabled = false;
    m_prepare_valid = false;
    m_history_valid = false;
    m_reset_requested = true;
    m_display_width = 0;
    m_display_height = 0;
    m_render_width = 0;
    m_render_height = 0;
    m_frame_id = 0;
    m_prepared_frame_id = 0;
    m_frames_generated = 0;
    return true;
}

bool Context::create(const ffxApiHeader* createDesc) noexcept try
{
    const auto* frameGeneration = find_desc<ffxCreateContextDescFrameGeneration>(
        createDesc, FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION);
    if (!frameGeneration) return fail("create without FrameGenerationCreate");
    if (m_device) return fail("context already created");
    constexpr uint32_t supported = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED |
        FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE | FFX_FRAMEGENERATION_ENABLE_ASYNC_WORKLOAD_SUPPORT;
    if (frameGeneration->flags & ~supported) return fail("unsupported frame generation create flags");
    if (frameGeneration->backBufferFormat != FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM)
        return fail("unsupported back buffer format");

    const auto* backend = find_desc<ffxCreateBackendDX12Desc>(
        createDesc, FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12);
    if (!backend || !backend->device) return fail("create without a DX12 device");

    if (frameGeneration->displaySize.width == 0 || frameGeneration->displaySize.height == 0) {
        return fail("create with an empty display size");
    }

    m_device = backend->device;
    m_create_flags = frameGeneration->flags;
    m_display_width = frameGeneration->displaySize.width;
    m_display_height = frameGeneration->displaySize.height;
    m_max_render_width = frameGeneration->maxRenderSize.width;
    m_max_render_height = frameGeneration->maxRenderSize.height;

    if (!m_stage.initialize(m_device)) return fail("stage 1 initialization");

    log_event(1, "fg: context created, display " + std::to_string(m_display_width) + "x" +
                     std::to_string(m_display_height) + ", max render " +
                     std::to_string(m_max_render_width) + "x" +
                     std::to_string(m_max_render_height) +
                     ((m_create_flags & FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED) ? ", depth inverted" : "") +
                     ((m_create_flags & FFX_FRAMEGENERATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS)
                          ? ", display-res motion" : ""));
    return true;
}
catch (...) { return fail("context initialization exception"); }

bool Context::configure(const ffxApiHeader* configureDesc) noexcept
{
    if (!m_device) return fail("configure without a context");

    const auto* modern = find_desc<ffxConfigureDescFrameGeneration>(
        configureDesc, FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION);
    const auto* legacy = find_desc<FfxFrameGenerationConfig>(
        configureDesc, FFX_API_FRAME_GENERATION_CONFIG);

    if (modern) {
        m_enabled = modern->frameGenerationEnabled;
        m_hudless = static_cast<ID3D12Resource*>(modern->HUDLessColor.resource);
        m_hudless_state = as_state(modern->HUDLessColor.state);
        m_frame_id = modern->frameID;
    } else if (legacy) {
        m_enabled = legacy->frameGenerationEnabled;
        m_hudless = static_cast<ID3D12Resource*>(legacy->HUDLessColor.resource);
        m_hudless_state = as_state(legacy->HUDLessColor.state);
        m_frame_id = legacy->frameID;
    } else {
        return fail("configure without a frame generation desc");
    }

    if (!m_enabled) {
        m_prepare_valid = false;
        m_history_valid = false;
        m_reset_requested = true;
    }
    return true;
}

bool Context::prepare(const ffxApiHeader* prepareDesc) noexcept
{
    // A rejected prepare must never leave the preceding frame eligible.
    m_prepare_valid = false;
    if (!m_device) return fail("prepare without a context");
    // A previously generated frame owns the single slot until it retires. Wait
    // briefly rather than reject: the game's next prepare follows immediately.
    if (!retired() && !m_recorded && !wait_for_retirement(50)) return fail("prepare slot is still in flight");
    if (!retired()) return fail("prepare slot is still recorded or in flight");
    // An invalidated prepare still owns its descriptors until its submission
    // retires. Only now may it be abandoned, without releasing its textures.
    if (m_snapshot) {
        m_stage.discard_prepare();
        m_snapshot = false;
        m_reset_requested = true;
        m_history_valid = false;
    }

    const auto* v2 = find_desc<ffxDispatchDescFrameGenerationPrepareV2>(
        prepareDesc, FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2);
    const auto* legacy = find_desc<ffxDispatchDescFrameGenerationPrepare>(
        prepareDesc, FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE);

    if (!v2 && !legacy) return fail("prepare without a prepare desc");

    const FfxApiResource* depth = v2 ? &v2->depth : &legacy->depth;
    const FfxApiResource* motion = v2 ? &v2->motionVectors : &legacy->motionVectors;
    const FfxApiDimensions2D renderSize = v2 ? v2->renderSize : legacy->renderSize;
    const FfxApiFloatCoords2D motionScale = v2 ? v2->motionVectorScale : legacy->motionVectorScale;
    const bool reset = v2 ? v2->reset : legacy->unused_reset;
    const uint64_t frameID = v2 ? v2->frameID : legacy->frameID;

    m_depth = static_cast<ID3D12Resource*>(depth->resource);
    m_motion = static_cast<ID3D12Resource*>(motion->resource);
    m_depth_state = as_state(depth->state);
    m_motion_state = as_state(motion->state);

    if (!m_depth || !m_motion) return fail("prepare without depth or motion vectors");
    if (!m_input_formats_logged) {
        m_input_formats_logged = true;
        try {
            const auto nativeDepth = m_depth->GetDesc();
            const auto nativeMotion = m_motion->GetDesc();
            log_event(1, "fg: native inputs depth{format=" + std::to_string(nativeDepth.Format) +
                " flags=" + std::to_string(nativeDepth.Flags) + " size=" +
                std::to_string(nativeDepth.Width) + "x" + std::to_string(nativeDepth.Height) +
                "} motion{format=" + std::to_string(nativeMotion.Format) + " flags=" +
                std::to_string(nativeMotion.Flags) + " size=" + std::to_string(nativeMotion.Width) +
                "x" + std::to_string(nativeMotion.Height) + "}");
        } catch (...) {}
    }
    if (renderSize.width == 0 || renderSize.height == 0) return fail("prepare without a render size");

    const bool geometry_ok = renderSize.width <= m_max_render_width &&
                             renderSize.height <= m_max_render_height &&
                             renderSize.width <= m_display_width &&
                             renderSize.height <= m_display_height;
    if (!geometry_ok) return fail("prepare outside the created sizes");

    const DXGI_FORMAT depthFormat = depth_srv_format(format_of(m_depth));
    if (depthFormat == DXGI_FORMAT_UNKNOWN) return fail("unsupported depth format");
    if (!is_supported_motion(format_of(m_motion))) return fail("unsupported motion vector format");
    for (auto* resource : {m_depth, m_motion}) {
        const auto desc = resource->GetDesc();
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            desc.Width != renderSize.width || desc.Height != renderSize.height ||
            desc.MipLevels != 1 || desc.DepthOrArraySize != 1 || desc.SampleDesc.Count != 1 ||
            (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
            return fail("unsupported input texture geometry or usage");
    }

    if (!std::isfinite(motionScale.x) || !std::isfinite(motionScale.y) ||
        motionScale.x == 0.0f || motionScale.y == 0.0f) return fail("invalid motion vector scale");
    if (m_render_width != renderSize.width || m_render_height != renderSize.height) {
        m_reset_requested = true;
        m_history_valid = false;
    }
    m_render_width = renderSize.width;
    m_render_height = renderSize.height;
    m_motion_scale_x = motionScale.x;
    m_motion_scale_y = motionScale.y;

    auto* commandList = static_cast<ID3D12GraphicsCommandList*>(v2 ? v2->commandList : legacy->commandList);
    if (!commandList) return fail("prepare without a command list");
    FrameParams params{};
    params.renderWidth = m_render_width;
    params.renderHeight = m_render_height;
    params.displayWidth = m_display_width;
    params.displayHeight = m_display_height;
    params.depthInverted = (m_create_flags & FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED) != 0;
    params.motionVectorScaleX = -m_motion_scale_x;
    params.motionVectorScaleY = -m_motion_scale_y;
    if (!m_stage.configure(params)) return fail("prepare stage configuration");
    InputFrame inputs{};
    inputs.depth = m_depth;
    inputs.motion = m_motion;
    inputs.depthState = m_depth_state;
    inputs.motionState = m_motion_state;
    inputs.depthFormat = depthFormat;
    inputs.motionFormat = format_of(m_motion);
    if (!m_stage.capture_prepare(commandList, inputs)) return fail("prepare snapshot rejected");
    m_recorded = true;
    m_snapshot = true;
    m_prepare_waited = false;
    m_prepare_list = commandList;
    m_prepare_valid = true;
    // The game must keep inputs alive until its prepare submission retires.
    // Descriptors do not own COM references. Generation uses private snapshots
    // and no longer dereferences depth/motion pointers from a previous call.
    m_depth = nullptr;
    m_motion = nullptr;

    if (reset || (m_history_valid && frameID != m_prepared_frame_id + 1)) {
        m_reset_requested = true;
        m_history_valid = false;
    }
    m_prepared_frame_id = frameID;
    return true;
}

bool Context::generate(ID3D12GraphicsCommandList* list, ID3D12Resource* output,
                       D3D12_RESOURCE_STATES outputState, const HudSource* hud) noexcept
{
    if (!list || !output) return fail("generate without a command list or output");
    const GeneratePhase phase{output, outputState, m_stage.params().alpha};
    return generate(list, &phase, 1, hud);
}

bool Context::generate(ID3D12GraphicsCommandList* list, const GeneratePhase* phases, uint32_t count,
                       const HudSource* hud) noexcept
{
    if (!list || !phases || count == 0) return fail("generate without a command list or output");
    if (!m_enabled) return fail("frame generation disabled");
    if (!m_prepare_valid) return fail("generate without prepare");
    if (m_recorded && list != m_prepare_list && !m_prepare_waited)
        return fail("split generation before prepare submission registration");
    if (!m_recorded && !m_prepare_waited)
        return fail("split generation without a prepare queue dependency");
    if (m_prepared_frame_id != m_frame_id) return fail("configure/prepare frame ID mismatch");
    if (!m_hudless) return fail("generate without a HUD-less colour resource");

    if (m_hudless->GetDesc().Format != DXGI_FORMAT_R8G8B8A8_UNORM) {
        return fail("HUD-less colour is not R8G8B8A8_UNORM");
    }

    InputFrame frame{};
    frame.color = m_hudless;
    frame.colorState = m_hudless_state;

    const bool reset = m_reset_requested || !m_history_valid;
    if (!m_stage.generate(list, frame, phases, count, reset, hud)) {
        return fail("stage 1 rejected the frame");
    }

    m_history_valid = true;
    m_reset_requested = false;
    m_prepare_valid = false;
    m_snapshot = false;
    m_recorded = true;
    m_prepare_list = nullptr;
    ++m_frames_generated;
    return true;
}

} // namespace sedici::fg

#pragma warning(pop)
