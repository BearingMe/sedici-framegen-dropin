#include "overlay.h"

#include "log.h"
#include "overlay_compose.h"
#include "timing.h"
#include "ui.h"

#include "ffx_api_framegeneration_dx12.h"

#include <windows.h>

#include <atomic>
#include <array>
#include <cstring>
#include <iomanip>
#include <iterator>
#include <mutex>
#include <set>
#include <sstream>
#include <string>

namespace {

constexpr size_t kMaxChainItems = 64;
constexpr uint64_t kReportEvery = 300;

struct CallbackBinding {
    FfxApiPresentCallbackFunc callback = nullptr;
    void* context = nullptr;
};
// FFX owns callback scheduling. Immutable bindings remain alive for the process
// lifetime, including callbacks still in flight after configure/destroy. The
// bounded pool fails open to the game's original callback when exhausted.
std::array<CallbackBinding, 128> g_bindings{};
size_t g_binding_count = 0;
std::mutex g_binding_mutex;

CallbackBinding* binding_for(FfxApiPresentCallbackFunc callback, void* context)
{
    std::lock_guard<std::mutex> lock(g_binding_mutex);
    for (size_t i = 0; i < g_binding_count; ++i)
        if (g_bindings[i].callback == callback && g_bindings[i].context == context)
            return &g_bindings[i];
    if (g_binding_count == g_bindings.size()) return nullptr;
    auto* binding = &g_bindings[g_binding_count++];
    *binding = CallbackBinding{callback, context};
    return binding;
}

struct Registration {
    void* context = nullptr; // swapchain context used by REGISTERUIRESOURCE
    void* swapchain = nullptr; // identity used by the separate FG configure
    bool ui = false;
    bool used = false;
};
std::array<Registration, 32> g_registrations{};
std::mutex g_registration_mutex;

// Caller holds g_registration_mutex. Preserve swapchain identity when a UI
// registration arrives; the FG and swapchain configure calls use different contexts.
Registration* registration_slot(void* context)
{
    Registration* empty = nullptr;
    for (auto& registration : g_registrations) {
        if (registration.used && registration.context == context) return &registration;
        if (!registration.used && !empty) empty = &registration;
    }
    return empty;
}

bool registered_ui_for_swapchain(void* swapchain)
{
    if (!swapchain) return false;
    std::lock_guard<std::mutex> lock(g_registration_mutex);
    for (const auto& registration : g_registrations)
        if (registration.used && registration.swapchain == swapchain && registration.ui)
            return true;
    return false;
}

// Set when a callback Sedici installed could not compose safely. The next
// configure then leaves the callback null and FFX's default takes over.
std::atomic<bool> g_composition_failed{false};

// Mode already reported in the log: 1 copy, 2 FFX default, 3 chained.
std::atomic<int> g_logged_mode{0};

void log_mode_once(int mode, const char* message) noexcept
{
    if (g_logged_mode.exchange(mode, std::memory_order_relaxed) == mode) return;
    log_event(1, message);
}

uint64_t qpc_now() noexcept
{
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return static_cast<uint64_t>(value.QuadPart);
}

const uint64_t g_frequency = [] {
    LARGE_INTEGER frequency{};
    return QueryPerformanceFrequency(&frequency) ? static_cast<uint64_t>(frequency.QuadPart) : 0;
}();

double ticks_to_ms(uint64_t ticks) noexcept
{
    if (!g_frequency) return 0.0;
    return static_cast<double>(ticks) * 1000.0 / static_cast<double>(g_frequency);
}

std::string pointer_text(const void* value)
{
    std::ostringstream out;
    out << "0x" << std::hex << reinterpret_cast<uintptr_t>(value);
    return out.str();
}

// Callback timing window. Guarded by g_mutex: FFX may present generated frames
// from a different thread than real frames, and a torn interval would be worse
// than the cost of the lock on a 4 s boundary.
struct Window {
    uint64_t calls = 0;
    uint64_t real = 0;
    uint64_t generated = 0;
    bool have_first = false;
    uint64_t first = 0;
    uint64_t last = 0;
    bool logged_active = false;
};

std::mutex g_mutex;
Window g_window;

std::string active_line_locked(const ffxCallbackDescFrameGenerationPresent& params)
{
    const FfxApiResource& output = params.outputSwapChainBuffer;
    std::ostringstream out;
    out << "sedici: overlay present callback active"
        << " device=" << pointer_text(params.device)
        << " commandList=" << pointer_text(params.commandList)
        << " output=" << pointer_text(output.resource)
        << " size=" << output.description.width << 'x' << output.description.height
        << " format=" << output.description.format
        << " state=" << output.state
        << " ui=" << (params.currentUI.resource ? "yes" : "no");
    return out.str();
}

std::string stats_line_locked()
{
    const uint64_t intervals = g_window.calls > 0 ? g_window.calls - 1 : 0;
    const double span_ms = g_window.have_first ? ticks_to_ms(g_window.last - g_window.first) : 0.0;
    const double seconds = span_ms / 1000.0;
    const double output_fps = seconds > 0.0 ? static_cast<double>(intervals) / seconds : 0.0;
    const double real_fps = seconds > 0.0 ? static_cast<double>(g_window.real) / seconds : 0.0;

    std::ostringstream out;
    out << std::fixed << std::setprecision(3);
    out << "sedici: overlay present frames=" << g_window.calls
        << " real=" << g_window.real
        << " generated=" << g_window.generated
        << " output_fps=" << std::setprecision(2) << output_fps
        << " real_fps=" << real_fps
        << " span_ms=" << std::setprecision(1) << span_ms;
    return out.str();
}

void record_present(const ffxCallbackDescFrameGenerationPresent* params)
{
    const uint64_t now = qpc_now();
    sedici::timing_on_output_present(now);

    std::string active;
    std::string stats;
    {
        std::lock_guard<std::mutex> lock(g_mutex);

        if (!g_window.logged_active) {
            g_window.logged_active = true;
            active = active_line_locked(*params);
        }

        if (!g_window.have_first) {
            g_window.have_first = true;
            g_window.first = now;
        }
        g_window.last = now;

        ++g_window.calls;
        if (params->isGeneratedFrame) {
            ++g_window.generated;
        } else {
            ++g_window.real;
        }

        if (g_window.calls >= kReportEvery) {
            stats = stats_line_locked();
            g_window.calls = 0;
            g_window.real = 0;
            g_window.generated = 0;
            g_window.have_first = false;
            g_window.first = 0;
            g_window.last = 0;
        }
    }

    if (!active.empty()) log_event(1, active);
    if (!stats.empty()) log_event(1, stats);
}

ffxReturnCode_t overlay_trampoline(ffxCallbackDescFrameGenerationPresent* params, void* user) noexcept
{
    overlay_on_present(params);
    if (!params) return FFX_API_RETURN_OK;

    const auto* binding = static_cast<const CallbackBinding*>(user);
    const FfxApiPresentCallbackFunc chained = binding ? binding->callback : nullptr;
    if (chained && chained != &overlay_trampoline) {
        try {
            return chained(params, binding->context);
        } catch (...) {
            return FFX_API_RETURN_OK;
        }
    }

    // A separate UI resource needs FFX's composition shader. Taking over here
    // would silently drop the UI, so fail closed and let the next configure
    // restore FFX's own callback.
    if (params->currentUI.resource != nullptr) {
        if (!g_composition_failed.exchange(true, std::memory_order_relaxed)) {
            sedici::ui_disable("a separate UI resource needs FFX composition");
            log_error("sedici: overlay callback received a separate UI resource; composition "
                      "is handed back to FFX and the overlay is disabled for this session");
        }
        return FFX_API_RETURN_OK;
    }

    const bool copied = sedici::copy_back_buffer(
        reinterpret_cast<ID3D12GraphicsCommandList*>(params->commandList),
        reinterpret_cast<ID3D12Resource*>(params->currentBackBuffer.resource),
        params->currentBackBuffer.state,
        reinterpret_cast<ID3D12Resource*>(params->outputSwapChainBuffer.resource),
        params->outputSwapChainBuffer.state);
    if (!copied) {
        if (!g_composition_failed.exchange(true, std::memory_order_relaxed)) {
            sedici::ui_disable("could not copy the output image");
            log_error("sedici: overlay callback could not copy the output image; composition "
                      "is handed back to FFX");
        }
        return FFX_API_RETURN_OK;
    }

    sedici::ui_on_present(params);
    return FFX_API_RETURN_OK;
}

} // namespace

