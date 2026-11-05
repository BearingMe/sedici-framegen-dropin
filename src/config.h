#pragma once

// Sedici configuration.
//
// The effective settings come from two places, in increasing priority:
//
//   1. `sedici.toml`, beside the deployed DLL, parsed with toml++.
//   2. Environment variables (`SEDICI_PACING`, `SEDICI_ASYNC`, `MFSRFG_LOG`).
//
// The file is read *once*, on the first `ffx*` call, and never again. Doing it
// in `DllMain` would run under the loader lock, so the first call is used
// instead; in practice that is roughly a second into process start, long before
// the game creates a frame generation context.
//
// Nothing in here can fail the game's call. Every problem - a missing file, a
// syntax error, a key with the wrong type, an unknown value - is written to
// `sedici.log` with its line and column, and the affected key falls back to its
// built-in default. The rest of the file still applies.

#include <cstdint>
#include <string>

namespace sedici {

// `pacing.mode`
enum class PacingMode {
    // Forward everything untouched. This is milestone-0 behavior: no extra
    // configure call is ever made.
    Off,

    // After a frame generation swapchain context is created, send the swapchain
    // frame pacing tuning configure with `allowHybridSpin = true`.
    Hybrid,
};

// `async.mode`
enum class AsyncMode {
    // Forward `allowAsyncWorkloads` exactly as the game set it.
    Game,

    // Clear `FFX_FRAMEGENERATION_ENABLE_ASYNC_WORKLOAD_SUPPORT` from the frame
    // generation create flags and force `allowAsyncWorkloads = false` in every
    // frame generation configure desc.
    Off,
};

// Where an effective value came from. Used only by the once-at-load log.
enum class Source {
    Default,
    File,
    Env,
};

struct PacingSettings {
    PacingMode mode = PacingMode::Hybrid;

    // `FfxApiSwapchainFramePacingTuning`. The defaults match the SDK's
    // documented defaults, except `allowHybridSpin`, which the SDK defaults to
    // false and Sedici defaults to true on purpose.
    float safety_margin_ms = 0.1f;
    float variance_factor = 0.1f;
    uint32_t hybrid_spin_time = 2;
    bool wait_for_single_object_on_fence = false;

    Source mode_source = Source::Default;
    Source safety_margin_ms_source = Source::Default;
    Source variance_factor_source = Source::Default;
    Source hybrid_spin_time_source = Source::Default;
    Source wait_for_single_object_on_fence_source = Source::Default;
};

struct AsyncSettings {
    AsyncMode mode = AsyncMode::Game;
    Source mode_source = Source::Default;
};

struct LogSettings {
    // 0 = errors only, 1 = normal, 2 = verbose. The legacy `MFSRFG_LOG`
    // environment variable still overrides this.
    int level = 1;
    Source level_source = Source::Default;
};

struct Settings {
    PacingSettings pacing;
    AsyncSettings async;
    LogSettings log;

    // Absolute path of the config file that was consulted, or empty when the
    // DLL directory could not be resolved.
    std::wstring path;

    // True when `sedici.toml` exists beside the DLL.
    bool file_exists = false;

    // True when the file existed and parsed, so at least some values may have
    // come from it. A file that exists but fails to parse leaves this false.
    bool file_parsed = false;
};

// The effective configuration. Loaded on first use, then immutable.
const Settings& settings() noexcept;

// "default", "file" or "env", for the provenance column of the load log.
const char* source_name(Source source) noexcept;

} // namespace sedici
