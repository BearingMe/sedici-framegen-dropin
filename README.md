# Sedici

A proxy for AMD's DX12 frame-generation DLL, `amd_fidelityfx_framegeneration_dx12.dll`,
that fixes the frame pacing Cyberpunk 2077 shows with native FSR frame
generation.

Sedici takes the original DLL's name. The original is renamed to
`amd_fidelityfx_framegeneration_dx12.dll.orig` and all five `ffx*` exports are
forwarded with identical arguments, return values and timing. On top of that
forwarding, it sends one optional configure call that removes a measured
frame-time tail, optionally caps the game's real frames against a deadline, and
writes a local log of what the game asks frame generation to do.

Nothing else changes. The loader and upscaler DLLs are never touched.

## What it does now

**Milestone 3: the pacing fix, the real-frame limiter, and an in-game overlay.**

| Behaviour | Default | How to change it |
|---|---|---|
| Frame pacing tuning sent after each FG swapchain is created | **on** (`hybrid`) | `pacing.mode = "off"` or `SEDICI_PACING=off` |
| `allowAsyncWorkloads` rewritten to `false` | **off** (`game`) | `async.mode = "off"` or `SEDICI_ASYNC=0` |
| Real-frame limiter | **off** (`0`) | `limiter.fps = 37` or `SEDICI_LIMIT_FPS=37` |
| Limiter busy-wait window | `1000` us | `limiter.spin_us` |
| Overlay menu | **off** until `Alt+F10` | `[ui] mode`, `[ui] hotkey` |
| Log level | `1` | `log.level` or `MFSRFG_LOG` |

With `pacing.mode = "off"`, `async.mode = "game"` and `limiter.fps = 0`, Sedici
forwards exactly what it forwarded in milestone 0: no extra calls, no desc
rewrites, and no limiter work at all.

### Why this specific fix

`docs/findings/002-pacing-baseline.md` measured nine configurations on a 75 Hz
panel. The only thing that separated a good run from a bad one was the tail, not
the median:

| | worst interval | stddev |
|---|---|---|
| no fix | 50-67 ms | 1.1-1.4 ms |
| `pacing.mode = "hybrid"` | **9.6 ms** | **0.45 ms** |

Throughput was unchanged, a fresh-process control ruled out a restart artefact,
and a 37 fps frame cap reproduced the same clean tail (so the cap and the fix
address the same contention). The async toggle removes the same tail and does
not stack with it, which is why it stays off by default and is kept for A/B
testing.

### Why a limiter

Run G in `002` used the game's own 37 fps cap with 2x frame generation and
produced the tightest distribution in the whole set: **74.0 fps output, standard
deviation 0.324 ms, worst interval 15.4 ms, and zero intervals outside
0.5-1.5x the median**. Cyberpunk will not let Anti-Lag 2 and Maximum FPS be
enabled together, however. The limiter moves that cap into Sedici, so the game
can keep Anti-Lag 2 on and run uncapped while the real presents are paced to
`limiter.fps`.

## How it works

```
Cyberpunk2077.exe
  └── amd_fidelityfx_framegeneration_dx12.dll      <- Sedici
        └── amd_fidelityfx_framegeneration_dx12.dll.orig   <- AMD's DLL
```

1. **Forward.** Every export is passed through unchanged: same arguments, same
   return code, same ordering. Unknown descs are forwarded untouched.
2. **Tune once per swapchain.** After a successful `ffxCreateContext` whose
   desc chain contains a frame generation swapchain create/wrap desc, Sedici
   sends

   ```
   FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_KEYVALUE_DX12  (0x30008)
     key = FFX_API_CONFIGURE_FG_SWAPCHAIN_KEY_FRAMEPACINGTUNING (2)
     ptr = FfxApiSwapchainFramePacingTuning{...}
   ```

   to that context. The state is tracked per context, so when the game recreates
   its swapchain (resolution change, frame generation toggled) the new context
   gets its own send rather than inheriting the old one's "already applied" flag.
   A new swapchain pointer on the same context also triggers a resend.
