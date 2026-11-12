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
    double now = 0.0;             // seconds, monotonic
    double lastCycle = 0.0;       // previous wrapper entry, never a delayed submission
    double interval = 1.0 / 60.0; // smoothed game frame interval, seconds
    bool cycleStarted = false;    // false before the first real Present
    bool generatedReady = false;  // a midpoint exists and is still current
};

struct PacingOutput {
    bool presentGenerated = false;
    double firstWait = 0.0;    // seconds before the first present of this cycle
    double secondWait = 0.0;   // seconds between the generated and real present
    double realDeadline = 0.0; // absolute deadline anchored at wrapper entry
};

// Current presenter policy: a later output whose content-time deadline is due
// supersedes this output, regardless of whether either output is real or generated.
// Inputs are seconds on the same monotonic clock; no later output means keep it.
bool later_output_is_due(bool hasLater, double laterContent, double latency, double now) noexcept;

// Earliest moment the tear guard lets the next flip start, or 0 for no floor.
// The panel scans for one refresh after a flip; the timer guard waits that out
// plus a small margin. The scan-line guard stops just before the scan can have
// ended and lets the scan line decide the exact moment.
double flip_floor(bool timerGuard, bool scanGuard, double lastFlip, double period, double lead) noexcept;

// The presenter's latency: how long after its content time an output is flipped.
// It is the schedule's own offset (structural) plus how long after the game's
// Present the frame's first image is ready (GPU), plus 1 ms.
class LatencyEstimator {
public:
    void reset() noexcept;
    // `structural`: entry - content of the frame's first output (exact).
    // `gpu`: seconds from the game's Present to that output being ready; with
    // `exact` false the frame was already finished when the presenter got to
    // it, so `gpu` is only an upper bound.
    void observe(double structural, double gpu, bool exact) noexcept;
    double latency() const noexcept { return m_latency; }
    double structural() const noexcept { return m_structural; }
    double gpu() const noexcept { return m_gpu; }

private:
    static constexpr uint32_t kStructuralWindow = 8;
    static constexpr uint32_t kGpuWindow = 16;
    double m_latency = 0.0;
    double m_structural = 0.0;
    double m_gpu = 0.0;
    double m_structural_ring[kStructuralWindow]{};
    uint32_t m_structural_next = 0;
    double m_gpu_ring[kGpuWindow]{};
    uint32_t m_gpu_next = 0;
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

// Output schedule for `[fg] present = "vrr"`: which frames to show for each
// real frame, and at what content time, for a fixed output/real ratio. (An
// adaptive output-rate target was removed on 2026-11-12, issue #1.)
//
// Outputs sit on an even grid in content time (the game's Present clock), so
// on-screen pacing is even. A grid point between real frames N-1 and N becomes
// a generated frame at alpha = (t - t[N-1]) / (t[N] - t[N-1]); a point on N is
// the real frame. At ratios below 2 most grid points miss the real frames, so
// some real frames are never shown on their own: 1.5x shows every
// second real frame, 1.25x every fourth.
//
// Fixed ratio p/q (5/4, 3/2, 7/4, 2/1): grid points at m*q/p real frames, a
// pattern that repeats every q real frames with p outputs.
//
// Pure: no clocks, no D3D12, so it is checked offline.

constexpr uint32_t kMaxPlannedOutputs = 3; // at most 2 generated + the real frame

struct PlannedOutput {
    float alpha = 1.0f; // (0, 1); 1 means the real frame itself
    bool real = true;
    double content = 0.0; // seconds on the entry clock
};

struct OutputPlan {
    uint32_t count = 0;
    PlannedOutput outputs[kMaxPlannedOutputs];
    uint32_t generated() const noexcept;
};

class OutputSchedule {
public:
    // `numerator/denominator` within [1, 2], e.g. 3/2. Resets the pattern.
    void set_ratio(uint32_t numerator, uint32_t denominator) noexcept;
    // Forget history: the next plan shows only its real frame.
    void reset() noexcept;

    // `entry`: seconds at this real frame's Present. The first frame after a
    // reset, after an interval over 50 ms (below 20 real fps) or over 2.5x the
    // recent one, returns just the real frame.
    OutputPlan plan(double entry) noexcept;

    uint32_t numerator() const noexcept { return m_numerator; }
    uint32_t denominator() const noexcept { return m_denominator; }

private:
    uint32_t m_numerator = 2;
    uint32_t m_denominator = 1;
    bool m_started = false;
    double m_previous = 0.0;
    uint32_t m_index = 0;       // real frames into the current pattern, 1..denominator
    double m_interval = 0.0;    // recent real-frame interval, for hitch detection
    double m_last_sample = 0.0; // previous real-frame interval, for the two-sample mean
    bool m_slow = false;        // smoothed rate below ~20 fps: real frames only (hysteresis)
};

} // namespace sedici::fg
