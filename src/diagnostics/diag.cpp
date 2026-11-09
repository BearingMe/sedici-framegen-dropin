#include "diagnostics/diag.h"

#include "fg_bookkeeping_faults.h"
#include "fg_route.h"
#include "fg_swapchain.h"

namespace sedici {

DiagSnapshot diag_snapshot() noexcept
{
    DiagSnapshot s;
    s.log = log_stats();
    s.swapchains_parked = fg::Swapchain::retained_now();
    s.swapchains_parked_total = fg::swapchain_retention_count();
    s.swapchains_released_total = fg::swapchain_release_count();
    s.router_owners_parked = fg::route_parked_count();
    s.settings_io_stalls = settings_io_stall_count();
    s.settings_io_stalled_now = settings_io_stalled();
    return s;
}

std::string diag_notable_line() noexcept
{
    try {
        const DiagSnapshot s = diag_snapshot();
        const bool notable = s.log.dropped || s.log.suppressed || s.log.lost || s.log.sink_failures ||
                             !s.log.sink_ok || s.swapchains_parked || s.router_owners_parked ||
                             s.settings_io_stalls || s.settings_io_stalled_now;
        if (!notable) return {};
        return "diag: log{queued=" + std::to_string(s.log.queued) + " written=" + std::to_string(s.log.written) +
               " dropped=" + std::to_string(s.log.dropped) + " suppressed=" + std::to_string(s.log.suppressed) +
               " lost=" + std::to_string(s.log.lost) + " sink_failures=" + std::to_string(s.log.sink_failures) +
               " max_depth=" + std::to_string(s.log.max_depth) + " sink=" + (s.log.sink_ok ? "ok" : "failing") +
               "} retention{swapchains_parked=" + std::to_string(s.swapchains_parked) +
               " parked_total=" + std::to_string(s.swapchains_parked_total) +
               " released_total=" + std::to_string(s.swapchains_released_total) +
               " router_parked=" + std::to_string(s.router_owners_parked) +
               "} settings_io{stalls=" + std::to_string(s.settings_io_stalls) +
               " stalled_now=" + (s.settings_io_stalled_now ? "yes" : "no") +
               "} unavailable{" + DiagSnapshot::unavailable + "}";
    } catch (...) {
        return {};
    }
}

} // namespace sedici
