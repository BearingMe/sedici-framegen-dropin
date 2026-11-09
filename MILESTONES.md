# Experiment milestones

## Phase overview

- **Phase 1:** establish the analytical generator and custom presentation path;
  retain the implementation history and evidence below. Component-level results
  are not proof that generation, overlay, limiter, and settings work together.
- **Phase 2:** audit, document, cover, and incrementally rebuild that integration
  around explicit ownership, coherent live
  settings, and useful diagnostics. The first gate is audit/documentation, the
  second is test coverage, and only then does structural migration begin.
- **Phase 3:** the full refactor. Finish the structure, error contract, configuration,
  test layout and defect backlog so Stage 2 has a stable base. Proposed 2026-11-09,
  starts after P2.11; see the Phase 3 section at the end.
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
the LUT-backed stage and presentation wrapper have offline coverage. The AMD backend
was later removed (tag `pre-amd-removal`); Sedici's own generator now answers every
call through the five exports. Later integration and its limitations are recorded in the
[P2.1 audit](docs/refactor/audit.md) and findings 008/009. Phase 1 acceptance is
not implied. Deployment and any agent-driven game launch require separate authorization.

### Current FFX implementation

| Area | Current state / reference |
| --- | --- |
| Five exports and FG ownership | `src/dllmain.cpp`, `src/fg_route.cpp`, Sedici answers every call (the forwarding path and `src/forward.cpp` were removed) |
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

**Status:** P2.1-P2.10 accepted (P2.3 and the AMD half of P2.9 superseded); P2.11 in progress.
The progress table below is the current state. The direction is recorded in
[docs/refactor](docs/refactor/README.md).

**Scope:** retain and validate the analytical implementation while migrating its
integration. The AMD backend and compatibility forwarding were removed by owner
decision (tag `pre-amd-removal`). Prepare
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
status, evidence or open items. Last updated: 2026-11-09, branch `refactor/phase-2`;
everything below is committed. Latest full CTest run: 37 of 37 passed on an idle GPU (with the owner's Pathtracer using the GPU at about 90%, sedici_fgcheck_present failed, K112) (offline GPU
suites, ABI scenarios on the real DLL with a tripwire stub "original", regression).

**Gate policy (owner decision, 2026-11-08):** P2.1-P2.10 are accepted on offline
evidence only. Every check that needs the game running, a display, or an external
overlay is collected in P2.11, the last gate, so nothing earlier waits on a game run.
Milestones list such items as "deferred to P2.11", not as blockers. Evidence on this
branch from the game: one user-run Alan Wake 2 session that asks for
`SwapChainNewDX12` and is refused before any frame (K100,
[finding 014](docs/findings/014-alan-wake-2-swapchain-new.md)). No frame generation
has run in the game since the AMD backend was removed.

