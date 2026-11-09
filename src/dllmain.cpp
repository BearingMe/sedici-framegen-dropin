#include "abi/describe.h"
#include "settings/config.h"
#include "abi/route.h"
#include "presentation/limiter.h"
#include "diagnostics/log.h"
#include "ui/overlay.h"
#include "diagnostics/stats.h"
#include "status/diag.h"
#include "diagnostics/timing.h"
#include "ui/ui.h"

#include "ffx_api_framegeneration_dx12.h"

#include <windows.h>

extern "C" const char g_sedici_proxy_marker[] = "SEDICI_PROXY_V1";

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        limiter_set_fg_probe(&sedici::fg::route_any_generation_enabled);
        stats_set_extra_line(&sedici::diag_notable_line); // the "diag:" line after each report
    } else if (reason == DLL_PROCESS_DETACH && reserved) {
        // The process is exiting and the log writer thread is already gone: write
        // what it had not yet written. Never waits on a lock (log_drain_at_exit).
        sedici::log_drain_at_exit();
    }
    return TRUE;
}

namespace {

// One call per game frame on Sedici's own swapchain: the real-frame timing the
// overlay shows, then the limiter, in the order the native Present hook uses.
void sedici_game_present(uint64_t now_qpc) noexcept
{
    sedici::timing_on_real_present(now_qpc);
    stats_on_present(now_qpc); // K99: present-interval statistics
    limiter_before_present(now_qpc);
}

// The overlay's hotkey and cursor handling need the game window.
void capture_wrapped_window(const ffxCreateContextDescHeader* desc) noexcept
{
    try {
        for (const ffxApiHeader* item = desc; item; item = item->pNext) {
            if (item->type == FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_NEW_DX12) {
                const auto& made = reinterpret_cast<const ffxCreateContextDescFrameGenerationSwapChainNewDX12&>(*item);
                if (made.desc && made.desc->OutputWindow) sedici::ui_capture_window(made.desc->OutputWindow);
                return;
            }
            if (item->type == FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12) {
                const auto& made = reinterpret_cast<const ffxCreateContextDescFrameGenerationSwapChainForHwndDX12&>(*item);
                if (made.hwnd) sedici::ui_capture_window(made.hwnd);
                return;
            }
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

// Every call is answered here. Sedici owns frame generation outright, so there is
// no second implementation to hand a call to. A handle Sedici never issued, and
// a descriptor it does not recognise, get the return codes below.
namespace {

constexpr ffxReturnCode_t kForeignHandle = FFX_API_RETURN_ERROR_PARAMETER;
constexpr ffxReturnCode_t kUnknownDesc = FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;

} // namespace

extern "C" ffxReturnCode_t ffxCreateContext(ffxContext* context,
                                            ffxCreateContextDescHeader* desc,
                                            const ffxAllocationCallbacks* /*callbacks*/)
{
    (void)sedici::settings();

    // The limiter attaches to Sedici's own wrapper, which is the only place that
    // sees one call per application frame, and the overlay draws from the wrapper.
    sedici::fg::route_set_present_hook(&sedici_game_present);
    sedici::fg::OutputHooks hooks;
    hooks.draw = &overlay_draw_output;
    hooks.presented = &overlay_output_presented;
    sedici::fg::route_set_output_hooks(hooks);

    ffxReturnCode_t routed = kUnknownDesc;
    if (!context || !desc) routed = kForeignHandle;
    else if (!sedici::fg::route_create(context, desc, &routed)) routed = kUnknownDesc;
    describe_call(FfxExportKind::Create, desc, routed, (routed == FFX_API_RETURN_OK && context) ? *context : nullptr);
    if (routed == FFX_API_RETURN_OK) capture_wrapped_window(desc);
    return routed;
}

extern "C" ffxReturnCode_t ffxDestroyContext(ffxContext* context,
                                             const ffxAllocationCallbacks* /*callbacks*/)
{
    (void)sedici::settings();
    void* existing = context ? *context : nullptr;
    ffxReturnCode_t routed = kForeignHandle;
    if (!sedici::fg::route_destroy(context, &routed)) routed = kForeignHandle;
    describe_call(FfxExportKind::Destroy, nullptr, routed, existing);
    return routed;
}

extern "C" ffxReturnCode_t ffxConfigure(ffxContext* context,
                                        const ffxConfigureDescHeader* desc)
{
    (void)sedici::settings();
    void* existing = context ? *context : nullptr;
    ffxReturnCode_t routed = kForeignHandle;
    if (!sedici::fg::route_configure(context, desc, &routed)) routed = kForeignHandle;
    describe_call(FfxExportKind::Configure, desc, routed, existing);
    return routed;
}

extern "C" ffxReturnCode_t ffxQuery(ffxContext* context, ffxQueryDescHeader* desc)
{
    (void)sedici::settings();
    void* existing = context ? *context : nullptr;
    ffxReturnCode_t routed = kForeignHandle;
    if (!sedici::fg::route_query(context, desc, &routed)) routed = kForeignHandle;
    describe_call(FfxExportKind::Query, desc, routed, existing);
    return routed;
}

extern "C" ffxReturnCode_t ffxDispatch(ffxContext* context,
                                       const ffxDispatchDescHeader* desc)
{
    (void)sedici::settings();
    void* existing = context ? *context : nullptr;
    ffxReturnCode_t routed = kForeignHandle;
    if (!sedici::fg::route_dispatch(context, desc, &routed)) routed = kForeignHandle;
    if (routed == FFX_API_RETURN_OK) stats_on_dispatch(desc);
    describe_call(FfxExportKind::Dispatch, desc, routed, existing);
    return routed;
}
