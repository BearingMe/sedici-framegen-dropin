# Configuration

> Reference moved out of the [README](../../README.md); the text is unchanged.

The target is the output rate the screen receives: `0` is uncapped (no wait is added),
`N` caps at N, and `"auto"` matches the monitor with the low-lag rule
`floor(Hz - Hz^2 / 3600)` (59 at 60 Hz, 73 at 75 Hz, 116 at 120 Hz, 138 at 144 Hz, 224 at 240 Hz),
uncapped if the refresh rate cannot be read. Sedici enforces it by capping real frames at
target / output ratio (73 on a 75 Hz screen at 2x caps the game at 36.5). Some games, such as
Cyberpunk 2077, have their own frame cap, which is why the default is uncapped.

### Why these defaults

`docs/findings/002-pacing-baseline.md` measured nine configurations of AMD's own
frame generation on a 75 Hz panel. The only thing that separated a good run from a
bad one was the tail, not the median: a clean tail came from capping the game's
real frames near 37 fps. That is why Sedici owns presentation and offers the
limiter, and why the `vrr` mode times each flip from when its image is finished on
the GPU.

### Why a limiter

Run G in `002` used the game's own 37 fps cap with 2x frame generation and
produced the tightest distribution in the whole set: **74.0 fps output, standard
deviation 0.324 ms, worst interval 15.4 ms, and zero intervals outside
0.5-1.5x the median**. Cyberpunk will not let Anti-Lag 2 and Maximum FPS be
enabled together, however. The limiter moves that cap into Sedici, so the game
can keep Anti-Lag 2 on and run uncapped while the real presents are paced to
`limiter.fps`.
## Configuration

`sedici.toml` sits beside the deployed DLL. It is read **once**, on the first
`ffx*` call of the process, and never again.

Reading it there rather than in `DllMain` is deliberate: `DllMain` runs under the
loader lock, where file and allocation work is a deadlock risk. The first `ffx*`
call happens roughly a second into process start and long before the game
creates a frame generation context, so it is a load-time read in every sense
that matters.

Every key is optional. A missing key uses its built-in default. A missing file
means every default applies.

### Removed sections

`[pacing]`, `[async]` and `fg.mode` belonged to the era when Sedici forwarded to
AMD's frame generation. They no longer do anything. A file that still contains
them loads normally and the log says they are ignored. The matching environment
variables `SEDICI_PACING` and `SEDICI_ASYNC` are no longer read.

### `[fg]`

| Key | Type | Default | Valid values | Effect |
|---|---|---|---|---|
| `present` | string | `"vrr"` | `"vrr"`, `"vsync"` (alias `"vblank"`), `"game"` | how output frames are presented. `vrr` presents from a dedicated thread, timing each flip from when its image is finished on the GPU; it falls back to `vsync` while RTSS is running or when the native chain cannot be recreated. `vsync` presents with `SyncInterval=1` and no `ALLOW_TEARING`: tear-free, up to one refresh of latency, real rate at or below half the refresh. `game` passes the game's own sync interval and flags through, which is lowest latency but can tear |
| `multiplier` | string | `"2x"` | `"2x"`, `"1.75x"`, `"1.5x"`, `"1.25x"`, `"adaptive"` | output frames per real frame; only `vrr` schedules anything but 2x. Keep real fps times the multiplier under the refresh rate |
| `target_fps` | number or `"auto"` | `0` | `0` to `1000`, or `"auto"` | the one frame-rate target, in output frames per second: `0` uncapped, `N` capped, `"auto"` = `floor(Hz - Hz^2/3600)` of the monitor (uncapped if unknown). With `adaptive` it is also the rate to aim for; `0` then behaves like `"auto"`. Replaces `limiter.fps` |
| `tear_guard` | string | `"scanline"` | `"scanline"`, `"timer"`, `"off"` | only with `vrr`; how a flip is kept from landing while the panel draws the previous image |

