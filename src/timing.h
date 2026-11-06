#pragma once

#include <cstddef>
#include <cstdint>

namespace sedici {

constexpr size_t kTimingSamples = 360;

struct TimingStats {
    float average_ms = 0.0f;
    float p50_ms = 0.0f;
    float p99_ms = 0.0f;
    float max_ms = 0.0f;
    float fps = 0.0f;
    size_t count = 0;
};

// Real frames: called from the wrapper Present hook, before the limiter wait.
void timing_on_real_present(uint64_t qpc) noexcept;

// Output frames: called from the FFX present callback, real and generated.
void timing_on_output_present(uint64_t qpc) noexcept;

TimingStats timing_real_stats() noexcept;
TimingStats timing_output_stats() noexcept;

// Copies intervals in chronological order (oldest first) for PlotLines.
// Returns the number copied.
size_t timing_real_samples(float* out, size_t capacity) noexcept;
size_t timing_output_samples(float* out, size_t capacity) noexcept;

} // namespace sedici