3. **Retry, bounded.** If the swapchain path rejects the tuning, Sedici retries
   through the next successful `ffxConfigure` on that context, at most five
   attempts. A rejected tuning is logged and never changes what the game sees.
4. **Cap, when asked.** The `Present`/`Present1` hook on the FG swapchain runs
   before the original call. With `limiter.fps > 0`, it targets
   `previous_deadline + 1/fps`, waits on a high-resolution waitable timer until
   `deadline - spin_us`, then spins on QPC to the deadline. If the frame is
   already past its deadline it does not wait; if it is more than one full
   interval late, the schedule reseeds at the current time instead of bursting
   to catch up. The arguments and the return value are passed through
   untouched.
5. **Observe.** Desc chains, return codes, cadence statistics, limiter
   statistics and detected prepare stalls are written to `sedici.log`.

The pacing send happens on Sedici's side of the call. It is an extra
`ffxConfigure` call into AMD's DLL made *after* the game's own call has already
completed and returned, so the game's call itself is untouched.

The limiter is different: it deliberately delays the game's `Present`, and
therefore the game's render loop, before forwarding. That is the mechanism, not
a side effect. The hook is installed on the object returned by the frame
generation swapchain create/wrap desc, which is FFX's wrapper. It sees exactly
one `Present` per real frame (`presentsPerPrepare` is 1.0); the generated frames
are presented inside AMD's swapchain and never reach Sedici. Capping here
therefore caps real frames, and with 2x frame generation the output lands at
about twice `limiter.fps`.

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

### `[pacing]`

| Key | Type | Default | Valid values | Effect |
|---|---|---|---|---|
| `mode` | string | `"hybrid"` | `"hybrid"`, `"off"` | `hybrid` sends the pacing tuning configure; `off` sends nothing |
| `safety_margin_ms` | float | `0.1` | finite, `>= 0`, `<= 1000` | `FfxApiSwapchainFramePacingTuning::safetyMarginInMs` |
| `variance_factor` | float | `0.1` | `0.0` to `1.0` | `varianceFactor` |
| `hybrid_spin_time` | integer | `2` | `0` to `1000` | `hybridSpinTime`, in timer resolution units |
| `wait_for_single_object_on_fence` | boolean | `false` | `true`, `false` | `allowWaitForSingleObjectOnFence` |

`allowHybridSpin` is always `true` when `mode = "hybrid"`; it is the switch the
whole fix hinges on, so it is not exposed. The SDK documents `hybrid_spin_time`
values below 2 as prone to overshoot; Sedici applies them anyway but logs a note.

### `[async]`

| Key | Type | Default | Valid values | Effect |
|---|---|---|---|---|
| `mode` | string | `"game"` | `"game"`, `"off"` | `game` forwards `allowAsyncWorkloads` untouched; `off` forces `allowAsyncWorkloads = false` in every FG configure desc and leaves the FG create flags as the game set them |

This differs from run D in `docs/findings/005-sedici-limiter.md`, which also
cleared `FFX_FRAMEGENERATION_ENABLE_ASYNC_WORKLOAD_SUPPORT` from the create
flags. The current `off` only changes the per-frame configure value.

The rewrite is scoped: Sedici changes the caller's desc in place for the duration
of one call and restores every byte before returning, so the game never observes
a modified desc.

### `[limiter]`

| Key | Type | Default | Valid values | Effect |
|---|---|---|---|---|
| `fps` | number | `0` | `0` to `1000` | target real-frame rate; `0` disables the limiter; fractions allowed (`36.5` gives 73 output with 2x) |
| `spin_us` | integer | `1000` | `0` to `100000` | final busy-wait window before the deadline, in microseconds |

For a 75 Hz panel with VRR in the 40-75 Hz range, `fps = 36.5` gives 73 output
fps: inside the VRR window, with about 2 fps of headroom under the 75 Hz
ceiling. That is the usual target when VSync cannot be used with FG. At 74 the
cap sits on the ceiling, and the 9/18 ms real/generated split is more likely to
tear.

