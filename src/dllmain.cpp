#include "describe.h"
#include "config.h"
#include "forward.h"
#include "log.h"
#include "overlay.h"
#include "present_hook.h"
#include "stats.h"
#include "toggles.h"

#include <windows.h>

extern "C" const char g_sedici_proxy_marker[] = "SEDICI_PROXY_V1";

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}

extern "C" ffxReturnCode_t ffxCreateContext(ffxContext* context,
                                            ffxCreateContextDescHeader* desc,
                                            const ffxAllocationCallbacks* callbacks)
{
    // Scoped: the original DLL sees the toggled desc, the caller keeps its own.
    // The override is still in effect for describe_call, so the log shows what
    // was actually forwarded, and is restored on return.
    (void)sedici::settings();
    ScopedDescOverride desc_override(desc);

    const bool loaded = ensure_original_loaded();
    const ffxReturnCode_t result = loaded && g_ffxCreateContext
        ? g_ffxCreateContext(context, desc, callbacks)
        : FFX_API_RETURN_ERROR;

    void* created = (result == FFX_API_RETURN_OK && context) ? *context : nullptr;
    describe_call(FfxExportKind::Create, desc, result, created);

    if (result == FFX_API_RETURN_OK) {
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
    const bool loaded = ensure_original_loaded();
    const ffxReturnCode_t result = loaded && g_ffxDispatch
        ? g_ffxDispatch(context, desc)
        : FFX_API_RETURN_ERROR;

    if (result == FFX_API_RETURN_OK) stats_on_dispatch(desc);
    describe_call(FfxExportKind::Dispatch, desc, result, existing);
    return result;
}
