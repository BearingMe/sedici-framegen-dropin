#include "stats.h"

#include "log.h"

#include "ffx_framegeneration.h"

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

constexpr size_t kReportEvery = 300;
constexpr size_t kMaxChainItems = 64;

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
    g_window.last_prepare_qpc = 0;
    g_window.last_present_qpc = 0;
    g_window.have_prepare = false;
    g_window.have_present = false;
    g_window.presents_since_prepare = 0;
}

bool find_prepare(const ffxDispatchDescHeader* desc, float& frame_time_delta)
{
    std::set<const ffxApiHeader*> seen;
    for (const ffxApiHeader* item = desc; item; item = item->pNext) {
        if (!seen.insert(item).second) break;
        switch (item->type) {
        case FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE:
            frame_time_delta = reinterpret_cast<const ffxDispatchDescFrameGenerationPrepare&>(*item).frameTimeDelta;
            return true;
        case FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2:
            frame_time_delta = reinterpret_cast<const ffxDispatchDescFrameGenerationPrepareV2&>(*item).frameTimeDelta;
            return true;
        default:
            break;
        }
        if (seen.size() >= kMaxChainItems) break;
    }
    return false;
}

} // namespace

void stats_on_dispatch(const ffxDispatchDescHeader* desc) noexcept
{
    if (log_level() < 1 || !desc) return;

    try {
        if (!g_qpc_frequency) return;

        float frame_time_delta = 0.0f;
        if (!find_prepare(desc, frame_time_delta)) return;

        const uint64_t now = qpc_now();
        std::string report;
        {
            std::lock_guard<std::mutex> lock(g_mutex);

            if (g_window.have_prepare) {
                g_window.prepare_interval.push_back(qpc_ms(now - g_window.last_prepare_qpc));
                g_window.presents_per_prepare.push_back(g_window.presents_since_prepare);
            }

            g_window.frame_time_delta.push_back(static_cast<double>(frame_time_delta));
            g_window.last_prepare_qpc = now;
            g_window.have_prepare = true;
            g_window.presents_since_prepare = 0;

            if (g_window.frame_time_delta.size() >= kReportEvery) {
                report = stats_line_locked();
                reset_locked();
            }
        }

        if (!report.empty()) log_event(1, report);
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

#pragma warning(pop)