void ScopedOverlayCallback::patch(void* address, size_t size, uint64_t value) noexcept
{
    if (!address || size == 0 || size > sizeof(uint64_t) || m_count >= std::size(m_restore)) return;

    uint64_t saved = 0;
    std::memcpy(&saved, address, size);
    if (saved == value) return;

    std::memcpy(address, &value, size);
    m_restore[m_count++] = Restore{address, size, saved};
}

ScopedOverlayCallback::ScopedOverlayCallback(const ffxApiHeader* desc, void* context) noexcept
    : m_context(context)
{
    if (!desc) return;

    try {
        {
            std::lock_guard<std::mutex> lock(g_registration_mutex);
            for (const auto& registration : g_registrations)
                if (registration.used && registration.context == context) {
                    m_ui_registered = registration.ui;
                    break;
                }
        }
        // A UI registration may appear anywhere in the chain, and the decision
        // below must see it regardless of order.
        std::set<const ffxApiHeader*> seen;
        for (const ffxApiHeader* item = desc; item; item = item->pNext) {
            if (!seen.insert(item).second) break;
            if (item->type ==
                FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_REGISTERUIRESOURCE_DX12) {
                const auto& d = reinterpret_cast<
                    const ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12&>(*item);
                m_ui_registered = d.uiResource.resource != nullptr;
                m_registration_changed = true;
            }
            if (seen.size() >= kMaxChainItems) break;
        }

        seen.clear();
        for (const ffxApiHeader* item = desc; item; item = item->pNext) {
            if (!seen.insert(item).second) break;

            switch (item->type) {
            case FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION: {
                auto* d = const_cast<ffxConfigureDescFrameGeneration*>(
                    reinterpret_cast<const ffxConfigureDescFrameGeneration*>(item));
                if (d->presentCallback == &overlay_trampoline) break;

                const bool game_callback = d->presentCallback != nullptr;
                const bool ui_registered = m_registration_changed ? m_ui_registered
                    : m_ui_registered || registered_ui_for_swapchain(d->swapChain);
                if (!game_callback && (ui_registered || g_composition_failed.load())) {
                    m_timing_only = true;
                    log_mode_once(2, "sedici: overlay present callback left to FFX (separate UI "
                                     "resource registered or composition unavailable)");
                    break;
                }

                CallbackBinding* binding = binding_for(d->presentCallback, d->presentCallbackUserContext);
                if (!binding) { m_timing_only = true; break; }
                m_timing_only = game_callback;

                patch(&d->presentCallback, sizeof(d->presentCallback),
                      static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&overlay_trampoline)));
                patch(&d->presentCallbackUserContext, sizeof(d->presentCallbackUserContext),
                      static_cast<uint64_t>(reinterpret_cast<uintptr_t>(binding)));
                log_mode_once(game_callback ? 3 : 1,
                              game_callback
                                  ? "sedici: overlay present callback chaining to the game "
                                    "callback; Sedici records timing only"
                                  : "sedici: overlay present callback active; copying the current "
                                    "back buffer to the output");
                break;
            }
            case FFX_API_FRAME_GENERATION_CONFIG: {
                auto* d = const_cast<FfxFrameGenerationConfig*>(
                    reinterpret_cast<const FfxFrameGenerationConfig*>(item));
                if (d->presentCallback == &overlay_trampoline) break;

                const bool game_callback = d->presentCallback != nullptr;
                const bool ui_registered = m_registration_changed ? m_ui_registered
                    : m_ui_registered || registered_ui_for_swapchain(d->swapChain);
                if (!game_callback && (ui_registered || g_composition_failed.load())) {
                    m_timing_only = true;
                    log_mode_once(2, "sedici: overlay present callback left to FFX (separate UI "
                                     "resource registered or composition unavailable)");
                    break;
                }

                CallbackBinding* binding = binding_for(d->presentCallback, d->presentCallbackContext);
                if (!binding) { m_timing_only = true; break; }
                m_timing_only = game_callback;

                patch(&d->presentCallback, sizeof(d->presentCallback),
                      static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&overlay_trampoline)));
                patch(&d->presentCallbackContext, sizeof(d->presentCallbackContext),
                      static_cast<uint64_t>(reinterpret_cast<uintptr_t>(binding)));
                log_mode_once(game_callback ? 3 : 1,
                              game_callback
                                  ? "sedici: overlay present callback chaining to the game "
                                    "callback; Sedici records timing only"
                                  : "sedici: overlay present callback active; copying the current "
                                    "back buffer to the output");
                break;
            }
            default:
                break;
            }

            if (seen.size() >= kMaxChainItems) break;
        }
    } catch (...) {
        log_error("failed to install the present callback");
    }
}

