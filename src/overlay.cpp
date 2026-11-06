#include "overlay.h"

#include "log.h"

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

// Opaque, non-null context for Sedici's own callback. FFX only passes it back,
// but a non-null value avoids any implementation that treats a null user
// context as "no callback". The game's own context is kept separately above.
int g_callback_token = 0;

void* callback_token() noexcept
{
    return static_cast<void*>(&g_callback_token);
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

void record_present(const ffxCallbackDescFrameGenerationPresent* params) noexcept
{
    const uint64_t now = qpc_now();

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

    const FfxApiPresentCallbackFunc chained = g_game_callback.load(std::memory_order_acquire);
    if (chained && chained != &overlay_trampoline) {
        void* context = g_game_context.load(std::memory_order_relaxed);
        try {
            return chained(params, context);
        } catch (...) {
            return FFX_API_RETURN_OK;
        }
    }
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
        std::set<const ffxApiHeader*> seen;
        for (const ffxApiHeader* item = desc; item; item = item->pNext) {
            if (!seen.insert(item).second) break;

            switch (item->type) {
            case FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION: {
                auto* d = const_cast<ffxConfigureDescFrameGeneration*>(
                    reinterpret_cast<const ffxConfigureDescFrameGeneration*>(item));

                // Remember what the game asked for. It is null in Cyberpunk;
                // storing it unconditionally also handles a game that clears
                // its callback again later.
                if (d->presentCallback != &overlay_trampoline) {
                    g_game_callback.store(d->presentCallback, std::memory_order_relaxed);
                    g_game_context.store(d->presentCallbackUserContext, std::memory_order_relaxed);

                    patch(&d->presentCallback, sizeof(d->presentCallback),
                          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&overlay_trampoline)));
                    patch(&d->presentCallbackUserContext, sizeof(d->presentCallbackUserContext),
                          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(callback_token())));
                }
                break;
            }
            case FFX_API_FRAME_GENERATION_CONFIG: {
                auto* d = const_cast<FfxFrameGenerationConfig*>(
                    reinterpret_cast<const FfxFrameGenerationConfig*>(item));

                if (d->presentCallback != &overlay_trampoline) {
                    g_game_callback.store(d->presentCallback, std::memory_order_relaxed);
                    g_game_context.store(d->presentCallbackContext, std::memory_order_relaxed);

                    patch(&d->presentCallback, sizeof(d->presentCallback),
                          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&overlay_trampoline)));
                    patch(&d->presentCallbackContext, sizeof(d->presentCallbackContext),
                          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(callback_token())));
                }
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
