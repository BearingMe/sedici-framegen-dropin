// fgcheck group: output pacing decisions, checked without a clock.
#include "fgcheck_common.h"

namespace fgcheck {

// Output pacing is a pure decision, so it is checked without a clock.
bool check_pacing()
{
    int failures = 0;
    auto expect = [&failures](bool condition, const char* what) {
        std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
        if (!condition) ++failures;
    };

    expect(sedici::fg::ticks_to_seconds(30000000, 10000000) == 3.0,
           "QPC counts divide by frequency to produce seconds");
    const double uptime = sedici::fg::ticks_to_seconds(987654321000000ULL, 10000000);
    const double later = sedici::fg::ticks_to_seconds(987654321277778ULL, 10000000);
    expect(std::abs((later - uptime) - 1.0 / 36.0) < 1e-7,
           "QPC conversion preserves a 36 fps interval after long uptime");
    expect(sedici::fg::ticks_to_seconds(1, 0) == 0.0,
           "missing QPC frequency does not divide by zero");

    sedici::fg::PacingInput input;
    input.cycleStarted = false;
    input.generatedReady = true;
    auto plan = sedici::fg::plan_present(input);
    expect(!plan.presentGenerated && plan.firstWait == 0.0 && plan.secondWait == 0.0,
           "the first cycle presents only the real seed without waiting");

    input.cycleStarted = true;
    input.interval = 1.0 / 60.0;
    input.lastCycle = 10.0;
    input.now = 10.0 + input.interval;
    plan = sedici::fg::plan_present(input);
    expect(plan.presentGenerated, "an on-time cycle presents the midpoint before the real frame");
    expect(plan.firstWait == 0.0, "an on-time cycle does not wait");
    expect(std::abs(plan.secondWait - input.interval * 0.5) < 1e-9,
           "the real frame follows the midpoint by half an interval");

    input.now = 10.0;
    plan = sedici::fg::plan_present(input);
    expect(plan.firstWait == 0.0,
           "an early cycle never waits before the current midpoint");

    input.now = 10.0 + 3.0 * input.interval;
    plan = sedici::fg::plan_present(input);
    expect(plan.firstWait == 0.0, "a late cycle presents without waiting");
    expect(!plan.presentGenerated && plan.secondWait == 0.0,
           "a midpoint past its slot is dropped without delaying the real frame");

    input.generatedReady = false;
    input.now = input.lastCycle;
    plan = sedici::fg::plan_present(input);
    expect(!plan.presentGenerated && plan.firstWait == 0.0 && plan.secondWait == 0.0,
           "without a midpoint the real frame is never held");

    input.interval = 0.0;
    input.now = input.lastCycle + 1.0 / 60.0;
    input.generatedReady = true;
    plan = sedici::fg::plan_present(input);
    expect(plan.secondWait > 0.0 && plan.secondWait <= 0.5,
           "an invalid interval falls back to a bounded pacing value");

    // Feed actual elapsed wrapper work back into the next entry. One-shot
    // decisions cannot expose a self-amplifying scheduler. Simulate over nine
    // minutes, including FG off/on and a 200 ms source stall, with no real sleeps.
    sedici::fg::PacingState pacing;
    constexpr double period = 1.0 / 36.0;
    double entry = 1000000.0;
    bool bounded = true, recovered = true;
    uint32_t outputs = 0;
    for (uint32_t cycle = 0; cycle < 20000; ++cycle) {
        const bool enabled = cycle < 5000 || cycle >= 5100;
        if (cycle == 5000 || cycle == 5100) pacing = {};
        if (cycle == 10000) entry += 0.2;
        const auto next = pacing.plan(entry, enabled);
        // CPU recording, native Present and copy work vary independently. They
        // consume the entry-based budget instead of extending its deadline.
        const double work = cycle % 2 ? 0.002 : 0.004;
        const double real = std::max(entry + work, next.realDeadline);
        const double followingEntry = std::max(entry + period, real + 0.004);
        bounded &= next.firstWait == 0.0 && next.realDeadline <= entry + pacing.interval * 0.5 + 1e-8;
        if (!enabled) bounded &= !next.presentGenerated && next.realDeadline == entry;
        if (cycle > 10500) recovered &= std::abs(pacing.interval - period) < 1e-7;
        outputs += next.presentGenerated ? 2 : 1;
        entry = followingEntry;
    }
    expect(bounded && recovered && std::abs(pacing.interval - period) < 1e-7 && outputs > 39800,
           "20000-cycle closed loop stays at 36 real / 72 output and recovers from off/on and a stall");

    {
        sedici::fg::OutputSchedule schedule;
        schedule.set_ratio(UINT32_MAX, UINT32_MAX);
        expect(schedule.numerator() == UINT32_MAX && schedule.denominator() == UINT32_MAX,
               "ratio validation handles large denominators without overflow");
        schedule.set_ratio(UINT32_MAX, 0);
        expect(schedule.numerator() == 2 && schedule.denominator() == 1,
               "ratio validation rejects a zero denominator");
    }

    // Current presenter policy, characterized without changing real-drop semantics.
    expect(!sedici::fg::later_output_is_due(false, 1.0, 0.1, 9.0) &&
           !sedici::fg::later_output_is_due(true, 1.0, 0.125, 1.124) &&
           sedici::fg::later_output_is_due(true, 1.0, 0.125, 1.125) &&
           sedici::fg::later_output_is_due(true, 1.0, 0.125, 1.126),
           "presenter stale-output policy uses the later deadline including equality");

    {
        // Characterize the production stale-output predicate after a hitch:
        // overdue outputs are discarded in order and the newest remains eligible.
        const double content[] = {1.0, 1.01, 1.02, 1.03};
        const double now = 1.035;
        unsigned dropped = 0;
        unsigned shown = 0;
        double lastShown = 0.0;
        for (size_t i = 0; i < std::size(content); ++i) {
            const bool hasLater = i + 1 < std::size(content);
            const double nextContent = hasLater ? content[i + 1] : 0.0;
            if (sedici::fg::later_output_is_due(hasLater, nextContent, 0.0, now)) {
                ++dropped;
            } else {
                ++shown;
                lastShown = content[i];
            }
        }
        const double recovery = 1.04;
        expect(dropped == 3 && shown == 1 && lastShown == 1.03 && recovery > lastShown,
               "stale-output policy drains a hitch backlog chronologically");
    }

    // Output schedule: every output lands on an even grid in content time.
    // For each ratio, count outputs over many real frames, check the pattern
    // never needs more than two generated frames per real frame, and that the
    // spacing between consecutive outputs is constant.
    struct Ratio { uint32_t p, q; const char* name; };
    for (const Ratio ratio : {Ratio{2, 1, "2x"}, Ratio{7, 4, "1.75x"}, Ratio{3, 2, "1.5x"}, Ratio{5, 4, "1.25x"}}) {
        sedici::fg::OutputSchedule schedule;
        schedule.set_ratio(ratio.p, ratio.q);
        const double interval = 1.0 / 48.0;
        double previousContent = -1.0, minGap = 1.0, maxGap = 0.0;
        uint32_t produced = 0, reals = 0, maxGenerated = 0;
        const uint32_t frames = 1 + 400 * ratio.q;
        for (uint32_t frame = 0; frame < frames; ++frame) {
            const auto step = schedule.plan(10.0 + frame * interval);
            maxGenerated = std::max(maxGenerated, step.generated());
            for (uint32_t i = 0; i < step.count; ++i) {
                const auto& out = step.outputs[i];
                if (frame > 0) {
                    ++produced;
                    reals += out.real ? 1u : 0u;
                    const double gap = out.content - previousContent;
                    minGap = std::min(minGap, gap);
                    maxGap = std::max(maxGap, gap);
                }
                previousContent = out.content;
            }
        }
        const double expectedGap = interval * ratio.q / ratio.p;
        char what[160];
        std::snprintf(what, sizeof(what),
                      "%s schedule: %u outputs per %u real frames, %u real, <=2 generated, even %.3f ms grid",
                      ratio.name, produced, frames - 1, reals, expectedGap * 1000.0);
        expect(produced == 400 * ratio.p && reals == 400 && maxGenerated <= 2 &&
               std::abs(minGap - expectedGap) < 1e-6 && std::abs(maxGap - expectedGap) < 1e-6, what);
    }
    {
        // A hitch is a discontinuity: the frame after a 3x interval shows only
        // itself, and the next normal frame resumes generating.
        sedici::fg::OutputSchedule schedule;
        schedule.set_ratio(3, 2);
        double t = 10.0;
        for (int frame = 0; frame < 20; ++frame) (void)schedule.plan(t += 1.0 / 48.0);
        const auto afterHitch = schedule.plan(t += 3.0 / 48.0);
        const auto resumed = schedule.plan(t += 1.0 / 48.0);
        expect(afterHitch.count == 1 && afterHitch.outputs[0].real && resumed.generated() >= 1,
               "1.5x schedule: a 3x hitch shows only the real frame, then generation resumes");
    }
    {
        // Adaptive: 48 real fps towards 72 output fps averages 1.5 outputs per
        // real frame on an even 1/72 s grid, snapping to real frames nearby.
        sedici::fg::OutputSchedule schedule;
        schedule.set_target(72.0);
        const double interval = 1.0 / 48.0;
        double previousContent = -1.0, maxGapError = 0.0;
        uint32_t produced = 0, maxGenerated = 0;
        for (uint32_t frame = 0; frame < 4801; ++frame) {
            const auto step = schedule.plan(10.0 + frame * interval);
            maxGenerated = std::max(maxGenerated, step.generated());
            for (uint32_t i = 0; i < step.count; ++i) {
                if (frame > 1) {
                    ++produced;
                    maxGapError = std::max(maxGapError, std::abs(step.outputs[i].content - previousContent - 1.0 / 72.0));
                }
                previousContent = step.outputs[i].content;
            }
        }
        expect(produced >= 7190 && produced <= 7210 && maxGenerated <= 2 && maxGapError <= 0.1 / 72.0 + 1e-9,
               "adaptive schedule: 48 real -> ~72 output fps, <=2 generated, within a tenth of a slot of the grid");
        // A pause resets to the real frame alone, and a slower real rate than the
        // target still never asks for more than two generated frames.
        const auto paused = schedule.plan(20.0);
        expect(paused.count == 1 && paused.outputs[0].real, "adaptive schedule: a 0.25 s gap shows only the real frame");
        // Below 20 real fps the schedule stops generating.
        uint32_t slowGenerated = 0;
        for (uint32_t frame = 0; frame < 40; ++frame) slowGenerated += schedule.plan(21.0 + frame * 0.055).generated();
        expect(slowGenerated == 0, "adaptive schedule: below 20 real fps shows only real frames");
        // K95: jitter around 20 real fps must not halve the output. Alternate
        // 46 ms and 54 ms (average 50 ms) with the fixed 2x ratio.
        {
            sedici::fg::OutputSchedule jitter;
            jitter.set_ratio(2, 1);
            double t = 40.0;
            uint32_t frames = 0, generated = 0;
            for (uint32_t frame = 0; frame < 400; ++frame) {
                t += (frame % 2 == 0) ? 0.046 : 0.054;
                const auto step = jitter.plan(t);
                if (frame >= 20) { ++frames; generated += step.generated() > 0 ? 1u : 0u; }
            }
            expect(generated >= frames * 95 / 100,
                   "2x schedule: jitter averaging 20 real fps still generates for almost every frame");
            // Steady 60 ms stays real-only, and recovery to 40 ms generates again.
            jitter.reset();
            uint32_t slowOnly = 0, recoveredCount = 0;
            for (uint32_t frame = 0; frame < 100; ++frame) slowOnly += jitter.plan(50.0 + frame * 0.06).generated();
            for (uint32_t frame = 0; frame < 100; ++frame) recoveredCount += jitter.plan(60.0 + frame * 0.04).generated();
            expect(slowOnly == 0 && recoveredCount > 90,
                   "2x schedule: a steady 60 ms interval is real-only and recovery to 40 ms resumes generation");
        }
        schedule.set_target(144.0);
        uint32_t worst = 0;
        for (uint32_t frame = 0; frame < 200; ++frame) worst = std::max(worst, schedule.plan(30.0 + frame / 40.0).generated());
        expect(worst <= 2, "adaptive schedule: 40 real fps towards 144 caps at two generated frames");
    }
    return failures == 0;
}


} // namespace fgcheck
