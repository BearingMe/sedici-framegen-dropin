# Experiment milestones

## Phase 1 — Mob-FGSR with custom scheduling in Cyberpunk

**Goal:** deploy a working drop-in replacement for AMD FSR 3.1 frame generation,
using Mob-FGSR and Sedici-owned presentation scheduling on `experiment/cobalt-otter`.
Start with 1080p output and 2× interpolation: one generated midpoint between
consecutive real frames. GFFE, super-resolution, extrapolation, additional
multipliers and a training framework are outside this phase.

**Status:** P1.1 complete ([baseline](docs/findings/007-fg-baseline.md)); P1.2 in
progress — the analytical stage is implemented as HLSL and a DX12 pipeline, with
no FFX integration yet, so the shipped DLL still forwards generation to AMD. The
existing Cyberpunk implementation and recorded performance are the baseline. Do
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

**Done.** `src/fg_params.*` (constants), `src/fg_packing.h` (11/11/10 mirror),
`src/fg_shaders.h` (dilate, clear, reproject, fill, warp) and `src/fg_stage1.*`
(DX12 targets, history, dispatch chain). Not yet called from the DLL; FFX
integration is P1.3.

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

**Host ready, real-frame scoring pending.** `tests/fgcheck.cpp` (built with
`-DSEDICI_BUILD_FGCHECK=ON`) runs the production stage 1 over three synthetic
scenes at 480x270 display / 320x180 render and prints PSNR, SSIM and GPU time:

| Scene | PSNR | SSIM | stage 1 |
| --- | --- | --- | --- |
| rightward pan | 61.98 dB | 1.0000 | 0.035 ms |
| leftward pan | 43.13 dB | 0.9990 | 0.035 ms |
| diagonal thin bar | 49.05 dB | 0.9990 | 0.035 ms |

These numbers validate the implementation (trajectory, sign, splat, fill, fusion,
resource states) on synthetic motion. They say nothing about image quality on a
rendered frame, they are not the 1080p budget, and the rescaled cost is a rough
estimate, not a measurement. Still to do: score real captured frames and read
the 1080p cost from the overlay/stats once P1.3 exists.

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