Sedici owns output pacing completely. A game's request to tune AMD's swapchain
pacing is accepted and ignored, so the game's own call sequence still succeeds.
Measurements of these modes are in
[the current findings](../findings/007-fg-baseline.md#vblank-presentation-removes-the-remaining-tearing)
and [finding 008](../findings/008-fg-tearing.md); none has been repeated since
the AMD backend was removed.

### `[limiter]`

| Key | Type | Default | Valid values | Effect |
|---|---|---|---|---|
| `fps` | number | absent | `0` to `1000` | **deprecated**, replaced by `fg.target_fps`. It capped real frames; it is still read, multiplied by the output ratio (25 at 2x becomes `target_fps = 50`) and a note is logged. `fg.target_fps` wins when both are set |
| `spin_us` | integer | `1000` | `0` to `100000` | final busy-wait window before the deadline, in microseconds |

For a 75 Hz panel with VRR in the 40-75 Hz range, `target_fps = "auto"` gives 73 output
fps: inside the VRR window, with about 2 fps of headroom under the 75 Hz
ceiling. That is the usual target when VSync cannot be used with FG. At 74 the
cap sits on the ceiling, and the uneven real/generated split is more likely to
tear (finding 008 measured 10.55/16.86 ms and 6.43/20.96 ms splits).

The timer is a `CreateWaitableTimerExW` handle created with
`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`, one at a time. It is recreated when the cap
changes live and armed per frame. There is no `Sleep` and no `timeBeginPeriod`. The spin is the last
`spin_us` before the deadline and uses QPC plus `YieldProcessor`.

`spin_us = 0` is timer-only and measurably coarser. In an offline test at 37 fps
with 320 frames, overshoot p99 was 0.000 ms with the default 1000 us spin and
0.533 ms with no spin; the average interval matched the target in all cases
([finding 005](../findings/005-sedici-limiter.md)).

The limiter only engages on Sedici's swapchain wrapper. If the game never
creates or wraps a frame generation swapchain, nothing is capped. When frame
generation is off the same code still caps at `fps`, and the log labels the mode
`fg=off` (so the output rate is about `fps`, not 2x).

### `[log]`

| Key | Type | Default | Valid values | Effect |
|---|---|---|---|---|
| `level` | integer | `1` | `0`, `1`, `2` | `0` errors only, `1` normal, `2` verbose |

### `[ui]`

| Key | Type | Default | Valid values | Effect |
|---|---|---|---|---|
| `mode` | string | `"off"` | `"off"`, `"mini"`, `"full"` | startup overlay mode; `off` stays invisible until the hotkey is pressed |
| `scale` | string or number | `"auto"` | `"auto"` or `0.5`-`2.0` | menu scale; `auto` uses output height / 1080, clamped |
| `hotkey` | string | `"alt+f10"` | modifier(s) plus F1-F12 or a lowercase letter or digit | cycles `Off -> Mini -> Full -> Off` |

The overlay is drawn on the final output image by the swapchain wrapper, after
frame generation, so it is not interpolated and does not ghost. Composition of a
game-registered UI resource and of a game present callback is not implemented (no
owning milestone is recorded); Cyberpunk registers neither.

`Alt+F10` cycles the mode. In `Full`, the game's window procedure is subclassed,
keyboard and mouse input is consumed while the menu is open, and the cursor is
shown. Closing the menu returns input to the game.

The `Full` menu can change the real-frame limit, log level and menu scale live. Controls overridden by `SEDICI_*`/`MFSRFG_LOG` are disabled
and say which variable wins. **Save** patches `sedici.toml` in place, preserving
comments and formatting; **Cancel** restores the values captured when the menu
opened; **Close** keeps the live values. `[ui] mode` is a startup preference and
is not rewritten by Save.

### Environment overrides

Environment variables win over the file. They are for A/B runs and for testing
one configuration without editing anything.

| Variable | Overrides | Values |
|---|---|---|
| `SEDICI_LIMIT_FPS` | `fg.target_fps` (deprecated; read as a real-frame cap and converted) | number `0` to `1000` |
| `SEDICI_FG_PRESENT` | `fg.present` | as the file |
| `SEDICI_FG_MULTIPLIER` | `fg.multiplier` | as the file |
| `SEDICI_FG_TARGET_FPS` | `fg.target_fps` | number `0` to `1000`, or `auto` |
| `SEDICI_FG_TEAR_GUARD` | `fg.tear_guard` | as the file |
| `MFSRFG_LOG` | `log.level` | `0`, `1`, `2` |

A few A/B switches are read straight from the environment and are not part of the
configuration (K20): `SEDICI_FG_ALLOW_RTSS=1` lets `vrr` run while RTSS is injected instead
of falling back to `vsync`, `SEDICI_FG_SYNC` and `SEDICI_FG_ALIGN` change the `vsync`/`game`
paths, and `SEDICI_FG_SCANLINE_LEAD_US` tunes the scanline guard.

String values are case-insensitive. Numeric overrides must be a plain
non-negative decimal; `36.5 ` or `-1` is reported and ignored rather than
half-parsed.

### Errors never fail the game

Configuration is best-effort. Every problem is written to `sedici.log`, the
affected key falls back to its default, and the call proceeds.

| Problem | Result |
|---|---|
| file missing | all defaults |
| syntax error | all defaults; logged with line and column |
| one key has the wrong type | that key's default; logged with line and column |
| one key has an unknown value | that key's default; logged with the value and the allowed set |
| one key is out of range | that key's default; logged with the constraint |
| a section is not a table | the whole section ignored; logged once |
| an unknown section or key (a typo) | ignored; logged with its line |
| environment value unknown | override ignored, file/default stays; logged |
| anything unexpected | defaults for keys not yet read; logged |

A syntax error looks like this, and the game still runs:

```text
sedici.toml:5:1: parse error: expected a value - using built-in defaults for every key
```

Per-key errors name the key and what was allowed:

```text
sedici.toml:2:11: fg.present: unknown value "turbo" (expected "game", "vsync" or "vrr") - using default "vrr"
sedici.toml:3:1: fg.target_fps: must be within [0, 1000] (0 = uncapped) - using default 0
```

### Effective configuration log

The first call writes the effective settings once, with provenance:

```text
sedici: read C:\...\bin\x64\sedici.toml (once, on the first ffx call)
sedici:   fg.target_fps                           = 0 [default]
sedici:   limiter.spin_us                         = 1000 [default]
sedici:   ui.mode                                 = "off" [default]
sedici:   ui.scale                                = "auto" [default]
sedici:   ui.hotkey                               = "alt+f10" [default]
sedici:   log.level                               = 1 [default]
sedici: effective: target fps uncapped
```

`[default]`, `[file]` and `[env]` say where each value came from. A missing file
prints `sedici: no sedici.toml beside the DLL; using built-in defaults` instead
of the path line.
