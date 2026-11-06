#include "fg_pacing.h"

#include <algorithm>

namespace sedici::fg {

namespace {

constexpr double kMinInterval = 1.0 / 480.0;
constexpr double kMaxInterval = 1.0;

double clamp_interval(double interval) noexcept
{
    if (!(interval > 0.0)) return 1.0 / 60.0; // also rejects NaN
    return std::clamp(interval, kMinInterval, kMaxInterval);
}

} // namespace

PacingOutput plan_present(const PacingInput& input) noexcept
{
    PacingOutput output;
    output.presentGenerated = input.generatedReady && input.cycleStarted;
    if (!input.cycleStarted) {
        // No cadence yet: the first real frame goes out immediately and there is
        // a midpoint to show only if the caller somehow already has one.
        output.presentGenerated = input.generatedReady;
        output.firstWait = 0.0;
        output.secondWait = 0.0;
        return output;
    }

    const double interval = clamp_interval(input.interval);
    const double half = 0.5 * interval;

    // Aim each cycle at the previous cycle boundary plus one interval, but never
    // block for more than half an interval: an over-long wait would lengthen the
    // game's next frame and feed back into the estimate.
    const double target = input.lastCycle + interval;
    output.firstWait = std::clamp(target - input.now, 0.0, half);

    // Without a midpoint the real frame should not be held at all.
    output.secondWait = output.presentGenerated ? half : 0.0;
    return output;
}

} // namespace sedici::fg
