#include "limiter.h"

#include "config.h"
#include "limiter_schedule.h"
#include "live.h"
#include "log.h"
#include "toggles.h"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr size_t kReportEvery = 300;

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

double average(const std::vector<double>& values) noexcept
{
    if (values.empty()) return 0.0;
    double sum = 0.0;
    for (double value : values) sum += value;
    return sum / static_cast<double>(values.size());
}

double maximum(const std::vector<double>& values) noexcept
{
    return values.empty() ? 0.0 : *std::max_element(values.begin(), values.end());
}

double percentile(std::vector<double> values, double fraction) noexcept
{
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    size_t index = static_cast<size_t>(std::ceil(fraction * static_cast<double>(values.size())));
    if (index == 0) index = 1;
    if (index > values.size()) index = values.size();
    return values[index - 1];
}

std::string format_fps(double fps)
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(3) << fps;
    std::string text = out.str();
    while (!text.empty() && text.back() == '0') text.pop_back();
    if (!text.empty() && text.back() == '.') text.pop_back();
    return text;
}

struct Window {
    std::vector<double> intervals;
    std::vector<double> waits;
    std::vector<double> overshoots;
    uint64_t late = 0;
    uint64_t resets = 0;

    void clear() noexcept
    {
        intervals.clear();
        waits.clear();
        overshoots.clear();
        late = 0;
        resets = 0;
    }
};

struct State {
    bool initialized = false;
    bool disabled = false;
    bool timer_ready = false;
    bool timer_high_resolution = false;
    HANDLE timer = nullptr;

    uint64_t interval = 0; // QPC ticks
    uint64_t spin = 0;     // QPC ticks

    bool have_deadline = false;
    uint64_t deadline = 0;

    bool have_last_actual = false;
    uint64_t last_actual = 0;

    bool logged_mode = false;
    bool logged_fg = false;

    Window window;
};

std::mutex g_mutex;
State g_state;

// Caller holds g_mutex.
bool initialize_locked() noexcept
{
    if (g_state.initialized) return !g_state.disabled;
    g_state.initialized = true;

    const sedici::LimiterSettings& limiter = sedici::settings().limiter;
    const double fps = sedici::live_limiter_fps();
    if (fps <= 0.0) {
        g_state.disabled = true;
        return false;
    }
    if (!g_frequency) {
        g_state.disabled = true;
        log_error("sedici: limiter disabled: QueryPerformanceFrequency failed");
        return false;
    }

    g_state.interval = static_cast<uint64_t>(
        std::llround(static_cast<double>(g_frequency) / fps));
    if (g_state.interval == 0) g_state.interval = 1;

    g_state.spin = static_cast<uint64_t>(
        std::llround(static_cast<double>(limiter.spin_us) * 1e-6 * static_cast<double>(g_frequency)));

    // The high-resolution flavour is what makes the timer wake close enough to
    // the deadline for the spin window to matter. If it is unavailable, a
    // standard waitable timer still honours the schedule, just more coarsely.
    g_state.timer = CreateWaitableTimerExW(nullptr, nullptr,
                                           CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (g_state.timer) {
        g_state.timer_high_resolution = true;
    } else {
        const DWORD first_error = GetLastError();
        g_state.timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        if (g_state.timer) {
            std::ostringstream out;
            out << "sedici: limiter: high-resolution waitable timer unavailable (GetLastError="
                << first_error << "); using the standard timer, spin accuracy may be worse";
            log_event(1, out.str());
        }
    }

    if (!g_state.timer) {
        std::ostringstream out;
        out << "sedici: limiter disabled: could not create a waitable timer (GetLastError="
            << GetLastError() << ')';
        log_error(out.str());
        g_state.disabled = true;
        return false;
    }

    g_state.timer_ready = true;
    return true;
}

// Caller holds g_mutex. Returns false when the timer could not be armed or
// waited on; the caller then disables the limiter instead of spinning for a
// whole frame.
bool wait_on_timer_locked(uint64_t target, uint64_t now) noexcept
{
    if (!g_state.timer_ready) return false;
    if (target <= now) return true;
    if (!g_frequency) return false;

    const double seconds = static_cast<double>(target - now) / static_cast<double>(g_frequency);
    LONGLONG due = -static_cast<LONGLONG>(seconds * 1e7);
    if (due >= 0) due = -1; // already due: fire immediately

    LARGE_INTEGER relative{};
    relative.QuadPart = due;
    if (!SetWaitableTimer(g_state.timer, &relative, 0, nullptr, nullptr, FALSE)) return false;
    return WaitForSingleObject(g_state.timer, INFINITE) == WAIT_OBJECT_0;
}

void spin_until(uint64_t deadline) noexcept
{
    while (qpc_now() < deadline) YieldProcessor();
}

std::string activation_line_locked(bool fg_active) noexcept
{
    const sedici::LimiterSettings& limiter = sedici::settings().limiter;
    const double fps = sedici::live_limiter_fps();
    std::ostringstream out;
    out << std::fixed << std::setprecision(3);
    out << "sedici: limiter active target=" << format_fps(fps)
        << " fps interval=" << ticks_to_ms(g_state.interval)
        << " ms spin=" << limiter.spin_us << " us"
        << " fg=" << (fg_active ? "on" : "off");
    return out.str();
}

std::string stats_line_locked(bool fg_active) noexcept
{
    const double fps = sedici::live_limiter_fps();

    std::ostringstream out;
    out << std::fixed << std::setprecision(3);
    out << "sedici: limiter stats frames=" << g_state.window.intervals.size()
        << " target_ms=" << (fps > 0.0 ? 1000.0 / fps : 0.0);

    out << " interval_ms{avg=" << average(g_state.window.intervals)
        << " p50=" << percentile(g_state.window.intervals, 0.50)
        << " p99=" << percentile(g_state.window.intervals, 0.99)
        << " max=" << maximum(g_state.window.intervals) << '}';

    out << " wait_ms{avg=" << average(g_state.window.waits) << '}';

    if (g_state.window.overshoots.empty()) {
        out << " overshoot_ms=n/a";
    } else {
        out << " overshoot_ms{avg=" << average(g_state.window.overshoots)
            << " p99=" << percentile(g_state.window.overshoots, 0.99)
            << " max=" << maximum(g_state.window.overshoots) << '}';
    }

    out << " late=" << g_state.window.late
        << " resets=" << g_state.window.resets
        << " fg=" << (fg_active ? "on" : "off");
    return out.str();
}

} // namespace

