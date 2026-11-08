#pragma once

#include "config.h"

#include <cstdint>

namespace sedici {

// A complete set of live controls. Call live_snapshot() once when several
// fields must be read coherently; separate accessor calls may span revisions.
struct LiveValues {
    double limiter_fps = 0.0;
    int log_level = 1;
    UiScaleMode scale_mode = UiScaleMode::Auto;
    float ui_scale = 1.0f;

    bool operator==(const LiveValues&) const = default;
};

LiveValues live_snapshot() noexcept;
void live_apply(const LiveValues& values) noexcept;

// Initialized once from settings().
void live_initialize(const Settings& config) noexcept;

double live_limiter_fps() noexcept;
void live_set_limiter_fps(double fps) noexcept;

int live_log_level() noexcept;
void live_set_log_level(int level) noexcept;

UiMode live_ui_mode() noexcept;
void live_set_ui_mode(UiMode mode) noexcept;

UiScaleMode live_ui_scale_mode() noexcept;
void live_set_ui_scale_mode(UiScaleMode mode) noexcept;

float live_ui_scale() noexcept;
void live_set_ui_scale(float scale) noexcept;

} // namespace sedici
