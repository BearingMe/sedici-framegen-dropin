#include "abi/describe.h"

#include "diagnostics/log.h"

#include "ffx_api.h"
#include "ffx_api_types.h"
#include "ffx_framegeneration.h"
#include "ffx_api_dx12.h"
#include "ffx_api_framegeneration_dx12.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <iomanip>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#pragma warning(push)
#pragma warning(disable : 4996) // The SDK intentionally exposes deprecated descs; we log both.

namespace {

constexpr unsigned kFullLogCallLimit = 50;
constexpr size_t kMaxChainItems = 64;

std::string hx(uint64_t value)
{
    std::ostringstream out;
    out << "0x" << std::hex << value;
    return out.str();
}

std::string ptr(const void* value)
{
    if (!value) return "null";
    std::ostringstream out;
    out << "0x" << std::hex << reinterpret_cast<uintptr_t>(value);
    return out.str();
}

std::string fl(float value)
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(4) << value;
    return out.str();
}

const char* tf(bool value)
{
    return value ? "true" : "false";
}

const char* rc_name(ffxReturnCode_t code)
{
    switch (code) {
    case FFX_API_RETURN_OK: return "OK";
    case FFX_API_RETURN_ERROR: return "ERROR";
    case FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE: return "UNKNOWN_DESCTYPE";
    case FFX_API_RETURN_ERROR_RUNTIME_ERROR: return "RUNTIME_ERROR";
    case FFX_API_RETURN_NO_PROVIDER: return "NO_PROVIDER";
    case FFX_API_RETURN_ERROR_MEMORY: return "ERROR_MEMORY";
    case FFX_API_RETURN_ERROR_PARAMETER: return "ERROR_PARAMETER";
    case FFX_API_RETURN_PROVIDER_NO_SUPPORT_NEW_DESCTYPE: return "PROVIDER_NO_SUPPORT_NEW_DESCTYPE";
    default: return "UNKNOWN";
    }
}

const char* type_name(uint64_t type)
{
    switch (type) {
    case FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION: return "FrameGenerationCreate";
    case FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS: return "FrameGenerationCreateHudless";
    case FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION: return "FrameGenerationCreateVersion";
    case FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WRAP_DX12: return "FrameGenerationSwapChainWrapDX12";
    case FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_NEW_DX12: return "FrameGenerationSwapChainNewDX12";
    case FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12: return "FrameGenerationSwapChainForHwndDX12";
    case FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12: return "FrameGenerationSwapChainVersionDX12";
    case FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION: return "FrameGenerationConfigure";
    case FFX_API_FRAME_GENERATION_CONFIG: return "FrameGenerationConfig";
    case FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION_KEYVALUE: return "FrameGenerationConfigureKeyValue";
    case FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION_REGISTERDISTORTIONRESOURCE: return "FrameGenerationDistortionResource";
    case FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_REGISTERUIRESOURCE_DX12: return "SwapChainRegisterUiResourceDX12";
    case FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_KEYVALUE_DX12: return "SwapChainConfigureKeyValueDX12";
    case FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE: return "FrameGenerationPrepare";
    case FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2: return "FrameGenerationPrepareV2";
    case FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_CAMERAINFO: return "FrameGenerationPrepareCameraInfo";
    case FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION: return "FrameGenerationDispatch";
    case FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WAIT_FOR_PRESENTS_DX12: return "SwapChainWaitForPresentsDX12";
    case FFX_API_CALLBACK_DESC_TYPE_FRAMEGENERATION_PRESENT: return "FrameGenerationPresentCallback";
    case FFX_API_CALLBACK_DESC_TYPE_FRAMEGENERATION_PRESENT_PREMUL_ALPHA: return "FrameGenerationPresentPremulAlpha";
    case FFX_API_QUERY_DESC_TYPE_FRAMEGENERATION_GPU_MEMORY_USAGE: return "FrameGenerationGpuMemoryUsage";
    case FFX_API_QUERY_DESC_TYPE_FRAMEGENERATION_GPU_MEMORY_USAGE_V2: return "FrameGenerationGpuMemoryUsageV2";
    case FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_INTERPOLATIONCOMMANDLIST_DX12: return "SwapChainInterpolationCommandListDX12";
    case FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_INTERPOLATIONTEXTURE_DX12: return "SwapChainInterpolationTextureDX12";
    case FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_GPU_MEMORY_USAGE_DX12: return "SwapChainGpuMemoryUsageDX12";
    case FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_GPU_MEMORY_USAGE_DX12_V2: return "SwapChainGpuMemoryUsageDX12V2";
    case FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12: return "BackendDX12";
    case FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12_ALLOCATION_CALLBACKS: return "BackendDX12AllocationCallbacks";
    case FFX_API_DESC_TYPE_OVERRIDE_VERSION: return "OverrideVersion";
    case FFX_API_QUERY_DESC_TYPE_GET_VERSIONS: return "GetVersions";
    case FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION: return "GetProviderVersion";
    case FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG: return "GlobalDebug";
    case FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG1: return "GlobalDebug1";
    default: return "Unknown";
    }
}

