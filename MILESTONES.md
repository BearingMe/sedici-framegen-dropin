# Experiment milestones

## Phase 1 — Mob-FGSR with custom scheduling in Cyberpunk

**Goal:** deploy a working drop-in replacement for AMD FSR 3.1 frame generation,
using Mob-FGSR and Sedici-owned presentation scheduling on `experiment/cobalt-otter`.
Start with 1080p output and 2× interpolation: one generated midpoint between
consecutive real frames. GFFE, super-resolution, extrapolation, additional
multipliers and a training framework are outside this phase.

**Status:** P1.1 baseline recorded ([baseline](docs/findings/007-fg-baseline.md));
P1.2 complete offline; P1.3 in progress. The LUT-backed stage, HUD composite,
bounded output schedule and a wrapper that presents a midpoint plus the real frame
all run end to end offline, including an FFX-shaped session bound to the wrapper.
The five exports still forward to AMD, so the shipped DLL is unchanged. Do
not launch the game before the first deploy or require a new contract-capture
run. Deployment and any later agent-driven launch require separate authorization.

### Current FFX implementation

| Area | Current state / reference |
| --- | --- |
| Five exports and FG ownership | `src/dllmain.cpp`, `src/forward.cpp`: exports forward to `.orig`; AMD owns generation and the swapchain |
| Pacing and context lifetime | `src/toggles.cpp`: tuning sends, async override and lifetime serialization |
| Real-frame scheduling | `src/present_hook.cpp`, `src/limiter.cpp`, `src/limiter_schedule.h`: application Present interception and opt-in real-frame limiter |
| Output and composition | `src/overlay.cpp`, `src/overlay_compose.cpp`: output callback, real/generated timing, back-buffer copy and state restoration |
| Contract logging | `src/describe.cpp`: desc-chain and return-code logging |
| Vendored API declarations | [FFX provenance](third_party/ffx/README.md): SDK 2.4.0, FG 4.0.2, DX12 swapchain 4.0.0; headers only, not the installed FSR 3.1 binary's implementation |

### P1.1 — Establish the baseline from existing evidence

**Done.** [007-fg-baseline.md](docs/findings/007-fg-baseline.md) records the
implementation state, the observed Cyberpunk contract, the measured reference
runs and the open items.

- Consolidate existing call flow, known input conventions, UI composition and
  measured cadence. Separate observations from assumptions and unresolved fields.
- Record each selected historical run's configuration; do not mix capped and
  uncapped results or treat callback counts as physical display cadence.
- Resolve remaining integration questions using local code, headers, existing
  captures/logs where available and upstream source. No new game run.

**References:** [002 pacing baseline](docs/findings/002-pacing-baseline.md),
especially runs E/G and comparisons;
[005 limiter](docs/findings/005-sedici-limiter.md), including the actual Sedici
limiter runs and latency results;
[006 overlay](docs/findings/006-overlay-ui.md), including composition and remaining
verification caveats; current implementation listed above.

**Done:** a short baseline and contract checklist in `docs/findings/`. Historical
run G is one reference: 74.0 output FPS, 13.513 ms average interval, 14.667 ms p99,
15.406 ms maximum. It used the game's 37 FPS cap, not Sedici's limiter; these
numbers are not universal acceptance thresholds.

### P1.2 — Implement the DX12 first-layer generator

**Done offline.** `src/fg_params.*` (constants), `src/fg_packing.h` (11/11/10 mirror),
`src/fg_shaders.h` (decode, dilate, clear, reproject, fill, warp, composite) and
`src/fg_stage1.*` (DX12 targets, history, dispatch chain). The FLOAT LUT is
embedded; FFX scaling, direction and state mapping are corrected. Prepare records
normalization/dilation into private textures on the supplied command list, and the
warp writes a private midpoint that a separate composite pass finishes.

- Normalize depth, motion vectors, jitter and render/output coordinates; keep
  two-frame history with discontinuity, resize and FG-toggle resets.
- Implement quadratic motion reconstruction, depth-tested splatting, thin-object
  hole filling, LUT backward warping and depth/brightness fusion in HLSL.
- Use the same compute implementation offline and in the DLL. Inspect the
  existing reference LUT's format and provenance before reuse; do not assume the
  design note's estimated representation matches the shipped asset.
- Record origin commit and attribution for adapted code/assets. MobFGSR's
  [license](references/sources/mobfgsr/LICENSE) is CC BY-NC 4.0, not MIT.

