#include "describe.h"
#include "config.h"
#include "fg_route.h"
#include "forward.h"
#include "limiter.h"
#include "log.h"
#include "overlay.h"
#include "present_hook.h"
#include "stats.h"
#include "timing.h"
#include "toggles.h"
#include "ui.h"

#include "ffx_api_framegeneration_dx12.h"

#include <windows.h>

extern "C" const char g_sedici_proxy_marker[] = "SEDICI_PROXY_V1";

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}

namespace {

// One call per game frame on Sedici's own swapchain: the real-frame timing the
// overlay shows, then the limiter, in the order the native Present hook uses.
void sedici_game_present(uint64_t now_qpc) noexcept
{
    sedici::timing_on_real_present(now_qpc);
    limiter_before_present(now_qpc);
}

// The overlay's hotkey and cursor handling need the game window, which the
// native path records in toggles_after_create.
void capture_wrapped_window(const ffxCreateContextDescHeader* desc) noexcept
{
    try {
        for (const ffxApiHeader* item = desc; item; item = item->pNext) {
            if (item->type != FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WRAP_DX12) continue;
            const auto& wrap = reinterpret_cast<const ffxCreateContextDescFrameGenerationSwapChainWrapDX12&>(*item);
            HWND window = nullptr;
            if (wrap.swapchain && *wrap.swapchain && SUCCEEDED((*wrap.swapchain)->GetHwnd(&window)) && window)
                sedici::ui_capture_window(window);
            return;
        }
    } catch (...) {
    }
}

} // namespace

extern "C" ffxReturnCode_t ffxCreateContext(ffxContext* context,
                                            ffxCreateContextDescHeader* desc,
                                            const ffxAllocationCallbacks* callbacks)
{
    // Scoped: the original DLL sees the toggled desc, the caller keeps its own.
    // The override is still in effect for describe_call, so the log shows what
    // was actually forwarded, and is restored on return.
    (void)sedici::settings();
    ScopedDescOverride desc_override(desc);

    // Under Sedici ownership the limiter attaches to Sedici's own wrapper, which
    // is the only place that still sees one call per application frame, and the
    // overlay draws from the wrapper instead of AMD's present callback.
    if (sedici::fg::routing_enabled()) {
        sedici::fg::route_set_present_hook(&sedici_game_present);
        sedici::fg::OutputHooks hooks;
        hooks.draw = &overlay_draw_output;
        hooks.presented = &overlay_output_presented;
        sedici::fg::route_set_output_hooks(hooks);
    }

    if (ffxReturnCode_t routed{}; sedici::fg::route_create(context, desc, &routed)) {
        describe_call(FfxExportKind::Create, desc, routed, context ? *context : nullptr);
        if (routed == FFX_API_RETURN_OK) capture_wrapped_window(desc);
        return routed;
    }

    const bool loaded = ensure_original_loaded();
    const ffxReturnCode_t result = loaded && g_ffxCreateContext
        ? g_ffxCreateContext(context, desc, callbacks)
        : FFX_API_RETURN_ERROR;

    void* created = (result == FFX_API_RETURN_OK && context) ? *context : nullptr;
    describe_call(FfxExportKind::Create, desc, result, created);

    if (result == FFX_API_RETURN_OK) {
        overlay_after_create(created, desc);
        present_hook_maybe_install(desc);
        toggles_after_create(context, desc);
    }
    return result;
}


extern "C" ffxReturnCode_t ffxDestroyContext(ffxContext* context,
                                               const ffxAllocationCallbacks* callbacks)
{
    (void)sedici::settings();
    std::lock_guard<std::recursive_mutex> lifetime(toggles_context_lifetime_mutex());
    void* existing = context ? *context : nullptr;

    if (ffxReturnCode_t routed{}; sedici::fg::route_destroy(context, &routed)) {
        describe_call(FfxExportKind::Destroy, nullptr, routed, existing);
        return routed;
    }

    const bool loaded = ensure_original_loaded();
    const ffxReturnCode_t result = loaded && g_ffxDestroyContext
        ? g_ffxDestroyContext(context, callbacks)
        : FFX_API_RETURN_ERROR;

    // Drop any pacing bookkeeping for this context so a recycled pointer does
    // not look like a context Sedici already tuned.
    if (result == FFX_API_RETURN_OK) {
        toggles_on_destroy(existing);
        overlay_on_destroy(existing);
    }

    describe_call(FfxExportKind::Destroy, nullptr, result, existing);
    return result;
}

extern "C" ffxReturnCode_t ffxConfigure(ffxContext* context,
                                        const ffxConfigureDescHeader* desc)
{
    (void)sedici::settings();
    ScopedDescOverride desc_override(desc);
    ScopedOverlayCallback overlay_callback(desc, context ? *context : nullptr);

    void* existing = context ? *context : nullptr;
    if (ffxReturnCode_t routed{}; sedici::fg::route_configure(context, desc, &routed)) {
        describe_call(FfxExportKind::Configure, desc, routed, existing);
        if (routed == FFX_API_RETURN_OK) toggles_note_fg_configure(desc);
        return routed;
    }

    const bool loaded = ensure_original_loaded();
    const ffxReturnCode_t result = loaded && g_ffxConfigure
        ? g_ffxConfigure(context, desc)
        : FFX_API_RETURN_ERROR;
    overlay_callback.complete(result);

    describe_call(FfxExportKind::Configure, desc, result, existing);
    if (result == FFX_API_RETURN_OK) toggles_after_configure(context, desc, result);
    return result;
}


extern "C" ffxReturnCode_t ffxQuery(ffxContext* context, ffxQueryDescHeader* desc)
{
    (void)sedici::settings();
    void* existing = context ? *context : nullptr;
    if (ffxReturnCode_t routed{}; sedici::fg::route_query(context, desc, &routed)) {
        describe_call(FfxExportKind::Query, desc, routed, existing);
        return routed;
    }

    const bool loaded = ensure_original_loaded();
    const ffxReturnCode_t result = loaded && g_ffxQuery
        ? g_ffxQuery(context, desc)
        : FFX_API_RETURN_ERROR;

    describe_call(FfxExportKind::Query, desc, result, existing);
    return result;
}

extern "C" ffxReturnCode_t ffxDispatch(ffxContext* context,
                                       const ffxDispatchDescHeader* desc)
{
    (void)sedici::settings();
    void* existing = context ? *context : nullptr;
    if (ffxReturnCode_t routed{}; sedici::fg::route_dispatch(context, desc, &routed)) {
        describe_call(FfxExportKind::Dispatch, desc, routed, existing);
        return routed;
    }

    const bool loaded = ensure_original_loaded();
    const ffxReturnCode_t result = loaded && g_ffxDispatch
        ? g_ffxDispatch(context, desc)
        : FFX_API_RETURN_ERROR;

    if (result == FFX_API_RETURN_OK) stats_on_dispatch(desc);
    describe_call(FfxExportKind::Dispatch, desc, result, existing);
    return result;
}