const char* surface_format_name(uint32_t format)
{
    switch (format) {
    case FFX_API_SURFACE_FORMAT_UNKNOWN: return "UNKNOWN";
    case FFX_API_SURFACE_FORMAT_R32G32B32A32_TYPELESS: return "R32G32B32A32_TYPELESS";
    case FFX_API_SURFACE_FORMAT_R32G32B32A32_UINT: return "R32G32B32A32_UINT";
    case FFX_API_SURFACE_FORMAT_R32G32B32A32_FLOAT: return "R32G32B32A32_FLOAT";
    case FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
    case FFX_API_SURFACE_FORMAT_R32G32B32_FLOAT: return "R32G32B32_FLOAT";
    case FFX_API_SURFACE_FORMAT_R32G32_FLOAT: return "R32G32_FLOAT";
    case FFX_API_SURFACE_FORMAT_R8_UINT: return "R8_UINT";
    case FFX_API_SURFACE_FORMAT_R32_UINT: return "R32_UINT";
    case FFX_API_SURFACE_FORMAT_R8G8B8A8_TYPELESS: return "R8G8B8A8_TYPELESS";
    case FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case FFX_API_SURFACE_FORMAT_R8G8B8A8_SNORM: return "R8G8B8A8_SNORM";
    case FFX_API_SURFACE_FORMAT_R8G8B8A8_SRGB: return "R8G8B8A8_SRGB";
    case FFX_API_SURFACE_FORMAT_B8G8R8A8_TYPELESS: return "B8G8R8A8_TYPELESS";
    case FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case FFX_API_SURFACE_FORMAT_B8G8R8A8_SRGB: return "B8G8R8A8_SRGB";
    case FFX_API_SURFACE_FORMAT_R11G11B10_FLOAT: return "R11G11B10_FLOAT";
    case FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
    case FFX_API_SURFACE_FORMAT_R16G16_FLOAT: return "R16G16_FLOAT";
    case FFX_API_SURFACE_FORMAT_R16G16_UINT: return "R16G16_UINT";
    case FFX_API_SURFACE_FORMAT_R16G16_SINT: return "R16G16_SINT";
    case FFX_API_SURFACE_FORMAT_R16_FLOAT: return "R16_FLOAT";
    case FFX_API_SURFACE_FORMAT_R16_UINT: return "R16_UINT";
    case FFX_API_SURFACE_FORMAT_R16_UNORM: return "R16_UNORM";
    case FFX_API_SURFACE_FORMAT_R16_SNORM: return "R16_SNORM";
    case FFX_API_SURFACE_FORMAT_R8_UNORM: return "R8_UNORM";
    case FFX_API_SURFACE_FORMAT_R8G8_UNORM: return "R8G8_UNORM";
    case FFX_API_SURFACE_FORMAT_R8G8_UINT: return "R8G8_UINT";
    case FFX_API_SURFACE_FORMAT_R32_FLOAT: return "R32_FLOAT";
    case FFX_API_SURFACE_FORMAT_R9G9B9E5_SHAREDEXP: return "R9G9B9E5_SHAREDEXP";
    case FFX_API_SURFACE_FORMAT_R16G16B16A16_TYPELESS: return "R16G16B16A16_TYPELESS";
    case FFX_API_SURFACE_FORMAT_R32G32_TYPELESS: return "R32G32_TYPELESS";
    case FFX_API_SURFACE_FORMAT_R10G10B10A2_TYPELESS: return "R10G10B10A2_TYPELESS";
    case FFX_API_SURFACE_FORMAT_R16G16_TYPELESS: return "R16G16_TYPELESS";
    case FFX_API_SURFACE_FORMAT_R16_TYPELESS: return "R16_TYPELESS";
    case FFX_API_SURFACE_FORMAT_R8_TYPELESS: return "R8_TYPELESS";
    case FFX_API_SURFACE_FORMAT_R8G8_TYPELESS: return "R8G8_TYPELESS";
    case FFX_API_SURFACE_FORMAT_R32_TYPELESS: return "R32_TYPELESS";
    case FFX_API_SURFACE_FORMAT_R32G32_UINT: return "R32G32_UINT";
    default: return nullptr;
    }
}

std::string surface_format(uint32_t format)
{
    if (const char* name = surface_format_name(format)) return std::string(name) + '(' + hx(format) + ')';
    return hx(format);
}

const char* dxgi_format_name(DXGI_FORMAT format)
{
    switch (format) {
    case DXGI_FORMAT_UNKNOWN: return "UNKNOWN";
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return "R32G32B32A32_FLOAT";
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
    case DXGI_FORMAT_R32G32_FLOAT: return "R32G32_FLOAT";
    case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
    case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10_FLOAT";
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
    case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
    default: return nullptr;
    }
}