void ScopedOverlayCallback::complete(ffxReturnCode_t result) noexcept
{
    if (result != FFX_API_RETURN_OK) return;
    try {
        if (m_registration_changed) {
            std::lock_guard<std::mutex> lock(g_registration_mutex);
            Registration* slot = registration_slot(m_context);
            if (slot) {
                slot->context = m_context;
                slot->ui = m_ui_registered;
                slot->used = true;
            }
            else g_composition_failed.store(true);
            if (m_ui_registered || !slot)
                sedici::ui_disable("a separate UI resource was registered or tracking is full");
        }
        if (m_timing_only) sedici::ui_disable("FFX or the game owns UI composition");
    } catch (...) {
        sedici::ui_disable("could not record the UI composition state");
    }
}

void overlay_after_create(void* context, const ffxCreateContextDescHeader* desc) noexcept
{
    if (!context) return;
    try {
        void* swapchain = nullptr;
        for (size_t i = 0; desc && i < kMaxChainItems; ++i, desc = desc->pNext) {
            IDXGISwapChain4** output = nullptr;
            switch (desc->type) {
            case FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WRAP_DX12:
                output = reinterpret_cast<const ffxCreateContextDescFrameGenerationSwapChainWrapDX12*>(desc)->swapchain;
                break;
            case FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_NEW_DX12:
                output = reinterpret_cast<const ffxCreateContextDescFrameGenerationSwapChainNewDX12*>(desc)->swapchain;
                break;
            case FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12:
                output = reinterpret_cast<const ffxCreateContextDescFrameGenerationSwapChainForHwndDX12*>(desc)->swapchain;
                break;
            default: break;
            }
            if (output && *output) { swapchain = *output; break; }
        }
        if (!swapchain) return;
        std::lock_guard<std::mutex> lock(g_registration_mutex);
        Registration* slot = registration_slot(context);
        if (slot) *slot = Registration{context, swapchain, false, true};
        else {
            g_composition_failed.store(true);
            sedici::ui_disable("swapchain composition tracking is full");
        }
    } catch (...) {
        g_composition_failed.store(true);
        sedici::ui_disable("could not track the swapchain for UI composition");
    }
}

