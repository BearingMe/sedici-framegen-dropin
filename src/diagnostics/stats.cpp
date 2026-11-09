#include "diagnostics/stats.h"

#include "diagnostics/log.h"

#include "ffx_framegeneration.h"

#include <atomic>
#include <windows.h>

#pragma warning(push)
#pragma warning(disable : 4996) // The SDK marks the legacy prepare desc deprecated.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {
std::atomic<StatsExtraLineFn> g_extra_line{nullptr};

constexpr size_t kReportEvery = 300;
constexpr size_t kMaxChainItems = 64;

// ---------------------------------------------------------------------------
// Stall detector
//
// Run I of docs/findings/002 showed five episodic stalls in a 30 s capture: a
// prepare interval of 24-72 ms followed immediately by a sub-2 ms one, i.e. a
// hitch and a catch-up present. Nothing else in the log explained them. This
// detector reports each one as it happens, with the context that might explain
// it (frame generation reset, presents queued behind the prepare, or a
// swapchain/configure event close in time).
//
// Design notes:
//  - The baseline is a rolling median of the last 61 prepare intervals, so it
//    tracks resolution changes and frame-rate caps without the stall itself
//    skewing the comparison (the median is taken before the new sample is
//    added).
//  - A stall must clear both a relative threshold (3x the median) and an
//    absolute floor (25 ms), so a slow but steady run never triggers.
//  - At most one line per second, so a genuinely broken minute cannot flood a
//    64 KB log.
// ---------------------------------------------------------------------------

constexpr size_t kStallMedianWindow = 61;
constexpr size_t kStallMinSamples = 10;
constexpr double kStallRatio = 3.0;
constexpr double kStallFloorMs = 25.0;
constexpr double kStallLogIntervalMs = 1000.0;

constexpr size_t kEventRing = 32;
constexpr double kEventWindowMs = 1000.0;

struct Event {
    uint64_t qpc = 0;
    const char* kind = nullptr;
};

// Fixed-size ring: when `count` is below capacity the live entries are
// [0, count); once it is full every slot is live. That keeps both the median
// and the event scan correct without a modulo walk.
struct Ring {
    double values[kStallMedianWindow]{};
    size_t next = 0;
    size_t count = 0;

    void push(double value) noexcept
    {
        values[next] = value;
        next = (next + 1) % kStallMedianWindow;
        if (count < kStallMedianWindow) ++count;
    }
};

struct EventRing {
    Event values[kEventRing]{};
    size_t next = 0;
    size_t count = 0;

    void push(const Event& event) noexcept
    {
        values[next] = event;
        next = (next + 1) % kEventRing;
        if (count < kEventRing) ++count;
    }
};

struct Window {
    std::vector<double> frame_time_delta;
    std::vector<double> prepare_interval;
    std::vector<double> present_interval;
    std::vector<unsigned> presents_per_prepare;

    uint64_t last_prepare_qpc = 0;
    uint64_t last_present_qpc = 0;
    bool have_prepare = false;
    bool have_present = false;
    unsigned presents_since_prepare = 0;

    Ring stall_baseline;
    EventRing events;
    bool have_stall_log = false;
    uint64_t last_stall_log_qpc = 0;
    uint64_t stall_count = 0;
    double last_stall_interval_ms = 0.0;
    uint32_t render_width = 0;
    uint32_t render_height = 0;
};

std::mutex g_mutex;
Window g_window;
const uint64_t g_qpc_frequency = [] {
    LARGE_INTEGER frequency{};
    return QueryPerformanceFrequency(&frequency) ? static_cast<uint64_t>(frequency.QuadPart) : 0;
}();

double qpc_ms(uint64_t delta) noexcept
{
    if (!g_qpc_frequency) return 0.0;
    return static_cast<double>(delta) * 1000.0 / static_cast<double>(g_qpc_frequency);
}

uint64_t qpc_now() noexcept
{
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return static_cast<uint64_t>(value.QuadPart);
}

double average(const std::vector<double>& values) noexcept
{
    if (values.empty()) return 0.0;
    double sum = 0.0;
    for (double value : values) sum += value;
    return sum / static_cast<double>(values.size());
}

double minimum(const std::vector<double>& values) noexcept
{
    return values.empty() ? 0.0 : *std::min_element(values.begin(), values.end());
}

double maximum(const std::vector<double>& values) noexcept
{
    return values.empty() ? 0.0 : *std::max_element(values.begin(), values.end());
}

double stddev(const std::vector<double>& values) noexcept
{
    if (values.size() < 2) return 0.0;
    const double mean = average(values);
    double sum = 0.0;
    for (double value : values) {
        const double delta = value - mean;
        sum += delta * delta;
    }
    return std::sqrt(sum / static_cast<double>(values.size()));
}

double percentile(std::vector<double> values, double fraction) noexcept
{
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    size_t index = static_cast<size_t>(std::ceil(fraction * static_cast<double>(values.size())));
    if (index == 0) index = 1;
    if (index > values.size()) index = values.size();
    return values[index - 1];
}

