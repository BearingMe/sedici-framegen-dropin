#include "timing.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>

namespace sedici {
namespace {

const uint64_t g_frequency = [] {
    LARGE_INTEGER frequency{};
    return QueryPerformanceFrequency(&frequency) ? static_cast<uint64_t>(frequency.QuadPart) : 0;
}();

struct Ring {
    std::array<float, kTimingSamples> samples{};
    size_t head = 0;
    size_t count = 0;
    uint64_t last_qpc = 0;
    bool have_last = false;
};

std::mutex g_mutex;
Ring g_real;
Ring g_output;

void push(Ring& ring, uint64_t qpc) noexcept
{
    if (!g_frequency) return;
    if (ring.have_last && qpc > ring.last_qpc) {
        const double ms = static_cast<double>(qpc - ring.last_qpc) * 1000.0 /
                          static_cast<double>(g_frequency);
        if (ms > 0.0 && ms < 10000.0) {
            ring.samples[ring.head] = static_cast<float>(ms);
            ring.head = (ring.head + 1) % kTimingSamples;
            if (ring.count < kTimingSamples) ++ring.count;
        }
    }
    ring.last_qpc = qpc;
    ring.have_last = true;
}

TimingStats stats_of(const Ring& ring) noexcept
{
    TimingStats stats;
    stats.count = ring.count;
    if (ring.count == 0) return stats;

    std::array<float, kTimingSamples> values{};
    for (size_t i = 0; i < ring.count; ++i) values[i] = ring.samples[i];
    std::sort(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(ring.count));

    double sum = 0.0;
    for (size_t i = 0; i < ring.count; ++i) sum += values[i];
    stats.average_ms = static_cast<float>(sum / static_cast<double>(ring.count));
    stats.max_ms = values[ring.count - 1];

    const auto percentile = [&](double fraction) {
        size_t index = static_cast<size_t>(std::ceil(fraction * static_cast<double>(ring.count)));
        if (index == 0) index = 1;
        if (index > ring.count) index = ring.count;
        return values[index - 1];
    };
    stats.p50_ms = percentile(0.50);
    stats.p99_ms = percentile(0.99);
    stats.fps = stats.average_ms > 0.0f ? 1000.0f / stats.average_ms : 0.0f;
    return stats;
}

size_t copy_samples(const Ring& ring, float* out, size_t capacity) noexcept
{
    if (!out || capacity == 0 || ring.count == 0) return 0;
    const size_t count = ring.count < capacity ? ring.count : capacity;
    const size_t start = (ring.head + kTimingSamples - ring.count) % kTimingSamples;
    for (size_t i = 0; i < count; ++i) {
        out[i] = ring.samples[(start + i) % kTimingSamples];
    }
    return count;
}

} // namespace

void timing_on_real_present(uint64_t qpc) noexcept
{
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        push(g_real, qpc);
    } catch (...) {
    }
}

void timing_on_output_present(uint64_t qpc) noexcept
{
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        push(g_output, qpc);
    } catch (...) {
    }
}

TimingStats timing_real_stats() noexcept
{
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        return stats_of(g_real);
    } catch (...) {
        return {};
    }
}

TimingStats timing_output_stats() noexcept
{
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        return stats_of(g_output);
    } catch (...) {
        return {};
    }
}

size_t timing_real_samples(float* out, size_t capacity) noexcept
{
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        return copy_samples(g_real, out, capacity);
    } catch (...) {
        return 0;
    }
}

size_t timing_output_samples(float* out, size_t capacity) noexcept
{
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        return copy_samples(g_output, out, capacity);
    } catch (...) {
        return 0;
    }
}

} // namespace sedici