std::string dxgi_format(DXGI_FORMAT format)
{
    if (const char* name = dxgi_format_name(format)) return std::string(name) + '(' + hx(static_cast<uint64_t>(format)) + ')';
    return hx(static_cast<uint64_t>(format));
}

std::string decode_bits(uint32_t value, std::initializer_list<std::pair<uint32_t, const char*>> names)
{
    if (!value) return "none";

    std::ostringstream out;
    uint32_t remaining = value;
    bool first = true;
    for (const auto& item : names) {
        if ((value & item.first) == 0) continue;
        if (!first) out << '|';
        out << item.second;
        first = false;
        remaining &= ~item.first;
    }
    if (remaining) {
        if (!first) out << '|';
        out << hx(remaining);
    }
    return out.str();
}

std::string decode_create_flags(uint32_t flags)
{
    return decode_bits(flags, {
        {FFX_FRAMEGENERATION_ENABLE_ASYNC_WORKLOAD_SUPPORT, "AsyncWorkloads"},
        {FFX_FRAMEGENERATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS, "DisplayResMotionVectors"},
        {FFX_FRAMEGENERATION_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION, "MVJitterCancellation"},
        {FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED, "DepthInverted"},
        {FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE, "DepthInfinite"},
        {FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE, "HDR"},
        {FFX_FRAMEGENERATION_ENABLE_DEBUG_CHECKING, "DebugChecking"},
    });
}

std::string decode_dispatch_flags(uint32_t flags)
{
    return decode_bits(flags, {
        {FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_TEAR_LINES, "DebugTearLines"},
        {FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_RESET_INDICATORS, "DebugResetIndicators"},
        {FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_VIEW, "DebugView"},
        {FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY, "NoSwapchainContextNotify"},
        {FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_PACING_LINES, "DebugPacingLines"},
        {FFX_FRAMEGENERATION_FLAG_RESERVED_1, "Reserved1"},
        {FFX_FRAMEGENERATION_FLAG_RESERVED_2, "Reserved2"},
    });
}

std::string decode_ui_flags(uint32_t flags)
{
    return decode_bits(flags, {
        {FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_USE_PREMUL_ALPHA, "PremulAlpha"},
        {FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_ENABLE_INTERNAL_UI_DOUBLE_BUFFERING, "InternalUiDoubleBuffer"},
    });
}

std::string resource(const FfxApiResource& value)
{
    if (!value.resource) return "none";

    std::ostringstream out;
    out << "res=" << ptr(value.resource)
        << " format=" << surface_format(value.description.format);
    if (value.description.type == FFX_API_RESOURCE_TYPE_BUFFER) {
        out << " bytes=" << value.description.size << " stride=" << value.description.stride;
    } else {
        out << " size=" << value.description.width << 'x' << value.description.height;
        if (value.description.depth > 1) out << " depth=" << value.description.depth;
    }
    out << " state=" << hx(value.state) << " usage=" << hx(value.description.usage);
    return out.str();
}

template <typename Fn>
void walk_chain(const ffxApiHeader* desc, Fn&& fn)
{
    std::set<const ffxApiHeader*> seen;
    for (const ffxApiHeader* item = desc; item; item = item->pNext) {
        if (!seen.insert(item).second) break;
        fn(*item);
        if (seen.size() >= kMaxChainItems) break;
    }
}

std::vector<uint64_t> collect_types(const ffxApiHeader* desc)
{
    std::vector<uint64_t> result;
    walk_chain(desc, [&](const ffxApiHeader& item) { result.push_back(item.type); });
    return result;
}

std::string rect(const FfxApiRect2D& value)
{
    std::ostringstream out;
    out << value.left << ',' << value.top << ' ' << value.width << 'x' << value.height;
    return out.str();
}

void append_swapchain_desc(std::ostringstream& out, const DXGI_SWAP_CHAIN_DESC* desc)
{
    if (!desc) {
        out << " desc=null";
        return;
    }
    out << " desc{buffers=" << desc->BufferCount
        << " size=" << desc->BufferDesc.Width << 'x' << desc->BufferDesc.Height
        << " format=" << dxgi_format(desc->BufferDesc.Format)
        << " refresh=" << desc->BufferDesc.RefreshRate.Numerator << '/' << desc->BufferDesc.RefreshRate.Denominator
        << " usage=" << hx(desc->BufferUsage)
        << " swapEffect=" << static_cast<uint32_t>(desc->SwapEffect)
        << " flags=" << hx(desc->Flags)
        << " windowed=" << tf(desc->Windowed != 0)
        << " outputWindow=" << ptr(desc->OutputWindow) << '}';
}

