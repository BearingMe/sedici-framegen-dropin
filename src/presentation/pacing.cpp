#include "presentation/pacing.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace sedici::fg {

namespace {

constexpr double kMinInterval = 1.0 / 480.0;
constexpr double kMaxInterval = 1.0;
// Two-sample mean real-frame interval at which generation stops (just over 50 ms,
// 20 fps) and resumes (about 21.7 fps), and the largest single interval that
// may be interpolated across.
constexpr double kSlowEnter = 0.0505;
constexpr double kSlowExit = 0.046;
constexpr double kSampleCap = 0.1;

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

bool later_output_is_due(bool hasLater, double laterContent, double latency, double now) noexcept
{
    return hasLater && laterContent + latency <= now;
}

double flip_floor(bool timerGuard, bool scanGuard, double lastFlip, double period, double lead) noexcept
{
    if (lastFlip <= 0.0) return 0.0;
    // Right after Present returns the flip has not latched yet, so the scan line
    // still reports the previous blank and the scan-line gate would open for a
    // second flip ~1 ms later: that frame then shows for a millisecond and the
    // next for ~20 (finding 009, adaptive runs).
    if (timerGuard) return lastFlip + period + 0.0002;
    if (scanGuard) return lastFlip + period - lead - 0.0005;
    return 0.0;
}

void LatencyEstimator::reset() noexcept
{
    *this = LatencyEstimator{};
}

void LatencyEstimator::observe(double structural, double gpu, bool exact) noexcept
{
    // Both parts are short windows rather than slow-decay averages: a single
    // hitch must not hold the latency up for long, or each frame's flips take
    // longer, the game blocks on the presenter, its frames get longer and the
    // latency stays up (finding 009, second session).
    //
    // Structural: the schedule's own offset, the largest of the last 8 frames,
    // which covers the alternating pattern of every fixed ratio.
    if (std::isfinite(structural)) {
        m_structural_ring[m_structural_next] = std::clamp(structural, 0.0, 0.25);
        m_structural_next = (m_structural_next + 1) % kStructuralWindow;
        m_structural = *std::max_element(std::begin(m_structural_ring), std::end(m_structural_ring));
    }
    // GPU: the median of the last 16 measurements. A slower-than-usual frame is
    // then simply late (flipped as soon as it is ready) instead of raising the
    // latency of the next 16; the second-highest estimate stacked with the
    // structural maximum reached 45-60 ms at 40 real fps (finding 009, adaptive).
    // A frame that was already finished when the presenter got to it only
    // bounds the GPU time from above: it is recorded only when it lowers the
    // estimate, never to raise it, which would count queueing as latency.
    if (std::isfinite(gpu)) {
        gpu = std::clamp(gpu, 0.0, 0.1);
        if (exact || gpu < m_gpu) {
            m_gpu_ring[m_gpu_next] = gpu;
            m_gpu_next = (m_gpu_next + 1) % kGpuWindow;
            double sorted[kGpuWindow];
            std::copy(std::begin(m_gpu_ring), std::end(m_gpu_ring), sorted);
            std::nth_element(sorted, sorted + kGpuWindow / 2, sorted + kGpuWindow);
            m_gpu = sorted[kGpuWindow / 2];
        }
    }
    m_latency = m_structural + m_gpu + 0.001;
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

uint32_t OutputPlan::generated() const noexcept
{
    uint32_t total = 0;
    for (uint32_t i = 0; i < count; ++i) total += outputs[i].real ? 0u : 1u;
    return total;
}

void OutputSchedule::set_ratio(uint32_t numerator, uint32_t denominator) noexcept
{
    if (denominator == 0 || numerator < denominator || static_cast<uint64_t>(numerator) > 2ull * denominator) {
        numerator = 2;
        denominator = 1;
    }
    m_adaptive = false;
    m_numerator = numerator;
    m_denominator = denominator;
    reset();
}

void OutputSchedule::set_target(double fps) noexcept
{
    m_adaptive = true;
    m_target = std::isfinite(fps) ? std::clamp(fps, 1.0, 1000.0) : 60.0;
    reset();
}

void OutputSchedule::reset() noexcept
{
    m_started = false;
    m_interval = 0.0;
    m_index = 0;
    m_slow = false;
    m_last_sample = 0.0;
}

OutputPlan OutputSchedule::plan(double entry) noexcept
{
    OutputPlan plan;
    const auto realOnly = [&] {
        plan.count = 1;
        plan.outputs[0] = PlannedOutput{1.0f, true, entry};
    };
    const double interval = entry - m_previous;
    // A hitch (more than 2.5x the recent interval) is a discontinuity too:
    // spreading generated frames across it would replay the stall in slow
    // motion and hold the presenter for its whole length. The recent interval
    // still follows a genuine slowdown, by at most 2.5x per frame.
    const bool hitch = m_interval > 0.0 && interval > 2.5 * m_interval;
    if (m_started && interval > 1e-4 && interval < 0.25)
        m_interval = m_interval > 0.0 ? 0.9 * m_interval + 0.1 * std::min(interval, 2.5 * m_interval) : interval;
    // Below 20 real fps interpolation would need over 50 ms of extra latency
    // and replays loading stutters (157 ms latency at 14 fps in finding 009):
    // show real frames only until the rate recovers. The cutoff applies to the
    // mean of the last two intervals with hysteresis: a single sample over 50 ms at an
    // average near 20 fps used to flip about half of the frames to real-only,
    // halving the output (K95). One sample over 100 ms is still a discontinuity.
    // Two-sample mean: reacts within a frame to a genuine slowdown and absorbs
    // alternating jitter. It needs two consecutive in-range samples; a gap over
    // 100 ms is a discontinuity and forgets the history.
    if (!m_started || interval <= 1e-4 || interval > kSampleCap) {
        m_last_sample = 0.0;
    } else {
        if (m_last_sample > 0.0) {
            const double recent = 0.5 * (interval + m_last_sample);
            if (recent > kSlowEnter)
                m_slow = true;
            else if (recent < kSlowExit)
                m_slow = false;
        } else if (interval > 1.1 * kSlowEnter) {
            m_slow = true; // a lone sample needs a 10% margin to count as slow
        }
        m_last_sample = interval;
    }
    const bool continuous = m_started && interval > 1e-4 && interval <= kSampleCap && !hitch && !m_slow;
    const double previous = m_previous;
    m_previous = entry;
    m_started = true;
    if (!continuous) {
        m_index = 0;
        m_next_slot = entry + (m_adaptive ? 1.0 / m_target : 0.0);
        realOnly();
        return plan;
    }

    const auto addGenerated = [&](double alpha) {
        if (plan.count >= kMaxPlannedOutputs || plan.generated() >= 2) return;
        alpha = std::clamp(alpha, 0.02, 0.98);
        plan.outputs[plan.count++] = PlannedOutput{static_cast<float>(alpha), false, previous + alpha * interval};
    };

    if (!m_adaptive) {
        // Grid points m with m*q/p in (n-1, n]: alpha = (m*q - (n-1)*p) / p.
        const uint64_t p = m_numerator, q = m_denominator;
        m_index = m_index % m_denominator + 1;
        const uint64_t n = m_index;
        const uint64_t first = ((n - 1) * p) / q + 1;
        const uint64_t last = (n * p) / q;
        for (uint64_t m = first; m <= last; ++m) {
            const uint64_t numerator = m * q - (n - 1) * p;
            if (numerator >= p) {
                if (plan.count < kMaxPlannedOutputs) plan.outputs[plan.count++] = PlannedOutput{1.0f, true, entry};
            } else {
                addGenerated(static_cast<double>(numerator) / static_cast<double>(p));
            }
        }
        return plan;
    }

    const double period = 1.0 / m_target;
    // A real frame stands in for a grid point only when it is within a tenth
    // of an output period; at a quarter (3.5 ms at 72) real frames visibly broke
    // the even spacing when the real rate was close to the target.
    const double snap = 0.1 * period;
    // After a hitch the grid may be behind the previous real frame; skip the
    // points that can no longer be shown rather than replaying them.
    if (m_next_slot <= previous) m_next_slot += std::ceil((previous - m_next_slot) / period + 1e-9) * period;
    for (int guard = 0; guard < 64 && m_next_slot <= entry + snap; ++guard) {
        if (std::abs(entry - m_next_slot) <= snap) {
            if (plan.count < kMaxPlannedOutputs) plan.outputs[plan.count++] = PlannedOutput{1.0f, true, entry};
            m_next_slot += period;
            break; // the real frame closes this interval
        }
        if (plan.generated() >= 2) {
            // No generated frame left for this grid point (kMaxPhases): skipping
            // it lost an output, 70.4 instead of 73 fps at 32 real fps (#35). The
            // real frame stands in for it instead, and the grid restarts after the
            // real frame no closer than a snapped real frame allows (0.9 period).
            if (plan.count < kMaxPlannedOutputs) plan.outputs[plan.count++] = PlannedOutput{1.0f, true, entry};
            m_next_slot = std::max(m_next_slot + period, entry + period - snap);
            break;
        }
        addGenerated((m_next_slot - previous) / interval);
        m_next_slot += period;
    }
    return plan;
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
