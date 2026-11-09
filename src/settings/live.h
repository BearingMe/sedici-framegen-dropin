#pragma once

#include "settings/config.h"

#include <cstdint>

namespace sedici {

// A complete set of live controls. Call live_snapshot() once when several
// fields must be read coherently; separate accessor calls may span revisions.
struct LiveValues {
    // The one target (FR-05): output fps, 0 = uncapped, or automatic = match the monitor.
    double target_fps = 0.0;
    bool target_auto = false;
    int log_level = 1;
    UiScaleMode scale_mode = UiScaleMode::Auto;
    float ui_scale = 1.0f;

    bool operator==(const LiveValues&) const = default;
};

LiveValues live_snapshot() noexcept;
void live_apply(const LiveValues& values) noexcept;

// Initialized once from settings().
void live_initialize(const Settings& config) noexcept;

// The real-frame cap the limiter enforces now: the target resolved against the
// published output ratio and refresh rate (settings/target_rate.h). 0 = uncapped.
double live_limiter_fps() noexcept;

void live_set_target_fps(double fps) noexcept;
void live_set_target_auto(bool automatic) noexcept;

// Published by the swapchain wrapper: output frames per real frame (2 for 2x,
// 1 when nothing is generated) and the refresh rate of the monitor in Hz (0 =
// unknown). Returns true when the resolved real-frame cap changed, so the caller
// can tell the limiter.
// The cap's consumer (the limiter) registers here so a settings change can tell it
// to reschedule without settings depending on presentation (P4.5).
using LiveCapListener = void (*)() noexcept;
void live_set_cap_listener(LiveCapListener listener) noexcept;
void live_notify_cap_changed() noexcept;

bool live_set_output_context(double ratio, double refresh_hz) noexcept;
double live_output_refresh_hz() noexcept;

int live_log_level() noexcept;
void live_set_log_level(int level) noexcept;

UiMode live_ui_mode() noexcept;
void live_set_ui_mode(UiMode mode) noexcept;

UiScaleMode live_ui_scale_mode() noexcept;
void live_set_ui_scale_mode(UiScaleMode mode) noexcept;

float live_ui_scale() noexcept;
void live_set_ui_scale(float scale) noexcept;

} // namespace sedici