void append_swapchain_desc1(std::ostringstream& out, const DXGI_SWAP_CHAIN_DESC1* desc)
{
    if (!desc) {
        out << " desc=null";
        return;
    }
    out << " desc{buffers=" << desc->BufferCount
        << " size=" << desc->Width << 'x' << desc->Height
        << " format=" << dxgi_format(desc->Format)
        << " stereo=" << tf(desc->Stereo != 0)
        << " sample=" << desc->SampleDesc.Count << '/' << desc->SampleDesc.Quality
        << " usage=" << hx(desc->BufferUsage)
        << " scaling=" << static_cast<uint32_t>(desc->Scaling)
        << " swapEffect=" << static_cast<uint32_t>(desc->SwapEffect)
        << " alpha=" << static_cast<uint32_t>(desc->AlphaMode)
        << " flags=" << hx(desc->Flags) << '}';
}

void append_swapchain_ptr(std::ostringstream& out, IDXGISwapChain4** swapchain)
{
    out << " swapchainPtr=" << ptr(swapchain);
    if (swapchain) out << " swapchain=" << ptr(*swapchain);
}

void append_camera(std::ostringstream& out, const char* label, const float (&value)[3])
{
    out << ' ' << label << "=(" << fl(value[0]) << ',' << fl(value[1]) << ',' << fl(value[2]) << ')';
}

void append_prepare(std::ostringstream& out,
                    uint64_t frameID,
                    uint32_t flags,
                    const void* commandList,
                    const FfxApiDimensions2D& renderSize,
                    const FfxApiFloatCoords2D& jitter,
                    const FfxApiFloatCoords2D& motionVectorScale,
                    float frameTimeDelta,
                    bool reset,
                    float cameraNear,
                    float cameraFar,
                    float cameraFov,
                    float viewSpaceToMeters,
                    const FfxApiResource& depth,
                    const FfxApiResource& motionVectors)
{
    out << " frameID=" << frameID
        << " flags=" << decode_dispatch_flags(flags)
        << " cmd=" << ptr(commandList)
        << " render=" << renderSize.width << 'x' << renderSize.height
        << " jitter=(" << fl(jitter.x) << ',' << fl(jitter.y) << ')'
        << " mvScale=(" << fl(motionVectorScale.x) << ',' << fl(motionVectorScale.y) << ')'
        << " frameTimeDelta=" << fl(frameTimeDelta)
        << " reset=" << tf(reset)
        << " near=" << fl(cameraNear)
        << " far=" << fl(cameraFar)
        << " fov=" << fl(cameraFov)
        << " viewToMeters=" << fl(viewSpaceToMeters)
        << " depth={" << resource(depth) << '}'
        << " mv={" << resource(motionVectors) << '}';
}

void describe_create(const ffxApiHeader* desc, std::ostringstream& out)
{
    walk_chain(desc, [&](const ffxApiHeader& item) {
        out << "\n  " << hx(item.type) << " (" << type_name(item.type) << ')';
        switch (item.type) {
        case FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION: {
            const auto& d = reinterpret_cast<const ffxCreateContextDescFrameGeneration&>(item);
            out << " flags=" << decode_create_flags(d.flags)
                << " display=" << d.displaySize.width << 'x' << d.displaySize.height
                << " maxRender=" << d.maxRenderSize.width << 'x' << d.maxRenderSize.height
                << " backbufferFormat=" << surface_format(d.backBufferFormat);
            break;
        }
        case FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS: {
            const auto& d = reinterpret_cast<const ffxCreateContextDescFrameGenerationHudless&>(item);
            out << " hudlessFormat=" << surface_format(d.hudlessBackBufferFormat);
            break;
        }
        case FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION: {
            const auto& d = reinterpret_cast<const ffxCreateContextDescFrameGenerationVersion&>(item);
            out << " version=" << hx(d.version) << " (" << (d.version >> 22) << '.'
                << ((d.version >> 12) & 0x3ff) << '.' << (d.version & 0xfff) << ')';
            break;
        }
        case FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WRAP_DX12: {
            const auto& d = reinterpret_cast<const ffxCreateContextDescFrameGenerationSwapChainWrapDX12&>(item);
            out << " variant=wrap";
            append_swapchain_ptr(out, d.swapchain);
            out << " gameQueue=" << ptr(d.gameQueue);
            break;
        }
        case FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_NEW_DX12: {
            const auto& d = reinterpret_cast<const ffxCreateContextDescFrameGenerationSwapChainNewDX12&>(item);
            out << " variant=new";
            append_swapchain_ptr(out, d.swapchain);
            append_swapchain_desc(out, d.desc);
            out << " factory=" << ptr(d.dxgiFactory) << " gameQueue=" << ptr(d.gameQueue);
            break;
        }
        case FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12: {
            const auto& d = reinterpret_cast<const ffxCreateContextDescFrameGenerationSwapChainForHwndDX12&>(item);
            out << " variant=forHwnd";
            append_swapchain_ptr(out, d.swapchain);
            out << " hwnd=" << ptr(d.hwnd);
            append_swapchain_desc1(out, d.desc);
            out << " fullscreenDesc=" << ptr(d.fullscreenDesc)
                << " factory=" << ptr(d.dxgiFactory) << " gameQueue=" << ptr(d.gameQueue);
            break;
        }
        case FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12: {
            const auto& d = reinterpret_cast<const ffxCreateContextDescFrameGenerationSwapChainVersionDX12&>(item);
            out << " version=" << hx(d.version) << " (" << (d.version >> 22) << '.'
                << ((d.version >> 12) & 0x3ff) << '.' << (d.version & 0xfff) << ')';
            break;
        }
        case FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12: {
            const auto& d = reinterpret_cast<const ffxCreateBackendDX12Desc&>(item);
            out << " device=" << ptr(d.device);
            break;
        }
        case FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12_ALLOCATION_CALLBACKS: {
            const auto& d = reinterpret_cast<const ffxCreateBackendDX12AllocationCallbacksDesc&>(item);
            out << " alloc=" << ptr(reinterpret_cast<const void*>(d.pfnFfxResourceAllocator))
                << " dealloc=" << ptr(reinterpret_cast<const void*>(d.pfnFfxResourceDeallocator))
                << " heapAlloc=" << ptr(reinterpret_cast<const void*>(d.pfnFfxHeapAllocator))
                << " heapDealloc=" << ptr(reinterpret_cast<const void*>(d.pfnFfxHeapDeallocator))
                << " cbAlloc=" << ptr(reinterpret_cast<const void*>(d.pfnFfxConstantBufferAllocator));
            break;
        }
        case FFX_API_DESC_TYPE_OVERRIDE_VERSION: {
            const auto& d = reinterpret_cast<const ffxOverrideVersion&>(item);
            out << " versionId=" << hx(d.versionId);
            break;
        }
        default:
            break;
        }
    });
}