| Milestone | Status | Evidence | Open items |
| --- | --- | --- | --- |
| P2.1 Audit and documentation | Complete | [audit](docs/refactor/audit.md) | None |
| P2.2 Regression and integration coverage | Complete, with accepted gaps | [finding 010](docs/findings/010-p22-coverage-gaps.md), [reliability](docs/refactor/reliability.md) | Offline gaps went to their milestones (P2.5 done, P2.6, P2.7). Deferred to P2.11: menu clicks and cursor path (K36), production-resolution performance |
| P2.3 AMD binary feasibility | Superseded by the owner decision to remove the AMD backend (tag `pre-amd-removal`) | [amd-binary](docs/refactor/amd-binary.md), [finding 011](docs/findings/011-p23-amd-binary-gaps.md), [NOTICE](NOTICE.md) | None; kept as reference |
| P2.4 Session, context and callback ownership | Complete (owner accepted 2026-11-08), with recorded gaps | [finding 012](docs/findings/012-p24-ownership.md), K29, K60, K63, K70, K102 | Accepted gaps: K64 (DLL unload/reload) moved to P2.10; allocation failures inside `Context::create` are not injected; post-transfer rollback, K104 and K105 go to P2.5; K103 documents the deliberate untracked-sink retention |
| P2.5 GPU submission, frame slots and retirement | Complete (owner accepted 2026-11-08), with recorded gaps | [finding 013](docs/findings/013-p25-gpu-retirement.md), K38, K104, K105, K41 swapchain half | Tested: injected signal/Close/Reset/presenter-submit failures, gated same-queue wrapper/session/registration recovery, swapchain retention timeout (`--retention-timeout`), destroy-wait lock release (K104). Accepted gaps: K37 cross-queue borrowed work and repeated signal failures; K77 consecutive-failure tests; K41 router parking/reaping test; K47 and K48 timeout retention is unbounded (watchdog moves to P2.10); K105 generator-history rollback not done (P2.7) |
| P2.6 Settings transactions and TOML migration | Complete (owner accepted 2026-11-09), with recorded gaps | Settings transaction and regressions (a54073d), 029ce7b: swap race restore, unknown-key diagnostics, legacy-key migration (`legacy-config`) | Recorded gaps: K83 stalled-I/O reporting and real proxy-DLL unload (P2.10); K43 residual (a write between swap and restore); integrated click-driven UI coverage (K36, P2.11). K56 and K91 are obsolete |
| P2.7 Real-frame admission and output pacing | Complete (owner accepted 2026-11-09), with recorded gaps | Schedule tests incl. 20 fps jitter (K95), limiter policy and live-rate reseed with controlled time (K108), `DO_NOT_WAIT` skip (K52), refused presenter job (K105), ratio boundary (K50); commit 3c3fc06 | Recorded gaps: K105 generator history re-advances after a refused job; K74 a failed slot wait consumes one limiter interval; `DO_NOT_WAIT` frames are not capped. Deferred to P2.11: measured display cadence against a baseline, the 20 fps / 2x behaviour in the game (K95), stalls (K98, K99) |
| P2.8 Composition, overlay and input integration | Complete (owner accepted 2026-11-09), with recorded gaps | `Stage1::synthesize` / `composite_hud` split, HUD preservation and reset-seed readback (K53), chained-callback error regression, `gpu-mini`/`gpu-full` with window replacement and an injected renderer failure in Full (K109), hotkey scenario; commit 995dc97 | Recorded gaps: composition stays a separate recording step inside `Stage1` (a standalone class waits for the Stage 2 handoff); K16 overlay counters include dropped outputs. Deferred to P2.11: intermittent hotkey timeout (K54), menu clicks and cursor path (K36), external-overlay compatibility |
| P2.9 Bundled AMD backend and packaging | Complete (offline gates met 2026-11-08); the AMD half is superseded by the owner decision to remove the backend | `sedici_deploy_tools` (22 checks of the deploy-safety rules on a fake game folder, 6dd31e0), K110 | Packaging is the proxy DLL, `sedici.toml` and the deploy tools. Recorded gaps: reparse-point refusal is untested (needs elevated rights); `restore.ps1` deletes `sedici.toml` by design (documented in the README); a real install with the game stopped is verified in P2.11 |
| P2.10 Errors, diagnostics and watchdog hardening | Complete (owner accepted 2026-11-09), with recorded gaps | (a) K47, K41, K48, K77, K37 resolved, K111, K113 fixed (65a30da, cf48198, 721546a). (b) K114, K115 fixed (c6ef9ab, aaa6327): bounded asynchronous log with rate limit, drop and sink-failure accounting, diag snapshot, call identity. (c) K64, K83 resolved, K116 fixed (446244d): the module pins itself once a handle is issued (`sedici_abi unload`), and a stalled settings worker is reported, cancelled best effort and its late result discarded (`sedici_regression`) | Recorded gaps: a GPU that never retires keeps its swapchain parked (option A); no background reaper thread; abandoned-prepare recovery not built; a game whose prepare queue differs from the wrap queue is untested; typed error codes and frame or settings identity are deferred to the upcoming refactor (the error contract in `docs/refactor/reliability.md`, owner decision); lines queued at a hard crash may be lost; a used DLL stays resident until process exit (behaviour change, README); a hung network share may ignore the settings cancel; in-game overhead of the observer and a real hung volume are checked in P2.11
| P2.11 Integrated acceptance and Stage 2 handoff | In progress: offline part done (2026-11-08); in-game checklist QA-01 to QA-16 run by the owner (QA-03 fails on K100; QA-04, QA-05, QA-12 partial or inconclusive; QA-14 skipped) | `sedici_abi_integrated` (35 checks), `sedici_regression observer` (overlay and log cost per frame, QA-12), the in-game QA record in [tests/qa](tests/qa/README.md), `sedici_fgcheck --resolution` quality and timing, [Stage 2 contract](docs/refactor/stage2-contract.md), K117, K118-K128 (QA findings), K129-K132 (Alan Wake 2, finding 015), [feature requests](docs/refactor/feature-requests.md) | Stage 1 measures 0.64-0.69 ms at 1080p against the 0.15-0.30 ms estimate (K117, open). The only gate that needs the game: see the in-game checklist in the P2.11 section |

