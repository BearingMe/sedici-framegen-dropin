#pragma once

#include "config.h"

#include <cstdint>

namespace sedici {

// Initialized once from `settings()`. Every setter is safe to call from the
// overlay thread; readers use relaxed atomics because they only need a
// consistent value, not ordering with other state.
void live_initialize(const Settings& config) noexcept;

double live_limiter_fps() noexcept;
void live_set_limiter_fps(double fps) noexcept;

PacingMode live_pacing_mode() noexcept;
void live_set_pacing_mode(PacingMode mode) noexcept;

AsyncMode live_async_mode() noexcept;
void live_set_async_mode(AsyncMode mode) noexcept;

int live_log_level() noexcept;
void live_set_log_level(int level) noexcept;

UiMode live_ui_mode() noexcept;
void live_set_ui_mode(UiMode mode) noexcept;

UiScaleMode live_ui_scale_mode() noexcept;
void live_set_ui_scale_mode(UiScaleMode mode) noexcept;

float live_ui_scale() noexcept;
void live_set_ui_scale(float scale) noexcept;

} // namespace sedici
