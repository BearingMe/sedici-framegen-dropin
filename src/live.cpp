#include "live.h"

#include <algorithm>
#include <atomic>

namespace sedici {
namespace {

std::atomic<double> g_limiter_fps{0.0};
std::atomic<int> g_pacing_mode{static_cast<int>(PacingMode::Hybrid)};
std::atomic<int> g_async_mode{static_cast<int>(AsyncMode::Game)};
std::atomic<int> g_log_level{1};
std::atomic<int> g_ui_mode{static_cast<int>(UiMode::Off)};
std::atomic<int> g_ui_scale_mode{static_cast<int>(UiScaleMode::Auto)};
std::atomic<float> g_ui_scale{1.0f};

} // namespace

void live_initialize(const Settings& config) noexcept
{
    g_limiter_fps.store(config.limiter.fps, std::memory_order_relaxed);
    g_pacing_mode.store(static_cast<int>(config.pacing.mode), std::memory_order_relaxed);
    g_async_mode.store(static_cast<int>(config.async.mode), std::memory_order_relaxed);
    g_log_level.store(config.log.level, std::memory_order_relaxed);
    g_ui_mode.store(static_cast<int>(config.ui.mode), std::memory_order_relaxed);
    g_ui_scale_mode.store(static_cast<int>(config.ui.scale_mode), std::memory_order_relaxed);
    g_ui_scale.store(config.ui.scale, std::memory_order_relaxed);
}

double live_limiter_fps() noexcept
{
    return g_limiter_fps.load(std::memory_order_relaxed);
}

void live_set_limiter_fps(double fps) noexcept
{
    if (fps < 0.0) fps = 0.0;
    if (fps > 1000.0) fps = 1000.0;
    g_limiter_fps.store(fps, std::memory_order_relaxed);
}

PacingMode live_pacing_mode() noexcept
{
    return g_pacing_mode.load(std::memory_order_relaxed) == static_cast<int>(PacingMode::Off)
        ? PacingMode::Off
        : PacingMode::Hybrid;
}

void live_set_pacing_mode(PacingMode mode) noexcept
{
    g_pacing_mode.store(static_cast<int>(mode), std::memory_order_relaxed);
}

AsyncMode live_async_mode() noexcept
{
    return g_async_mode.load(std::memory_order_relaxed) == static_cast<int>(AsyncMode::Off)
        ? AsyncMode::Off
        : AsyncMode::Game;
}

void live_set_async_mode(AsyncMode mode) noexcept
{
    g_async_mode.store(static_cast<int>(mode), std::memory_order_relaxed);
}

int live_log_level() noexcept
{
    return g_log_level.load(std::memory_order_relaxed);
}

void live_set_log_level(int level) noexcept
{
    g_log_level.store(std::clamp(level, 0, 2), std::memory_order_relaxed);
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
    return g_ui_scale_mode.load(std::memory_order_relaxed) ==
                   static_cast<int>(UiScaleMode::Fixed)
        ? UiScaleMode::Fixed
        : UiScaleMode::Auto;
}

void live_set_ui_scale_mode(UiScaleMode mode) noexcept
{
    g_ui_scale_mode.store(static_cast<int>(mode), std::memory_order_relaxed);
}

float live_ui_scale() noexcept
{
    return g_ui_scale.load(std::memory_order_relaxed);
}

void live_set_ui_scale(float scale) noexcept
{
    g_ui_scale.store(std::clamp(scale, 0.5f, 2.0f), std::memory_order_relaxed);
}

} // namespace sedici