double median_of(const Ring& ring) noexcept
{
    if (ring.count == 0) return 0.0;
    double scratch[kStallMedianWindow];
    std::copy(ring.values, ring.values + ring.count, scratch);
    const size_t middle = ring.count / 2;
    std::nth_element(scratch, scratch + middle, scratch + ring.count);
    return scratch[middle];
}

std::string stats_line_locked()
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(3);
    out << "stats prepares=" << g_window.frame_time_delta.size();

    out << " frameTimeDelta_ms{avg=" << average(g_window.frame_time_delta)
        << " min=" << minimum(g_window.frame_time_delta)
        << " max=" << maximum(g_window.frame_time_delta)
        << " sd=" << stddev(g_window.frame_time_delta) << '}';

    out << " prepareQpc_ms{avg=" << average(g_window.prepare_interval)
        << " min=" << minimum(g_window.prepare_interval)
        << " max=" << maximum(g_window.prepare_interval)
        << " sd=" << stddev(g_window.prepare_interval) << '}';

    if (g_window.present_interval.empty()) {
        out << " presentInterval_ms=n/a";
    } else {
        out << " presentInterval_ms{avg=" << average(g_window.present_interval)
            << " p50=" << percentile(g_window.present_interval, 0.50)
            << " p95=" << percentile(g_window.present_interval, 0.95)
            << " p99=" << percentile(g_window.present_interval, 0.99)
            << " max=" << maximum(g_window.present_interval) << '}';
    }

    if (g_window.presents_per_prepare.empty()) {
        out << " presentsPerPrepare=n/a";
    } else {
        double sum = 0.0;
        unsigned min_value = g_window.presents_per_prepare[0];
        unsigned max_value = g_window.presents_per_prepare[0];
        for (unsigned value : g_window.presents_per_prepare) {
            sum += static_cast<double>(value);
            min_value = std::min(min_value, value);
            max_value = std::max(max_value, value);
        }
        out << " presentsPerPrepare{avg=" << (sum / static_cast<double>(g_window.presents_per_prepare.size()))
            << " min=" << min_value << " max=" << max_value << '}';
    }

    return out.str();
}

void reset_locked() noexcept
{
    g_window.frame_time_delta.clear();
    g_window.prepare_interval.clear();
    g_window.present_interval.clear();
    g_window.presents_per_prepare.clear();
    g_window.have_present = false;
    g_window.presents_since_prepare = 0;
    // last_prepare_qpc is intentionally kept: it is the interval baseline for
    // the stall detector, which spans stats windows.
}

// The prepare descs carry the same information under two type ids. The
// deprecated one predates `reset` (its field is `unused_reset`), so `reset` is
// only reported for the V2 form.
struct PrepareInfo {
    float frame_time_delta = 0.0f;
    uint64_t frame_id = 0;
    bool reset = false;
    bool reset_known = false;
    uint32_t render_width = 0;
    uint32_t render_height = 0;
};

bool find_prepare(const ffxDispatchDescHeader* desc, PrepareInfo& info)
{
    std::set<const ffxApiHeader*> seen;
    for (const ffxApiHeader* item = desc; item; item = item->pNext) {
        if (!seen.insert(item).second) break;
        switch (item->type) {
        case FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE: {
            const auto& prepare =
                reinterpret_cast<const ffxDispatchDescFrameGenerationPrepare&>(*item);
            info.frame_time_delta = prepare.frameTimeDelta;
            info.frame_id = prepare.frameID;
            info.reset = false;
            info.reset_known = false;
            info.render_width = prepare.renderSize.width;
            info.render_height = prepare.renderSize.height;
            return true;
        }
        case FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2: {
            const auto& prepare =
                reinterpret_cast<const ffxDispatchDescFrameGenerationPrepareV2&>(*item);
            info.frame_time_delta = prepare.frameTimeDelta;
            info.frame_id = prepare.frameID;
            info.reset = prepare.reset;
            info.reset_known = true;
            info.render_width = prepare.renderSize.width;
            info.render_height = prepare.renderSize.height;
            return true;
        }
        default:
            break;
        }
        if (seen.size() >= kMaxChainItems) break;
    }
    return false;
}