The timer is a `CreateWaitableTimerExW` handle created with
`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`, one per process, and it is armed per
frame. There is no `Sleep` and no `timeBeginPeriod`. The spin is the last
`spin_us` before the deadline and uses QPC plus `YieldProcessor`.

`spin_us = 0` is timer-only and measurably coarser. In an offline test at 37 fps
with 320 frames, overshoot p99 was 0.017 ms with the default 1000 us spin and
0.533 ms with no spin; the median interval was exact in both cases.

The limiter only engages on the game's swapchain path. If the game never
creates or wraps a frame generation swapchain, or the present hook could not be
installed, nothing is capped and the reason is logged. When frame generation is
off the same code still caps at `fps`, and the log labels the mode `fg=off`
(so the output rate is about `fps`, not 2x).

### `[log]`

| Key | Type | Default | Valid values | Effect |
|---|---|---|---|---|
| `level` | integer | `1` | `0`, `1`, `2` | `0` errors only, `1` normal, `2` verbose |

### `[ui]`

| Key | Type | Default | Valid values | Effect |
|---|---|---|---|---|
| `mode` | string | `"off"` | `"off"`, `"mini"`, `"full"` | startup overlay mode; `off` stays invisible until the hotkey is pressed |
| `scale` | string or number | `"auto"` | `"auto"` or `0.5`-`2.0` | menu scale; `auto` uses output height / 1080, clamped |
| `hotkey` | string | `"alt+f10"` | modifier(s) plus F1-F12 or a letter/digit | cycles `Off -> Mini -> Full -> Off` |

The overlay is drawn on the final output image inside FFX's present callback,
after frame generation, so it is not interpolated and does not ghost. It is
only drawn when no separate UI resource is registered; in that configuration
Sedici also performs the same back-buffer copy AMD's default callback does.
Cyberpunk registers no UI resource, so the overlay runs there. If a game does
register one, Sedici leaves composition to FFX and does not draw.

`Alt+F10` cycles the mode. In `Full`, the game's window procedure is subclassed,
keyboard and mouse input is consumed while the menu is open, and the cursor is
shown. Closing the menu returns input to the game.

The `Full` menu can change the real-frame limit, pacing, async mode, log level
and menu scale live. Controls overridden by `SEDICI_*`/`MFSRFG_LOG` are disabled
and say which variable wins. **Save** patches `sedici.toml` in place, preserving
comments and formatting; **Cancel** restores the values captured when the menu
opened; **Close** keeps the live values. `[ui] mode` is a startup preference and
is not rewritten by Save.

### Environment overrides

Environment variables win over the file. They are for A/B runs and for testing
one configuration without editing anything.

| Variable | Overrides | Values |
|---|---|---|
| `SEDICI_PACING` | `pacing.mode` | `hybrid`, `off` |
| `SEDICI_ASYNC` | `async.mode` | `0`, `game` |
| `SEDICI_LIMIT_FPS` | `limiter.fps` | number `0` to `1000` |
| `MFSRFG_LOG` | `log.level` | `0`, `1`, `2` |

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
| environment value unknown | override ignored, file/default stays; logged |
| anything unexpected | defaults for keys not yet read; logged |

A syntax error looks like this, and the game still runs:

```text
sedici.toml:5:1: parse error: expected a value - using built-in defaults for every key
```

Per-key errors name the key and what was allowed:

```text
sedici.toml:2:8: pacing.mode: unknown value "turbo" (expected "hybrid" or "off") - using default "hybrid"
sedici.toml:3:1: pacing.variance_factor: must be within [0.0, 1.0] - using default 0.1
```

### Effective configuration log

The first call writes the effective settings once, with provenance:

```text
sedici: read C:\...\bin\x64\sedici.toml (once, on the first ffx call)
sedici:   pacing.mode                             = "hybrid" [file]
sedici:   pacing.safety_margin_ms                 = 0.1 [default]
sedici:   pacing.variance_factor                  = 0.1 [default]
sedici:   pacing.hybrid_spin_time                 = 2 [default]
sedici:   pacing.wait_for_single_object_on_fence  = false [default]
sedici:   async.mode                              = "game" [default]
sedici:   limiter.fps                             = 0 [default]
sedici:   limiter.spin_us                         = 1000 [default]
sedici:   ui.mode                                 = "off" [default]
sedici:   ui.scale                                = "auto" [default]
sedici:   ui.hotkey                               = "alt+f10" [default]
sedici:   log.level                               = 1 [default]
sedici: effective: frame pacing tuning ON (hybrid spin), async workloads as the game sets them, real-frame limiter off
```

