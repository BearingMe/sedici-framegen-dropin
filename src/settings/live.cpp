#include "settings/live.h"

#include <algorithm>
#include <atomic>
#include <mutex>

namespace sedici {
namespace {

std::mutex g_values_mutex;
LiveValues g_values;
std::atomic<int> g_ui_mode{static_cast<int>(UiMode::Off)};

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
}

void live_initialize(const Settings& config) noexcept
{
    LiveValues values;
    values.limiter_fps = config.limiter.fps;
    values.log_level = config.log.level;
    values.scale_mode = config.ui.scale_mode;
    values.ui_scale = config.ui.scale;
    live_apply(values);
    g_ui_mode.store(static_cast<int>(config.ui.mode), std::memory_order_relaxed);
}

double live_limiter_fps() noexcept { return live_snapshot().limiter_fps; }
void live_set_limiter_fps(double fps) noexcept
{
    std::lock_guard lock(g_values_mutex);
    g_values.limiter_fps = std::clamp(fps, 0.0, 1000.0);
}
int live_log_level() noexcept { return live_snapshot().log_level; }
void live_set_log_level(int level) noexcept
{
    std::lock_guard lock(g_values_mutex);
    g_values.log_level = std::clamp(level, 0, 2);
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

UiScaleMode live_ui_scale_mode() noexcept { return live_snapshot().scale_mode; }
void live_set_ui_scale_mode(UiScaleMode mode) noexcept
{
    std::lock_guard lock(g_values_mutex);
    g_values.scale_mode = mode;
}
float live_ui_scale() noexcept { return live_snapshot().ui_scale; }
void live_set_ui_scale(float scale) noexcept
{
    std::lock_guard lock(g_values_mutex);
    g_values.ui_scale = std::clamp(scale, 0.5f, 2.0f);
}

} // namespace sedici