// Builds the one-line stall report. Caller holds the lock.
std::string stall_line_locked(const PrepareInfo& info, double interval_ms, double median_ms,
                              uint64_t now_qpc) noexcept
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(3);
    out << "sedici: stall prepare interval=" << interval_ms
        << " ms median=" << median_ms
        << " ms ratio=" << (median_ms > 0.0 ? interval_ms / median_ms : 0.0) << 'x'
        << " frameID=" << info.frame_id;

    if (info.reset_known) {
        out << " reset=" << (info.reset ? "true" : "false");
    } else {
        out << " reset=n/a";
    }

    out << " presentsSincePrepare=" << g_window.presents_since_prepare;

    // Anything that might have disturbed the swapchain in the last second.
    size_t recent = 0;
    const Event* newest = nullptr;
    double newest_age_ms = 0.0;
    for (size_t i = 0; i < g_window.events.count; ++i) {
        const Event& event = g_window.events.values[i];
        const double age_ms = qpc_ms(now_qpc - event.qpc);
        if (age_ms > kEventWindowMs) continue;
        ++recent;
        if (!newest || event.qpc > newest->qpc) {
            newest = &event;
            newest_age_ms = age_ms;
        }
    }

    out << " swapchainEvents1s{count=" << recent;
    if (newest) {
        out << " last=" << newest->kind << ' ' << newest_age_ms << " ms ago";
    }
    out << '}';

    out << " qpc=" << now_qpc;
    return out.str();
}

} // namespace

void stats_on_dispatch(const ffxDispatchDescHeader* desc) noexcept
{
    if (!desc) return;

    try {
        if (!g_qpc_frequency) return;

        PrepareInfo info;
        if (!find_prepare(desc, info)) return;

        const uint64_t now = qpc_now();
        std::string report;
        std::string stall;
        {
            std::lock_guard<std::mutex> lock(g_mutex);

            if (g_window.have_prepare) {
                const double interval_ms = qpc_ms(now - g_window.last_prepare_qpc);

                // Compare against the baseline that predates this sample, so a
                // stall cannot hide itself in its own median.
                const double median_ms = median_of(g_window.stall_baseline);
                const bool enough_samples = g_window.stall_baseline.count >= kStallMinSamples;
                const bool rate_limited =
                    g_window.have_stall_log &&
                    qpc_ms(now - g_window.last_stall_log_qpc) < kStallLogIntervalMs;

                if (enough_samples && interval_ms > kStallRatio * median_ms &&
                    interval_ms >= kStallFloorMs) {
                    ++g_window.stall_count;
                    g_window.last_stall_interval_ms = interval_ms;
                    if (!rate_limited) {
                        stall = stall_line_locked(info, interval_ms, median_ms, now);
                        g_window.have_stall_log = true;
                        g_window.last_stall_log_qpc = now;
                    }
                }

                g_window.stall_baseline.push(interval_ms);

                g_window.prepare_interval.push_back(interval_ms);
                g_window.presents_per_prepare.push_back(g_window.presents_since_prepare);
            }

            g_window.frame_time_delta.push_back(static_cast<double>(info.frame_time_delta));
            g_window.render_width = info.render_width;
            g_window.render_height = info.render_height;
            g_window.last_prepare_qpc = now;
            g_window.have_prepare = true;
            g_window.presents_since_prepare = 0;

            if (g_window.frame_time_delta.size() >= kReportEvery) {
                report = stats_line_locked();
                reset_locked();
            }
        }

        if (!stall.empty()) log_event(1, stall);
        if (!report.empty()) {
            log_event(1, report);
            if (const StatsExtraLineFn extra = g_extra_line.load(std::memory_order_acquire)) {
                const std::string notable = extra();
                if (!notable.empty()) log_event(1, notable);
            }
        }
    } catch (...) {
        log_error("failed to record prepare stats");
    }
}

void stats_on_present(uint64_t qpc) noexcept
{
    if (log_level() < 1) return;

    try {
        if (!g_qpc_frequency) return;

        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_window.have_present) {
            g_window.present_interval.push_back(qpc_ms(qpc - g_window.last_present_qpc));
        }
        g_window.last_present_qpc = qpc;
        g_window.have_present = true;
        ++g_window.presents_since_prepare;
    } catch (...) {
        log_error("failed to record present stats");
    }
}

void stats_on_swapchain_event(const char* kind) noexcept
{
    if (log_level() < 1 || !kind) return;

    try {
        if (!g_qpc_frequency) return;
        std::lock_guard<std::mutex> lock(g_mutex);
        g_window.events.push(Event{qpc_now(), kind});
    } catch (...) {
        log_error("failed to record a swapchain event");
    }
}

namespace {
std::atomic<uint64_t> g_real_dropped{0};
}

void stats_set_extra_line(StatsExtraLineFn fn) noexcept { g_extra_line.store(fn, std::memory_order_release); }

void stats_on_real_dropped() noexcept { g_real_dropped.fetch_add(1, std::memory_order_relaxed); }
uint64_t stats_real_dropped() noexcept { return g_real_dropped.load(std::memory_order_relaxed); }

uint64_t stats_stall_count() noexcept
{
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_window.stall_count;
    } catch (...) {
        return 0;
    }
}

double stats_last_stall_ms() noexcept
{
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_window.last_stall_interval_ms;
    } catch (...) {
        return 0.0;
    }
}

void stats_render_size(uint32_t& width, uint32_t& height) noexcept
{
    width = 0;
    height = 0;
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        width = g_window.render_width;
        height = g_window.render_height;
    } catch (...) {
    }
}

#pragma warning(pop)
