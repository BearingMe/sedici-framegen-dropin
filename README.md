# Sedici

A drop-in replacement for AMD's DX12 frame-generation DLL,
`amd_fidelityfx_framegeneration_dx12.dll`, built for Cyberpunk 2077.

The game loads Sedici instead of AMD's DLL. Sedici exports the same five `ffx*`
entry points and answers every call with its own frame generation, swapchain
wrapper, frame-pacing scheduler, optional real-frame limiter and in-game overlay.
There is no AMD backend: the original DLL is never loaded, and a call Sedici does
not implement gets a defined FFX error code and a log line instead of being
forwarded.

The loader and upscaler DLLs are never touched.

> **History.** Sedici began as a pass-through proxy that sent AMD's frame pacing
> tuning and optionally rewrote one async flag. Those measurements still motivate
> the design and are kept in [`docs/findings/`](docs/findings/); the AMD-forwarding
> code was removed afterwards. The last commit that still has it is the tag
> `pre-amd-removal`. [`docs/refactor/amd-binary.md`](docs/refactor/amd-binary.md)
> keeps what was learned about AMD's binary.

## What it does now

| Behaviour | Default | How to change it |
|---|---|---|
| Frame generation | Sedici's own generator and swapchain wrapper | always answered by Sedici; the game's own frame generation toggle still turns generation off; `[fg]` selects how frames are shown |
| Output multiplier | `2x` | `fg.multiplier` |
| Presentation | `vrr` with the `scanline` tear guard | `fg.present`, `fg.tear_guard` |
| Real-frame limiter | **off** (`0`) | `limiter.fps = 37` or `SEDICI_LIMIT_FPS=37` |
| Limiter busy-wait window | `1000` us | `limiter.spin_us` |
| Overlay menu | **off** until `Alt+F10` | `[ui] mode`, `[ui] hotkey` |
| Log level | `1` | `log.level` or `MFSRFG_LOG` |

With `limiter.fps = 0` the limiter adds no wait.

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

## Two-stage frame generation

Sedici generates frames itself, as a **drop-in replacement** for
`amd_fidelityfx_framegeneration_dx12.dll`. Same DLL name, same five exports, same
desc types and the same swapchain behaviour, so a game built against FSR 3.1 FG
cannot tell the difference; only the frames come from a different pipeline.

The design is in [`references/papers/realtime-fg.md`](references/papers/realtime-fg.md),
with the two source papers transcribed beside it.

| Stage | Source | Resolution | What it does | Budget |
|---|---|---|---|---|
| 1. Analytical | Mob-FGSR | 1080p | quadratic trajectory from two motion-vector frames, depth-atomic splat, thin-object hole fill, threshold fusion, LUT (32×32×16) backward warping → `I_base`, `M_input` | ~0.15–0.30 ms estimated; **0.64–0.69 ms measured** offline at 1080p ([K117](docs/refactor/known-issues.md)) |
| 2. Residual | GFFE SCN / STSSNet backbone | 540p | 8 conv layers, 92,992 params (~90.8 KB INT8), predicts `ΔI` and `M̂`; final `clamp(I_base + M̂ · ΔI)` | ~1.10–1.35 ms |

Inputs are only what an FFX FG hook already receives: HUD-less color, depth,
motion vectors. Camera data arrives in the descriptor but stage 1 does not use it yet. No engine
G-buffers, no engine patches.

Budget target: **≤ 2.0 ms at 1080p** on a Radeon RX 7700 XT — 1.30–1.67 ms in FP16
WMMA, 0.95–1.20 ms in INT8 dp4a. These are design estimates from the papers, not
measurements.

Status: **experimental.** The offline suite is accepted (P2.1 to P2.10; 37 CTest entries,
2026-11-09) and the owner has run an in-game acceptance checklist on the current build
(QA-01 to QA-16, [results](tests/qa/README.md)), with findings recorded as open issues. P2.11
is not closed. A recognized create that Sedici cannot satisfy returns an error to the game;
there is no fallback. See the [known issues](docs/refactor/known-issues.md), the
[feature requests](docs/refactor/feature-requests.md), the
[historical audit](docs/refactor/audit.md), the
[historical checkpoint](docs/findings/007-fg-baseline.md) and the
[Mob-FGSR attribution](docs/mobfgsr-attribution.md). Constraints are in
[`AGENTS.md`](AGENTS.md); the delivery plan is in [`MILESTONES.md`](MILESTONES.md).

