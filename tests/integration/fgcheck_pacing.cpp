// fgcheck group: output pacing decisions, checked without a clock.
#include "fgcheck_common.h"

namespace fgcheck {

namespace {

// Closed loop of the vrr presenter in fake time (issue #1): a game that blocks
// when two frames are in flight, a GPU that finishes each frame some time after
// its Present, and one presenter thread that copies, holds and flips each output
// in turn on a VRR panel. It drives the production pure policies (OutputSchedule,
// LatencyEstimator, later_output_is_due, flip_floor; the scan-line gate is modelled), so a pacing cascade found in
// the game can be reproduced and its fix checked without a device.
struct LoopConfig {
    double interval = 1.0 / 32.0; // game frame time, seconds
    double jitter = 0.001;        // +/- uniform on each frame time
    uint32_t p = 2, q = 1;        // output/real ratio
    double refresh = 75.0;
    double gpu = 0.019;                      // game Present -> frame finished on the GPU
    double gpuLow = -0.002, gpuHigh = 0.004; // uniform spread around `gpu`
    uint32_t spikeEvery = 20;                // one job in N takes `spike` longer
    double spike = 0.006;
    uint32_t jobs = 3100, warmup = 100;
    double copy = 0.0005;   // present-queue copy of one output
    double oversleep = 0.0; // per-flip lateness after a hold (timer wake-up, flip latch)
};

struct LoopResult {
    double seconds = 0.0;
    uint32_t planned = 0, shown = 0, late = 0;
    uint32_t realsPlanned = 0, realsDropped = 0, generatedDropped = 0;
    uint32_t floorPastGate = 0; // flips where flip_floor ended after the scan-line gate
    double latencySum = 0.0;
    uint32_t latencySamples = 0;
    double shownFps() const { return seconds > 0.0 ? shown / seconds : 0.0; }
    double plannedFps() const { return seconds > 0.0 ? planned / seconds : 0.0; }
    double lateShare() const { return shown ? static_cast<double>(late) / shown : 1.0; }
    double latency() const { return latencySamples ? latencySum / latencySamples : 0.0; }
};

LoopResult run_present_loop(const LoopConfig& c)
{
    uint64_t seed = 0x9E3779B97F4A7C15ULL;
    const auto uniform = [&seed](double low, double high) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        return low + (high - low) * static_cast<double>(seed >> 11) * (1.0 / 9007199254740992.0);
    };
    struct Job {
        double entry = 0.0, submit = 0.0, gpuReady = 0.0, done = 0.0;
        sedici::fg::OutputPlan plan;
    };
    sedici::fg::OutputSchedule schedule;
    schedule.set_ratio(c.p, c.q);
    sedici::fg::LatencyEstimator estimator;
    const double period = 1.0 / c.refresh;
    const double lead = 0.00025, copy = c.copy, call = 0.0003;
    const double early = std::min(0.0015, period * 0.12);

    std::vector<Job> jobs(c.jobs + 1);
    const auto make = [&](uint32_t k) {
        Job& job = jobs[k];
        const double frame = c.interval + uniform(-c.jitter, c.jitter);
        // The game's next frame starts after its last Present returned (it was
        // blocked while two frames were in flight).
        job.entry = k == 0 ? 10.0 : std::max(jobs[k - 1].entry + frame, jobs[k - 1].submit + 0.0005);
        job.submit = k >= 2 ? std::max(job.entry, jobs[k - 2].done) : job.entry;
        job.gpuReady = job.submit + c.gpu + uniform(c.gpuLow, c.gpuHigh) +
                       (c.spikeEvery && k % c.spikeEvery == 0 ? c.spike : 0.0);
        job.plan = schedule.plan(job.entry);
    };

