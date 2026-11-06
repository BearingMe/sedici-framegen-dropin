#include "fg_context.h"

#include "ffx_api_framegeneration_dx12.h"
#include "ffx_framegeneration.h"
#include "log.h"

#include <string>

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
    // FFX's DX12 resources carry D3D12_RESOURCE_STATES in `state`.
    return static_cast<D3D12_RESOURCE_STATES>(value);
}

DXGI_FORMAT format_of(ID3D12Resource* resource) noexcept
{
    return resource ? resource->GetDesc().Format : DXGI_FORMAT_UNKNOWN;
}

bool is_supported_depth(DXGI_FORMAT format) noexcept
{
    switch (format) {
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_D16_UNORM:
        return true;
    default:
        return false;
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
    log_event(2, std::string("fg: rejected: ") + reason);
    return false;
}

void Context::destroy() noexcept
{
    m_stage.release_targets();
    m_device = nullptr;
    m_hudless = nullptr;
    m_depth = nullptr;
    m_motion = nullptr;
    m_enabled = false;
    m_prepare_valid = false;
    m_history_valid = false;
    m_reset_requested = true;
    m_display_width = 0;
    m_display_height = 0;
    m_render_width = 0;
    m_render_height = 0;
}

bool Context::create(const ffxApiHeader* createDesc) noexcept
{
    const auto* frameGeneration = find_desc<ffxCreateContextDescFrameGeneration>(
        createDesc, FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION);
    if (!frameGeneration) return fail("create without FrameGenerationCreate");

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
    if (!m_device) return fail("prepare without a context");

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

    m_depth = static_cast<ID3D12Resource*>(depth->resource);
    m_motion = static_cast<ID3D12Resource*>(motion->resource);
    m_depth_state = as_state(depth->state);
    m_motion_state = as_state(motion->state);

    if (!m_depth || !m_motion) return fail("prepare without depth or motion vectors");
    if (renderSize.width == 0 || renderSize.height == 0) return fail("prepare without a render size");

    const bool geometry_ok = renderSize.width <= m_max_render_width &&
                             renderSize.height <= m_max_render_height &&
                             renderSize.width <= m_display_width &&
                             renderSize.height <= m_display_height;
    if (!geometry_ok) return fail("prepare outside the created sizes");

    if (!is_supported_depth(format_of(m_depth))) return fail("unsupported depth format");
    if (!is_supported_motion(format_of(m_motion))) return fail("unsupported motion vector format");

    m_render_width = renderSize.width;
    m_render_height = renderSize.height;
    m_motion_scale_x = motionScale.x;
    m_motion_scale_y = motionScale.y;
    m_prepare_valid = true;

    if (reset) {
        m_reset_requested = true;
        m_history_valid = false;
    }
    return true;
}

bool Context::generate(ID3D12GraphicsCommandList* list, ID3D12Resource* output,
                       D3D12_RESOURCE_STATES outputState) noexcept
{
    if (!list || !output) return fail("generate without a command list or output");
    if (!m_enabled) return fail("frame generation disabled");
    if (!m_prepare_valid) return fail("generate without prepare");
    if (!m_hudless) return fail("generate without a HUD-less colour resource");

    if (m_hudless->GetDesc().Format != DXGI_FORMAT_R8G8B8A8_UNORM) {
        return fail("HUD-less colour is not R8G8B8A8_UNORM");
    }

    FrameParams params{};
    params.renderWidth = m_render_width;
    params.renderHeight = m_render_height;
    params.displayWidth = m_display_width;
    params.displayHeight = m_display_height;
    params.depthInverted = (m_create_flags & FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED) != 0;
    params.motionVectorScaleX = m_motion_scale_x;
    params.motionVectorScaleY = m_motion_scale_y;

    if (!m_stage.configure(params)) return fail("stage 1 configuration");

    InputFrame frame{};
    frame.color = m_hudless;
    frame.depth = m_depth;
    frame.motion = m_motion;
    frame.colorState = m_hudless_state;
    frame.depthState = m_depth_state;
    frame.motionState = m_motion_state;
    frame.depthFormat = format_of(m_depth);
    frame.motionFormat = format_of(m_motion);

    const bool reset = m_reset_requested || !m_history_valid;
    if (!m_stage.generate(list, frame, output, reset, outputState)) {
        return fail("stage 1 rejected the frame");
    }

    m_history_valid = true;
    m_reset_requested = false;
    m_prepare_valid = false;
    ++m_frames_generated;
    return true;
}

} // namespace sedici::fg