void overlay_on_destroy(void* context) noexcept
{
    try {
        std::lock_guard<std::mutex> lock(g_registration_mutex);
        for (auto& registration : g_registrations)
            if (registration.context == context) registration = {};
    } catch (...) {
    }
}

ScopedOverlayCallback::~ScopedOverlayCallback()
{
    for (size_t i = m_count; i > 0; --i) {
        const Restore& entry = m_restore[i - 1];
        uint64_t saved = entry.saved;
        std::memcpy(entry.address, &saved, entry.size);
    }
    m_count = 0;
}

void overlay_draw_output(ID3D12Device* device, ID3D12GraphicsCommandList* list,
                         ID3D12Resource* backbuffer, bool generated) noexcept
{
    if (!device || !list || !backbuffer) return;
    // The fields ui_on_present reads, as AMD's callback fills them when no
    // separate UI resource is registered.
    ffxCallbackDescFrameGenerationPresent params{};
    params.device = device;
    params.commandList = list;
    params.outputSwapChainBuffer.resource = backbuffer;
    params.outputSwapChainBuffer.state = FFX_API_RESOURCE_STATE_PRESENT;
    params.isGeneratedFrame = generated;
    sedici::ui_on_present(&params);
}

void overlay_output_presented(bool generated) noexcept
{
    ffxCallbackDescFrameGenerationPresent params{};
    params.isGeneratedFrame = generated;
    overlay_on_present(&params);
}

void overlay_on_present(const ffxCallbackDescFrameGenerationPresent* params) noexcept
{
    if (!params) return;
    try {
        record_present(params);
    } catch (...) {
    }
}
