#pragma once

#include "diagnostics/log.h"
#include "settings/settings_io.h"

#include <cstdint>
#include <string>

namespace sedici {

// What Sedici's own safety machinery has done, read without taking any lock the
// game's calls hold. Every counter is a count of something that really happened;
// anything Sedici cannot measure is named in `unavailable` instead of being
// reported as zero.
struct DiagSnapshot {
    LogStats log;
    uint32_t swapchains_parked = 0;      // timed-out swapchains kept alive until their GPU work retires
    uint64_t swapchains_parked_total = 0;
    uint64_t swapchains_released_total = 0;
    uint32_t router_owners_parked = 0;   // refused destroys waiting for their work to retire
    uint64_t settings_io_stalls = 0;     // settings jobs reported as not responding
    bool settings_io_stalled_now = false;// a reported-stalled worker is still running

    // Present timestamps come from the CPU. They are not physical flips and not
    // input-to-photon latency, and Sedici has no source for either.
    static constexpr const char* unavailable = "physical_flip_time,input_to_photon_latency";
};

DiagSnapshot diag_snapshot() noexcept;

// One line for the log, or empty when nothing is out of the ordinary (no drops,
// suppression, sink failure or parked objects). Reported with the periodic stats.
std::string diag_notable_line() noexcept;

} // namespace sedici