**References:** [Mob-FGSR](references/papers/mob-fgsr.md), sections 3–4;
[architecture note](references/papers/realtime-fg.md), section 4.1;
`references/sources/mobfgsr/src/offscreen_renderer.cpp` for pass order;
`references/sources/mobfgsr/resources/` for shaders and `lut.exr`.

**Done:** actual generated midpoint images from Sedici's compute path, not
passthrough images or AMD-generated output. Measure GPU time; the architecture
note's ~0.15–0.30 ms analytical budget is an estimate, not an existing result.

### P1.3 — Integrate FFX and Sedici's custom schedule

**Integrated offline; game acceptance blocked.** `src/fg_context.*` is a
supported-subset desc adapter with queue-signalled retirement, abandoned-snapshot
recovery and a bounded retirement wait before slot reuse. `src/fg_pacing.*` owns
the output decision: no pre-midpoint wait and a real-frame deadline half an
interval after entry, with already elapsed work consuming that budget; missed
midpoints are dropped rather than queued.
`src/fg_swapchain.*` presents private game-facing backbuffers, one generated frame
for the current pair before its current real frame, and `src/fg_integration.*` binds an FFX
session to that wrapper. `src/fg_route.*` routes the five exports, selected by
`fg.mode`, and the offline host runs the whole path.

Open before this can be called verified: which queries Cyberpunk issues (the route
currently refuses all queries on a Sedici-owned context and logs them), and
whether the game accepts the wrapped swapchain in every path it uses. The prior
deployed pacing candidate regressed to 18 fps; the replacement entry-budget and
chronological-output implementation restored 72 output fps in gameplay, but
GPU/display bunching and reported tearing/startup artifacts remain. `fg.mode` still
defaults to `"amd"` and forwards to AMD unless deliberately switched for a session.

- Implement the supported game's create/configure/prepare/dispatch/query/destroy
  contract and Sedici's swapchain wrapper, retaining the five-export interface.
- Present each generated midpoint before its corresponding real frame at evenly
  spaced deadlines. Account explicitly for interpolation's buffering delay.
- Protect textures, command allocators and submissions with fences. Skip missed
  generated-frame deadlines rather than accumulate a presentation backlog.
- Preserve HUD composition, overlay and real-frame limiter controls. Give output
  pacing one owner; do not stack AMD's scheduler and Sedici's scheduler.
- Select `.orig` when disabled or unavailable at initialization. Runtime recovery
  must respect context ownership and in-flight work, not forward a Sedici-owned
  context to AMD. Unsupported calls still follow the forwarding rules.

**References:** `third_party/ffx/framegeneration/include/ffx_framegeneration.h`;
`third_party/ffx/framegeneration/include/dx12/ffx_api_framegeneration_dx12.h`;
[AMD DX12 swapchain source](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/main/Kits/FidelityFX/framegeneration/fsr3/dx12/FrameInterpolationSwapchainDX12.cpp);
[AMD UI composition source](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/main/Kits/FidelityFX/framegeneration/fsr3/dx12/FrameInterpolationSwapchainDX12_UiComposition.cpp);
`src/present_hook.cpp`, `src/limiter_schedule.h`, `src/overlay_compose.cpp`.
Upstream links track `main`, not necessarily the installed binary; record the
reviewed revision when implementing against them.

**Done:** Sedici-generated frames pass through its own wrapper and schedule;
limiter, pacing controls and overlay remain coherent with that ownership.

### P1.4 — One small offline acceptance run

**Partial offline validation.** `tests/fgcheck.cpp` (built with
`-DSEDICI_BUILD_FGCHECK=ON`) runs the production stage 1 over three synthetic
scenes through the FFX adapter at 480x270 display / 320x180 render, using RGBA16F
UV motion and the original FLOAT LUT. Times include debug-layer overhead:

| Scene | PSNR | SSIM | stage 1 |
| --- | --- | --- | --- |
| rightward pan | 42.92 dB | 0.9985 | ~0.050 ms |
| leftward pan | 40.60 dB | 0.9975 | ~0.050 ms |
| diagonal thin bar | 45.99 dB | 0.9984 | ~0.050 ms |