**Bugs** (all in [known-issues.md](docs/refactor/known-issues.md), 135 entries): 48 open
(K1-K17, K19-K24, K32, K36, K54, K59, K98-K101, K112, K117-K132; K16, K23, K24, K36, K54, K101 and K112 are accepted gaps or not product defects, see the file header), 76 fixed or resolved (K25-K29, K33-K35, K37-K53, K55, K57, K58, K60, K61, K63-K86, K92, K93, K95-K97, K102-K111, K113-K116, K133-K135; hashes are in each entry), 10 obsolete with the AMD path (K18, K31, K56, K62, K87-K91, K94) and 1 retracted claim (K30). Decision waiting on the owner: K129 (accept the HDR flag or leave Alan Wake 2 without generation). K100 (Alan Wake 2's `SwapChainNewDX12` create is refused; [finding 014](docs/findings/014-alan-wake-2-swapchain-new.md)) and K101 (game-side `sedici.log` files mix several builds and games) were recorded on 2026-11-08 from a user-run AW2 session.

| Milestone | Size | Prerequisites |
| --- | --- | --- |
| P2.1 Audit and documentation | Large | Existing code, findings, refactor proposals |
| P2.2 Regression and integration coverage | Large | P2.1 contracts and evidence map |
| P2.3 AMD binary feasibility (superseded) | Large | P2.1, P2.2 test seams |
| P2.4 Session, context, and callback ownership | Very large | P2.2 |
| P2.5 GPU submission, frame slots, and retirement | Very large | P2.4 ownership boundary |
| P2.6 Settings transactions and TOML migration | Very large | P2.1 schema decisions, P2.4 owner |
| P2.7 Real-frame admission and output pacing | Very large | P2.5 retirement, P2.6 application path |
| P2.8 Composition, overlay, and input integration | Very large | P2.5-P2.7 contracts |
| P2.9 Bundled AMD backend and packaging (superseded) | Very large | None |
| P2.10 Errors, diagnostics, and watchdog hardening | Large; cross-cutting | Contracts begin in P2.1; infrastructure begins in P2.2-P2.4 |
| P2.11 Integrated acceptance and Stage 2 handoff | Large | P2.4-P2.8 and P2.10 accepted offline; owner game runs |

The numbering is a dependency guide, not a reason to postpone all error handling
until P2.10. Instrument and test each migrated slice as it lands. No production
migration may bypass the coverage gate.

For each implementation slice, record the responsibility changing owner, behavior
preserved, intentional fixes, acceptance evidence, and a safe rollback. Remove the
superseded path after acceptance; do not keep two authoritative settings stores,
registries, or lifecycle managers. Preserve existing user work and verified
algorithms. Separate mechanical moves from behavior changes where practical.

### P2.1 — Audit and documentation

**Status:** audit and documentation prepared for review; see the
[current-state evidence and P2.2 handoff](docs/refactor/audit.md). Acceptance awaits
review. This milestone does not move or fix code.

- Trace the backend paths (AMD path since removed) from the five exports through contexts, callbacks,
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

**Status:** complete, with explicitly accepted gaps (P2.2 baseline: 18 CTest entries; 34 now). 18 CTest entries passed at the time (12 `abi`
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

**Status:** complete, owner accepted 2026-11-08, with recorded gaps (evidence:
[finding 012](docs/findings/012-p24-ownership.md)). K64 moved to P2.10; K103 is a
deliberate design choice.

- Introduce one logical session relating FG contexts, swapchain contexts, backend
  identity, game-facing buffers, and effective status. Keep ABI dispatch thin.
- Provide lifetime-safe registry access and explicit callback registration,
  reentrancy, detach, and destruction contracts. Do not hold registry locks across
  backend calls, external callbacks, or GPU waits.
- Represent probing, history warmup, active/degraded/suspended operation, draining,
  and stopped/device-lost behavior. Define accepted work and observable results at
  each transition, including partially initialized sessions.
- Publish coherent FG/status identity. Reject handles Sedici did not issue.

**Acceptance:** production-path tests cover separate context creation order,
failed/partial initialization, FG toggles, callback detachment, and destruction.
No dangling registry lookup or callback may reach a destroyed owner. The old
authoritative lifetime/status bookkeeping is removed after its replacement passes.

### P2.5 — Make GPU submission and retirement explicit

**Status:** complete, owner accepted 2026-11-08, with recorded gaps
([finding 013](docs/findings/013-p25-gpu-retirement.md)). Accepted gaps: K37 cross-queue,
K77, K41 router parking (all to P2.10), unbounded timeout retention K47/K48 (watchdog,
P2.10), K105 generator-history rollback (P2.7).

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

**Status:** complete on offline evidence, 2026-11-08, with recorded gaps (K83 to P2.10, click-driven UI to P2.11). The transaction, worker I/O, external-edit detection and restore, environment priority, legacy-key migration and unknown-key diagnostics are committed and tested. Parsing, live application, UI acknowledgement, and persistence
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
  and environment switches. The removed keys (`fg.mode`, `[pacing]`, `[async]`) are
  accepted and ignored with one log line; do not remove opt-outs/defaults silently.

**Acceptance:** parser, runtime, UI, and persisted values agree under successful,
rejected, concurrent, and failed-save changes. Unknown/deprecated keys get actionable
diagnostics. Existing configurations have a documented migration, not silent drift.
The proposed schema in docs/refactor is not enabled until this gate is satisfied.

### P2.7 — Coordinate the real-frame cap and output pacing

**Status:** complete on offline evidence, 2026-11-08, with recorded gaps (see the progress table).
Policy extraction, backpressure and controlled-time tests are committed; production-path
cadence validation is deferred to P2.11. This is feedback/timing work, not simply
reconnecting the limiter callback.

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

**Acceptance (offline):** deterministic closed-loop tests and the integrated host
cover cap changes, FG off/on, overload, stalls, sync policy, and supported output
ratios. The modelled output rate must not collapse through self-amplifying queue
latency. Measured display cadence and CPU/GPU wait costs against a
configuration-matched baseline are deferred to P2.11.

### P2.8 — Separate composition and integrate overlay/input

**Status:** initial HUD-pass extraction in progress; acceptance pending. Use distinct sub-gates for HUD-less generation, output rendering,
and window/input ownership; rendering a demo overlay does not complete this work.

- Move final HUD composition out of the analytical stage. Preserve an explicit
  HUD-less boundary for residual correction before HUD and Sedici overlay drawing.
- Render UI on final real/generated outputs. Preserve caller callbacks and game
  UI; do not return success while dropping required composition.
- Give ImGui rendering resources explicit queue/descriptor retirement and thread
  ownership. UI reads status snapshots and submits settings requests only.
- Preserve hotkey behavior, cursor/clipping ownership, input restoration, coordinate
  mapping, window replacement, and overlay-disable recovery independently of a
  functioning GPU renderer. Remove monitor-specific control assumptions.

**Acceptance:** the combined scenario validates Mini/Full operation, HUD preservation,
cap/settings changes, resize/window replacement, FG toggles, and renderer failure
with input returned to the host window, in the offline host. A direct `ui_on_present`
unit call alone is insufficient. Game-side checks are deferred to P2.11.

### P2.9 — Integrate and package the pinned AMD backend

> **Superseded in part.** There is no AMD backend to integrate or package, so the text
> below is history. What remains is packaging the proxy DLL, `sedici.toml` and the deploy
> tools; that is complete on offline evidence (`sedici_deploy_tools`, K110).

**Status:** complete on offline evidence, 2026-11-08 (packaging only; the AMD backend work
below was never done and is superseded).

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

**Status:** complete on offline evidence, 2026-11-08, with recorded gaps, owner accepted 2026-11-09. Split into three sub-gates by the owner: (a) watchdog
and bounded retention (met offline 2026-11-08: K47, K41, K48, K77, K37), (b) diagnostics
and metrics (met offline 2026-11-08: K114, K115), (c) DLL unload and stalled settings I/O (met offline 2026-11-08: K64, K83, K116). Decision: when a GPU
never retires, the swapchain stays parked (option A). Cross-cutting. Establish error vocabulary and event identity
early, emit evidence in every migrated slice, then complete recovery/overhead checks.
Carries the items accepted as gaps in P2.4 and P2.5: the timeout-retention watchdog
(K47, K48), DLL unload/reload (K64), K37 cross-queue work, K77 consecutive failures
and the K41 router parking test. All offline.

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

**Status:** offline part done 2026-11-08 (integrated scenario `sedici_abi_integrated`; production-size quality and timing with `sedici_fgcheck --resolution WxH [--no-debug-layer]`; [Stage 2 contract](docs/refactor/stage2-contract.md); new finding K117). In-game checklist pending the owner. This is the only gate that needs the game, a display or an
external overlay. Everything in P2.1-P2.10 is accepted on offline evidence; every
check that needed a game run has been moved here so no earlier milestone is blocked.
Reserve real effort for stabilization and acceptance; do not assume this milestone
is just rerunning an already-green command.

**Owner decision 2026-11-08:** all AI integration (Stage 2) is postponed until the current system is fully refactored and stable. The Stage 2 contract below stays a draft and no Stage 2 code, weights or training start before then.

**Offline part (agent can run):**

- Run the production-boundary combined scenarios in the offline host: generate,
  open Mini/Full UI, change the cap, save/cancel, resize/replace the window, toggle
  FG, exercise declared failure paths, and destroy with work outstanding.
- Verify exactly five exports, drop-in return codes, preserved HUD, restored input,
  bounded backlog/retirement, and consistent requested/applied state. Record fixes
  and remaining limitations rather than hiding failing cases.
- Recheck analytical quality and production-resolution GPU/memory/wait costs.
  Separate validation-heavy runs from timing acceptance. Paper estimates are not
  measured budgets.
- Document the Stage 2 feature/model contract: tensor layout, phase/time conventions,
  color normalization, reset, dimensions/padding, provenance, scratch, and retirement.
  Keep neural execution before HUD; account for activation memory and all frame
  slots, not only model weight size. Training/inference delivery needs its own plan.

**In-game checklist (owner runs; deployment and any launch need explicit authorization,
and an agent never launches the game):**

| Check | Deferred from | Evidence needed |
| --- | --- | --- |
| Cyberpunk 2077: generation runs, no crash, `sedici.log` clean | all | log segment for the current build only (K101) |
| Alan Wake 2: `SwapChainNewDX12` create is accepted and frames present | K100, [finding 014](docs/findings/014-alan-wake-2-swapchain-new.md), [finding 015](docs/findings/015-alan-wake-2-first-sedici-session.md), K129-K132 | log plus a short capture |
| Display cadence: steady output at the target ratio, no 20 fps / 2x collapse | P2.7, K95 | present-interval capture, configuration-matched baseline |
| Stalls of several seconds (slot rejection, K98) and K99 | P2.7 | log segment and frame-time capture |
| Intermittent hotkey timeout and key-up (K54) | P2.8 | repeated hotkey runs |
| Menu clicks and the Full-menu cursor path (K36) | P2.2, P2.8 | foreground-window run |
| External-overlay compatibility (RTSS and similar), VRR presenter readback | P2.7, P2.8 | runs with and without the overlay |
| Packaging and rollback with the game stopped | P2.9 | deploy then restore, `.orig` untouched |

**Phase exit:** the selected backend meets the integrated contract, known blockers
are resolved or explicitly scope-limited and accepted, and current docs, tests,
configuration, and diagnostic status tell the same story. Verified old paths have
been retired where replaced. A clean directory tree, isolated test success, or a
successful deployment alone does not complete Phase 2.

## Phase 3 — Full refactor: structure, contracts and a closed backlog

**Goal:** leave the code base in the state Stage 2 can be built on. Phase 2 made the
runtime reliable by hardening the existing flat modules in place. Phase 3 finishes the
job: the module structure matches the ownership rules, the error and diagnostic
contracts are real, the open defects are closed or knowingly accepted, the tests are
organised by domain, and the documentation has one owner per fact.

**Status:** proposed, awaiting owner approval (2026-11-09). Nothing in Phase 3 has
started. It begins when P2.11 closes, because the in-game QA record
([tests/qa](tests/qa/README.md)) is the behavioural baseline that every structural step
must keep intact.

**Why a full refactor, from the Phase 2 evidence:**

- `src/` is flat and three files carry most of the weight: `fg_swapchain.cpp` (1,173
  lines), `ui.cpp` (1,128) and `config.cpp` (958). The tests are three monoliths:
  `fgcheck.cpp` (3,333), `abi.cpp` (1,470) and `regression.cpp` (1,333). The target tree in
  [file-structure.md](docs/refactor/file-structure.md) was never adopted.
- 48 defects are open ([known-issues.md](docs/refactor/known-issues.md)). Many are
  accepted gaps, but the in-game ones (K118-K128) and the Alan Wake 2 set (K100, K129-K132,
  K14) change what the product does.
- Two contracts are still targets: typed error codes with call, frame and settings
  identity (deferred in P2.10), and a watchdog (not built).
- Hidden environment switches remain (K20), and the design docs were found to describe a
  schema and a tree that were never shipped (docs audit, 2026-11-09).
- Stage 1 costs 0.64-0.69 ms against a 0.15-0.30 ms estimate (K117), and there is no
  per-pass timing to say where it goes.

**Scope:** behaviour-preserving restructuring, plus the product fixes the QA run asked
for, plus the Stage 2 seams. **Not in scope:** training or running the residual network,
any AMD backend, the loader or upscaler DLLs, more than the five exports, or anything that
needs the game to be launched by the agent (the owner runs every in-game check).

### Rules for every Phase 3 step

- **Behaviour first, structure second.** A commit either moves or restructures code with
  no behaviour change, or changes behaviour with a test and a K entry. Never both.
- **Pin before moving.** Before a module is split, the checks that cover it must pass and
  must still pass unchanged afterwards (`ctest` 37 of 37 on an idle GPU, the ABI scenarios
  on the real DLL, `sedici_fgcheck` quality numbers).
- **Small, reviewable commits**, each building and passing `ctest`. No big-bang move.
- The AGENTS.md hard rules apply: five exports, no game launch, third-party and loader or
  upscaler files untouched, every bug gets a K entry, the progress table stays current.

### Phase 3 progress

**Keep this table current**, in the same commit as any change to a milestone. Last
updated: 2026-11-09.

| Milestone | Size | Status | Prerequisites |
| --- | --- | --- | --- |
| P3.1 Baseline freeze | Large | Proposed | P2.11 closed |
| P3.2 In-game defect fixes | Large | Proposed | P3.1 |
| P3.3 Backlog triage | Large | Proposed | P3.1 |
| P3.4 Module structure | Very large | Proposed | P3.1, P3.2 |
| P3.5 Error contract and diagnostics | Very large | Proposed | P3.4 |
| P3.6 Configuration surface | Large | Proposed | P3.4 |
| P3.7 Test suite reorganisation | Large | Proposed | P3.1; runs alongside P3.4 |
| P3.8 Format and game compatibility | Very large | Proposed, owner decision first | P3.4, P3.5 |
| P3.9 Stage 2 seams and cost | Large | Proposed | P3.4, P3.7 |
| P3.10 Documentation and release | Large | Proposed | all |

### P3.1 — Freeze the baseline

Pin what must not change. Tag the last Phase 2 commit. Record the performance baseline:
per-pass GPU timestamps for Stage 1 (K117), prepare and present cost, overlay cost. Turn
the QA findings that can be automated into offline checks (the observer cost mode of
QA-12 is the first). Capture the owner-side gaps still open from P2.11 (QA-04/05
present-interval captures, a K119 repro) or record them as accepted.

**Exit:** a tagged baseline; `ctest` 37 of 37; a recorded timing table; the QA record
lists which results are baseline and which are accepted gaps.

### P3.2 — Fix what the owner found in the game

Behaviour changes, each with a test and a K entry, before any structural move so the
fixes are not hidden inside a refactor.

- K126 Alt+F4 passes through the Full menu; K127 destroy at process exit waits and
  retires; K118/FR-01/FR-02 slider in clamped 0.5 steps with an Apply button;
  K125 truthful Real fps labelling; FR-04 menu scales 0.8/0.9/1.0; FR-05 warning when
  real fps times the multiplier exceeds the refresh.
- K128 loading-screen generation and K98/K121/K122/K123 stalls: investigate, then fix or
  accept with the log evidence.

**Exit:** each item fixed with its commit, or accepted with a reason in its K entry; the
owner re-runs the matching QA tests (QA-08, QA-13, QA-16) and signs them off.

### P3.3 — Close the backlog

Walk every open K entry (48 at the time of writing) and give each an outcome: fixed,
accepted with a reason, or obsolete. Merge true duplicates by cross-reference (entries
are never deleted). Resolve the ones that block structure work first (K11, K12, K14,
K19, K20, K22). Entries that are not product defects (K16, K23, K24, K36, K54, K101,
K112) get a standing "accepted" note.

**Exit:** no open entry without an owner, a reason or a milestone; the MILESTONES bug
counts match the file.

### P3.4 — Module structure

Replace the flat `src/` with directories chosen from the code as it is, not from the
Phase 2 proposal. Start from the existing ownership rules in
[architecture.md](docs/refactor/architecture.md) and the current file owners
(ABI, routing and sessions, presentation, UI, settings, diagnostics, GPU stages).

- Split `fg_swapchain.cpp` along its real seams (wrap and resize, slot and retirement,
  present admission, recording), `ui.cpp` (input, Mini, Full, settings client) and
  `config.cpp` (parse, validate, report).
- One library target per module with an explicit dependency direction; no module reaches
  into another's private state. Public headers stay minimal.
- Rewrite [file-structure.md](docs/refactor/file-structure.md) to describe the tree that
  exists, and update the AGENTS.md layout in the same commit as each move.

**Exit:** each module builds as its own target; the dependency graph has no cycles; the
exports, the ABI scenarios and `sedici_fgcheck` results are unchanged.

### P3.5 — Error contract and diagnostics

Build the targets that Phase 2 deferred.

- Typed internal error codes mapped to the documented FFX return codes in one place, with
  call, frame and settings identity in every rejection.
- A structured event vocabulary for the log (the P2.10 log keeps its bounded queue).
- Decide the watchdog: build the minimal monitor (stuck prepare, stuck retirement, stuck
  settings I/O), or record that parking and bounded waits are the final design.
- Close the P2.10 recorded gaps that remain (abandoned-prepare recovery, background reaper).

**Exit:** the error table in the README is generated from the same table the code uses;
each recorded P2.10 gap is closed or explicitly accepted.

### P3.6 — Configuration surface

One schema, one owner. Remove or document the hidden environment switches (K20); version
the schema; keep the migration for old keys. Make `sedici.toml`, `config.cpp`, the README
key table and [configuration.md](docs/refactor/configuration.md) agree, preferably with a
test that fails when a key exists in code but not in the shipped file or the README.

**Exit:** a drift test passes; no environment-only switch remains undocumented.

### P3.7 — Test suite reorganisation

Split the three monoliths by domain behind a shared harness (fakes for the device, queue
and clock; the stub original is gone). Tag timing-sensitive checks so a busy GPU is
reported as such (K112) instead of as a failure. Add address and undefined-behaviour
checks for the host-side objects where the toolchain allows it. Keep the quality numbers
(PSNR/SSIM) as pinned expectations.

**Exit:** no test file over about 1,000 lines; `ctest` labels (`fast`, `gpu`, `timing`);
the same checks as before still run and pass.

### P3.8 — Format and game compatibility

Owner decision first, then implementation. The items change what the product accepts:
10-bit back buffers (K14), the HDR create flag (K129), the `SwapChainNewDX12` route
(K100), the device-object rule (K130), the interpolation queries (K131) and the Full
menu cursor interception (K132). The worktree `aw2-patch` ([finding
015](docs/findings/015-alan-wake-2-first-sedici-session.md)) is a starting point, not
code to merge as it stands. Each accepted item gets offline tests; the owner runs the
game.

**Exit:** for each item, a recorded decision (support, or documented refusal with its
return code) and, where supported, an owner-run QA entry.

### P3.9 — Stage 2 seams and cost

Prepare the boundary without building the network. Implement the pieces the
[Stage 2 contract](docs/refactor/stage2-contract.md) marks as seams: the focus-mask
target, scratch ownership and retirement for a second pass, and per-pass timing in the
log. Decide the Stage 1 cost (K117): optimise it, or revise the budget in the design
note with the measurement.

**Exit:** the contract's seams exist and are tested with a stub second pass; the
budget in `references/papers/realtime-fg.md` and AGENTS.md matches a measurement.

### P3.10 — Documentation and release

Make the documents agree: one owner per fact (README for users, `docs/refactor` for
contracts, MILESTONES for status, `known-issues.md` for defects, `tests/qa` for evidence),
historical findings bannered, and every K reference checked. Re-run the full QA checklist
(QA-01 to QA-16) on the final build, including a second game if the owner chooses.

**Exit:** a docs link and reference check passes; QA-01 to QA-16 re-run by the owner with
no new regression; the licence and provenance files are current; a release note lists the
behaviour changes since the Phase 1 baseline.

### Phase 3 exit

The module tree, the error and diagnostic contracts, the configuration schema and the
test layout are what the documents say they are. No open defect lacks an owner or a
reason. The Stage 1 cost is measured and the budget reflects it. The owner has re-run
the in-game checklist on the final build. Only then does the Stage 2 work, postponed on
2026-11-08, start.

### Decisions needed from the owner

1. Start Phase 3 after P2.11 closes, or earlier on the parts that do not need the game
   (P3.3, P3.7)?
2. P3.8: support HDR and 10-bit and the Alan Wake 2 create path, or document the refusal?
3. P3.5: build the watchdog, or declare parking and bounded waits the final design?
4. P3.4: is a directory tree the goal, or is splitting the three large files enough?
5. K117: optimise Stage 1, or accept the cost and revise the budget?
