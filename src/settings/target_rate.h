#pragma once

// The one frame-rate target (FR-05, P4.2). Pure functions, no clock and no I/O,
// so they are tested without a display.
//
//   target_fps = 0      uncapped
//   target_fps = N      the screen should receive N output frames per second
//   target = "auto"     match the monitor: floor(Hz - Hz^2 / 3600), uncapped if
//                       the refresh rate is unknown
//
// The auto value is the low-lag rule for VRR and v-sync (a cap just under the
// ceiling so the display never waits on the blanking interval). It is not
// "refresh minus a constant": it grows with the refresh rate, 59 at 60 Hz,
// 73 at 75 Hz, 138 at 144 Hz, 224 at 240 Hz.

#include <cmath>

namespace sedici {

// 0 when the refresh rate is unknown or not plausible (uncapped fallback).
inline double auto_target_fps(double refresh_hz) noexcept
{
    if (!std::isfinite(refresh_hz) || refresh_hz < 2.0 || refresh_hz > 1000.0) return 0.0;
    return std::floor(refresh_hz - refresh_hz * refresh_hz / 3600.0);
}

// The output rate asked for, or 0 for uncapped.
inline double resolve_target_fps(double target_fps, bool automatic, double refresh_hz) noexcept
{
    const double value = automatic ? auto_target_fps(refresh_hz) : target_fps;
    return std::isfinite(value) && value > 0.0 ? value : 0.0;
}

// The real-frame cap the limiter enforces for an output target. `ratio` is the
// number of output frames per real frame (2 for 2x, 1 when nothing is
// generated). 0 means uncapped.
inline double resolve_real_cap(double target_fps, bool automatic, double refresh_hz,
                               double ratio) noexcept
{
    const double output = resolve_target_fps(target_fps, automatic, refresh_hz);
    if (output <= 0.0) return 0.0;
    if (!std::isfinite(ratio) || ratio < 1.0) ratio = 1.0;
    return output / ratio;
}

// A legacy `limiter.fps` capped real frames. As an output target it is the same
// cap times the output ratio.
inline double legacy_limiter_to_target(double limiter_fps, double ratio) noexcept
{
    if (!std::isfinite(ratio) || ratio < 1.0) ratio = 1.0;
    return limiter_fps * ratio;
}

} // namespace sedici
