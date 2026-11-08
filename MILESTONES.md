# Experiment milestones

## Phase overview

- **Phase 1:** establish the analytical generator and custom presentation path;
  retain the implementation history and evidence below. Component-level results
  are not proof that generation, overlay, limiter, and settings work together.
- **Phase 2:** audit, document, cover, and incrementally rebuild that integration
  around explicit ownership, a pinned official AMD binary fallback, coherent live
  settings, and useful diagnostics. The first gate is audit/documentation, the
  second is test coverage, and only then does structural migration begin.
- **Later delivery:** implement and validate residual inference against
  [the two-stage design](references/papers/realtime-fg.md). Phase 2 must prepare
  the boundary, not quietly absorb model training, inference optimization, and
  quality/performance acceptance into a small refactor task.

The Phase 1 entries include historical implementation/status snapshots. Later
evidence in [008](docs/findings/008-fg-tearing.md) and
[009](docs/findings/009-output-multipliers.md) supersedes some presentation details;
Phase 2 begins by reconciling them with the actual code. Starting Phase 2 does not
retroactively mark the remaining Phase 1 acceptance requirements complete.

## Phase 1 — Mob-FGSR with custom scheduling in Cyberpunk

**Goal:** deploy a working drop-in replacement for AMD FSR 3.1 frame generation,
using Mob-FGSR and Sedici-owned presentation scheduling on `experiment/cobalt-otter`.
Start with 1080p output and 2× interpolation: one generated midpoint between
consecutive real frames. GFFE, super-resolution, extrapolation, additional
multipliers and a training framework are outside this phase.

**Status:** P1.1 baseline recorded ([baseline](docs/findings/007-fg-baseline.md));
the LUT-backed stage and presentation wrapper have offline coverage. The default
mode forwards to `.orig`; experimental Sedici mode routes recognized contexts via
the exports. Later integration and its limitations are recorded in the
[P2.1 audit](docs/refactor/audit.md) and findings 008/009. Phase 1 acceptance is
not implied. Deployment and any agent-driven game launch require separate authorization.

### Current FFX implementation