void limiter_before_present(uint64_t now_qpc) noexcept
{    // The overwhelmingly common case is the limiter being off. Keep it to one
    // immutable read so milestone-1 behaviour is untouched.
    if (sedici::live_limiter_fps() <= 0.0) return;

    try {
        std::string activation;
        std::string stats;
        bool timer_failed = false;

        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (!initialize_locked()) return;

            const bool fg_active = toggles_fg_active();
            if (!g_state.logged_mode || g_state.logged_fg != fg_active) {
                g_state.logged_mode = true;
                g_state.logged_fg = fg_active;
                activation = activation_line_locked(fg_active);
            }

            const sedici::LimiterStep step = sedici::limiter_step(
                now_qpc, g_state.deadline, g_state.have_deadline, g_state.interval);

            g_state.deadline = step.deadline;
            g_state.have_deadline = true;

            if (step.wait) {
                const uint64_t wait_target = step.deadline > g_state.spin
                    ? step.deadline - g_state.spin
                    : now_qpc;
                if (wait_target > now_qpc) {
                    if (!wait_on_timer_locked(wait_target, now_qpc)) timer_failed = true;
                }
                if (!timer_failed) spin_until(step.deadline);
            }

            const uint64_t actual = qpc_now();

            if (timer_failed) g_state.disabled = true;

            if (g_state.have_last_actual) {
                g_state.window.intervals.push_back(ticks_to_ms(actual - g_state.last_actual));
            }
            g_state.last_actual = actual;
            g_state.have_last_actual = true;

            g_state.window.waits.push_back(ticks_to_ms(actual - now_qpc));
            if (step.wait && !timer_failed) {
                const double overshoot = actual > step.deadline
                    ? ticks_to_ms(actual - step.deadline)
                    : 0.0;
                g_state.window.overshoots.push_back(overshoot);
            }

            if (step.late) ++g_state.window.late;
            if (step.reset) ++g_state.window.resets;

            if (g_state.window.intervals.size() >= kReportEvery) {
                stats = stats_line_locked(fg_active);
                g_state.window.clear();
            }
        }

        if (!activation.empty()) log_event(1, activation);
        if (timer_failed) {
            log_error("sedici: limiter disabled: a waitable timer operation failed; the game "
                      "continues uncapped");
        }
        if (!stats.empty()) log_event(1, stats);
    } catch (...) {
        log_error("sedici: limiter failed; the game continues uncapped");
    }
}

void limiter_on_config_changed() noexcept
{
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_state.timer) CloseHandle(g_state.timer);
        g_state = State{};
    } catch (...) {
        log_error("sedici: failed to reset the limiter after a configuration change");
    }
}