`[default]`, `[file]` and `[env]` say where each value came from. A missing file
prints `sedici: no sedici.toml beside the DLL; using built-in defaults` instead
of the path line.

## Logging

The log is `sedici.log`, beside the deployed DLL, flushed on every line.

```powershell
$env:MFSRFG_LOG = "1"   # 0 = errors only, 1 = normal, 2 = verbose
```

Levels:

- **0** - only load/forward failures and internal errors.
- **1** - the effective configuration; first 50 calls of each export; one full
  description per newly seen desc type; every non-OK return; present-hook
  installation; pacing tuning sends; limiter activation and stats; stats every
  300 prepares; one line per detected prepare stall.
- **2** - every call, in addition to level 1.

The log level controls logging only. Turning it down does not turn the pacing
fix off.

### What the descriptions include

- **create** - FG context flags (decoded), display size, max render size,
  backbuffer format; HUD-less format; FG version; swapchain wrap/new/for-HWND
  variant, swapchain pointer, queue, DXGI swapchain desc; DX12 backend device.
- **configure** - FG `frameGenerationEnabled`, `allowAsyncWorkloads`, present and
  generation callbacks set/not set, `HUDLessColor` present yes/no and format,
  `generationRect`, `frameID`, flags, `onlyPresentGenerated`; swapchain UI
  resource registered and its format; every key/value configure including
  `FRAMEPACINGTUNING` and its five tuning fields; global debug descs.
- **dispatch** - prepare `frameID`, render size, jitter, MV scale,
  `frameTimeDelta`, reset, camera near/far/FOV, `viewSpaceToMetersFactor`,
  depth/MV formats; the deprecated prepare and V2 prepare are both handled;
  camera-info pNext; the generation desc if the game uses one.
- **query** - type and result, plus known output fields/pointers.
- **any non-OK return** - logged at level 1 regardless of call count.

### Pacing tuning sends

Every send is logged with the values used and AMD's return code:

```text
sedici: FramePacingTuning sent after swapchain create ctx=0x... key=2 safetyMarginInMs=0.100 varianceFactor=0.100 allowHybridSpin=true hybridSpinTime=2 allowWaitForSingleObjectOnFence=false result=0 (OK)
```

A rejection keeps forwarding intact and logs a second line:

```text
sedici: frame pacing tuning was rejected (result=...); forwarding continues unchanged and the retry path through ffxConfigure stays armed
```

### Stats

Every 300 prepare calls:

- `frameTimeDelta` (ms): average, minimum, maximum, standard deviation.
- Real interval between prepares from QPC (ms): average, minimum, maximum,
  standard deviation.
- Present interval from QPC (ms): average, p50, p95, p99, maximum.
- Presents per prepare: average, minimum, maximum. About 2.0 is expected with
  2x frame generation.

If the present hook could not be installed, the present-interval fields show
`n/a`; the rest of the log still works.

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
| `fg` | frame generation state reported by the game's last configure |

`late` and `resets` are the two numbers to watch for a hitch: a long stall
produces one reset and no burst, rather than a queue of back-to-back presents.

### Stall instrumentation

Run I in `docs/findings/002-pacing-baseline.md` contained five episodic stalls: a
prepare interval of 24-72 ms followed immediately by a sub-2 ms one. Nothing
else in the log explained them, so Sedici now reports each one as it happens, at
log level 1.

A stall is a real-frame prepare interval that is **both** more than 3x the
rolling median of the last 61 prepare intervals **and** at least 25 ms. The
median is taken before the new sample is added, so a stall cannot hide in its
own baseline. The log is rate-limited to one line per second.

