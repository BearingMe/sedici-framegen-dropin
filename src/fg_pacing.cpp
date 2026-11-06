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

double ticks_to_seconds(uint64_t ticks, uint64_t frequency) noexcept
{
    return frequency ? static_cast<double>(ticks) / static_cast<double>(frequency) : 0.0;
}

PacingOutput plan_present(const PacingInput& input) noexcept
{
    PacingOutput output;
    output.realDeadline = input.now;
    output.presentGenerated = input.generatedReady && input.cycleStarted;
    if (!input.cycleStarted) {
        // No temporal pair yet: never show the seeded real image as a midpoint.
        output.presentGenerated = false;
        output.firstWait = 0.0;
        output.secondWait = 0.0;
        return output;
    }
    // Menus/FG-off and failed generation must remain real-frame-only, without
    // inheriting a timing delay from an earlier generated cycle.
    if (!input.generatedReady) return output;

    const double interval = clamp_interval(input.interval);
    const double half = 0.5 * interval;

    // Use entries for both sides of the comparison. In particular, do not wait
    // for last submission + interval: that accumulates the previous first wait
    // and submission overhead into the next first wait.
    const double target = input.lastCycle + interval;
    // A midpoint whose slot is more than half a cycle late must not become a
    // backlog. Keep the real frame moving instead of showing stale output.
    if (input.now > target + half) output.presentGenerated = false;

    // Without a midpoint the real frame should not be held at all.
    output.secondWait = output.presentGenerated ? half : 0.0;
    output.realDeadline = input.now + output.secondWait;
    return output;
}

PacingOutput PacingState::plan(double entry, bool generatedReady) noexcept
{
    if (cycleStarted) {
        const double delta = entry - lastEntry;
        if (delta > 1e-4 && delta < 1.0) {
            interval = intervalValid ? interval * 0.9 + delta * 0.1 : delta;
            intervalValid = true;
        }
    }
    const PacingInput input{entry, lastEntry, interval, cycleStarted, generatedReady};
    const auto output = plan_present(input);
    lastEntry = entry;
    cycleStarted = true;
    return output;
}

} // namespace sedici::fg
