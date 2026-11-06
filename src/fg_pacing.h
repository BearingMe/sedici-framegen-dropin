#pragma once

#include <cstdint>

// Output pacing for Sedici's frame generation swapchain wrapper.
//
// The wrapper generates from the current pair N-1/N before either output, then
// presents midpoint(N-1,N) followed by real N. It never carries an older midpoint
// past a real frame that is newer than it. The first cycle only seeds history.
//
// Kept free of clocks, fences and D3D12 so the decision can be checked offline.

namespace sedici::fg {

// QPC counts are ticks, not seconds. Keep the conversion on the tested path.
double ticks_to_seconds(uint64_t ticks, uint64_t frequency) noexcept;

struct PacingInput {
    double now = 0.0;              // seconds, monotonic
    double lastCycle = 0.0;        // previous wrapper entry, never a delayed submission
    double interval = 1.0 / 60.0;  // smoothed game frame interval, seconds
    bool cycleStarted = false;     // false before the first real Present
    bool generatedReady = false;   // a midpoint exists and is still current
};

struct PacingOutput {
    bool presentGenerated = false;
    double firstWait = 0.0;   // seconds before the first present of this cycle
    double secondWait = 0.0;  // seconds between the generated and real present
    double realDeadline = 0.0; // absolute deadline anchored at wrapper entry
};

// No wait before the midpoint. The real-frame deadline is at most half an
// interval after entry; work/API time already spent consumes that budget.
PacingOutput plan_present(const PacingInput& input) noexcept;

// The same entry-based clock drives both production and the sustained-loop
// check. Keeping one anchor prevents feeding a delayed submission into the
// next cycle's entry-based interval estimate.
struct PacingState {
    double lastEntry = 0.0;
    double interval = 1.0 / 60.0;
    bool cycleStarted = false;
    bool intervalValid = false;

    PacingOutput plan(double entry, bool generatedReady) noexcept;
};

} // namespace sedici::fg
