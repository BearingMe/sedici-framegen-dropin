// Stub "original" FFX frame-generation DLL for the export-level harness.
// See tests/abi_stub.h. Built only with SEDICI_BUILD_TESTS; never deployed.

#include "abi_stub.h"

#include <cstring>

namespace {

StubState g_state{};

void record(StubExport kind, ffxContext* context, const ffxApiHeader* desc, const void* callbacks)
{
    StubCall& call = g_state.last[kind];
    call = StubCall{};
    call.contextArg = context;
    call.contextValue = context ? *context : nullptr;
    call.desc = desc;
    call.type = desc ? desc->type : 0;
    call.callbacks = callbacks;
    if (desc && desc->type == kStubProbeType) std::memcpy(call.probe, desc, sizeof(StubProbe));
    ++g_state.calls[kind];
}

} // namespace

extern "C" __declspec(dllexport) StubState* stub_state()
{
    return &g_state;
}

ffxReturnCode_t ffxCreateContext(ffxContext* context, ffxCreateContextDescHeader* desc,
                                 const ffxAllocationCallbacks* memCb)
{
    record(StubCreate, context, desc, memCb);
    for (const ffxApiHeader* item = desc; item; item = item->pNext) {
        if (item->type == FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION) {
            g_state.sawFgCreate = true;
            g_state.fgCreateFlags = reinterpret_cast<const ffxCreateContextDescFrameGeneration*>(item)->flags;
        }
    }
    const ffxReturnCode_t result = g_state.script[StubCreate];
    if (result == FFX_API_RETURN_OK && context)
        *context = reinterpret_cast<ffxContext>(0x10000 + 0x10 * ++g_state.nextHandle);
    return result;
}

ffxReturnCode_t ffxDestroyContext(ffxContext* context, const ffxAllocationCallbacks* memCb)
{
    record(StubDestroy, context, nullptr, memCb);
    return g_state.script[StubDestroy];
}

ffxReturnCode_t ffxConfigure(ffxContext* context, const ffxConfigureDescHeader* desc)
{
    record(StubConfigure, context, desc, nullptr);
    for (const ffxApiHeader* item = desc; item; item = item->pNext) {
        if (item->type == FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_KEYVALUE_DX12) {
            const auto* keyValue = reinterpret_cast<const ffxConfigureDescFrameGenerationSwapChainKeyValueDX12*>(item);
            if (keyValue->key == FFX_API_CONFIGURE_FG_SWAPCHAIN_KEY_FRAMEPACINGTUNING && keyValue->ptr) {
                ++g_state.pacingSends;
                g_state.tuning = *static_cast<const FfxApiSwapchainFramePacingTuning*>(keyValue->ptr);
            }
        } else if (item->type == FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION) {
            const auto* fg = reinterpret_cast<const ffxConfigureDescFrameGeneration*>(item);
            g_state.sawFgConfigure = true;
            g_state.fgConfigureAllowAsync = fg->allowAsyncWorkloads;
            g_state.fgConfigurePresentCallback = fg->presentCallback != nullptr;
        }
    }
    return g_state.script[StubConfigure];
}

ffxReturnCode_t ffxQuery(ffxContext* context, ffxQueryDescHeader* desc)
{
    record(StubQuery, context, desc, nullptr);
    return g_state.script[StubQuery];
}

ffxReturnCode_t ffxDispatch(ffxContext* context, const ffxDispatchDescHeader* desc)
{
    record(StubDispatch, context, desc, nullptr);
    return g_state.script[StubDispatch];
}