```text
sedici: stall prepare interval=72.406 ms median=13.402 ms ratio=5.402x frameID=4242 reset=n/a presentsSincePrepare=0 swapchainEvents1s{count=1 last=configure 812.4 ms ago} qpc=78860918227
```

| Field | Meaning |
|---|---|
| `interval` | measured gap between prepares, ms |
| `median` | rolling median the comparison used, ms |
| `ratio` | `interval / median` |
| `frameID` | the prepare's frame id |
| `reset` | the prepare's reset flag, or `n/a` for the legacy prepare desc, whose reset field is unused |
| `presentsSincePrepare` | presents seen since the previous prepare |
| `swapchainEvents1s` | swapchain creates and successful configures in the last second, with the age of the most recent |
| `qpc` | raw QPC timestamp, for correlating with external tools |

If a stall line appears while playing, the `swapchainEvents1s` field is the first
thing to look at: a non-zero count means the hitch coincided with the game
rebuilding its swapchain or reconfiguring frame generation.

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
compile-time switches are set in `src/config.cpp` before the include, not in the
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
`ffxConfigure`, `ffxQuery`, `ffxDispatch`. No helper exports, including the
proxy marker.

Optional offline regression checks (do not launch the game):

```powershell
cmake -B build -A x64 -DSEDICI_BUILD_TESTS=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

These cover config-save safety, window/vtable forwarding, callback bindings,
live limiter/pacing, failure handling, and Mini/Full DX12 rendering with window
replacement. GPU tests use the installed debug layer when available; they never
download one. Their explicit GPU waits do not prove FFX's in-game queue bounds.

## Deploy and restore

```powershell
.\tools\deploy.ps1 -Game cp2077
.\tools\restore.ps1 -Game cp2077
```

`deploy.ps1`:

- resolves the game from `tools/games.json` or an explicit `-GamePath`;
- refuses to run while `Cyberpunk2077.exe` is running;
- never overwrites an existing `.orig` backup;
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

- **M0 - observe and forward.** No behavior changes. Validate desc shapes,
  pacing tuning, `frameTimeDelta` versus measured intervals, present cadence.
  Shipped.
- **M1 - pacing fix on by default.** `sedici.toml` configuration, per-context
  pacing send, stall instrumentation, confirmation capture runs. Shipped.
- **M2 - built-in real-frame limiter.** `[limiter]` configuration, a
  high-resolution timer plus QPC spin in front of the game's `Present`,
  per-300-frame accuracy stats, so Anti-Lag 2 can stay on with a cap inside
  Sedici. Shipped.
- **M3 - in-game overlay (current).** Dear ImGui on the final output image,
  `Alt+F10` Off/Mini/Full, live limiter/pacing/async/log/scale controls,
  comment-preserving Save. Composition-safe callback shipped; in-game
  confirmation pending.
- **M4 - further tuning.** Only if M3's runs point somewhere: correcting
  `frameTimeDelta`, or extending the tuning to latency-driven cases.

## Present-hook risk

The hook patches the shared DXGI swapchain vtable's `Present` (slot 8) and
`Present1` (slot 22) entries. It chains to whatever was there before (D3D
runtime or another overlay) and restores the original page protection. Both
hooks forward exactly the arguments they received, including `Present1`'s
`PresentFlags` and `DXGI_PRESENT_PARAMETERS`; `DXGI_PRESENT_TEST` is passed
through without being capped. If `VirtualProtect` fails, the hook is skipped and
an error is logged. When the hook is active it serves two purposes: timing
measurements and, when `limiter.fps > 0`, the real-frame cap. If it is skipped,
neither happens; the pacing fix and forwarding are unaffected.

The limiter's timer is created lazily on the first capped present, never in
`DllMain`, and is never closed: the process exit reclaims it.

## Rules

- Forward everything. Only the documented pacing send, the opt-in async
  rewrite and the opt-in real-frame limiter may change behavior, and all three
  are inert when configured off.
- Never fail a game call because of Sedici's own bookkeeping.
- Do not modify the loader or the upscaler DLL.
- Keep `third_party/ffx/` and `third_party/tomlplusplus/` unmodified.
- Do not download SDKs or launch the game as part of a build.