These are synthetic checks, not proof of in-game quality or full ABI compatibility.
Earlier bicubic scores used a different sampler and bypassed FFX conversion;
do not compare them as a quality regression. `--present` additionally checks the
real-frame wrapper (identity, Present/Present1, TEST, resize, teardown), the
schedule (one midpoint per cycle, capped waits, sink removal), HUD preservation
(100% of a known HUD block survives compositing in all three scenes) and the whole
session path (describe, prepare, generate, present, FG off, clean destroy).
The same host now includes a 20,000-cycle closed-loop pacing simulation and native
buffer readback of five consecutive moving frames with current-frame HUD labels;
`--routing` additionally exercises owned-handle routing. Headless checks pass,
but the strict windowed host exits nonzero on known RTSS-injected state warnings
despite passing its functional assertions. These warnings are not filtered.
Production-size timing and in-game verification remain. A new Cyberpunk capture is
not a pre-deploy gate.

- One DX12 executable exercises the production generator and scheduler. Use a
  few short sequences covering camera motion, moving/thin geometry, disocclusion
  and lighting changes. Use available small inputs or locally generated fixtures;
  do not download large datasets or launch Cyberpunk to acquire them.
- Withhold real midpoint frames as ground truth. Generate them from surrounding
  frames; report PSNR, SSIM and GPU time, using a documented common color space
  and range. Save comparison images and inspect obvious artifacts.
- In the same executable, briefly present at 2×, exercise resize/history reset
  and clean shutdown. Verify the release DLL has exactly five exports.

**References:** [Mob-FGSR evaluation](references/papers/mob-fgsr.md), section 5;
existing DX12 test-host patterns in `tests/regression.cpp`;
build/export commands in [AGENTS.md](AGENTS.md#build-and-verify).

**Done:** review images and scores, resolve obvious coordinate/history defects,
and confirm functioning presentation. No invented universal PSNR/SSIM threshold,
new testing framework, broad matrix or dozens of new tests.

### P1.5 — First deploy and Cyberpunk verification

**Deployed and measured; not accepted.** The registration fix prevented the
startup crash, and the native depth/stencil SRV fix cleared prepare rejections
in a subsequent 30-second authorized `sedici`-mode session. FG context destruction
still returns `destroy before GPU retirement`; generated cadence/quality are not
accepted. The limiter's FG-active bookkeeping also remains unwired on the routed path.
The initial depth-validation test process was closed normally, `.orig` is
preserved, and the user's config was restored byte-for-byte (`amd` on the next
launch unless deliberately switched). See
[007](docs/findings/007-fg-baseline.md#native-depthstencil-view-fix).

The subsequent user-driven gameplay capture confirms stable 72 fps presentation
with a 36 fps in-game cap, but reports alternating sub-ms / roughly 28 ms
display-change intervals. The first clock/deadline correction was deployed and
regressed to 17.9 output fps. A replacement entry-budget/current-pair scheduler
passes a sustained closed-loop simulation and native GPU-buffer chronology/HUD
checks and was deployed/launched with authorization. The authorized 30-second
gameplay capture records 72.0 fps, present-interval SD 0.845 ms and p99 16.307 ms,
but half its display-change intervals remain below 1 ms, alternating with roughly
28 ms gaps. The user confirms the regression is fixed, with some tearing and
artifacts during initial game boot still present. Even GPU/display cadence,
startup quality and clean teardown are not accepted. See
[current validation/open issues](docs/findings/007-fg-baseline.md#current-pair-gameplay-validation-and-remaining-issues).

- With deployment authorization and the game stopped, deploy through the existing
  safety script. Preserve `.orig` and the user's configuration.
- Only after this first deploy, have the user launch Cyberpunk, or obtain
  separate authorization for an agent-driven launch. Enable FSR 3.1 FG and check
  one short session: camera pan, driving, menus and FG off/on.
- Confirm frames actually come from Sedici, the HUD is intact, cadence is even
  and no persistent backlog develops. Record GPU time and cadence against P1.1,
  stating configuration differences and historical comparison limitations.
- Fix deployment blockers or restore safely. Do not modify game settings files,
  the loader or the upscaler.

**References:** `tools/deploy.ps1`, `tools/restore.ps1`, `src/timing.cpp`,
`src/stats.cpp`, and findings 002/005/006 above.

**Done:** a usable deployed Cyberpunk build with Mob-FGSR and custom scheduling,
or a specific blocker and safe rollback. Offline images alone do not complete
Phase 1. GFFE residual inference is deferred to a later phase.