    LoopResult result;
    double lastFlip = 0.0, cursor = 0.0;
    make(0);
    for (uint32_t k = 0; k < c.jobs; ++k) {
        make(k + 1); // depends only on jobs that are already done
        Job& job = jobs[k];
        const Job& next = jobs[k + 1];
        const bool counted = k >= c.warmup;
        const double dequeued = std::max(job.submit, cursor);
        const bool alreadyReady = job.gpuReady <= dequeued;
        double t = dequeued;
        for (uint32_t i = 0; i < job.plan.count; ++i) {
            const auto& out = job.plan.outputs[i];
            t = std::max(t, job.gpuReady) + copy;
            const double ready = t;
            if (i == 0) {
                estimator.observe(job.entry - out.content, (alreadyReady ? dequeued : ready) - job.entry,
                                  !alreadyReady);
                if (counted) {
                    result.latencySum += estimator.latency();
                    ++result.latencySamples;
                }
            }
            const double latency = estimator.latency();
            const double target = out.content + latency;
            const bool inJob = i + 1 < job.plan.count;
            const bool hasLater = inJob || next.submit <= ready;
            const double laterContent = inJob ? job.plan.outputs[i + 1].content : next.plan.outputs[0].content;
            if (counted) {
                ++result.planned;
                result.realsPlanned += out.real ? 1u : 0u;
            }
            if (sedici::fg::later_output_is_due(hasLater, laterContent, latency, ready)) {
                if (counted) (out.real ? result.realsDropped : result.generatedDropped) += 1;
                continue;
            }
            // VRR panel with the scan-line guard, as in Presenter::hold: wait for the
            // production flip_floor, then for the gate, which opens once the previous
            // image's scan has ended (one refresh minus the lead after its flip). A
            // floor past the gate would make every flip wait for the next blank.
            const double floor = sedici::fg::flip_floor(false, true, lastFlip, period, lead);
            const double gate = lastFlip > 0.0 ? lastFlip + period - lead : 0.0;
            if (counted && floor > gate) ++result.floorPastGate;
            double flip = std::max({target - early, ready, floor, gate});
            if (flip > ready) flip += c.oversleep;
            if (counted) {
                ++result.shown;
                result.late += flip > target + 0.001 ? 1u : 0u;
            }
            lastFlip = flip;
            t = flip + call;
        }
        job.done = t;
        cursor = t;
    }
    result.seconds = jobs[c.jobs - 1].entry - jobs[c.warmup].entry;
    return result;
}

void print_loop(const char* name, const LoopResult& r)
{
    std::printf(
        "  %s: planned %.1f fps, shown %.1f fps, late %.1f%%, reals dropped %u of %u, generated dropped %u, "
        "latency %.1f ms\n",
        name, r.plannedFps(), r.shownFps(), 100.0 * r.lateShare(), r.realsDropped, r.realsPlanned,
        r.generatedDropped, 1000.0 * r.latency());
}

} // namespace

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
    expect(sedici::fg::ticks_to_seconds(1, 0) == 0.0, "missing QPC frequency does not divide by zero");

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
    expect(plan.firstWait == 0.0, "an early cycle never waits before the current midpoint");

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
        const double work = (cycle % 2) ? 0.002 : 0.004;
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
    // The tear guard's flip floor; the presenter loop below also checks that it
    // never ends after the scan-line gate.
    {
        const double refresh = 1.0 / 75.0, lead = 0.00025;
        expect(sedici::fg::flip_floor(true, false, 0.0, refresh, lead) == 0.0 &&
                   sedici::fg::flip_floor(false, true, 0.0, refresh, lead) == 0.0 &&
                   std::abs(sedici::fg::flip_floor(true, false, 1.0, refresh, lead) - (1.0 + refresh + 0.0002)) <
                       1e-12 &&
                   std::abs(sedici::fg::flip_floor(false, true, 1.0, refresh, lead) -
                            (1.0 + refresh - lead - 0.0005)) < 1e-12 &&
                   sedici::fg::flip_floor(false, false, 1.0, refresh, lead) == 0.0,
               "flip floor: none before the first flip; timer one refresh + 0.2 ms; scan-line one refresh - lead - "
               "0.5 ms; none with the guard off");
    }
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
            const double nextContent = i + 1 < std::size(content) ? content[i + 1] : 0.0;
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
    struct Ratio {
        uint32_t p, q;
        const char* name;
    };
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
                   std::abs(minGap - expectedGap) < 1e-6 && std::abs(maxGap - expectedGap) < 1e-6,
               what);
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
        // A pause resets to the real frame alone, and below 20 real fps the
        // schedule stops generating.
        sedici::fg::OutputSchedule schedule;
        schedule.set_ratio(2, 1);
        for (uint32_t frame = 0; frame < 100; ++frame) (void)schedule.plan(10.0 + frame / 48.0);
        const auto paused = schedule.plan(20.0);
        expect(paused.count == 1 && paused.outputs[0].real, "2x schedule: a 0.25 s gap shows only the real frame");
        uint32_t slowGenerated = 0;
        for (uint32_t frame = 0; frame < 40; ++frame)
            slowGenerated += schedule.plan(21.0 + frame * 0.055).generated();
        expect(slowGenerated == 0, "2x schedule: below 20 real fps shows only real frames");
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
                if (frame >= 20) {
                    ++frames;
                    generated += step.generated() > 0 ? 1u : 0u;
                }
            }
            expect(generated >= frames * 95 / 100,
                   "2x schedule: jitter averaging 20 real fps still generates for almost every frame");
            // Steady 60 ms stays real-only, and recovery to 40 ms generates again.
            jitter.reset();
            uint32_t slowOnly = 0, recoveredCount = 0;
            for (uint32_t frame = 0; frame < 100; ++frame) slowOnly += jitter.plan(50.0 + frame * 0.06).generated();
            for (uint32_t frame = 0; frame < 100; ++frame)
                recoveredCount += jitter.plan(60.0 + frame * 0.04).generated();
            expect(slowOnly == 0 && recoveredCount > 90,
                   "2x schedule: a steady 60 ms interval is real-only and recovery to 40 ms resumes generation");
        }
    }
    {
        // Issue #1: closed presenter loop. 2x at 32 real fps (64 output fps) on a
        // 75 Hz VRR panel leaves ~2.3 ms per flip over the refresh, so a 2 ms
        // per-flip overhead still keeps it on time.
        const LoopResult twice = run_present_loop(LoopConfig{});
        print_loop("2x at 32 real fps, 75 Hz", twice);
        LoopConfig slowFlip;
        slowFlip.oversleep = 0.002;
        const LoopResult twiceSlowFlip = run_present_loop(slowFlip);
        print_loop("2x at 32 real fps, 2 ms per-flip overhead", twiceSlowFlip);
        // Late flips here (~7%) come from the median GPU estimate, #36; not asserted.
        expect(twice.shownFps() >= 0.95 * 64.0 && twice.realsDropped == 0 &&
                   twiceSlowFlip.shownFps() >= 0.95 * 64.0 && twiceSlowFlip.realsDropped == 0,
               "presenter loop: 2x at 32 real fps shows >= 95% of 64 fps and drops no real frame");
        expect(twice.floorPastGate == 0 && twiceSlowFlip.floorPastGate == 0,
               "presenter loop: the scan-line flip floor never ends after the scan-line gate");
        // Near the refresh (1.5x at 48 = 72 on 75 Hz) the same per-flip overhead
        // keeps the presenter on its floor, as adaptive did at 73 in the game (#39).
        // Its late share is #39 and is not asserted; the bounds below only catch a
        // change that starts dropping or losing more of the rate.
        LoopConfig nearRefresh;
        nearRefresh.interval = 1.0 / 48.0;
        nearRefresh.p = 3;
        nearRefresh.q = 2;
        const LoopResult near72 = run_present_loop(nearRefresh);
        print_loop("1.5x at 48 real fps, 75 Hz", near72);
        nearRefresh.oversleep = 0.002;
        const LoopResult near72SlowFlip = run_present_loop(nearRefresh);
        print_loop("1.5x at 48 real fps, 2 ms per-flip overhead", near72SlowFlip);
        expect(near72.shownFps() >= 0.95 * 72.0 && near72.realsDropped == 0 &&
                   near72SlowFlip.shownFps() >= 0.9 * 72.0 && near72SlowFlip.realsDropped == 0,
               "presenter loop: 1.5x at 48 real fps keeps >= 95% of 72 fps (>= 90% with 2 ms per flip), no real "
               "frame dropped");
        // Over the refresh the panel caps the output: outputs are dropped, but the
        // latency stays bounded and the panel stays busy.
        LoopConfig over;
        over.interval = 1.0 / 47.0;
        over.gpu = 0.012;
        const LoopResult overRefresh = run_present_loop(over);
        print_loop("2x at 47 real fps, 75 Hz (over refresh)", overRefresh);
        expect(overRefresh.shownFps() >= 0.95 * 75.0 && overRefresh.latency() <= 0.06 &&
                   overRefresh.realsDropped <= overRefresh.realsPlanned / 50,
               "presenter loop: 2x over the refresh shows >= 95% of 75 fps, latency <= 60 ms, <= 2% real frames "
               "dropped");
    }
    return failures == 0;
}

} // namespace fgcheck