| Area | Current state / reference |
| --- | --- |
| Five exports and FG ownership | `src/dllmain.cpp`, `src/fg_route.cpp`, `src/forward.cpp`: default `.orig` forwarding or experimental Sedici-owned recognized contexts |
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
chronological-output implementation restored 72 output fps in gameplay. Tearing
is now resolved by `[fg] present = "vblank"` (default): 74.9 output fps with every
frame displayed for one 13.33 ms refresh and the user reporting no tears. Startup
artifacts and shutdown retirement remain. `fg.mode` still defaults to `"amd"` and
forwards to AMD unless deliberately switched for a session.

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
28 ms gaps. Follow-up probes traced that to the midpoint not being GPU-ready when
the real frame was presented (98% of cycles), and to flips landing mid-scan with
the game's tearing flags. `[fg] present = "vblank"`, now the default, presents both
outputs with SyncInterval=1 and without ALLOW_TEARING: the last capture records
74.9 fps with every frame displayed 13.31-13.40 ms and the user reports no tearing
and normal performance. Startup artifacts and clean teardown remain open. See
[vblank presentation](docs/findings/007-fg-baseline.md#vblank-presentation-removes-the-remaining-tearing).

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

## Phase 2 — Reliable runtime and incremental architecture refactor

**Goal:** make the existing generator, frame cap, overlay, settings, and fallback
operate as one maintainable product rather than independently working components.
Use a session-owned runtime with explicit lifecycle, backend/context ownership,
GPU retirement, acknowledged settings, and bounded diagnostics/recovery.

**Status:** planned. The direction is recorded in
[docs/refactor](docs/refactor/README.md); the
[documentation improvement plan](docs/refactor/planning/README.md) is the first
work package, not completed acceptance. No milestone below is complete merely
because its proposed structure has been documented.

**Scope:** retain and validate the analytical implementation while migrating its
integration. Add an official pinned AMD **binary** backend, not AMD's full source
implementation. Keep compatibility forwarding distinct from that backend. Prepare
the HUD-less feature/composition boundary for Stage 2 without claiming to deliver
the trained residual network in this phase.

### Sizing and execution rules

This is a substantial engineering phase, not a file-moving exercise. Sizes below
describe uncertainty and breadth, **not calendar estimates**:

- **Large:** several reviewable changes with discovery, implementation or analysis,
  focused verification, and documentation. Not one cleanup commit.
- **Very large:** a cross-cutting ownership or timing change requiring smaller
  sub-gates, failure-path work, and integrated regression verification. Do not
  estimate it as routine extraction before the relevant spike/tests exist.

### Phase 2 progress

**Keep this table current.** Update it in the same commit that changes a milestone's
status, evidence or open items. Last updated: 2026-11-08, branch `refactor/phase-2`;
this table includes uncommitted working-tree changes. Latest full CTest run after the AMD backend removal: 32 of 32 passed (the AMD probe and the AMD-forwarding ABI scenarios were deleted and a no-original contract scenario added, so the count is not comparable with the earlier 37 of 37). An earlier `sedici_fgcheck_present` run produced a different skipped-frame count and failed its signature check (K72); the prior hotkey key-up timeout (K54) remains unproven despite the K93 pairing fix. The stale-handle test checks the owner-approved token namespace; guarded AMD descriptor variants 256–259 ran after explicit authorization; their evidence is bounded. The only gameplay evidence on this branch is a user-run Alan Wake 2 session that asks for `SwapChainNewDX12` and is refused before any frame (K100, [finding 014](docs/findings/014-alan-wake-2-swapchain-new.md)); no frame generation has run in the game since the AMD backend was removed.

| Milestone | Status | Evidence | Open items |
| --- | --- | --- | --- |
| P2.1 Audit and documentation | Complete | [audit](docs/refactor/audit.md) | None |
| P2.2 Regression and integration coverage | Complete, with accepted gaps | [finding 010](docs/findings/010-p22-coverage-gaps.md), [reliability](docs/refactor/reliability.md) | Menu clicks (P2.6, P2.8), GPU failure injection (P2.5), Save-time failures (P2.6), vrr presenter readback (P2.7), production-resolution performance (P2.11) |
| P2.3 AMD binary feasibility | Complete; superseded by the owner decision to remove the AMD backend (tag `pre-amd-removal`) | [amd-binary](docs/refactor/amd-binary.md), [finding 011](docs/findings/011-p23-amd-binary-gaps.md), [NOTICE](NOTICE.md) | Owner review of the gate decision; production-size inputs and overlay through AMD's callback (P2.8, P2.9) |
| P2.4 Session, context and callback ownership | Complete (owner accepted 2026-11-08), with recorded gaps | [finding 012](docs/findings/012-p24-ownership.md), K29, K60, K63, K70, K102 | Accepted gaps: K64 (DLL unload/reload) moved to P2.10; allocation failures inside `Context::create` are not injected; post-transfer rollback, K104 and K105 go to P2.5; K103 documents the deliberate untracked-sink retention |
| P2.5 GPU submission, frame slots and retirement | In progress; prior completion claim withdrawn | [finding 013](docs/findings/013-p25-gpu-retirement.md) | Retirement, timeout and session-recovery hazards (K37, K41, K45–K48, K66); injected signal/Close/Reset checks and gated same-queue wrapper/session/registration recovery pass, acceptance not met |
| P2.6 Settings transactions and TOML migration | In progress, not accepted | Uncommitted transaction prototype and regression checks | Worker unload/stall handling and atomic external-edit handling (K43, K83–K86), backend capability and coherent applied acknowledgements (K42, K56, K91–K92), persistence coordination (K80); schema and migration acceptance remain |
| P2.7 Real-frame admission and output pacing | In progress, initial policy test only | `sedici_fgcheck` ratio boundary regression (K50) | Unified admission/output policy and integrated acceptance still pending; P2.5/P2.6 dependencies remain |
| P2.8 Composition, overlay and input integration | In progress, initial boundary extraction | `Stage1::synthesize` / `composite_hud`, image tests and chained-callback error regression | Composition remains inside Stage1; HUD-less reset seed (K53), intermittent hotkey timeout (K54), AMD caller-callback/UI-resource gap (K62), dynamic probe input and readback (K90), independent ownership, overlay/input integration and both-backend acceptance are pending |
| P2.9 Bundled AMD backend and packaging | Superseded | None | The AMD backend is removed; packaging reduces to the proxy DLL, `sedici.toml` and the deploy tools; `aw2` was added to `tools/games.json` (K101 log evidence) |
| P2.10 Errors, diagnostics and watchdog hardening | Not started | None | None yet |
| P2.11 Integrated acceptance and Stage 2 handoff | Not started | None | Needs in-game runs by the owner |

**Bugs** (all in [known-issues.md](docs/refactor/known-issues.md), 106 entries): 91 open
(K1-K24, K26, K29, K32, K36, K37, K39-K69, K71-K93, K95, K98-K101, K103-K105; AMD-only entries among them are listed as obsolete at the end of the file, and K64 is an owner-accepted limitation moved to P2.10), 2 hazards (K31, K94, both now obsolete), 12
fixed or resolved (K25, K27, K28, K33, K34, K35, K38, K70, K96, K97, K102, K106; K96, K97 and K35 in d95d454, K70 by owner decision, K102 and K106 uncommitted), and 1 retracted claim (K30). Decision waiting on the owner: none for the `AGENTS.md` branch override (K35 is resolved). K31 is obsolete with the AMD path. K100 (Alan Wake 2's `SwapChainNewDX12` create is refused; [finding 014](docs/findings/014-alan-wake-2-swapchain-new.md)) and K101 (game-side `sedici.log` files mix several builds and games) were recorded on 2026-11-08 from a user-run AW2 session.

| Milestone | Size | Prerequisites |
| --- | --- | --- |
| P2.1 Audit and documentation | Large | Existing code, findings, refactor proposals |
| P2.2 Regression and integration coverage | Large | P2.1 contracts and evidence map |
| P2.3 AMD binary feasibility and fallback contract | Large; uncertainty may expand scope | P2.1, P2.2 test seams |
| P2.4 Session, context, and callback ownership | Very large | P2.2, relevant P2.3 decisions |
| P2.5 GPU submission, frame slots, and retirement | Very large | P2.4 ownership boundary |
| P2.6 Settings transactions and TOML migration | Very large | P2.1 schema decisions, P2.4 owner |
| P2.7 Real-frame admission and output pacing | Very large | P2.5 retirement, P2.6 application path |
| P2.8 Composition, overlay, and input integration | Very large | P2.5-P2.7 contracts |
| P2.9 Bundled AMD backend and safe packaging | Very large | P2.3 feasibility, P2.4-P2.8 integration |
| P2.10 Errors, diagnostics, and watchdog hardening | Large; cross-cutting | Contracts begin in P2.1; infrastructure begins in P2.2-P2.4 |
| P2.11 Integrated acceptance and Stage 2 handoff | Large | P2.4-P2.10 accepted |

The numbering is a dependency guide, not a reason to postpone all error handling
until P2.10. Instrument and test each migrated slice as it lands. P2.3 discovery
may proceed alongside coverage development after P2.1; no production migration
may bypass the coverage gate. Re-scope dependent work if the binary investigation
invalidates an assumed interface or presentation model.

For each implementation slice, record the responsibility changing owner, behavior
preserved, intentional fixes, acceptance evidence, and a safe rollback. Remove the
superseded path after acceptance; do not keep two authoritative settings stores,
registries, or lifecycle managers. Preserve existing user work and verified
algorithms. Separate mechanical moves from behavior changes where practical.

### P2.1 — Audit and documentation

**Status:** audit and documentation prepared for review; see the
[current-state evidence and P2.2 handoff](docs/refactor/audit.md). Acceptance awaits
review. This milestone does not move or fix code.

- Trace both backend paths from the five exports through contexts, callbacks,
  prepare/generation, Present, settings, overlay, limiter, and shutdown.
- Inventory ownership, thread/lock boundaries, outstanding GPU work, public and
  hidden settings, error handling, and test entry points. Identify what tests
  bypass or serialize and distinguish missing integration from algorithm defects.
- Reconcile code, current configuration, README, milestone snapshots, and findings.
  Separate verified behavior, historical evidence, reported failures, assumptions,
  and unresolved questions. Existing claims are not fresh validation results.
- Improve the architecture, file-structure, TOML, reliability, and planning documents
  with decided/proposed/unresolved status, enforceable boundaries, lifecycle
  transitions, setting application semantics, and a dependency-ordered migration.
- Record expensive unknowns as explicit investigation tasks, including AMD binary
  capabilities, context recreation, and queue/composition interoperability. Settle
  contracts needed for the next slice; do not invent answers to unblock a diagram.

**Acceptance:** a developer can identify an upcoming slice's owner, allowed
dependencies, observable behavior, failure policy, tests, and unresolved blockers
without guessing. Produce a prioritized gap/evidence map and reviewable contracts.
No implementation estimate assumes that an unverified AMD interface exists.

**References:** [refactor decisions](docs/refactor/README.md),
[documentation plan](docs/refactor/planning/README.md), findings 002/005/006/007/008/009,
and [the two-stage objective](references/papers/realtime-fg.md).

### P2.2 — Regression and integration coverage before migration

**Status:** complete, with explicitly accepted gaps. 18 CTest entries pass (12 `abi`
scenarios on the real DLL plus a stub `.orig`, 3 `fgcheck`, regression and two GPU
overlay checks) on one shared `sedici_core` object library. Baseline, known failures
and the accepted gaps with their owning milestones are in
[reliability.md](docs/refactor/reliability.md#current-coverage-and-gaps-p22-baseline)
(evidence: [finding 010](docs/findings/010-p22-coverage-gaps.md)). Establish a trustworthy baseline, not merely more isolated
unit checks or an arbitrary line-coverage percentage.

- Build tests against the same production implementation targets. Include a
  focused check of the actual DLL entry points and exactly five exports; do not
  validate only a source list that omits the integration boundary.
- Protect valid behavior with characterization tests: compatibility forwarding,
  arguments/results, descriptor restoration, input restoration, and existing
  analytical image correctness. Do not freeze known bugs as correct behavior.
- Extend the small offline DX12 host to exercise generation, Mini/Full overlay,
  real-frame cap changes, settings acknowledgement/save/cancel, and metrics together.
  Add focused resize, FG-toggle, window replacement, and teardown scenarios.
- Test pure policy with controlled time and injected outcomes. Exercise genuinely
  in-flight resources rather than waiting for the GPU after every frame. Cover
  initialization, submission, settings, logging, and persistence failures.
- Register relevant checks with CTest, isolate fixtures from game files, and record
  validation availability and third-party overlay interference. Keep a few composed
  scenarios rather than a broad GPU option matrix or dozens of new GPU tests.

**Acceptance:** reproducible passing characterization checks plus explicit failing
product-acceptance cases for known integration defects. A failing case is tracked,
not silently skipped or relabeled green. Migration cannot regress protected behavior
or add unexplained failures. Record which gaps remain before starting P2.4.

### P2.3 — Investigate the official AMD binary and fallback boundaries

> **Superseded.** The owner removed the AMD backend (tag `pre-amd-removal` has the last
> code that used it). The investigation and its evidence stay as reference; nothing below
> is an open requirement.

**Status:** complete; gate decision recorded in
[amd-binary.md](docs/refactor/amd-binary.md#gate-decision) (review pending). **GO** for
an AMD backend using the installed, hash-pinned copy; **NO** to AMD feeding Sedici's
presenter and to seamless handoff; bundling unblocked by the owner's licence review (`NOTICE.md`). This is a compatibility investigation with a go/no-go gate,
not just a `LoadLibrary` task. Use a separately obtained/approved official artifact;
never acquire runtime DLLs automatically during build or test.

- Identify the exact version, architecture, hash, origin, dependencies, and
  redistribution obligations. Validate the five-export ABI and relevant descriptors
  against the binary, not only the newer vendored headers.
- Verify private loading under the intended filename/path, dependency resolution,
  coexistence with the proxy and `.orig`, and correct ownership of every handle.
- Establish supported generation/query/callback and UI-composition interfaces.
  Determine whether AMD can feed Sedici's presenter or must retain AMD-owned
  scheduling/composition. Do not stack two output schedulers.
- Specify pre-ownership fallback, unavailable resources, real-frame degradation,
  and recreation/restart behavior. Include separate FG/swapchain creation order
  and failures after a native chain has been consumed or replaced.

**Acceptance:** documented binary/compatibility evidence and a testable integration
contract, or a specific blocker requiring a revised design. Seamless mid-session
handoff is not promised without proof of coordinated draining and recreation.
Re-estimate P2.4/P2.8/P2.9 after this gate; stubs cannot establish binary compatibility.

### P2.4 — Establish session, context, and callback ownership

**Status:** reopened; the earlier completion claim was not established. See K29 and deferred items below (evidence:
[finding 012](docs/findings/012-p24-ownership.md)). Split this into lookup/identity, lifecycle transitions, and
callback lifetime sub-gates; it is not a rename of `fg_route` or `fg_integration`.

- Introduce one logical session relating FG contexts, swapchain contexts, backend
  identity, game-facing buffers, and effective status. Keep ABI dispatch thin.
- Provide lifetime-safe registry access and explicit callback registration,
  reentrancy, detach, and destruction contracts. Do not hold registry locks across
  backend calls, external callbacks, or GPU waits.
- Represent probing, history warmup, active/degraded/suspended operation, draining,
  and stopped/device-lost behavior. Define accepted work and observable results at
  each transition, including partially initialized sessions.
- Publish coherent backend/FG/status identity on both paths. Never route a handle
  to a binary or implementation that did not create it.

**Acceptance:** production-path tests cover separate context creation order,
failed/partial initialization, FG toggles, callback detachment, and destruction.
No dangling registry lookup or callback may reach a destroyed owner. The old
authoritative lifetime/status bookkeeping is removed after its replacement passes.

### P2.5 — Make GPU submission and retirement explicit

**Status:** in progress; prior completion claim withdrawn after review ([finding 013](docs/findings/013-p25-gpu-retirement.md)). Treat allocation/descriptor ownership, queue ordering, and
resize/shutdown retirement as separate sub-gates with their own failure checks.

- Attach resources, allocator memory, descriptor ranges, scratch, and output images
  to explicit frame slots and queue-signalled retirement tokens.
- Specify dependencies between game capture, generation, composition, and present
  queues. A resource barrier or COM reference is not proof of execution ordering
  or GPU completion; borrowed game command lists cannot be deferred arbitrarily.
- Return meaningful submission acceptance/rejection; do not advance buffer/history
  ownership after silently dropping a job. Handle partial recording/submission.
- Stop admission before resize or teardown, retain outstanding resources, and
  define bounded waits/recovery. Timeout is not permission to free in-flight memory
  or CPU-signal fictitious completion. Include device loss and abandoned work.

**Acceptance:** multiple frames in flight, failed submission, resize, FG reset, and
teardown pass focused lifetime checks with the installed DX12 validation facilities.
Document bounded retention/recovery on timeout. Do not claim success by serializing
every frame, leaking without bounds, filtering warnings, or ignoring retirement errors.

### P2.6 — Implement coherent settings and safe TOML migration

**Status:** in progress; transaction prototype and tests are uncommitted and acceptance remains open (K42–K44, K49). Parsing, live application, UI acknowledgement, and persistence
are a coordinated change, not a parser cleanup. Agree the schema before switching it.

- Finalize supported user-facing keys, defaults, bounds, units, backend capability,
  application timing, and any retained override/deprecation policy.
- Apply complete validated candidates through revisioned requests and acknowledged
  snapshots. Distinguish pending/rejected/recreation-required from applied values.
  Define partial failure and concurrent request/conflict behavior.
- Route live controls through this contract, not independent global writes and
  manual subsystem resets. Apply cap/sync changes at safe boundaries.
- Implement save/cancel against acknowledged state with external-edit detection,
  comment-preserving safe persistence, and non-fatal I/O failure. Keep disk work
  outside output callbacks. Do not add file watching unless separately required.
- Account explicitly for legacy pacing/async controls, fractional/adaptive output,
  and environment switches. `fg.mode = "amd"` currently identifies `.orig`; do not
  silently reinterpret it as the bundled binary or remove opt-outs/defaults.

**Acceptance:** parser, runtime, UI, and persisted values agree under successful,
rejected, concurrent, and failed-save changes. Unknown/deprecated keys get actionable
diagnostics. Existing configurations have a documented migration, not silent drift.
The proposed schema in docs/refactor is not enabled until this gate is satisfied.

### P2.7 — Coordinate the real-frame cap and output pacing

**Status:** initial policy boundary work only (K50); integrated milestone pending. Separate deterministic policy extraction, wait/backpressure
integration, and production-path cadence validation. This is feedback/timing work,
not simply reconnecting the limiter callback.

- Give real-frame admission and output scheduling one consistent policy with
  explicit clock units, GPU readiness, sync intent, elapsed budget, and overload
  handling. Avoid compounded limiter/presenter/vsync waits and latency feedback.
- Keep one admission per real game frame, no limiter work for TEST/off paths, and
  correct FG-enabled/disabled behavior. A live rate change resets coherently.
- Preserve chronological output and bounded backlog through hitches and missed
  deadlines. Drop stale generated work rather than accumulating delayed output.
- Define supported multiplier/adaptive combinations, their latency and real-frame
  omission semantics, and each backend's applicability. Do not silently discard
  existing work or equate planned output rate with physical display cadence.

**Acceptance:** deterministic closed-loop tests and the integrated host cover cap
changes, FG off/on, overload, stalls, sync policy, and supported output ratios.
The output rate must not collapse through self-amplifying queue latency. Compare
measured cadence and CPU/GPU wait costs with a configuration-matched baseline.

### P2.8 — Separate composition and integrate overlay/input

**Status:** initial HUD-pass extraction in progress; acceptance pending. Use distinct sub-gates for HUD-less generation, output rendering,
and window/input ownership; rendering a demo overlay does not complete this work.

- Move final HUD composition out of the analytical stage. Preserve an explicit
  HUD-less boundary for residual correction before HUD and Sedici overlay drawing.
- Render UI on final real/generated outputs through each backend's supported
  composition contract. Preserve caller callbacks and game UI; do not return
  success while dropping required composition.
- Give ImGui rendering resources explicit queue/descriptor retirement and thread
  ownership. UI reads status snapshots and submits settings requests only.
- Preserve hotkey behavior, cursor/clipping ownership, input restoration, coordinate
  mapping, window replacement, and overlay-disable recovery independently of a
  functioning GPU renderer. Remove monitor-specific control assumptions.

**Acceptance:** the combined scenario validates Mini/Full operation, HUD preservation,
cap/settings changes, resize/window replacement, FG toggles, and renderer failure
with input returned to the game. Demonstrate both backend integrations; a direct
`ui_on_present` unit call alone is insufficient.

### P2.9 — Integrate and package the pinned AMD backend

> **Superseded.** There is no AMD backend to integrate or package. Packaging reduces to
> the proxy DLL, `sedici.toml` and the deploy tools. The text below is kept as history.

**Status:** pending. P2.3 proves feasibility; this milestone delivers the full backend
and lifecycle integration, not just a successful binary load.

- Implement the validated binary adapter, context/callback ownership, capability
  reporting, and the agreed presentation model using the shared runtime services.
- Exercise unavailable, incompatible, partially initialized, and rejected-resource
  cases before publishing ownership. Keep `.orig` compatibility forwarding distinct.
- Integrate UI, limiter, settings, and diagnostics without passing foreign handles
  or attempting per-call takeover of a live Sedici-owned session.
- Resolve distribution rights/notices, exact dependency/deployed paths, integrity
  checks, and uninstall/rollback behavior. Reconcile existing deploy allowlists and
  fallback instructions explicitly before expanding them. Preserve `.orig` and
  existing user configuration; builds/tests must remain offline.

**Acceptance:** integration tests exercise the actual selected AMD binary as well
as failure stubs. Selection/degradation behavior is observable and consistent with
the approved contract. Packaging has a reviewable safety/rollback plan and does not
touch the loader/upscaler; deployment remains separately authorized.

### P2.10 — Harden errors, diagnostics, and watchdog recovery

**Status:** pending; cross-cutting. Establish error vocabulary and event identity
early, emit evidence in every migrated slice, then complete recovery/overhead checks.

- Use typed, checked outcomes with stable codes, underlying API results, session/
  frame/settings identity, and explicit recovery actions. Contain exceptions at
  foreign boundaries; allocating inside `noexcept` is not graceful failure handling.
- Capture bounded events and recent history without synchronous file I/O in output
  paths. Rate-limit repetition, count dropped diagnostics, and report sink failure.
- Publish truthful metrics for real admission, output calls, GPU work, drops, waits,
  queue depth, and requested/applied state. Label unavailable measurements; CPU
  Present timestamps are not physical flips or input-to-photon latency.
- Monitor progress and invariant violations without depending on the stalled
  operation's lock. Define retry/degradation limits and avoid recovery oscillation.
  Preserve genuine device errors; do not promise recovery from arbitrary corruption.

**Acceptance:** injected log failures, queue stalls, component failures, and rejected
settings leave bounded behavior and actionable evidence. Watchdog actions follow
the session owner and retain GPU safety. Measure diagnostic overhead; the observer
must not materially create the timing failures it is intended to diagnose.

### P2.11 — Accept the integrated product and prepare Stage 2

**Status:** pending. Reserve real effort for stabilization and acceptance; do not
assume the final milestone is just rerunning an already-green command.

- Run the production-boundary combined scenarios for both backends: generate,
  open Mini/Full UI, change the cap, save/cancel, resize/replace the window, toggle
  FG, exercise declared fallback, and destroy with work outstanding.
- Verify exactly five exports, unchanged compatibility forwarding, preserved HUD,
  restored input, bounded backlog/retirement, and consistent requested/applied state.
  Record fixes and remaining limitations rather than hiding failing cases.
- Recheck analytical quality and production-resolution GPU/memory/wait costs.
  Separate validation-heavy runs from timing acceptance and state configuration
  differences from historical captures. Paper estimates are not measured budgets.
- Document the Stage 2 feature/model contract: tensor layout, phase/time conventions,
  color normalization, reset, dimensions/padding, provenance, scratch, and retirement.
  Keep neural execution before HUD; account for activation memory and all frame
  slots, not only model weight size. Training/inference delivery needs its own plan.
- With explicit deployment authorization, verify packaging/rollback with the game
  stopped. Any agent-driven game launch requires separate authorization. Perform
  the agreed focused game acceptance only when authorized; offline checks alone
  cannot prove display cadence, external-overlay compatibility, or game ABI coverage.

**Phase exit:** each selected backend meets the agreed integrated contract, known
blockers are resolved or explicitly scope-limited and accepted, and current docs,
tests, configuration, and diagnostic status tell the same story. Verified old paths
have been retired where replaced. A clean directory tree, isolated test success,
or a successful deployment alone does not complete Phase 2.
