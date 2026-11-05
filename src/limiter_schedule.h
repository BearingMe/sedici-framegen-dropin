#pragma once

#include <cstdint>

// Pure scheduling decision for the real-frame limiter, in QPC ticks.
//
// Keeping this separate from the timer and the hook makes the catch-up rule
// testable without a clock: the caller supplies `now`, the previous deadline
// and the interval, and gets back the deadline to target this frame.
//
// Rules (milestone 2):
//  - the first frame seeds the schedule and never waits;
//  - every later frame targets `previous_deadline + interval`, so the average
//    rate is exactly the requested one and overshoot does not accumulate;
//  - a frame that arrives more than one full interval after its deadline
//    reseeds the schedule at `now` instead of bursting to catch up;
//  - a frame that is already past its deadline never waits.

namespace sedici {

struct LimiterStep {
    uint64_t deadline = 0; // QPC ticks to aim at
    bool wait = false;     // false when the frame is already late
    bool late = false;     // arrived at or after the deadline
    bool reset = false;    // schedule was reseeded because the frame was very late
};

inline LimiterStep limiter_step(uint64_t now, uint64_t previous_deadline, bool have_previous,
                                uint64_t interval) noexcept
{
    LimiterStep step;
    if (!have_previous || interval == 0) {
        step.deadline = now;
        return step;
    }

    uint64_t deadline = previous_deadline + interval;
    if (now > deadline + interval) {
        deadline = now;
        step.reset = true;
    }

    step.deadline = deadline;
    step.late = now >= deadline;
    step.wait = !step.late;
    return step;
}

} // namespace sedici
