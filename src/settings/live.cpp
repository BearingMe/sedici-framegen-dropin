#include "settings/live.h"
#include "settings/target_rate.h"
#include "diagnostics/log.h"

#include <algorithm>
#include <atomic>
#include <mutex>

namespace sedici {
namespace {

std::mutex g_values_mutex;
LiveValues g_values;
std::atomic<int> g_ui_mode{static_cast<int>(UiMode::Off)};
std::atomic<LiveCapListener> g_cap_listener{nullptr};
std::atomic<double> g_ratio{1.0};
std::atomic<double> g_refresh_hz{0.0};

} // namespace

LiveValues live_snapshot() noexcept
{
    std::lock_guard lock(g_values_mutex);
    return g_values;
}

void live_apply(const LiveValues& values) noexcept
{
    std::lock_guard lock(g_values_mutex);
    g_values = values;
    log_publish_level(values.log_level);
}

void live_initialize(const Settings& config) noexcept
{
    LiveValues values;
    values.target_fps = config.fg.target_fps;
    values.target_auto = config.fg.target_auto;
    values.log_level = config.log.level;
    values.scale_mode = config.ui.scale_mode;
    values.ui_scale = config.ui.scale;
    live_apply(values);
    g_ui_mode.store(static_cast<int>(config.ui.mode), std::memory_order_relaxed);
}

double live_limiter_fps() noexcept
{
    const LiveValues values = live_snapshot();
    return resolve_real_cap(values.target_fps, values.target_auto, g_refresh_hz.load(std::memory_order_relaxed),
                            g_ratio.load(std::memory_order_relaxed));
}
void live_set_target_fps(double fps) noexcept
{
    std::lock_guard lock(g_values_mutex);
    g_values.target_fps = std::clamp(fps, 0.0, 1000.0);
}
void live_set_target_auto(bool automatic) noexcept
{
    std::lock_guard lock(g_values_mutex);
    g_values.target_auto = automatic;
}
void live_set_cap_listener(LiveCapListener listener) noexcept
{
    g_cap_listener.store(listener, std::memory_order_release);
}
void live_notify_cap_changed() noexcept
{
    if (const LiveCapListener listener = g_cap_listener.load(std::memory_order_acquire)) listener();
}

bool live_set_output_context(double ratio, double refresh_hz) noexcept
{
    const double before = live_limiter_fps();
    g_ratio.store(ratio >= 1.0 ? ratio : 1.0, std::memory_order_relaxed);
    g_refresh_hz.store(refresh_hz > 0.0 ? refresh_hz : 0.0, std::memory_order_relaxed);
    return live_limiter_fps() != before;
}
double live_output_refresh_hz() noexcept
{
    return g_refresh_hz.load(std::memory_order_relaxed);
}
int live_log_level() noexcept
{
    return live_snapshot().log_level;
}
void live_set_log_level(int level) noexcept
{
    std::lock_guard lock(g_values_mutex);
    g_values.log_level = std::clamp(level, 0, 2);
    log_publish_level(g_values.log_level);
}

UiMode live_ui_mode() noexcept
{
    const int value = g_ui_mode.load(std::memory_order_relaxed);
    if (value == static_cast<int>(UiMode::Mini)) return UiMode::Mini;
    if (value == static_cast<int>(UiMode::Full)) return UiMode::Full;
    return UiMode::Off;
}

void live_set_ui_mode(UiMode mode) noexcept
{
    g_ui_mode.store(static_cast<int>(mode), std::memory_order_relaxed);
}

UiScaleMode live_ui_scale_mode() noexcept
{
    return live_snapshot().scale_mode;
}
void live_set_ui_scale_mode(UiScaleMode mode) noexcept
{
    std::lock_guard lock(g_values_mutex);
    g_values.scale_mode = mode;
}
float live_ui_scale() noexcept
{
    return live_snapshot().ui_scale;
}
void live_set_ui_scale(float scale) noexcept
{
    std::lock_guard lock(g_values_mutex);
    g_values.ui_scale = std::clamp(scale, 0.5f, 2.0f);
}

} // namespace sedici