**Phase 1 shipped the first layer:** Mob-FGSR at 1080p, 2× interpolation and
Sedici-owned presentation scheduling, running in Cyberpunk. GFFE (stage 2) is postponed.

Reference code lives in `references/sources/` as shallow, blobless, sparsely
checked-out submodules (`mobfgsr`, `stssnet`, `tbfe`, `lmv`, `moflow`). They are
never compiled into the DLL.

## How it works

```
Cyberpunk2077.exe
  └── amd_fidelityfx_framegeneration_dx12.dll      <- Sedici (the only implementation)
```

1. **Answer every call.** `ffxCreateContext` makes either Sedici's frame
   generation context or its swapchain wrapper. `ffxConfigure`, `ffxDispatch`,
   `ffxQuery` and `ffxDestroyContext` act on handles Sedici issued. Handles come
   from a reserved token namespace, so a destroyed or foreign handle is
   recognised and refused locally.
2. **Version queries.** `ffxQuery(GetVersions)` needs no context and is answered
   with one provider per effect: frame generation `3.1.6` (id
   `0xf600000000c01006`) and the swapchain wrap `3.1.7` (id `0xf65cdd1200c01007`),
   the values AMD's binary reported.
3. **Wrap the swapchain.** The wrapper takes over the game's native swapchain, as
   AMD's did, and presents the real and generated frames itself.
4. **Generate.** Each `Prepare` dispatch hands depth, motion vectors and the render
   size to Sedici's stage 1 (and later stage 2) generator; camera data in the descriptor is
   received but not used yet.
5. **Cap, when asked.** The wrapper sees exactly one `Present` per real frame.
   With `limiter.fps > 0`, it targets `previous_deadline + 1/fps`, waits on a
   high-resolution waitable timer until `deadline - spin_us`, then spins on QPC to
   the deadline. A frame already past its deadline does not wait; one more than a
   full interval late reseeds the schedule instead of bursting. Capping the real
   frames puts the output at about twice `limiter.fps` with 2x generation.
6. **Observe.** Desc chains, return codes, cadence statistics and limiter
   statistics are written to `sedici.log`.

### Return codes for calls Sedici does not answer

