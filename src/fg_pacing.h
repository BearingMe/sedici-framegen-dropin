#pragma once

// Output pacing for Sedici's frame generation swapchain wrapper.
//
// The wrapper presents two frames per game Present: the midpoint Sedici generated
// for the *previous* pair, then the real frame the game just finished. Presenting
// the midpoint one cycle late is deliberate: a midpoint between frames N-1 and N
// cannot be shown before N exists. The cost is one cycle of latency, and the
// owner must drop a midpoint whose deadline already passed instead of queueing it.
//
// Kept free of clocks, fences and D3D12 so the decision can be checked offline.

namespace sedici::fg {

struct PacingInput {
    double now = 0.0;              // seconds, monotonic
    double lastCycle = 0.0;        // when the previous cycle started presenting
    double interval = 1.0 / 60.0;  // smoothed game frame interval, seconds
    bool cycleStarted = false;     // false before the first real Present
    bool generatedReady = false;   // a midpoint exists and is still current
};

struct PacingOutput {
    bool presentGenerated = false;
    double firstWait = 0.0;   // seconds before the first present of this cycle
    double secondWait = 0.0;  // seconds between the generated and real present
};

// Waits never exceed half an interval before the first present and half an
// interval between presents. A capped wait keeps a slow cycle from stretching
// the game's own frame time into a feedback loop.
PacingOutput plan_present(const PacingInput& input) noexcept;

} // namespace sedici::fg
