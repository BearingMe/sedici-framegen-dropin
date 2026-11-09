#include "ui/overlay.h"

#include "diagnostics/log.h"
#include "diagnostics/timing.h"
#include "ui/ui.h"

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

constexpr uint64_t kReportEvery = 300;


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
        << " device=" << (params.device ? pointer_text(params.device) : std::string("n/a"))
        << " commandList=" << (params.commandList ? pointer_text(params.commandList) : std::string("n/a"))
        << " output=" << (output.resource ? pointer_text(output.resource) : std::string("n/a"))
        << " size=";
    if (output.resource) out << output.description.width << 'x' << output.description.height;
    else out << "n/a";
    out
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

} // namespace

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