void describe_configure(const ffxApiHeader* desc, std::ostringstream& out)
{
    walk_chain(desc, [&](const ffxApiHeader& item) {
        out << "\n  " << hx(item.type) << " (" << type_name(item.type) << ')';
        switch (item.type) {
        case FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION: {
            const auto& d = reinterpret_cast<const ffxConfigureDescFrameGeneration&>(item);
            out << " swapChain=" << ptr(d.swapChain)
                << " presentCallback=" << (d.presentCallback ? "set" : "none")
                << " presentCtx=" << ptr(d.presentCallbackUserContext)
                << " generationCallback=" << (d.frameGenerationCallback ? "set" : "none")
                << " generationCtx=" << ptr(d.frameGenerationCallbackUserContext)
                << " frameGenerationEnabled=" << tf(d.frameGenerationEnabled)
                << " allowAsyncWorkloads=" << tf(d.allowAsyncWorkloads)
                << " HUDLessColor=" << (d.HUDLessColor.resource ? "yes" : "no")
                << " HUDLess={" << resource(d.HUDLessColor) << '}'
                << " flags=" << decode_dispatch_flags(d.flags)
                << " onlyPresentGenerated=" << tf(d.onlyPresentGenerated)
                << " generationRect=" << rect(d.generationRect)
                << " frameID=" << d.frameID;
            break;
        }
        case FFX_API_FRAME_GENERATION_CONFIG: {
            const auto& d = reinterpret_cast<const FfxFrameGenerationConfig&>(item);
            out << " swapChain=" << ptr(d.swapChain)
                << " presentCallback=" << (d.presentCallback ? "set" : "none")
                << " presentCtx=" << ptr(d.presentCallbackContext)
                << " generationCallback=" << (d.frameGenerationCallback ? "set" : "none")
                << " generationCtx=" << ptr(d.frameGenerationCallbackContext)
                << " frameGenerationEnabled=" << tf(d.frameGenerationEnabled)
                << " allowAsyncWorkloads=" << tf(d.allowAsyncWorkloads)
                << " allowAsyncPresent=" << tf(d.allowAsyncPresent)
                << " HUDLessColor=" << (d.HUDLessColor.resource ? "yes" : "no")
                << " HUDLess={" << resource(d.HUDLessColor) << '}'
                << " flags=" << decode_dispatch_flags(d.flags)
                << " onlyPresentInterpolated=" << tf(d.onlyPresentInterpolated)
                << " interpolationRect=" << rect(d.interpolationRect)
                << " frameID=" << d.frameID
                << " drawDebugPacingLines=" << tf(d.drawDebugPacingLines);
            break;
        }
        case FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION_KEYVALUE: {
            const auto& d = reinterpret_cast<const ffxConfigureDescFrameGenerationKeyValue&>(item);
            out << " key=" << d.key;
            if (d.key == FFX_API_CONFIGURE_FRAMEGENERATION_KEY_DEBUG_VIEW_MODE) out << " (DebugViewMode)";
            else if (d.key == FFX_API_CONFIGURE_FRAMEGENERATION_KEY_DEBUG_VIEW_FLOW_SCALE) out << " (DebugViewFlowScale)";
            out << " u64=" << d.u64 << " ptr=" << ptr(d.ptr);
            break;
        }
        case FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION_REGISTERDISTORTIONRESOURCE: {
            const auto& d = reinterpret_cast<const ffxConfigureDescFrameGenerationRegisterDistortionFieldResource&>(item);
            out << " distortionField={" << resource(d.distortionField) << '}';
            break;
        }
        case FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_REGISTERUIRESOURCE_DX12: {
            const auto& d = reinterpret_cast<const ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12&>(item);
            out << " uiResource=" << (d.uiResource.resource ? "yes" : "no")
                << " ui={" << resource(d.uiResource) << '}'
                << " flags=" << decode_ui_flags(d.flags);
            break;
        }
        case FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_KEYVALUE_DX12: {
            const auto& d = reinterpret_cast<const ffxConfigureDescFrameGenerationSwapChainKeyValueDX12&>(item);
            out << " key=" << d.key;
            if (d.key == FFX_API_CONFIGURE_FG_SWAPCHAIN_KEY_WAITCALLBACK) out << " (WaitCallback)";
            else if (d.key == FFX_API_CONFIGURE_FG_SWAPCHAIN_KEY_FRAMEPACINGTUNING) out << " (FramePacingTuning)";
            out << " u64=" << d.u64 << " ptr=" << ptr(d.ptr);
            if (d.key == FFX_API_CONFIGURE_FG_SWAPCHAIN_KEY_FRAMEPACINGTUNING) {
                if (const auto* tuning = static_cast<const FfxApiSwapchainFramePacingTuning*>(d.ptr)) {
                    out << " tuning{safetyMarginInMs=" << fl(tuning->safetyMarginInMs)
                        << " varianceFactor=" << fl(tuning->varianceFactor)
                        << " allowHybridSpin=" << tf(tuning->allowHybridSpin)
                        << " hybridSpinTime=" << tuning->hybridSpinTime
                        << " allowWaitForSingleObjectOnFence=" << tf(tuning->allowWaitForSingleObjectOnFence)
                        << '}';
                } else {
                    out << " tuning=null";
                }
            }
            break;
        }
        case FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG: {
            const auto& d = reinterpret_cast<const ffxConfigureDescGlobalDebug&>(item);
            out << " effectId=" << hx(d.effectId)
                << " messageCallback=" << (d.fpMessage ? "set" : "none")
                << " debugLevel=" << d.debugLevel;
            break;
        }
        case FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG1: {
            const auto& d = reinterpret_cast<const ffxConfigureDescGlobalDebug1&>(item);
            out << " messageCallback=" << (d.fpMessage ? "set" : "none")
                << " debugLevel=" << d.debugLevel;
            break;
        }
        case FFX_API_DESC_TYPE_OVERRIDE_VERSION: {
            const auto& d = reinterpret_cast<const ffxOverrideVersion&>(item);
            out << " versionId=" << hx(d.versionId);
            break;
        }
        default:
            break;
        }
    });
}

