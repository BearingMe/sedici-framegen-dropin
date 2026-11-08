#pragma once

// Sedici configuration.
//
// The effective settings come from two places, in increasing priority:
//
//   1. `sedici.toml`, beside the deployed DLL, parsed with toml++.
//   2. Environment variables (`SEDICI_PACING`, `SEDICI_ASYNC`,
//      `SEDICI_LIMIT_FPS`, `MFSRFG_LOG`).
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

// Where an effective value came from. Used only by the once-at-load log.
enum class Source {
    Default,
    File,
    Env,
};

// `[limiter]`
struct LimiterSettings {
    // Target real-frame rate. 0 disables the limiter, which is the default and
    // is exact milestone-1 behaviour. A fraction is allowed: with 2x frame
    // generation, 36.5 gives 73 output fps, the usual target for a 75 Hz panel
    // when VSync cannot be used.
    double fps = 0.0;

    // Final busy-wait window before the deadline, in microseconds. The waitable
    // timer covers everything before this window.
    int spin_us = 1000;

    Source fps_source = Source::Default;
    Source spin_us_source = Source::Default;
};

// `[ui]`
enum class UiMode {
    Off,
    Mini,
    Full,
};

enum class UiScaleMode {
    Auto,
    Fixed,
};

struct UiSettings {
    UiMode mode = UiMode::Off;
    UiScaleMode scale_mode = UiScaleMode::Auto;
    float scale = 1.0f;

    // Modifier+key, lowercase, for example "alt+f10".
    std::string hotkey = "alt+f10";

    Source mode_source = Source::Default;
    Source scale_source = Source::Default;
    Source hotkey_source = Source::Default;
};

struct LogSettings {
    // 0 = errors only, 1 = normal, 2 = verbose. The legacy `MFSRFG_LOG`
    // environment variable still overrides this.
    int level = 1;
    Source level_source = Source::Default;
};

// `[fg]`
struct FrameGenerationSettings {
    // How the wrapper presents its two outputs per cycle.
    enum class Present {
        // "game": use the game's own SyncInterval and flags, including
        // ALLOW_TEARING, from the game's thread. Lowest latency, but flips are
        // paced in CPU time and can land mid-scan and tear (finding 008).
        Game,
        // "vsync" (alias "vblank"): present with SyncInterval=1 and without
        // ALLOW_TEARING, so each output takes its own vertical blank. Tear-free
        // in game sessions; costs up to one refresh of latency and expects a real
        // rate at or below half the panel refresh.
        Vsync,
        // "vrr": present from a dedicated thread and queue with the game's own
        // flags, timing each flip from when its image is finished on the GPU, so
        // a variable-refresh display receives evenly spaced flips. Combined with
        // `tear_guard`. The default: with "scanline" it was tear-free in the game
        // session (finding 008). Falls back to "vsync" if the native chain
        // cannot be recreated on Sedici's present queue or RTSS is injected.
        Vrr,
    };

    // What "vrr" does to keep a flip from landing while the panel is still
    // drawing the previous image. Ignored by the other present modes.
    enum class TearGuard {
        // "scanline": read the panel's current scan line and hold the flip until
        // the panel is in its vertical blank (or about to be).
        Scanline,
        // "timer": hold the flip until at least one refresh period has passed
        // since the previous flip. No hardware query; VRR displays only.
        Timer,
        // "off": flip as soon as the pacing schedule says so.
        Off,
    };

    // Output frames per real frame, `[fg] multiplier`. Only "vrr" schedules
    // anything but 2x; "vsync" and "game" always present one midpoint per
    // real frame. Below 2x outputs stay evenly paced, so some real frames are
    // shown only as nearby generated frames.
    enum class Multiplier {
        X2,       // "2x": one generated frame between every two real frames
        X1_75,    // "1.75x": 7 outputs per 4 real frames
        X1_5,     // "1.5x": 3 outputs per 2 real frames
        X1_25,    // "1.25x": 5 outputs per 4 real frames
        Adaptive, // "adaptive": aim for `target_fps` outputs per second
    };

    Multiplier multiplier = Multiplier::X2;
    Source multiplier_source = Source::Default;
    // "adaptive" only. 0 = three below the display's refresh rate.
    double target_fps = 0.0;
    Source target_fps_source = Source::Default;
    Present present = Present::Vrr;
    Source present_source = Source::Default;
    TearGuard tear_guard = TearGuard::Scanline;
    Source tear_guard_source = Source::Default;
};

// Config/report spelling of the enums above.
const char* present_name(FrameGenerationSettings::Present present) noexcept;
const char* tear_guard_name(FrameGenerationSettings::TearGuard guard) noexcept;
const char* multiplier_name(FrameGenerationSettings::Multiplier multiplier) noexcept;

struct Settings {
    LimiterSettings limiter;
    LogSettings log;
    UiSettings ui;
    FrameGenerationSettings fg;

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