| Call | Result |
|---|---|
| any call on a null handle or a handle Sedici never issued | `FFX_API_RETURN_ERROR_PARAMETER` |
| configure, dispatch, query or destroy on a Sedici handle that was already destroyed | `FFX_API_RETURN_ERROR`, logged |
| create whose chain names both a swapchain wrap and frame generation | `FFX_API_RETURN_ERROR`, logged |
| create with a descriptor chain that is not frame generation or the swapchain wrap | `FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE` |
| `GetVersions` for another effect | `FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE` |
| `GetVersions` without an output count | `FFX_API_RETURN_ERROR_PARAMETER` |
| any other query or dispatch type on a Sedici context (including the game's own `FrameGeneration` dispatch) | `FFX_API_RETURN_ERROR`, logged |
| swapchain wrap without a swapchain or a queue | `FFX_API_RETURN_ERROR`, logged |
| configure, dispatch or query on a Sedici context with a null descriptor | `FFX_API_RETURN_ERROR`, logged |
| destroy while GPU work has not retired (K127) | `FFX_API_RETURN_ERROR`, logged; the object is parked until it retires |
| frame generation create with unsupported flags, back-buffer format, depth format, motion-vector format or input geometry | `FFX_API_RETURN_ERROR`, logged |
| prepare while the previous prepare slot is still in flight (K98) | `FFX_API_RETURN_ERROR`, logged |
| `FrameGenerationSwapChainNewDX12` and `ForHwndDX12` creates | `FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE` (see [Known limits](#known-limits)) |

Each is logged at level 1 and above (level 0 logs internal errors only). None can crash
the host.

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
| `target_fps` | number | `0` | `0` or a positive number | only with `adaptive`; `0` means three below the refresh rate |
| `tear_guard` | string | `"scanline"` | `"scanline"`, `"timer"`, `"off"` | only with `vrr`; how a flip is kept from landing while the panel draws the previous image |

Sedici owns output pacing completely. A game's request to tune AMD's swapchain
pacing is accepted and ignored, so the game's own call sequence still succeeds.
Measurements of these modes are in
[the current findings](docs/findings/007-fg-baseline.md#vblank-presentation-removes-the-remaining-tearing)
and [finding 008](docs/findings/008-fg-tearing.md); none has been repeated since
the AMD backend was removed.

### `[limiter]`

| Key | Type | Default | Valid values | Effect |
|---|---|---|---|---|
| `fps` | number | `0` | `0` to `1000` | target real-frame rate; `0` disables the limiter; fractions allowed (`36.5` gives 73 output with 2x) |
| `spin_us` | integer | `1000` | `0` to `100000` | final busy-wait window before the deadline, in microseconds |

For a 75 Hz panel with VRR in the 40-75 Hz range, `fps = 36.5` gives 73 output
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
([finding 005](docs/findings/005-sedici-limiter.md)).

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
| `SEDICI_LIMIT_FPS` | `limiter.fps` | number `0` to `1000` |
| `SEDICI_FG_PRESENT` | `fg.present` | as the file |
| `SEDICI_FG_MULTIPLIER` | `fg.multiplier` | as the file |
| `SEDICI_FG_TARGET_FPS` | `fg.target_fps` | as the file |
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
sedici.toml:3:1: limiter.fps: must be within [0, 1000] - using default 0
```

### Effective configuration log

The first call writes the effective settings once, with provenance:

```text
sedici: read C:\...\bin\x64\sedici.toml (once, on the first ffx call)
sedici:   limiter.fps                             = 0 [default]
sedici:   limiter.spin_us                         = 1000 [default]
sedici:   ui.mode                                 = "off" [default]
sedici:   ui.scale                                = "auto" [default]
sedici:   ui.hotkey                               = "alt+f10" [default]
sedici:   log.level                               = 1 [default]
sedici: effective: real-frame limiter off
```

`[default]`, `[file]` and `[env]` say where each value came from. A missing file
prints `sedici: no sedici.toml beside the DLL; using built-in defaults` instead
of the path line.

## DLL lifetime

A DLL that was loaded but never used unloads normally. Once a game has created a Sedici
context the DLL pins itself in memory for the rest of the process: its handle namespace,
any swapchain still waiting for the GPU, and its worker threads all live in the module, so
`FreeLibrary` becomes a no-op and a reload returns the same module. The log says
`module pinned in memory` when this happens.

## Logging

The log is `sedici.log`, beside the deployed DLL. A background thread writes it, so a
slow or failing disk can never stall a game call. Lines wait in a fixed 256-line queue;
when it is full the extra lines are dropped, counted, and reported in the log once the
disk keeps up. A message repeated in a tight loop is rate limited (a burst of eight,
then two a second) and the next line that gets through says how many were suppressed.
If the file cannot be written, the lines are discarded and counted, and the log says how
many were lost when it becomes writable again. The last lines before a hard crash may
not have reached the file.

```powershell
$env:MFSRFG_LOG = "1"   # 0 = errors only, 1 = normal, 2 = verbose
```

Levels:

- **0** - only internal errors.
- **1** - the effective configuration; first 50 calls of each export; one full
  description per newly seen desc type; every non-OK return; limiter activation
  and stats; overlay and presentation statistics.
- **2** - every call, in addition to level 1.

The log level controls logging only.

### What the descriptions include

- **create** - FG context flags (decoded), display size, max render size,
  backbuffer format; HUD-less format; FG version; swapchain wrap/new/for-HWND
  variant, swapchain pointer, queue, DXGI swapchain desc; DX12 backend device.
- **configure** - FG `frameGenerationEnabled`, `allowAsyncWorkloads`, present and
  generation callbacks set/not set, `HUDLessColor` present yes/no and format,
  `generationRect`, `frameID`, flags, `onlyPresentGenerated`; swapchain UI
  resource registered and its format; every key/value configure including
  `FRAMEPACINGTUNING` and its five tuning fields (accepted and ignored); global
  debug descs.
- **dispatch** - prepare `frameID`, render size, jitter, MV scale,
  `frameTimeDelta`, reset, camera near/far/FOV, `viewSpaceToMetersFactor`,
  depth/MV formats; the deprecated prepare and V2 prepare are both handled;
  camera-info pNext; the generation desc if the game uses one.
- **query** - type and result, plus known output fields/pointers.
- **any non-OK return** - logged at level 1 regardless of call count.

### Stats

The periodic report also carries a `diag:` line (log queue drops, lost lines, parked
objects) and, when the planned output rate would exceed the display refresh, an
`OVER_REFRESH` note with the real-frame cap that would fit.

Every 300 prepare calls:

- `frameTimeDelta` (ms): average, minimum, maximum, standard deviation.
- Real interval between prepares from QPC (ms): average, minimum, maximum,
  standard deviation.

The present-interval and presents-per-prepare statistics of the old proxy are not
produced any more; nothing feeds them from Sedici's wrapper yet (K99).

### Limiter

Activation is logged once, and again whenever the frame-generation state
changes:

```text
sedici: limiter active target=37 fps interval=27.027 ms spin=1000 us fg=on
```

Every 300 real frames it reports what the schedule actually achieved:

```text
sedici: limiter stats frames=300 target_ms=27.027 interval_ms{avg=27.027 p50=27.027 p99=27.027 max=27.132} wait_ms{avg=26.936} overshoot_ms{avg=0.001 p99=0.000 max=0.105} late=0 resets=0 fg=on
```

| Field | Meaning |
|---|---|
| `frames` | real frames in this window |
| `target_ms` | `1000 / limiter.fps` |
| `interval_ms` | measured interval between real presents after the wait: avg, p50, p99, max |
| `wait_ms` | average time spent waiting (timer + spin) before the original Present |
| `overshoot_ms` | how far past the deadline the spin ended, for frames that waited; this is spin accuracy, not lateness |
| `late` | frames that arrived at or after their deadline and were not waited on |
| `resets` | times the schedule reseeded because a frame arrived more than one interval late |
| `fg` | frame generation state the game last requested |

`late` and `resets` are the two numbers to watch for a hitch: a long stall
produces one reset and no burst, rather than a queue of back-to-back presents.

### Stall instrumentation

Run I in `docs/findings/002-pacing-baseline.md` contained five episodic stalls: a
prepare interval of 22-72 ms followed immediately by a sub-2 ms one. Nothing
else in the log explained them, so Sedici now reports each one as it happens, at
log level 1.

A stall is a real-frame prepare interval that is **both** more than 3x the
rolling median of the last 61 prepare intervals **and** at least 25 ms. The
median is taken before the new sample is added, so a stall cannot hide in its
own baseline. The log is rate-limited to one line per second.

```text
sedici: stall prepare interval=72.406 ms median=13.402 ms ratio=5.402x frameID=4242 reset=n/a presentsSincePrepare=0 swapchainEvents1s{count=0} qpc=78860918227
```

| Field | Meaning |
|---|---|
| `interval` | measured gap between prepares, ms |
| `median` | rolling median the comparison used, ms |
| `ratio` | `interval / median` |
| `frameID` | the prepare's frame id |
| `reset` | the prepare's reset flag, or `n/a` for the legacy prepare desc, whose reset field is unused |
| `presentsSincePrepare` | always 0 at present: nothing counts presents for the detector yet (K99) |
| `swapchainEvents1s` | always empty at present: nothing records swapchain events on the Sedici path yet (K99) |
| `qpc` | raw QPC timestamp, for correlating with external tools |

A stall line means the game's own prepare calls arrived late. Whether that
coincided with a swapchain rebuild cannot be told from this line yet.

## Requirements

- Windows x64, Visual Studio 2022 C++ tools, CMake 3.21+, Windows SDK.
- Static CRT, C++20.
- The FidelityFX/FSR headers in `third_party/ffx/` are unmodified SDK v2.4.0
  headers. See `third_party/ffx/README.md` for upstream paths and blob IDs.
- toml++ v3.4.0, single-header flavour, unmodified, in
  `third_party/tomlplusplus/toml.hpp`. MIT.
- Dear ImGui v1.93.0, unmodified, in `third_party/imgui/` with the DX12 and
  Win32 backends. MIT. See `third_party/imgui/README.md`.

No other dependencies.

### Dear ImGui provenance

| | |
|---|---|
| Project | [ocornut/imgui](https://github.com/ocornut/imgui) |
| Version | **v1.93.0** (released 2026-10-14) |
| Tag commit | `c1aaac65e896a45f81f47a4d68f9959e0f86c859` |
| Sources | core `imgui*.cpp/.h`, `imconfig.h`, `imstb_*.h`; `backends/imgui_impl_dx12.*`, `backends/imgui_impl_win32.*` |
| Licence | MIT (`third_party/imgui/LICENSE.txt`) |

The files are copied byte-for-byte from the release archive. `imgui_demo.cpp`
is vendored for completeness and is not compiled.

### toml++ provenance

| | |
|---|---|
| Project | [marzer/tomlplusplus](https://github.com/marzer/tomlplusplus) |
| Version | **v3.4.0** (tag `v3.4.0`, released 2023-10-13) |
| File | single-header `toml.hpp`, 485,931 bytes |
| SHA-256 | `6B5172AD4DD6519AEC67B919181FA7A38A2234131E5B2AFA232DFE444819783E` |
| Licence | MIT (`SPDX-License-Identifier: MIT` in the header preamble) |
| Source | `https://raw.githubusercontent.com/marzer/tomlplusplus/v3.4.0/toml.hpp` |

The file is vendored byte-for-byte; nothing in it is edited. Two documented
compile-time switches are set in `src/settings/config.cpp` before the include, not in the
header:

- `TOML_EXCEPTIONS=0` so `toml::parse` returns a `parse_result` instead of
  throwing. Nothing about configuration parsing may throw across an `ffx*`
  export boundary.
- `TOML_ENABLE_FORMATTERS=0` because Sedici only parses; this drops the
  serialisers.

`CMakeLists.txt` fails the configure step with an explanatory message if the
header is missing.

## Build

```powershell
cmake -B build -A x64
cmake --build build --config Release
```

Output: `build/Release/amd_fidelityfx_framegeneration_dx12.dll`.

Verify the export table from a Visual Studio developer shell:

```powershell
dumpbin /exports build\Release\amd_fidelityfx_framegeneration_dx12.dll
```

There must be exactly five exports: `ffxCreateContext`, `ffxDestroyContext`,
`ffxConfigure`, `ffxQuery`, `ffxDispatch`. No helper exports. The DLL carries a
non-exported `SEDICI_PROXY_V1` marker that `deploy.ps1` uses to recognise it.

Optional offline regression checks (do not launch the game):

```powershell
cmake -B build -A x64 -DSEDICI_BUILD_TESTS=ON -DSEDICI_BUILD_FGCHECK=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

These cover config-save safety, settings transactions, token and retirement
policies, window and input handling, the live limiter, failure handling, and
Mini/Full DX12 rendering with window replacement. GPU tests use the installed debug layer when available; they never
download one. Their explicit GPU waits do not prove FFX's in-game queue bounds.

`sedici_abi_*` load the built DLL through its five exports. A stub beside it,
named like the original DLL, is a tripwire: Sedici must never load it, and
`sedici_abi_contract` checks that every call, including version queries and the
error codes for foreign, null and unknown inputs, is answered locally. Other
scenarios cover a malformed config, handle rejection, and a combined session with
the Mini overlay and limiter (`sedici_abi_session`). Each run writes a new
`build/tests/<config>/abi/<scenario>-<time>-<pid>` directory and never deletes
anything. `sedici_fgcheck*` need `SEDICI_BUILD_FGCHECK`; known failures print
`XFAIL` and are listed in `docs/refactor/reliability.md`. Close RTSS first:
its overlay is injected into test windows (vrr checks are then skipped).

## Deploy and restore

```powershell
.\tools\deploy.ps1 -Game cp2077
.\tools\restore.ps1 -Game cp2077
```

Game ids in `tools/games.json`: `cp2077` (Cyberpunk 2077) and `aw2` (Alan Wake 2,
Epic launcher, no `bin\x64` subdirectory).

`deploy.ps1`:

- resolves the game from `tools/games.json` or an explicit `-GamePath`;
- refuses to run while the game's executable is running;
- resolves Steam library folders automatically; installs outside Steam (for
  example Epic) come from `installDirHints` or `-GamePath`;
- never overwrites an existing `.orig` backup (AMD's original DLL, kept only so
  `restore.ps1` can put it back; Sedici itself never loads it);
- refuses to overwrite a DLL that is not Sedici's;
- copies `sedici.toml` only if one is not already there, so a tuned file is kept.

`restore.ps1` puts the original DLL back and removes `sedici.log` and
`sedici.toml`. Restore is an uninstall, not a merge: **copy your `sedici.toml`
out first if you tuned it.** It refuses to touch a DLL that is not Sedici's.

Both scripts only touch `amd_fidelityfx_framegeneration_dx12.dll`, its `.orig`
backup, `sedici.toml` and `sedici.log`. The loader and upscaler DLLs are never
modified.

## Capture analysis

`tools/capture.ps1` writes one PresentMon v1 CSV per run. `tools/analyze.py`
summarizes it per swapchain and can append the Markdown report to a findings
file:

```powershell
python tools\analyze.py --histogram captures\2026-11-05-N1.csv
python tools\analyze.py --histogram --append docs\findings\005-sedici-limiter.md captures\2026-11-05-N1.csv
```

`--histogram` adds the `MsBetweenDisplayChange` distribution in 1 ms bins
(`--bin-ms` changes the width). The histogram is how the two clusters of a
non-uniform pattern become visible; the summary tables alone hide them.

When the CSV contains them, these columns are reported as well. They are
relative proxies, not click-to-photon measurements:

| Column | What it measures | What it does not measure |
|---|---|---|
| `MsUntilDisplayed` | PresentMon's software estimate from the Present call to the display change | input sampling, game simulation, photon emission; not comparable across present modes |
| `MsInPresentAPI` | how long the application blocked inside Present | latency; with the limiter it includes the deliberate wait |
| `MsUntilRenderComplete` | Present call to GPU render completion | display latency; mostly queue depth |
| `MsGPUActive` | GPU busy time for the frame | CPU frame time or latency |

## Milestones

The history in short: a pass-through proxy that validated desc shapes (removed), AMD pacing
tuning (removed), the built-in real-frame limiter, the in-game overlay, and Sedici's own
two-stage frame generation as a drop-in replacement. The plan and the current state are in
[`MILESTONES.md`](MILESTONES.md): Phase 1 (Stage 1 shipped), Phase 2 (reliability,
P2.1-P2.10 accepted, P2.11 open), Phase 3 (proposed: organise the code without changing
behaviour) and Phase 4 (placeholder: the open issues and improvements).

## Present timing

The wrapper presents on its own thread and queue in `vrr` mode and runs the real
frame limiter from the game's thread, before the real `Present`. `DXGI_PRESENT_TEST`
calls never reach the limiter. If the limiter's high-resolution timer cannot be
created it falls back to a standard waitable timer, and if no timer can be created the
limiter disables itself and the game runs uncapped; the error is logged and nothing fails
the game's call. The timer is created lazily on the first capped present, never in
`DllMain`, and is not closed at process exit.

## Known limits

- **Supported games.** Cyberpunk 2077 is the tested game. **Alan Wake 2 does not work:** it asks
  for `FrameGenerationSwapChainNewDX12` with a 10-bit back buffer and an HDR create flag, which
  Sedici answers with `UNKNOWN_DESCTYPE` (QA-03 fails; K100, K129-K132,
  [finding 014](docs/findings/014-alan-wake-2-swapchain-new.md) and
  [015](docs/findings/015-alan-wake-2-first-sedici-session.md)).
- **Formats.** The back buffer and the HUD-less image must be `R8G8B8A8_UNORM`; depth
  `D32_FLOAT`, `R32_TYPELESS` or `R32G8X24_TYPELESS`; motion vectors `R16G16B16A16_FLOAT`,
  `R16G16_FLOAT` or `R32G32_FLOAT`. No HDR or 10-bit (K14). Only the swapchain wrap and the
  frame generation create types are routed.
- **Open issues seen in the game** ([known-issues.md](docs/refactor/known-issues.md)): the
  overlay can be dead at the next launch after frame generation was turned off (K119) or
  disable itself after an exception (K124); Alt+F4 does not close the game while the Full menu
  is open (K126); the limit slider cannot reliably raise the cap (K118); disocclusion at high
  speed (K120); borders on loading screens (K128); short unexplained stalls (K121) and a slow
  settle of the real rate (K123); the overlay "Real" counter counts shown frames (K125);
  Cyberpunk sends no generation inputs in its menus, so the rate falls to the real fps there.
- **Requests and test record.** [feature-requests.md](docs/refactor/feature-requests.md) holds
  the requests; [tests/qa](tests/qa/README.md) holds the in-game acceptance record.

## Rules

[`AGENTS.md`](AGENTS.md) owns the rules. In short: stay a drop-in replacement (exactly five
exports, defined FFX error codes, no crash), never fail a game call because of Sedici's own
bookkeeping, never modify the loader or the upscaler DLL, keep `third_party/` unmodified, and
never download SDKs or launch the game as part of a build.