void describe_dispatch(const ffxApiHeader* desc, std::ostringstream& out)
{
    walk_chain(desc, [&](const ffxApiHeader& item) {
        out << "\n  " << hx(item.type) << " (" << type_name(item.type) << ')';
        switch (item.type) {
        case FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE: {
            const auto& d = reinterpret_cast<const ffxDispatchDescFrameGenerationPrepare&>(item);
            append_prepare(out, d.frameID, d.flags, d.commandList, d.renderSize,
                           d.jitterOffset, d.motionVectorScale, d.frameTimeDelta,
                           d.unused_reset, d.cameraNear, d.cameraFar,
                           d.cameraFovAngleVertical, d.viewSpaceToMetersFactor,
                           d.depth, d.motionVectors);
            break;
        }
        case FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2: {
            const auto& d = reinterpret_cast<const ffxDispatchDescFrameGenerationPrepareV2&>(item);
            append_prepare(out, d.frameID, d.flags, d.commandList, d.renderSize,
                           d.jitterOffset, d.motionVectorScale, d.frameTimeDelta,
                           d.reset, d.cameraNear, d.cameraFar,
                           d.cameraFovAngleVertical, d.viewSpaceToMetersFactor,
                           d.depth, d.motionVectors);
            append_camera(out, "camPos", d.cameraPosition);
            append_camera(out, "camUp", d.cameraUp);
            append_camera(out, "camRight", d.cameraRight);
            append_camera(out, "camForward", d.cameraForward);
            break;
        }
        case FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_CAMERAINFO: {
            const auto& d = reinterpret_cast<const ffxDispatchDescFrameGenerationPrepareCameraInfo&>(item);
            append_camera(out, "camPos", d.cameraPosition);
            append_camera(out, "camUp", d.cameraUp);
            append_camera(out, "camRight", d.cameraRight);
            append_camera(out, "camForward", d.cameraForward);
            break;
        }
        case FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION: {
            const auto& d = reinterpret_cast<const ffxDispatchDescFrameGeneration&>(item);
            out << " cmd=" << ptr(d.commandList)
                << " presentColor={" << resource(d.presentColor) << '}'
                << " numGeneratedFrames=" << d.numGeneratedFrames
                << " reset=" << tf(d.reset)
                << " transferFunction=" << d.backbufferTransferFunction
                << " minLuminance=" << fl(d.minMaxLuminance[0])
                << " maxLuminance=" << fl(d.minMaxLuminance[1])
                << " generationRect=" << rect(d.generationRect)
                << " frameID=" << d.frameID;
            for (unsigned i = 0; i < 4; ++i) {
                if (i < d.numGeneratedFrames || d.outputs[i].resource) {
                    out << " output[" << i << "]={" << resource(d.outputs[i]) << '}';
                }
            }
            break;
        }
        case FFX_API_CALLBACK_DESC_TYPE_FRAMEGENERATION_PRESENT: {
            const auto& d = reinterpret_cast<const ffxCallbackDescFrameGenerationPresent&>(item);
            out << " device=" << ptr(d.device)
                << " cmd=" << ptr(d.commandList)
                << " currentBackBuffer={" << resource(d.currentBackBuffer) << '}'
                << " currentUI={" << resource(d.currentUI) << '}'
                << " outputSwapChainBuffer={" << resource(d.outputSwapChainBuffer) << '}'
                << " isGeneratedFrame=" << tf(d.isGeneratedFrame)
                << " frameID=" << d.frameID;
            break;
        }
        case FFX_API_CALLBACK_DESC_TYPE_FRAMEGENERATION_PRESENT_PREMUL_ALPHA: {
            const auto& d = reinterpret_cast<const ffxCallbackDescFrameGenerationPresentPremulAlpha&>(item);
            out << " usePremulAlpha=" << tf(d.usePremulAlpha);
            break;
        }
        case FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WAIT_FOR_PRESENTS_DX12:
            break;
        default:
            break;
        }
    });
}

