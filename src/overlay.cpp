#include "overlay.h"

#include "log.h"
#include "overlay_compose.h"
#include "timing.h"
#include "ui.h"

#include "ffx_api_framegeneration_dx12.h"

#include <windows.h>

#include <atomic>
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

// The game's own callback, if it set one. FFX calls the callback from its
// present thread while the game is on the render thread, so these are atomic.
// A frame generation context has no user context of its own in the callback
// signature, so a single chain target is all that can be represented; in
// practice Cyberpunk passes null.
std::atomic<FfxApiPresentCallbackFunc> g_game_callback{nullptr};
std::atomic<void*> g_game_context{nullptr};

// True while the application has registered a non-empty UI resource. In that
// configuration FFX's default callback performs a shader-based composition
// that Sedici does not reproduce; Sedici then leaves the callback null so FFX
// keeps its own path.
std::atomic<bool> g_ui_registered{false};

// Set when a callback Sedici installed could not compose safely. The next
// configure then leaves the callback null and FFX's default takes over.
std::atomic<bool> g_composition_failed{false};

// Mode already reported in the log: 1 copy, 2 FFX default, 3 chained.
std::atomic<int> g_logged_mode{0};

// Opaque, non-null context for Sedici's own callback. FFX only passes it back,
// but a non-null value avoids any implementation that treats a null user
// context as "no callback". The game's own context is kept separately above.
int g_callback_token = 0;

void* callback_token() noexcept
{
    return static_cast<void*>(&g_callback_token);
}

void log_mode_once(int mode, const char* message) noexcept
{
    if (g_logged_mode.exchange(mode, std::memory_order_relaxed) == mode) return;
    log_event(1, message);
}

bool can_compose_here() noexcept
{
    return !g_ui_registered.load(std::memory_order_relaxed) &&
           !g_composition_failed.load(std::memory_order_relaxed);
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
        << " ui=" << (params.currentUI.resource ? "yes" : "no")
        << " composition=" << (g_game_callback.load(std::memory_order_relaxed) ? "chain" : "copy");
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

void record_present(const ffxCallbackDescFrameGenerationPresent* params) noexcept
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

ffxReturnCode_t overlay_trampoline(ffxCallbackDescFrameGenerationPresent* params, void*) noexcept
{
    overlay_on_present(params);
    if (!params) return FFX_API_RETURN_OK;

    const FfxApiPresentCallbackFunc chained = g_game_callback.load(std::memory_order_acquire);
    if (chained && chained != &overlay_trampoline) {
        void* context = g_game_context.load(std::memory_order_relaxed);
        try {
            return chained(params, context);
        } catch (...) {
            return FFX_API_RETURN_OK;
        }
    }

    // A separate UI resource needs FFX's composition shader. Taking over here
    // would silently drop the UI, so fail closed and let the next configure
    // restore FFX's own callback.
    if (params->currentUI.resource != nullptr) {
        if (!g_composition_failed.exchange(true, std::memory_order_relaxed)) {
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

ScopedOverlayCallback::ScopedOverlayCallback(const ffxApiHeader* desc) noexcept
{
    if (!desc) return;

    try {
        // A UI registration may appear anywhere in the chain, and the decision
        // below must see it regardless of order.
        std::set<const ffxApiHeader*> seen;
        for (const ffxApiHeader* item = desc; item; item = item->pNext) {
            if (!seen.insert(item).second) break;
            if (item->type ==
                FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_REGISTERUIRESOURCE_DX12) {
                const auto& d = reinterpret_cast<
                    const ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12&>(*item);
                g_ui_registered.store(d.uiResource.resource != nullptr, std::memory_order_relaxed);
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
                g_game_callback.store(d->presentCallback, std::memory_order_relaxed);
                g_game_context.store(d->presentCallbackUserContext, std::memory_order_relaxed);

                if (!game_callback && !can_compose_here()) {
                    log_mode_once(2, "sedici: overlay present callback left to FFX (separate UI "
                                     "resource registered or composition unavailable)");
                    break;
                }

                patch(&d->presentCallback, sizeof(d->presentCallback),
                      static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&overlay_trampoline)));
                patch(&d->presentCallbackUserContext, sizeof(d->presentCallbackUserContext),
                      static_cast<uint64_t>(reinterpret_cast<uintptr_t>(callback_token())));
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
                g_game_callback.store(d->presentCallback, std::memory_order_relaxed);
                g_game_context.store(d->presentCallbackContext, std::memory_order_relaxed);

                if (!game_callback && !can_compose_here()) {
                    log_mode_once(2, "sedici: overlay present callback left to FFX (separate UI "
                                     "resource registered or composition unavailable)");
                    break;
                }

                patch(&d->presentCallback, sizeof(d->presentCallback),
                      static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&overlay_trampoline)));
                patch(&d->presentCallbackContext, sizeof(d->presentCallbackContext),
                      static_cast<uint64_t>(reinterpret_cast<uintptr_t>(callback_token())));
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

ScopedOverlayCallback::~ScopedOverlayCallback()
{
    for (size_t i = m_count; i > 0; --i) {
        const Restore& entry = m_restore[i - 1];
        uint64_t saved = entry.saved;
        std::memcpy(entry.address, &saved, entry.size);
    }
    m_count = 0;
}

void overlay_on_present(const ffxCallbackDescFrameGenerationPresent* params) noexcept
{
    if (!params) return;
    try {
        record_present(params);
    } catch (...) {
    }
}
