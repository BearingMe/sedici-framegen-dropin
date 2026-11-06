# 007: Frame generation baseline and observed contract

Baseline and chronological integration findings for Mob-FGSR on
`experiment/cobalt-otter`. The initial sections describe the pre-deployment
baseline; later sections record authorized game sessions and superseding fixes.

**Current status (2026-11-06):** the entry-budget/current-pair build is deployed.
The user confirms the FPS regression is fixed, and the gameplay capture records
steady 72 output fps with a 36 fps in-game cap. CPU submission pacing improved,
but display-change events still bunch in pairs. Tearing and artifacts during game
startup remain reported and unresolved. Phase 1 is not accepted. See
[current validation and open issues](#current-pair-gameplay-validation-and-remaining-issues).

## Existing implementation

At the initial baseline Sedici was a forwarding proxy and AMD owned generation:

| Area | File | State |
| --- | --- | --- |
| Five exports | `src/dllmain.cpp` | forward to `.orig` unchanged |
| Pacing tuning, async override, context lifetime | `src/toggles.cpp` | active, pacing on by default |
| `Present`/`Present1` hook and limiter | `src/present_hook.cpp`, `src/limiter.cpp` | hook installed when a FG swapchain is created; limiter off by default |
| FG present callback, composition | `src/overlay.cpp`, `src/overlay_compose.cpp` | Sedici owns the callback when no UI resource is registered |
| Desc and return-code logging | `src/describe.cpp` | level 1 logs the first 50 calls per export plus new desc types |

The vendored declarations are FidelityFX SDK 2.4.0 (FG 4.0.2, DX12 swapchain
4.0.0, see `third_party/ffx/README.md`). They are not evidence of what the
installed FSR 3.1 binary does internally.

## Observed Cyberpunk contract

From `001` (first 50 calls per export) and `006` (callback payloads):

| Flow | Observation |
| --- | --- |
| Create | `FrameGenerationCreate` flags `AsyncWorkloads \| DepthInverted`, display and max render `2560x1080`, backbuffer `R8G8B8A8_UNORM`; `FrameGenerationCreateVersion` 4.0.1; `OverrideVersion` present; swapchain variant `FrameGenerationSwapChainWrapDX12` |
| Configure | `frameGenerationEnabled=true`, `allowAsyncWorkloads=true`, `generationCallback=set`, `presentCallback=none`, `HUDLessColor=yes` (`R8G8B8A8_UNORM`, `2560x1080`), `onlyPresentGenerated=false`, no pacing key from the game |
| UI | `uiResource=no` — the separate-UI composition path is unused; AMD's default copies the back buffer |
| Prepare | legacy `FrameGenerationPrepare` (`0x20004`), not V2 and no camera-info desc; render size `1707x720`; 8-tap jitter sequence; `motionVectorScale=(1707,720)`; depth `R32_FLOAT`; motion vectors `R16G16B16A16_FLOAT`; `frameTimeDelta` tracks the real prepare interval but is clamped at 66 ms |
| Dispatch | `FrameGenerationDispatch` every frame with `numGeneratedFrames=1` |
| Present callback | one call per output frame, `isGeneratedFrame` alternating; 150 real and 150 generated per logged window; `currentUI` absent; output state `FFX_API_RESOURCE_STATE_PRESENT` (128) |
| Teardown | contexts destroyed on swapchain recreation (resolution change, FG toggled off) |

Consequences for the generator:

- The FG inputs arrive at **render resolution** (1707x720 at FSR Quality), while
  HUD-less color and the output are **display resolution** (2560x1080). Stage 1
  therefore runs motion reconstruction at render resolution and its
  final warp at display resolution, which is exactly the `render_size` /
  `presentation_size` / `render_scale` split in the reference implementation.
- Depth is reversed (near = 1) because `DepthInverted` is set. An infinite far
  plane is not established by the recorded flags. The reference packs an
  11-bit "nearer is smaller" depth, so the
  dilated depth is remapped with `depth * depthScale + depthBias` (`-1, +1` for
  inverted).
- FFX specifies `pixelMotion = mv * motionVectorScale`; the recorded scale
  `(1707,720)` indicates UV inputs. Normalize with `mv * scale / renderSize`.
  FFX reprojects current-to-previous with `uv + mv`; MobFGSR uses `uv - mv`,
  requiring sign inversion at the boundary. No jitter-cancellation flag is set.
- 2x interpolation means one midpoint between consecutive real frames, presented
  before its corresponding current real frame. Both inputs must exist before
  computing that midpoint; the current real frame need not be displayed first.

## Measured cadence available as a baseline

All from `002` and `005`; they are historical references, not acceptance gates,
and each used different Sedici settings than the new path will.

| Reference | Configuration | Output fps | avg | p99 | max | sd |
| --- | --- | --- | --- | --- | --- | --- |
| `002` run G | game's own 37 fps cap, 2x FG, no Sedici toggles | 74.0 | 13.513 ms | 14.667 ms | 15.406 ms | 0.324 ms |
| `002` run E | `SEDICI_PACING=hybrid`, uncapped | — | — | — | worst 9.6 ms | 0.45 ms |
| `002` unfixed | no toggles, uncapped | — | — | — | worst 50-67 ms | 1.1-1.4 ms |

`005` shows the limiter reaching the same 27.027 ms real-frame interval with
sub-0.11 ms overshoot. These numbers describe AMD's generator; Sedici's own path
must be compared against them only with the configuration differences stated.

## Open items carried into implementation

- Motion-vector sign convention is not verified in-engine. The reference assumes
  screen-space offsets that move content from the current frame towards the
  previous one (`uv - mv`); if Cyberpunk's vectors point the other way the
  generated frame will tear. Verify on the offline sequence first, then in game.
- Depth is non-linear (reversed-Z), so the paper's `0.004` depth
  threshold is not directly comparable; it is a tuning value, not a constant.
- Camera-cut behaviour remains unverified. Current resets cover explicit
  prepare reset, frame-ID discontinuities, first use and size changes; there
  is no measured camera-cut detector or frame-time heuristic yet.
- The reference LUT (`lut.exr`) is exported offline and embedded as a 128x128
  FLOAT atlas (64 KiB), not the estimated 16 KiB INT8 representation. See
  `tools/export_mobfgsr_lut.py` and [attribution](../mobfgsr-attribution.md).

## Integration corrections and wrapper checkpoint

The old synthetic pixel-vector host bypassed FFX normalization and state mapping.
It could not detect inverse motion scaling or direct FFX-to-DX12 state casting.
The host now scores through `fg::Context` with RGBA16F current-to-previous UV
motion and FFX resource-state flags.

AMD's reconstruction source confirms `fReprojectedUv = fUv + fMotionVector`:
`Kits/FidelityFX/framegeneration/fsr3/include/gpu/frameinterpolation/`
`ffx_frameinterpolation_reconstruct_dilated_velocity_and_previous_depth.h`,
reviewed at SDK commit `7539ae8b6568ad29ed78a753404351daf3b5cca7`.

`fg::Swapchain` wraps the existing native SDR swapchain with private game-facing
buffers, without changing the native buffer count. The offline `--present` path
covers COM identity, Present/Present1, TEST, resize rejection while a buffer is
held, resize success and teardown. Its fence must be signalled **after** native
Present: a pre-Present fence failed to retire DXGI's queued work, causing final
release of an in-flight buffer during resize (caught by the debug layer).

Prepare-time normalization/dilation is now recorded into private textures using
the prepare command list. Generation no longer reads raw depth/motion pointers
retained from prepare. The owner must order the two submissions and retire the
single slot before reuse or resize. `Context::track_submission` registers a
queue-signalled completion fence after recorded work is submitted; `retired`
guards reuse and explicit destruction. A split generation submission requires
`wait_prepare` on its execution queue. The owner must retire work before ending
the C++ object's lifetime, too; a failed explicit destroy does not make scope
exit safe. Calls must be serialized by the owner.

A rejected prepare or FG-off configure can invalidate a pending snapshot without
retiring its GPU work. Previously this left Stage1's `m_prepared` occupied
forever. Once the registered fence completes, the adapter now discards the stale
snapshot without releasing its targets and resets history for the next frame.
The host deterministically holds a compute prepare behind a test gate, checks
that reuse/destruction are refused, releases the gate, and verifies recovery
and split compute-to-graphics generation. Retirement fences are queue-signalled;
only the test gate is CPU-signalled. A bounded multi-frame retirement policy is
still not implemented.

## Generated presentation, HUD and the session path

The initial wrapper presented the midpoint for the *previous* pair, then the
current real frame. The original justification that this one-cycle lag was
inherent was incorrect: computing a midpoint requires current real-frame data,
not prior display of that real frame. This implementation went backward in scene
time. The [current-pair correction](#clock-fix-regression-and-current-pair-scheduling-correction)
supersedes it. Likewise, capping individual waits alone did not prevent the
subsequently observed closed-loop pacing regression.

Pacing is owned by the wrapper alone. The limiter still caps real frames, and the
overlay callback is untouched.

The generated image is HUD-less by construction, so the composite pass adds back
the HUD layer as `currentBackbuffer - HUDLessColor`. This preserves a static HUD
exactly (100% of a known block survived in all three scenes) but, over fast
motion, a semi-transparent HUD leaves background change behind. A registered UI
resource would avoid that; Cyberpunk registers none.

Presentation uses three separate command allocators: two alternating ones for the
per-cycle copies and one for generation. Sharing one is wrong, not merely slow:
an early implementation reset an allocator while earlier copies still executed,
which the debug layer reported as a device removal. The corrected current-pair
schedule generates before the copies but retains separate allocators and fences.

`fg::Session` binds the FFX-shaped context to a wrapped swapchain found by
identity, records generation on the wrapper's list, and reports the wrapper's
fence so retirement stays with the object that owns the list. The offline host
runs describe, prepare, generate, present, FG off and clean destroy through it.

## Export routing and what is still unverified

`fg.mode` selects the path once, at the first create. `"amd"` is the default and
forwards everything, so every earlier release's behaviour is intact. `"sedici"`
creates the generator context and the swapchain wrapper and answers the five
exports for those handles only; a handle Sedici did not create is still forwarded,
so an AMD-owned context keeps working even in that mode.

The real-frame limiter attaches through `route_set_present_hook` rather than
through the swapchain directly. That keeps the wrapper free of limiter and
configuration symbols, and gives the limiter exactly one call per application
frame — the same rule the old vtable hook followed.

Two things cannot be established from the recorded contract, and both are left
deliberately conservative:

- **Queries.** No capture records which `ffxQuery` descs Cyberpunk issues on a
  frame generation context. Rather than guess an answer, a query on a Sedici-owned
  context is refused and logged. Any observed unsupported query must drive the
  next implementation, rather than guessing an answer from header declarations.
- **Unsupported calls.** Once Sedici owns a context there is no per-call fallback,
  because a context cannot be handed to AMD mid-life. Anything unsupported is
  refused and logged instead.

Neither changes `"amd"` forwarding. The subsequent game sessions exercise Sedici's
owned contexts but do not establish every possible query/unsupported-call path.

## First game session: `sedici` mode crashes, `amd` mode is sound

The first authorized launch exercised both modes on 2026-11-06.

`fg.mode = "amd"`: healthy. 73.23 output FPS, 36.74 real FPS, limiter intervals
27.397 ms with p99 = 27.397 and overshoot 0.001 ms, `fg=on`, overlay reporting
150 real / 150 generated per 300 output frames. The deployed build behaves
exactly like the previous one.

`fg.mode = "sedici"`: **the game dies with `0xC0000005` about 2.2 s in**, right
after `ffxCreateContext` returns for the swapchain wrap, and before any
configure, query or prepare is logged:

```
[2193.852 ms] ffxCreateContext #2 ctx=0x... result=0 (OK) chain=0x30001 (FrameGenerationSwapChainWrapDX12) ...
```

Reproduced three times with identical timing and no WER report. Two real bugs
were found and fixed on the way:

1. The swapchain-wrap create returned `FFX_API_RETURN_OK` but never wrote
   `*context`, so the caller held a null handle. Now set before returning.
2. The wrapper pointer was handed to the game without a reference, so the game's
   `Release()` and our `ComPtr` shared one count. Now `AddRef()`ed at hand-off.

Neither fixed the crash. A third run, with the swapchain deliberately left
**unwrapped** and the game's own pointer returned unchanged, crashed identically.
That diagnostic retained the registration defect described below, so it did not
establish that the rest of routing was sound. The earlier conclusion that queries
and dispatches were ruled out by missing log entries was incorrect: export logs
are written after routing or forwarding returns, so a crash inside AMD can occur
before the call is logged.

## Context registration fix and short game verification

Both branches of `route_create` registered ownership with this expression:

```cpp
g_owned.emplace(owned.get(), Owned{..., std::move(owned)});
```

Argument evaluation order permits moving the owner before reading `get()`. The
existing Release `fg_route.obj` disassembly confirmed that MSVC passed a null key
for both registrations. The session occupied that key; the wrap insertion then
collided and destroyed its owner while returning success and a dangling context.
Subsequent calls missed the registry and could forward Sedici handles into AMD.
This also explains why the unwrapped diagnostic did not prevent the crash. The
exact instruction at which the old build crashed was not captured.

The fix captures the raw handle before moving, checks insertion success, and
publishes the context and replacement swapchain only after successful registration.
The caller's COM reference is retained. No query implementations or FG algorithms
were changed as part of this fix.

Validation on 2026-11-06:

- `sedici_fgcheck --present --routing` passed with no DX12 debug-layer messages.
  The new focused check creates both contexts through the production router,
  checks configure/query/dispatch routing, unrelated-handle forwarding eligibility,
  destruction, and the caller's surviving swapchain reference. The previous
  session check called `Session` and `WrapHandle` directly and missed registration.
- CTest passed 3/3; the Release DLL has exactly the same five exports.
- After authorized deployment, a single `sedici`-mode Cyberpunk session remained
  running for 45 seconds, progressing past both creates to configure and 1,333
  prepare calls. The startup crash did not recur. No WER registry change was made.
- **Frame generation still does not work in this session:** every prepare was
  rejected as `unsupported depth format`. The game's desc reports `R32_FLOAT`
  depth and RGBA16F motion at 1707x720, with 2560x1080 output. Resolving this
  resource-format mismatch is the next blocker, not part of the registration fix.
- The limiter ran, but logged `fg=off`; the routed path still bypasses the legacy
  FG-active bookkeeping. This is not evidence that generated cadence is correct.
- The user's configuration was restored byte-for-byte after the check (no `[fg]`
  section, hence `amd` on the next launch), and the test process was closed normally.

Phase 1 remains incomplete. A surviving process is not acceptance of image quality,
generated HUD handling, production GPU cost, pacing, or overlay integration.

## Native depth/stencil view fix

The `unsupported depth format` rejection compared the allocation's native DXGI
format with shader-view formats. FFX's translated `R32_FLOAT` description does
not establish that the native allocation is `R32_FLOAT`.

Initially adding `D32_FLOAT`/`R32_TYPELESS` support passed offline checks but did
not clear the game's rejection in a 30-second check. A once-per-context native
descriptor log then established the actual input in a 12-second diagnostic run:

```
fg: native inputs depth{format=19 flags=2 size=1707x720} motion{format=10 flags=1 size=1707x720}
```

Format 19 is `DXGI_FORMAT_R32G8X24_TYPELESS`, a two-plane depth/stencil resource
with `ALLOW_DEPTH_STENCIL`. Its depth-plane SRV must use
`DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS`, not the allocation format or plain
`R32_FLOAT`. The fix explicitly resolves the native format to a compatible typed
SRV, leaving plane selection at depth plane 0; stencil is not sampled. The same
mapping handles `D32_FLOAT_S8X24_UINT`. Existing R32/R16 float support remains,
and unrelated formats are still rejected. Source: the vendored FFX DX12 resource
translation and Microsoft's [descriptor documentation](https://learn.microsoft.com/en-us/windows/win32/direct3d12/creating-descriptors),
with actual SRV compatibility checked under the DX12 debug layer.

Validation after the exact mapping was added:

- The existing offline executable exercises D32 float, R32 typeless, and both
  native depth/stencil allocation forms through prepare, generation and GPU
  retirement, restoring the caller's shader-read states. No debug-layer messages.
- The three image cases now use native `D32_FLOAT` depth targets and retain
  42.92/40.60/45.99 dB PSNR, 0.9985/0.9975/0.9984 SSIM and 100% synthetic HUD
  preservation; stage 1 is about 0.054 ms at 480x270 with the debug layer. This
  timing is not a production-resolution measurement.
- `sedici_fgcheck --present --routing` and CTest 3/3 pass; five exports remain.
- A subsequent 30-second `sedici`-mode game check survived and returned `OK` for
  prepares using the observed format-19 depth allocation. No prepare rejections
  were logged. This clears the depth-format blocker, not Phase 1 acceptance.
- Normal window close succeeded, but FG context destruction returned `ERROR`
  with `destroy before GPU retirement`. That teardown/lifetime defect is the next
  blocker; no change to retirement rules was included in this fix. The limiter's
  `fg=off` bookkeeping issue also remains.
- Each check restored the user's config byte-for-byte and closed the test process
  normally. No WER policy, upscaler, loader, or `.orig` changes were made.

## User gameplay capture and first pacing clock correction

The user reported an in-game cap of 36 real fps, an Afterburner reading of 72 fps,
good subjective input response, and occasional tearing at the top of the screen.
This is useful feedback, not an objective click-to-photon or image-quality result.
The attempted GDI video capture was black throughout and provides no visual
evidence. Cyberpunk disables FG in menus, so the first mixed menu/gameplay capture
is not a steady-state comparison.

The repeat used `tools/capture.ps1`, a 10-second warm-up and a 30-second capture at
the user's standard test location, in gameplay throughout:
`captures/2026-11-06-sedici-user-cap36-gameplay.csv`. The process used `fg.mode =
"sedici"` and `SEDICI_LIMIT_FPS=0`; the user supplied the 36 fps in-game cap.

- PresentMon recorded 2,155 presents, 72.0 fps; each five-second bucket contained
  357-360 presents. It reported no drops, independent flip, sync interval 0 and
  tearing allowed. This supports stable 2x presentation, not proof of correctly
  interpolated content or complete, distinct physical scanouts.
- Present intervals: mean 13.889 ms, SD 4.029 ms, p99 19.906 ms.
- Display-change intervals: mean 13.882 ms, p99 29.576 ms, max 31.916 ms; 1,078 of
  2,155 intervals (50.02%) were below 1 ms, with most remaining intervals near
  27-29 ms. This is not an even 13.9 ms output cadence.
- Earlier AMD run `captures/005/2026-11-05-D.csv` recorded 73.0 fps, present SD
  0.470 ms and display-change p99 19.202 ms, with no intervals below 1 ms.
  That run used Sedici's 36.5 fps limiter, not the game's 36 fps cap, so this is a
  reference comparison, not a matched A/B or a latency verdict.
- `MsUntilDisplayed` remains a software present-to-display estimate. In this
  tearing path it is not a physical scanout measurement or input latency.

Inspection found a definite units error in `fg_swapchain.cpp`: `now_seconds()`
multiplied QPC counts by frequency rather than dividing. Resulting timestamps were
so large that ordinary frame differences failed the interval filter, leaving the
default 60 Hz estimate instead of learning 36 Hz; small deadlines also lost
precision. The previous CPU pacing and its tests had not exercised this conversion.

The first clock-fix candidate (subsequently deployed and rejected below) included:

- A tested tick/frequency conversion, including realistic long-uptime precision
  and a missing-frequency guard.
- Learning the first real interval directly, then smoothing later samples, with
  interval history reset when the generator changes or the swapchain resizes.
- Absolute-QPC deadline waits using an optional high-resolution waitable timer
  and a 250 us spin tail. Timer failure falls back without failing game calls;
  deliberate waits stay capped at half an interval (and at most 50 ms per wait).
  The midpoint-to-real deadline is anchored at the actual first native Present
  submission, not followed by another full relative sleep after that call.
- Dropping a midpoint when its cycle is over half an interval late instead of
  presenting stale output; no deliberate wait at all when no midpoint exists.
- Per-300-cycle logs of generated/skipped counts, the learned interval, and
  measured CPU submission gaps. These are explicitly CPU timings, not GPU or
  physical display timings.

No new GPU-completion wait, presentation queue, worker thread or VSync/tearing
override is included. The capture also suggests that asynchronous GPU work can
collapse separately timed CPU submissions into nearly simultaneous display-change
events. Adding an unconditional GPU wait could affect real fps and latency, so
that hypothesis must be evaluated separately from the confirmed clock error.

Validation: conversion/deadline/drop checks, existing image/contract checks and
CTest pass. The windowed production-path functional checks pass, but the host's
strict debug-message check exits nonzero with 12 `GENERIC_READ` buffer-state
warnings while RTSS is active. A temporary synchronous warning stack traced
those allocations through `RTSSHooks64.dll -> d3d11 -> d3d11on12`; it was removed
after diagnosis. Warnings have not been filtered or hidden, and RTSS settings
were not changed. The headless check has no debug messages. Runtime improvement
was still unverified at that checkpoint; the subsequent capture exposed the
feedback regression below.

## Clock-fix regression and current-pair scheduling correction

The authorized clock-fix deployment was SHA256
`21E2ACCB27BBEC1F7337048B9BFE90A21B766A77EA7CA49147D8B13FE55E5A7C`.
The user immediately reported unstable frame rate. A 30-second capture,
`captures/2026-11-06-sedici-clockfix-cap36.csv`, confirmed a major regression:
535 presents, 17.9 output fps, mean present interval 55.909 ms and display-change
p99 62.459 ms. Every five-second bucket was around 18 fps. Logs showed a learned
real-frame interval of 111.6 ms and a roughly 50 ms CPU submission gap; earlier
windows also had long stretches with no generated presents and rising skips.

The QPC division was correct. The added scheduling anchor was not: interval
measurement used wrapper entries, but `m_last_cycle` was moved to the first native
submission, after the first wait and copy overhead. Targeting that submission
plus the entry interval fed the prior wait/overhead into the next first wait.
The learned interval then included those waits. Per-wait 50 ms caps did not bound
this closed-loop feedback. An in-memory model reproduced the drift; restoring an
entry anchor in that model stayed at the requested source rate.

The user's video also showed displaced overlapping scene fragments and doubled
HUD during the final camera pan, settling when the camera stopped, with the OSD
at 18 fps. There was no interpolation/HUD shader change in that deploy. Review
found two temporal problems, not proof of a newly introduced shader defect:

- The earlier pipeline showed an old midpoint after an already newer real frame:
  `real N-1 -> midpoint(N-2,N-1) -> real N`. This is backward in scene time,
  not a valid way of accepting one-cycle latency. The slowdown made the gap larger.
- Depth/motion were captured at prepare, but colour/HUD were consumed only after
  the presentation waits and real Present. That extended the required lifetime
  of application-owned colour and could expose frame-input mismatches. This is
  a code-level risk, not a proven explanation for every fragment in the video.

The replacement build (deployed and measured below) changes:

- One pure `PacingState`, used by production and the sustained-loop check, stores
  entry timestamps only. There is no pre-midpoint wait. The real deadline is
  entry + half an interval, so generation, copy and API time already spent use
  that budget rather than extending it. No-midpoint cycles do not wait.
- Generation runs before either native Present and before the pacing wait,
  consuming colour/HUD alongside the current prepare. Output order is now
  `real N-1 -> midpoint(N-1,N) -> real N`; the first cycle seeds history and shows
  only its real frame. A midpoint is consumed/discarded in its own cycle.
- The generated texture starts in COPY_SOURCE, matching the sink's restore-state
  contract. The wrapper no longer claims that the sink changed a COMMON output
  to COPY_SOURCE without recording such a transition.
- No VSync, tearing flag, shader, extra queue/worker or GPU-completion wait changes.
  Even physical display cadence and in-game artifact removal remain unverified.

Validation is still in the same offline executable, not a new test framework:

- A 20,000-cycle simulated closed loop (over nine minutes of source time) feeds
  variable wrapper/API costs into subsequent entries, includes FG off/on and a
  200 ms source stall, and stays/recovers at 36 real / 72 output cadence in that
  bounded-work model. This is a scheduling test, not measured game performance.
- The production session host now uses five consecutive moving frames rather
  than repeating one reset pair. It reads the actual native presentation buffers
  and verifies real times 0,1,2,3,4 with intervening midpoint times 0.5,1.5,2.5,3.5.
  Real-image ROI bytes match exactly; midpoint ROI PSNR is 60.39-73.09 dB, and each
  output carries the current frame's colour-coded HUD label. The ROI excludes the
  top-left external OSD area; this is an ordering/input-coherence check, not a
  replacement for the full-image quality cases or a general HUD accuracy claim.
  Only this test uses FLIP_SEQUENTIAL to preserve buffers for readback; the DLL
  still leaves the game's native swap effect unchanged.
- A sink check also verifies generation is called before any native Present of
  its own cycle. Counts alone had allowed the backward-order pipeline to pass.
- Existing headless images/contract checks and CTest 3/3 pass; exactly five exports.
  Windowed functional assertions pass, while the strict host still exits nonzero
  on the same 12 RTSS-injected GENERIC_READ state warnings. They remain unfiltered.
- The local Release DLL SHA256 is
  `B2D4BF17B70077D4E620C908538C876092EA6DBDFE7F4A75CC3C7C4ABC68058C`.
  No game launch, deployment, config change or RTSS setting change was made during
  this correction. It was subsequently deployed with authorization as recorded
  below, and the following section records its authorized validation capture.

The in-game shutdown-retirement error and FG-active bookkeeping still require
follow-up. Offline chronology fixes do not establish Phase 1 acceptance.

## Current-pair gameplay validation and remaining issues

After deployment, the user authorized a repeat capture at the standard test
location: 10-second warm-up, 30 seconds of gameplay, 36 fps in-game cap,
`fg.mode = "sedici"`, and process-local `SEDICI_LIMIT_FPS=0`. Evidence is
`captures/2026-11-06-sedici-currentpair-cap36.csv` and its matching session log.
The capture ended at 05:37:18 on 2026-11-06 and targets the launched PID 35728.

| Metric | Original 72 fps Sedici run | Rejected clock-fix run | Current-pair run |
| --- | --- | --- | --- |
| Present count | 2,155 | 535 | 2,156 |
| Output rate | 72.0 fps | 17.9 fps | 72.0 fps |
| Mean present interval | 13.889 ms | 55.909 ms | 13.898 ms |
| Present-interval SD | 4.029 ms | 5.399 ms | 0.845 ms |
| Present-interval p99 | 19.906 ms | 62.429 ms | 16.307 ms |
| Mean display-change interval | 13.882 ms | 55.909 ms | 13.892 ms |
| Display-change p99 | 29.576 ms | 62.459 ms | 29.851 ms |
| Display-change intervals below 1 ms | 50.02% | 0% | 50.00% |

The current-pair capture has 358/360/360/359/360/359 presents in its six five-second
buckets (71.6-72.0 fps). PresentMon reports no dropped presents, independent flip,
sync interval 0, `PresentFlags=512` (`ALLOW_TEARING`) and tearing allowed for every
row. These are presentation events, not a count of verified interpolated images
or complete physical panel scans. PresentMon's drop flag also does not count
midpoints discarded internally by Sedici before calling Present.

The matching log stays near a 27.7-27.9 ms real interval with roughly 13.88 ms CPU
submission gaps in the stable capture section, instead of drifting toward 111 ms.
Latest logged windows advance generated counts by 300 per 300 real cycles without
new skips. Earlier log windows include menus/transitions and are not the gameplay
capture interval; the user reports that Cyberpunk disables FG in menus.

**User assessment after this build:** the frame-rate regression is fixed, but
some tearing remains, along with weird artifacts while the game first boots.
The earlier good input-response assessment remains subjective; this run is not
an objective latency comparison. The user has not supplied evidence establishing
that every earlier camera-pan ghosting or HUD issue is gone.

Outstanding issues, with confidence distinguished:

1. **GPU/presentation bunching remains measured.** Exactly 1,078/2,156 reported
   display-change intervals are below 1 ms; most others are near 27-29 ms. CPU
   timing improved, but evenly spaced CPU calls did not produce evenly spaced
   display-change events. GPU queue/readiness versus actual scanout needs further
   investigation. This is not proof of the precise scanout/VRR failure mechanism.
2. **Tearing remains user-observed.** The capture still permits tearing. Being
   inside the VRR range does not alone establish tear-free presentation. No VSync
   or present-flag override has been applied, and an unconditional GPU wait is
   not yet justified by a latency/performance measurement.
3. **Startup artifacts remain user-observed, cause unproven.** Investigate initial
   history seeding, FG/menu transitions, current colour/depth/motion/HUD identity
   and resource validity. Do not label this a solved shader or motion-vector bug,
   or assume the synthetic HUD block/ROI checks establish in-game quality.
4. **Shutdown lifetime remains unresolved.** The earlier current-depth game run
   returned `destroy before GPU retirement`. No successful in-game retirement
   verification supersedes that finding yet; do not merely suppress the error.
5. **Integration/acceptance remains incomplete.** The routed limiter's `fg=off`
   bookkeeping, overlay/live pacing behaviour, production-resolution GPU cost,
   broad game contract coverage and in-game motion/camera-cut quality still need
   verification or correction. No GFFE work or training was added.

Verification scope and reproducibility:

- Release build succeeds; exactly five exports remain. CTest passes 3/3.
- The headless `sedici_fgcheck` passes its full-image PSNR/SSIM/HUD checks, native
  depth-format checks, retirement checks and 20,000-cycle pacing simulation.
- `sedici_fgcheck --present --routing` passes functional assertions, including
  native-buffer chronology/HUD readback, but returns nonzero on the 12 known
  RTSS-injected state warnings. This is not reported as a fully green debug run.
- Analyze the ignored local CSVs with `python tools/analyze.py --histogram <csv>`.
  Raw captures/logs, the user video, screenshots and build products are not
  committed. The metrics and evidence filenames are recorded here instead.
- The capture helper copies an append-only session log and its metadata parser
  takes first matches. Fields such as `sedici_tuning=sent` can therefore refer to
  an older AMD session. Mode/limiter claims above use the last session header and
  matching PID, not that historical metadata flag. No helper change is included.

**Outcome:** the 18 fps regression is corrected in gameplay and steady 2x output
is restored. Tearing/startup artifacts and the other acceptance items remain
open. No further deploy, launch or configuration change was performed while
documenting this result; another test/deploy remains an explicit user decision.

## Deployment record

Deployment chronology on 2026-11-06, using `tools/deploy.ps1` with the game stopped.
The first deploy did not launch the game; later launches were separately authorized.
- First deployed DLL SHA256 `89A6987B2DEDB5AFC9F75BE88CF7589BBC4D7B9EDD3AFCA419E63E2290659B98`,
  identical to the Release build from commit `3fddbe4`.
- Redeploy after the registration fix: SHA256
  `3464AFFCE4F4E5981FDD78899F641AF5917D7351B78C8D2D7BAA5BC48B7F3750`,
  identical to the local Release build used for the short session above.
- Redeploy after the native depth/stencil view fix: SHA256
  `7C11819029C329E0389CF67F2518184335F407B6490FB3911CD397459C8F2F7D`,
  matching the local Release build used for the final 30-second check.
- Rejected first clock-fix candidate, SHA256
  `21E2ACCB27BBEC1F7337048B9BFE90A21B766A77EA7CA49147D8B13FE55E5A7C`.
  It was superseded by the entry-budget/current-pair build.
- Latest authorized deploy: SHA256
  `B2D4BF17B70077D4E620C908538C876092EA6DBDFE7F4A75CC3C7C4ABC68058C`.
  The game was stopped for deployment, then launched as PID 35728 in `sedici`
  mode with process-local `SEDICI_LIMIT_FPS=0` for the user's in-game cap. The
  original config was restored byte-for-byte after initialization; `.orig` is
   unchanged. The process was left running. The user subsequently authorized the
   current-pair capture documented above.
- Original `.orig` untouched at `02297BEEDD285E822D3A64F314CF00FAF378DCEC0EDC47FF0C4DD71B3A8C2F18`.
- The existing `sedici.toml` was kept, not overwritten: it is the user's, and
  deploy never replaces it.
- The restored on-disk file has no `[fg]` section, so future launches default to
  `fg.mode = "amd"`. The authorized test process read `"sedici"` before restoration
  and retained it for its lifetime; restoring the file does not switch a live
  process back to AMD. The process-local limiter override likewise is not saved.

To exercise Sedici's own generator, add to the deployed `sedici.toml`:

```toml
[fg]
mode = "sedici"
```

That is a deliberate, reversible change to the user's configuration, not
something deployment does. Roll back with `tools/restore.ps1 -Game cp2077`, which
restores the `.orig` and removes `sedici.toml`.

Metrics use linear UNORM RGB (alpha excluded) for PSNR and pooled
8x8 uniform luma SSIM windows with stride 4, not Gaussian-window SSIM.

The startup crash did not recur after the registration fix, depth prepares are
accepted, and current-pair gameplay is back at 72 fps. Tearing, startup artifacts,
teardown, GPU/display cadence and overlay integration remain open. The restored
on-disk config selects `amd` for the next launch unless deliberately switched.