void describe_query(const ffxApiHeader* desc, std::ostringstream& out, ffxReturnCode_t result)
{
    walk_chain(desc, [&](const ffxApiHeader& item) {
        out << "\n  " << hx(item.type) << " (" << type_name(item.type) << ')';
        switch (item.type) {
        case FFX_API_QUERY_DESC_TYPE_GET_VERSIONS: {
            const auto& d = reinterpret_cast<const ffxQueryDescGetVersions&>(item);
            out << " createDescType=" << hx(d.createDescType)
                << " device=" << ptr(d.device)
                << " outputCount=" << ptr(d.outputCount);
            if (d.outputCount && result == FFX_API_RETURN_OK) out << " count=" << *d.outputCount;
            out << " versionIds=" << ptr(d.versionIds) << " versionNames=" << ptr(d.versionNames);
            break;
        }
        case FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION: {
            const auto& d = reinterpret_cast<const ffxQueryGetProviderVersion&>(item);
            out << " versionId=" << hx(d.versionId) << " versionName=" << ptr(d.versionName);
            break;
        }
        case FFX_API_QUERY_DESC_TYPE_FRAMEGENERATION_GPU_MEMORY_USAGE: {
            const auto& d = reinterpret_cast<const ffxQueryDescFrameGenerationGetGPUMemoryUsage&>(item);
            out << " out=" << ptr(d.gpuMemoryUsageFrameGeneration);
            if (d.gpuMemoryUsageFrameGeneration && result == FFX_API_RETURN_OK) {
                out << " total=" << d.gpuMemoryUsageFrameGeneration->totalUsageInBytes
                    << " aliasable=" << d.gpuMemoryUsageFrameGeneration->aliasableUsageInBytes;
            }
            break;
        }
        case FFX_API_QUERY_DESC_TYPE_FRAMEGENERATION_GPU_MEMORY_USAGE_V2: {
            const auto& d = reinterpret_cast<const ffxQueryDescFrameGenerationGetGPUMemoryUsageV2&>(item);
            out << " device=" << ptr(d.device)
                << " maxRender=" << d.maxRenderSize.width << 'x' << d.maxRenderSize.height
                << " display=" << d.displaySize.width << 'x' << d.displaySize.height
                << " createFlags=" << decode_create_flags(d.createFlags)
                << " dispatchFlags=" << decode_dispatch_flags(d.dispatchFlags)
                << " backbufferFormat=" << surface_format(d.backBufferFormat)
                << " hudlessFormat=" << surface_format(d.hudlessBackBufferFormat)
                << " out=" << ptr(d.gpuMemoryUsageFrameGeneration);
            break;
        }
        case FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_INTERPOLATIONCOMMANDLIST_DX12: {
            const auto& d = reinterpret_cast<const ffxQueryDescFrameGenerationSwapChainInterpolationCommandListDX12&>(item);
            out << " outCommandList=" << ptr(d.pOutCommandList);
            if (d.pOutCommandList) out << " commandList=" << ptr(*d.pOutCommandList);
            break;
        }
        case FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_INTERPOLATIONTEXTURE_DX12: {
            const auto& d = reinterpret_cast<const ffxQueryDescFrameGenerationSwapChainInterpolationTextureDX12&>(item);
            out << " outTexture=" << ptr(d.pOutTexture);
            if (d.pOutTexture) out << " texture={" << resource(*d.pOutTexture) << '}';
            break;
        }
        case FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_GPU_MEMORY_USAGE_DX12: {
            const auto& d = reinterpret_cast<const ffxQueryFrameGenerationSwapChainGetGPUMemoryUsageDX12&>(item);
            out << " out=" << ptr(d.gpuMemoryUsageFrameGenerationSwapchain);
            break;
        }
        case FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_GPU_MEMORY_USAGE_DX12_V2: {
            const auto& d = reinterpret_cast<const ffxQueryFrameGenerationSwapChainGetGPUMemoryUsageDX12V2&>(item);
            out << " device=" << ptr(d.device)
                << " display=" << d.displaySize.width << 'x' << d.displaySize.height
                << " backbufferFormat=" << surface_format(d.backBufferFormat)
                << " backBufferCount=" << d.backBufferCount
                << " uiSize=" << d.uiResourceSize.width << 'x' << d.uiResourceSize.height
                << " uiFormat=" << surface_format(d.uiResourceFormat)
                << " flags=" << decode_ui_flags(d.flags)
                << " out=" << ptr(d.gpuMemoryUsageFrameGenerationSwapchain);
            break;
        }
        case FFX_API_DESC_TYPE_OVERRIDE_VERSION: {
            const auto& d = reinterpret_cast<const ffxOverrideVersion&>(item);
            out << " versionId=" << hx(d.versionId);
            break;
        }
        default:
            break;
        }
    });
}

unsigned kind_index(FfxExportKind kind) noexcept
{
    return static_cast<unsigned>(kind);
}

std::array<unsigned, 5> g_calls{};
std::array<std::set<uint64_t>, 5> g_seen_types{};
std::mutex g_call_mutex;

} // namespace

const char* ffx_export_name(FfxExportKind kind) noexcept
{
    switch (kind) {
    case FfxExportKind::Create: return "ffxCreateContext";
    case FfxExportKind::Destroy: return "ffxDestroyContext";
    case FfxExportKind::Configure: return "ffxConfigure";
    case FfxExportKind::Query: return "ffxQuery";
    case FfxExportKind::Dispatch: return "ffxDispatch";
    default: return "unknown";
    }
}

const char* ffx_return_code_name(ffxReturnCode_t code) noexcept
{
    return rc_name(code);
}


void describe_call(FfxExportKind kind, const ffxApiHeader* desc,
                   ffxReturnCode_t result, void* context) noexcept
{
    try {
        const int level = log_level();
        if (level < 1) return;

        const unsigned index = kind_index(kind);
        const auto types = collect_types(desc);

        bool new_type = false;
        unsigned number = 0;
        {
            std::lock_guard<std::mutex> lock(g_call_mutex);
            number = ++g_calls[index];
            for (uint64_t type : types) {
                if (g_seen_types[index].insert(type).second) new_type = true;
            }
        }

        const bool should_log = level >= 2 || number <= kFullLogCallLimit || new_type || result != FFX_API_RETURN_OK;
        if (!should_log) return;

        std::ostringstream out;
        out << ffx_export_name(kind) << " #" << number
            << " ctx=" << ptr(context)
            << " result=" << result << " (" << rc_name(result) << ") chain=";
        if (types.empty()) {
            out << "none";
        } else {
            for (size_t i = 0; i < types.size(); ++i) {
                if (i) out << " -> ";
                out << hx(types[i]) << " (" << type_name(types[i]) << ')';
            }
        }

        switch (kind) {
        case FfxExportKind::Create: describe_create(desc, out); break;
        case FfxExportKind::Configure: describe_configure(desc, out); break;
        case FfxExportKind::Query: describe_query(desc, out, result); break;
        case FfxExportKind::Dispatch: describe_dispatch(desc, out); break;
        case FfxExportKind::Destroy: break;
        }

        log_event(1, out.str());
    } catch (...) {
        log_error("failed to describe an FFX call");
    }
}

#pragma warning(pop)
