# Experiment milestones

## Phase overview

- **Phase 1:** establish the analytical generator and custom presentation path;
  retain the implementation history and evidence below. Component-level results
  are not proof that generation, overlay, limiter, and settings work together.
- **Phase 2:** audit, document, cover, and incrementally rebuild that integration
  around explicit ownership, coherent live
  settings, and useful diagnostics. The first gate is audit/documentation, the
  second is test coverage, and only then does structural migration begin.
- **Phase 3:** organise the code into the module layout and prove against the Phase 2
  baseline that nothing changed; document it in the commits and in `docs/dev/`. No
  behaviour change. Complete 2026-11-09; see the Phase 3 section.
- **Phase 4:** cover all open issues and improvements, on the Phase 3 layout. In progress:
  the offline work is done (P4.1 to P4.5 complete, offline parts of P4.6 to P4.9); the
  game verdicts wait for the owner's runs. See the Phase 4 section at the end. The open
  issues themselves are tracked on GitHub, not in this file.
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

**Status:** P2.1-P2.10 accepted (P2.3 and the AMD half of P2.9 superseded); P2.11 closed 2026-11-09 with accepted gaps.
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
status, evidence or open items. Last updated: 2026-11-12, branch `BearingMe/k1-adaptive-pacing-at-32-real-fps-55-60-ms-laten`
(issue #1, K1: QA-18 steps 1-3 measured in the game, bugs #35 to #38 opened, presenter trace added). Before that: 2026-11-11,
branch `docs/issues-cutover` (open bugs moved to GitHub issues #1 to #32). Latest full CTest run: 41 of 41 passed (Release, 2026-11-10, after the tooling merge; under heavy GPU load sedici_fgcheck_present can still fail, issue #20, was K112) (offline GPU
suites, ABI scenarios on the real DLL with a tripwire stub "original", regression).

**Gate policy (owner decision, 2026-11-08):** P2.1-P2.10 are accepted on offline
evidence only. Every check that needs the game running, a display, or an external
overlay is collected in P2.11, the last gate, so nothing earlier waits on a game run.
Milestones list such items as "deferred to P2.11", not as blockers. Evidence on this
branch from the game: the owner's QA runs (`tests/qa/`, QA-01 to QA-20) and the Alan Wake 2
sessions in [finding 014](docs/findings/014-alan-wake-2-swapchain-new.md) to
[016](docs/findings/016-alan-wake-2-black-screen.md): the create is accepted and generation
starts, but the scene stays black (K138 fixed, K139 open as an issue).

| Milestone | Status | Evidence | Open items |
| --- | --- | --- | --- |
| P2.1 Audit and documentation | Complete | [audit](docs/refactor/audit.md) | None |
| P2.2 Regression and integration coverage | Complete, with accepted gaps | [finding 010](docs/findings/010-p22-coverage-gaps.md), [reliability](docs/refactor/reliability.md) | Offline gaps went to their milestones (P2.5 done, P2.6, P2.7). Deferred to P2.11: menu clicks and cursor path (K36), production-resolution performance |
| P2.3 AMD binary feasibility | Superseded by the owner decision to remove the AMD backend (tag `pre-amd-removal`) | [amd-binary](docs/refactor/amd-binary.md), [finding 011](docs/findings/011-p23-amd-binary-gaps.md), [NOTICE](NOTICE.md) | None; kept as reference |
| P2.4 Session, context and callback ownership | Complete (owner accepted 2026-11-08), with recorded gaps | [finding 012](docs/findings/012-p24-ownership.md), K29, K60, K63, K70, K102 | Accepted gaps: K64 (DLL unload/reload) moved to P2.10; allocation failures inside `Context::create` are not injected; post-transfer rollback, K104 and K105 go to P2.5; K103 documents the deliberate untracked-sink retention |
| P2.5 GPU submission, frame slots and retirement | Complete (owner accepted 2026-11-08), with recorded gaps | [finding 013](docs/findings/013-p25-gpu-retirement.md), K38, K104, K105, K41 swapchain half | Tested: injected signal/Close/Reset/presenter-submit failures, gated same-queue wrapper/session/registration recovery, swapchain retention timeout (`--retention-timeout`), destroy-wait lock release (K104). Accepted gaps: K37 cross-queue borrowed work and repeated signal failures; K77 consecutive-failure tests; K41 router parking/reaping test; K47 and K48 timeout retention is unbounded (watchdog moves to P2.10); K105 generator-history rollback not done (P2.7) |
| P2.6 Settings transactions and TOML migration | Complete (owner accepted 2026-11-09), with recorded gaps | Settings transaction and regressions (a54073d), 029ce7b: swap race restore, unknown-key diagnostics, legacy-key migration (`legacy-config`) | Recorded gaps: K83 stalled-I/O reporting and real proxy-DLL unload (P2.10); K43 residual (a write between swap and restore); integrated click-driven UI coverage (K36, P2.11). K56 and K91 are obsolete |
| P2.7 Real-frame admission and output pacing | Complete (owner accepted 2026-11-09), with recorded gaps | Schedule tests incl. 20 fps jitter (K95), limiter policy and live-rate reseed with controlled time (K108), `DO_NOT_WAIT` skip (K52), refused presenter job (K105), ratio boundary (K50); commit 3c3fc06 | Recorded gaps: K105 generator history re-advances after a refused job; K74 a failed slot wait consumes one limiter interval; `DO_NOT_WAIT` frames are not capped. Deferred to P2.11: measured display cadence against a baseline, the 20 fps / 2x behaviour in the game (K95), stalls (K98, K99). Issue #1 (K1, adaptive at 32 real fps) in progress: QA-18 step 3 on 2026-11-12 measured ~66 of 73 fps shown, `late` ~99%; #35 (schedule lost grid points: 70.4 instead of 73 planned) fixed, 72.9 planned offline; #37 (late attribution, level-2 trace) fixed; #36 (latency estimate) and #38 open. The offline presenter loop reproduces the late/drop cascade only with more than ~0.4 ms per-flip overhead at 73 on 75 Hz; the level-2 trace from the next QA-18 step 3 run decides the remaining fix |
| P2.8 Composition, overlay and input integration | Complete (owner accepted 2026-11-09), with recorded gaps | `Stage1::synthesize` / `composite_hud` split, HUD preservation and reset-seed readback (K53), chained-callback error regression, `gpu-mini`/`gpu-full` with window replacement and an injected renderer failure in Full (K109), hotkey scenario; commit 995dc97 | Recorded gaps: composition stays a separate recording step inside `Stage1` (a standalone class waits for the Stage 2 handoff); K16 overlay counters include dropped outputs. Deferred to P2.11: intermittent hotkey timeout (K54), menu clicks and cursor path (K36), external-overlay compatibility |
| P2.9 Bundled AMD backend and packaging | Complete (offline gates met 2026-11-08); the AMD half is superseded by the owner decision to remove the backend | `sedici_deploy_tools` (22 checks of the deploy-safety rules on a fake game folder, 6dd31e0), K110 | Packaging is the proxy DLL, `sedici.toml` and the deploy tools. Recorded gaps: reparse-point refusal is untested (needs elevated rights); `restore.ps1` deletes `sedici.toml` by design (documented in the README); a real install with the game stopped is verified in P2.11 |
| P2.10 Errors, diagnostics and watchdog hardening | Complete (owner accepted 2026-11-09), with recorded gaps | (a) K47, K41, K48, K77, K37 resolved, K111, K113 fixed (65a30da, cf48198, 721546a). (b) K114, K115 fixed (c6ef9ab, aaa6327): bounded asynchronous log with rate limit, drop and sink-failure accounting, diag snapshot, call identity. (c) K64, K83 resolved, K116 fixed (446244d): the module pins itself once a handle is issued (`sedici_abi unload`), and a stalled settings worker is reported, cancelled best effort and its late result discarded (`sedici_regression`) | Recorded gaps: a GPU that never retires keeps its swapchain parked (option A); no background reaper thread; abandoned-prepare recovery not built; a game whose prepare queue differs from the wrap queue is untested; typed error codes and frame or settings identity are deferred to the upcoming refactor (the error contract in `docs/refactor/reliability.md`, owner decision); lines queued at a hard crash may be lost; a used DLL stays resident until process exit (behaviour change, README); a hung network share may ignore the settings cancel; in-game overhead of the observer and a real hung volume are checked in P2.11
| P2.11 Integrated acceptance and Stage 2 handoff | Complete (owner closed 2026-11-09), with accepted gaps: offline part done (2026-11-08); in-game checklist QA-01 to QA-16 run by the owner (QA-03 fails on K100; QA-04 and QA-05 partial; QA-12 closed offline; QA-14 skipped) | `sedici_abi_integrated` (35 checks), `sedici_regression observer` (overlay and log cost per frame, QA-12), the in-game QA record in [tests/qa](tests/qa/README.md), `sedici_fgcheck --resolution` quality and timing, [Stage 2 contract](docs/refactor/stage2-contract.md), K117, K118-K128 (QA findings), K129-K132 (Alan Wake 2, finding 015), [feature requests](docs/refactor/feature-requests.md) | Accepted gaps, carried to Phase 4: QA-03 (Alan Wake 2, K100, K129-K132), the QA-04 and QA-05 present-interval captures, QA-14, and Stage 1 at 0.64-0.69 ms against the 0.15-0.30 ms estimate (K117). The in-game defects K118-K128 are open. The owner considers the behaviours proven, which is the baseline for Phase 3 |

**Bugs and requests: GitHub is the source of truth.** Open items are the
[GitHub issues](https://github.com/BearingMe/sedici-framegen-dropin/issues) (open bugs: label `bug`; in-game work: `needs-game`;
kept for the record: `not-a-defect`). The 32 that were open on 2026-11-11 are #1 to #32, titled with their old K number.
Since then (2026-11-12, issue #1 analysis): #35 to #38 opened; #35 and #37 closed. Bug issues now: 34 open, 2 closed. The
retired K1-K147 and FR-01 to FR-05 lists are in [docs/archive](docs/archive/known-issues.md) and are not updated. Counts as of
that date, from the archive: 32 open
(K1, K2, K4, K5, K7-K11, K13, K15, K16, K20, K23, K24, K36, K54, K98, K101, K112, K119-K124, K127, K128, K130, K137, K139, K147; K16, K23, K24, K36, K54, K101 and K112 are accepted gaps or not product defects, see the file header), 102 fixed or resolved (K144, K143, K145, K146, K141, K142, K140, K138, K6, K14, K100, K129, K131, K132, K3, K12, K17, K117, K19, K22, K59, K99, K25-K29, K33-K35, K37-K53, K55, K57, K58, K60, K61, K63-K86, K92, K93, K95-K97, K102-K111, K113-K116, K118, K125, K126, K133-K136; hashes are in each entry), 12 obsolete with the AMD path (K18, K21, K31, K32, K56, K62, K87-K91, K94) and 1 retracted claim (K30). Decision waiting on the owner: K129 (accept the HDR flag or leave Alan Wake 2 without generation). K100 (Alan Wake 2's `SwapChainNewDX12` create is refused; [finding 014](docs/findings/014-alan-wake-2-swapchain-new.md)) and K101 (game-side `sedici.log` files mix several builds and games) were recorded on 2026-11-08 from a user-run AW2 session.

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

**Status:** offline part done 2026-11-08 (integrated scenario `sedici_abi_integrated`; production-size quality and timing with `sedici_fgcheck --resolution WxH [--no-debug-layer]`; [Stage 2 contract](docs/refactor/stage2-contract.md); new finding K117). **Closed by the owner on 2026-11-09** with the accepted gaps listed in the progress table. This is the only gate that needs the game, a display or an
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

## Phase 3 — Organise the code and prove nothing changed

**Goal:** put the code where the ownership rules say it belongs, prove against the Phase 2
baseline that nothing changed, and document the result twice: in the commits, for whoever
reads the history, and in `docs/dev/`, for agents and contributors who work in the tree.

**Status:** complete (2026-11-09). The owner set the scope: it started after P2.11 closed and
**does not change behaviour**. P3.1 to P3.6 are done; the tree matches `file-structure.md`
version 2 and the baseline comparison is identical to the `phase2-baseline` tag (see
`docs/dev/baseline.md`).

**Scope (three things only):**

1. **Organise.** Move the files into the module layout in
   [file-structure.md](docs/refactor/file-structure.md) (version 2, derived from the
   Phase 2 proposal and applied to the code as it is).
2. **Test against the baseline.** The Phase 2 test suite is the baseline. After every
   slice it must give the same results as before the move.
3. **Document both.** The commit history carries the what and why of every move. A new
   `docs/dev/` guide carries the layout, the workflow and the move ledger for agents.

**Out of scope, by the owner's decision:** any behaviour change, any bug fix, new
features, game or format support (10-bit, HDR, Alan Wake 2), the error-contract work, the
watchdog, the configuration schema, Stage 2 seams, splitting or rewriting large files, and
renaming symbols, or renaming files other than dropping the `fg_` prefix. Those stay in the backlog (at the time [known-issues.md](docs/archive/known-issues.md),
[feature-requests.md](docs/archive/feature-requests.md); now GitHub issues) and are covered by Phase 4. The
AGENTS.md hard rules still apply: five exports, no game launch, `third_party/` and the loader
and upscaler DLLs untouched.

### What "the baseline" is

Taken at the commit that closes P2.11, tagged `phase2-baseline`, and recorded in
`docs/dev/baseline.md`:

- `ctest -C Release` result per test name (37 of 37 on an idle GPU; the names are the
  contract, so a renamed or dropped test counts as a failure).
- `dumpbin /exports` of the DLL: exactly the five exports, in order.
- The quality and timing lines printed by `sedici_fgcheck` (PSNR, SSIM, cost) and the
  `sedici_regression observer` numbers, as recorded values with the tolerance for noise
  stated beside them.
- The set of source files, their `#include` lines excluded, hashed per file. This is the
  "nothing changed" proof: after a move, every moved file (matched through the old-to-new
  name ledger) must hash the same once its `#include` lines are removed.

The in-game QA record ([tests/qa](tests/qa/README.md)) is the behavioural reference. The owner
considers the behaviours proven (2026-11-09), so Phase 3 needs no new in-game runs.

### Rules for every slice

- One module per slice, one slice per commit group, each commit building and passing `ctest`.
- Move with `git mv` so history follows. Drop the `fg_` prefix from file names (the folder
  replaces it) and change nothing else except `#include` lines and the source lists in
  `CMakeLists.txt`. If a move needs any other edit, stop and record why.
- Run the baseline comparison before and after the slice. A difference is not "fixed in
  passing"; the slice is reverted or the difference is recorded as a finding for the next phase.
- Existing cycles between modules are recorded in `docs/dev/layout.md`, not removed.
- Update the AGENTS.md layout section in the same commit as the move it describes.

### Commit convention (documentation in the history)

Every Phase 3 commit is a Conventional Commit of type `refactor` (or `docs`/`test`), with a
scope naming the module, and a body that a reader can use without opening the diff:

```text
refactor(presentation): move the swapchain, presenter, pacing and limiter files

Moved (git mv, fg_ prefix dropped):
  src/fg_swapchain.{h,cpp}  -> src/presentation/swapchain.{h,cpp}
  ...
Changed: #include lines and the source lists in CMakeLists.txt only.
Baseline: ctest 37/37, exports unchanged, fgcheck quality lines identical,
          non-include file hashes identical (tools/check_moves.ps1).
No behaviour change.

Co-Authored-By: ...
```

### Phase 3 progress

**Keep this table current**, in the same commit as any change to a milestone. Last
updated: 2026-11-09.

| Milestone | Size | Status | Prerequisites |
| --- | --- | --- | --- |
| P3.1 Baseline and move checker | Large | Complete (2026-11-09) | P2.11 closed (done) |
| P3.2 Leaf modules: diagnostics, settings, gpu | Large | Complete (2026-11-09) | P3.1 |
| P3.3 Core modules: generator, runtime, abi | Large | Complete (2026-11-09) | P3.2 |
| P3.4 Presentation and UI | Large | Complete (2026-11-09) | P3.3 |
| P3.5 Tests, `docs/dev` and AGENTS layout | Large | Complete (2026-11-09) | P3.1; finishes after P3.4 |
| P3.6 Final proof | Large | Complete (2026-11-09) | P3.4, P3.5 |

### P3.1 — Baseline and move checker

Create the tag `phase2-baseline`. Write `docs/dev/baseline.md` with the recorded values
above and the exact commands to reproduce them. Add a small script, `tools/check_moves.ps1`,
that compares the working tree with the baseline tag and fails unless every difference is a
rename, an `#include` line, a source list entry in `CMakeLists.txt`, or a documentation file.
Add the `src/` include path and prove the build still produces the same five exports before
any file moves.

**Exit:** the tag exists; the baseline document reproduces on a clean checkout; the checker
passes on the baseline commit and fails on a deliberately changed line.

### P3.2 — Leaf modules: diagnostics, settings, gpu

Move the modules everything else depends on and nothing here depends on the rest of the
product: `src/diagnostics/` (log, diag, stats, timing), `src/settings/` (config,
config_patch, live, settings_io, settings_tx) and `src/gpu/` (the three policy headers).
Rewrite the includes of every file that uses them.

**Exit:** per slice, the baseline comparison is identical; the checker passes.

### P3.3 — Core modules: generator, runtime, abi

Move `src/generator/` (stage 1, params, packing, shaders, LUT), `src/runtime/` (integration,
context, bookkeeping faults) and `src/abi/` (describe, route, token namespace). `dllmain.cpp`
stays in `src/`.

**Exit:** as P3.2; the ABI scenarios on the real DLL pass unchanged.

### P3.4 — Presentation and UI

Move `src/presentation/` (swapchain, presenter, pacing, limiter, limiter schedule, overlay
compose) and `src/ui/` (overlay, ui, ui input, imgui config). The `src/` flat layout is now
empty apart from `dllmain.cpp`.

**Exit:** as P3.2; the GPU host and window suites (`gpu-mini`, `gpu-full`, the `fgcheck`
family) pass unchanged.

### P3.5 — Tests, `docs/dev` and AGENTS layout

Move the tests into `tests/abi`, `tests/integration`, `tests/regression`, `tests/settings`
and `tests/tools` (whole files, no splitting) and keep every CTest name. Write `docs/dev/`
for agents:

| File | Content |
| --- | --- |
| `README.md` | Start here: what Sedici is, the hard rules, where things live, how to build and run the baseline |
| `layout.md` | Each module: purpose, owner files, public headers, the real include edges and the known cycles |
| `workflow.md` | How to build and test, how to make a move slice, the commit convention, what to do when the checker fails |
| `baseline.md` | The recorded baseline values and how to reproduce them |
| `moves.md` | The old path to new path ledger, one row per file |

Update the AGENTS.md layout section and the root README links, and mark
`docs/refactor/file-structure.md` as completed.

**Exit:** a link check over the tracked documents passes; every file in `moves.md` exists at
its new path; AGENTS.md describes the tree that is on disk.

### P3.6 — Final proof

Run the full baseline comparison on the final tree and attach the result to
`docs/dev/baseline.md`. No in-game run is needed: the owner's QA record is the behavioural
reference and the offline baseline proves the code is unchanged. Update references to old
file names in the living documents (the historical findings and K entries keep the names
they were written with; `docs/dev/moves.md` maps them).

**Exit:** all comparisons identical; the milestone and progress tables are current.

### Phase 3 exit

The tree on disk matches `docs/refactor/file-structure.md`, version 2. The baseline
comparison is identical to the `phase2-baseline` tag, apart from paths and `#include`
lines. The history explains every move, and `docs/dev/` lets an agent find its way
without reading the history. The backlog is untouched and waits for the next phase.

### Owner decisions

1. **Decided (2026-11-09):** Phase 3 starts after P2.11 closes; refactoring does not change
   behaviour.
2. **Decided (2026-11-09):** drop the `fg_` prefix during the move; no other renames.
3. **Decided (2026-11-09):** no in-game smoke check; the owner considers all behaviours proven.

## Phase 4 — Cover all issues and improvements

**Goal:** every open entry (at the time in [known-issues.md](docs/archive/known-issues.md), now the
GitHub issues) ends as fixed, accepted with a reason, or obsolete, and the requests in
[feature-requests.md](docs/archive/feature-requests.md) are decided. Fixes land in the Phase 3
module layout. Unlike Phase 3, behaviour is allowed to change, so each fix needs: a test or
measurement that fails before and passes after, the issue closed with the commit hash, and the
Phase 2 baseline (`tools/baseline.ps1`) still passing except for the lines the fix is meant to
change (record those in the commit).

**Status:** in progress (2026-11-09). The owner set the decisions below and the goal "implement all
milestones that can run offline": P4.1 to P4.5 are done and committed on the branch `phase-4`. The second goal,
"complete P4.6 to P4.9 offline and get the QA entries ready", is done for everything that does not need the
game: what remains is the owner's sessions (QA-03, QA-17 to QA-20 and the cadence parts of QA-04, QA-05,
QA-10), judged with `tools/analyze_log.ps1` and `tools/analyze_presents.ps1` by the rules in
[phase4-game-checklist.md](docs/refactor/phase4-game-checklist.md).

**Owner decisions (2026-11-09):**

1. **Alan Wake 2 is its own milestone** (P4.8). HDR is not its only problem, so it is not
   folded into format support.
2. **FR-03 (intro skipping) is out of Phase 4.** It is unrelated to the system. A standalone
   helper already exists, `tools/intro_skip.ps1` (see FR-03), and it is not tracked here.
3. **Offline first.** Everything that can be proven by a test or a measurement on this machine
   comes first (P4.1 to P4.5). Everything that needs the game, a VRR display or the owner's
   eyes comes after (P4.6 to P4.8), so the game sessions are batched at the end instead of
   interleaved with the fixes.

**Where each part is tested:**

| Needs | Examples | Milestones |
| --- | --- | --- |
| Offline only (CTest, `sedici_fgcheck`, `sedici_regression`, ABI scenarios, log analysis) | UI value logic, settings, lifetimes, error contracts, Stage 1 cost and quality on the synthetic scenes, module structure | P4.1 to P4.5 |
| The game, a real display or RTSS (cannot be driven unattended: K23, K24, K36) | pacing on a VRR or fixed-refresh panel, stalls, window-mode changes, loading screens, Alt+F4, Alan Wake 2 | P4.6 to P4.8 |

An entry that has an offline half and a game half is fixed and tested offline first and then
listed in the game checklist of its later milestone. The code is never launched by an agent.

### Phase 4 progress

**Keep this table current**, in the same commit as any change to a milestone. Last
updated: 2026-11-09.

| Milestone | Size | Where | Status | Prerequisites |
| --- | --- | --- | --- | --- |
| P4.1 Triage of the open entries and requests | Medium | Offline | Complete (2026-11-09) | none |
| P4.2 Menu, settings and counter logic | Medium | Offline | Complete (2026-11-09); K10, K119 and K124 move on, see below | P4.1 |
| P4.3 Lifetimes, shutdown and error contracts | Large | Offline | Complete (2026-11-09); game confirmations on the P4.7 list | P4.1 |
| P4.4 Stage 1 cost and generated-frame quality | Large | Offline | Complete (2026-11-09); K120 capture on the P4.7 list | P4.1 |
| P4.5 Structure: module cycles and large files | Large | Offline | Complete (2026-11-09); file splitting partial, see below | P4.2 to P4.4 |
| P4.6 Pacing and presentation | Large | Game | Offline part complete (2026-11-09): K6 accepted, analyzers, QA-18; verdicts wait for the owner's runs | P4.5 |
| P4.7 In-game defects from the owner's QA run | Large | Game | Offline part complete (2026-11-09): settle instrumentation, analyzer, QA-19 and QA-20; verdicts wait for the owner's runs | P4.5 |
| P4.8 Alan Wake 2 | Large | Offline build, game acceptance | Offline part complete (2026-11-09); acceptance is QA-03 and QA-17 | P4.5; owner sessions |
| P4.9 Final acceptance | Medium | Game | Checklist ready (`docs/refactor/phase4-game-checklist.md`); sign-off waits for the runs | P4.6 to P4.8 |

### P4.1 — Triage (offline)

Go through all 48 open entries (done: [phase4-triage.md](docs/refactor/phase4-triage.md); 46 remain) and the five requests. Each gets one of: fix in P4.x, accept
(reason and owner OK), obsolete (evidence), or needs owner decision, and a tag **offline** or
**game**. Entries that are not product defects stay open by design (K16, K23, K24, K36, K54,
K101, K112). Fix stale statuses and duplicates (K54/K112, K20/K21/K22) without deleting
entries. Write the decision beside each entry and update the counts.

**Exit:** no open entry without a target milestone or an accepted reason.

### P4.2 — Menu, settings and counter logic (offline)

K118 with FR-01 and FR-02 (limit slider as a half-step integer slider plus an Apply button),
FR-04 (scale list; check the high-DPI case first), K125 (the Real counter counts shown frames),
K124 (reproduce the exception while drawing, then fix or contain it), K119 (overlay state
across launches: test the persisted state and the start-up path), K19 (re-resolve `target_fps`
after a mode change), K22 (multiplier fallback), K10 and K17 (diagnostic wording and the first
overlay log line), FR-05 (one target frame rate: `0` uncapped, `N` capped, `auto` = floor(`Hz - Hz^2/3600`),
uncapped if the refresh is unknown; replaces `limiter.fps` and `fg.target_fps`, which keep loading as
aliases; K3 and the `OVER_REFRESH` note are fixed by it). The slider step
logic, the settings transaction and the counters are pure logic and get regression tests.

**Exit:** each entry fixed with a test that failed before; the Full-menu click path itself goes
on the P4.7 game checklist (K36).

**Result (2026-11-09):** done offline: FR-05 as one target fps (`0`, `N`, `"auto"`; `limiter.fps`
read as a deprecated alias), the half-fps slider with Apply (K118, FR-01, FR-02), the scale list
(FR-04), the dropped-frames counter (K125), Alt+F4 pass-through (K126), the exception text (K124,
partly), K3, K17, K19 and K22. Moved: K10 to P4.6; K119 and the cause of K124 need the game (P4.7).
Tests added: `sedici_regression` target and UI checks, the `sedici_abi_target-auto` scenario; the
suite is 38 of 38. The Phase 2 baseline differs only by those additions.

### P4.3 — Lifetimes, shutdown and error contracts (offline)

K127 and K98 (destroy refused before GPU retirement, repeated prepare-slot and depth-format
rejections: reproduce with `sedici_fgcheck`), K37, K48, K77 and K103 (generator ownership,
drain timeout and callback lifetime), K64 (token namespace across a DLL reload), K83 (blocked
settings I/O and the watchdog decision), and the P2.10 deferrals: typed error codes with call,
frame and settings identity, structured log events, the configuration schema and the hidden
environment switches (K20, K21).

**Exit:** each contract is written, implemented or rejected, with a test; the hard rules in
`AGENTS.md` still hold.

**Result (2026-11-09):**

- **Found by a new test:** K12 was real. In `vsync`/`game` the wrapper kept the game's native chain
  reference, so a new chain on the same window failed after the wrapper was destroyed
  (`sedici_abi_window-replace-vsync`). Fixed in `Swapchain::wrap`.
- **Fixed:** K59 (mutex inside `noexcept`), K99 (present and swapchain-event statistics fed).
- **Mitigated, game confirmation needed:** K98 (prepare slot wait 50 to 250 ms), K127 (retirement wait
  200 to 1000 ms).
- **Instrumented:** K121 and K122 (calls of 50 ms or more are logged), K101 (build line in the log).
- **Accepted:** K11, K20, with reasons in the entries.
- **Error contract:** every rejection is logged with a stable code, `rejected: <reason> [code=E_...]
  [op= ctx=]` (`diagnostics/error_code.h`, listed in the README). Proposed decisions, for the owner to
  confirm or reverse: frame identity is not added (rejections happen before the descriptor is parsed;
  `slow call` lines carry the context), settings identity stays the revision in `ApplyResult`, structured
  log events and the configuration schema (`docs/refactor/configuration.md`) are not enabled: the text log
  with stable codes is the contract. The watchdog (K83) and parked-owner reaping were already resolved
  in P2.10.
- **Tests:** 39 of 39 CTest entries (new: `sedici_abi_window-replace-vsync`), 156 regression checks.

### P4.4 — Stage 1 cost and generated-frame quality (offline)

K117 (0.66 ms at 1080p against the 0.15-0.30 ms estimate: profile first, then reduce) and the
offline half of K120 (disocclusion at speed: add a fast-motion scene to `sedici_fgcheck` and
measure). Use `sedici_fgcheck --resolution`; PSNR and SSIM must not drop except where the owner
accepts it.

**Exit:** cost and quality recorded at 1080p and 1440p, and a decision on the estimate.

**Result (2026-11-09):** `sedici_fgcheck --profile` prints the GPU time of each Stage 1 pass
(`stage1_set_pass_mark`, a null hook in production). At 1080p the total is 0.61 ms: decode+dilate 0.127,
clear 0.059, reproject 0.080, fill 0.050, warp 0.267, composite 0.022. Decode and dilate were merged into
one pass with bit-identical output (same PSNR and SSIM), 0.66 to 0.61 ms at 1080p and about 1.3 to 1.09 ms
at 1440p. Decision on the estimate (K117): withdrawn; Stage 1 is 0.61 ms at 1080p and, with the Stage 2
estimate, the 2.0 ms budget still holds with 0.04 ms to spare. K120: the synthetic fast scene cannot show
the artefact (a capture is needed); K15: estimated by arithmetic, to be measured in a game run.

### P4.5 — Structure: module cycles and large files (offline)

Break or document the seven cycles in `docs/dev/layout.md` (start with `diagnostics`, which
should be a leaf) and split the largest files (`fgcheck.cpp` 186 KB, `abi.cpp`,
`regression.cpp`, `swapchain.cpp`, `ui.cpp`). It comes after the offline fixes so it does not
fight them, and before the game milestones so the game sessions run on the final structure.
Use the Phase 3 method: one module per commit, `tools/baseline.ps1` and `check_moves.ps1` where
a commit is a pure move.

**Exit:** no cycle left unrecorded, no new one added, baseline identical.

**Result (2026-11-09):**

- **Cycles:** all seven are gone and a CTest entry, `sedici_layers` (`tools/check_layers.ps1`), fails any
  include that goes to the same or a higher layer. Moves: `bookkeeping_faults` to `diagnostics/`, `diag` to
  the new `status/` module; inversions: the log level (`log_publish_level`), the limiter cap listener
  (`live_set_cap_listener`), the limiter's frame generation probe (`limiter_set_fg_probe`), the stats extra
  line (`stats_set_extra_line`). Commit ac86c5e. The layer table is in `docs/dev/layout.md`.
- **Large files:** `fgcheck.cpp` (186 KB) is split into `fgcheck.cpp`, `fgcheck_common.h` and five check
  groups; the largest is 53 KB (commit b97463f). **Not done:** `abi.cpp` (73 KB), `regression.cpp` (71 KB),
  `swapchain.cpp` (56 KB) and `ui.cpp` (43 KB) are unchanged. The two test files could be split the same way;
  the two production files would move code that the in-game sessions still have to confirm, so they wait
  until after P4.9 unless the owner asks sooner.
- **Baseline:** identical to the P4.4 record (293 lines, 40 of 40) after both commits.

### P4.6 — Pacing and presentation (game)

K1 to K9 and K12 to K15: output above refresh (K3), generation load feeding back into the real
rate (K2), throttling with FG off (K4), VRR flip spacing (K5), the tuned constants (K6), the
waitable object (K8), the vrr/RTSS limits (K11, K13), the QA-04 and QA-05 present-interval
captures. These need measurements on the owner's machine; each ends fixed or accepted with a
written reason. Offline changes are tested first, then the owner runs the checklist.

**Exit:** every entry in the range has a verdict.

**Offline part (2026-11-09):**

- **Analyzers (new, tested by `sedici_analyzers`):** `tools/analyze_log.ps1` turns a `sedici.log` into
  rejections by stable code, stalls, slow calls, create and destroy results, overlay events and the settle
  time of the real rate; `tools/analyze_presents.ps1` judges a PresentMon or CapFrameX CSV (percentiles,
  1% low, stalls, share of intervals within a tolerance of the target).
- **Verdicts decided offline:** K6 accepted (tuned constants with tests). K1, K2, K4, K5, K7, K8 and K15
  have a measurement and a criterion in QA-18; K9 stays an `XFAIL`.
- **Not decided:** every other entry in the range, because each is a measurement on the owner's display.

### P4.7 — In-game defects from the owner's QA run (game)

K121 to K123 (stalls, slow settle of the real rate, window-mode gaps), K126 (Alt+F4 with the Full
menu open), K128 (loading-screen borders), the game half of K120, K132's Cyberpunk equivalent
if any, and the game halves of P4.2/P4.3 (Full-menu click path, shutdown at Alt+F4). Add the
narrowest log or counter that makes the owner's next run conclusive before changing behaviour.

**Exit:** each entry fixed and confirmed in a run, or its cause recorded.

**Offline part (2026-11-09):** instrumentation and QA entries, no behaviour change except the log.

- After a swapchain create or resize the overlay reports every 60 outputs for eight reports (K123), so
  the settle shows as several lines; the periodic line is otherwise unchanged.
- The cursor interception log line says what it found (K132, below).
- QA-19 (window changes, settle, exit, menu clicks) and QA-20 (K120, K128, K119, K121 evidence) are
  ready. A capture feature in the DLL was considered for K120 and K128 and not built: a screen recorder
  gives the same evidence without a readback path in a drop-in DLL.

### P4.8 — Alan Wake 2 (standalone)

Everything Alan Wake 2 needs, as one milestone: routing of `FrameGenerationSwapChainNewDX12`
(K100), the HDR create flag with a 10-bit back buffer (K14, K129), the swapchain and queue
device-object mismatch (K130), the interpolation command list and texture queries (K131), the
Full-menu cursor interception (K132), and any further problem the first sessions show
(finding 014 and 015 are the evidence; branch `bearingme/aw2-patch` holds an earlier attempt).
The first step is to re-read the findings and list every call the game makes, then decide what
is in scope with the owner. Routing, formats and queries get ABI and `sedici_fgcheck` tests
first; acceptance is the owner's session (QA-03).

**Exit:** QA-03 passes, or each remaining failure is a recorded, owner-accepted refusal.

**Offline part (2026-11-09):** the call list was re-read from findings 014 and 015 and the earlier branch
(`bearingme/aw2-patch`) was ported onto the refactored modules, with tests first.

- **Scope decided:** everything the findings list is in: the `New` and `ForHwnd` routes (K100), the 10-bit
  back buffer (K14), the HDR flag (K129, accepted; Stage 1 is encoding-agnostic), the same-adapter device rule
  (K130), the interpolation queries and the game's own dispatch (K131), and the cursor interception (K132).
  Reversible by the owner: K129 (accepting the flag), K130 (the same-adapter rule).
- **Done:** routes with error codes and no handle on failure; `GetVersions` for both forms; 10-bit through the
  wrapper, context and Stage 1; interpolation shim; broader cursor scan with a probe-module test.
- **Tests:** `sedici_abi` (null `New` and `ForHwnd`, versions), `sedici_fgcheck --routing` (both forms, a
  10-bit HDR-flagged session, queries, dispatch, destroy order), `sedici_fgcheck` (10-bit against 8-bit on two
  scenes), `sedici_regression` (cursor scan). 41 of 41 CTest entries.
- **First game run (2026-11-09, QA-03):** the create, the device rule, the generation context and the HDR flag worked; the scene was black afterwards. K138 (queries blocked behind Present) found and fixed, K139 (prepare slot refusals) open; [finding 016](docs/findings/016-alan-wake-2-black-screen.md).
- **Waiting for the game:** QA-03 again and QA-17 (generation, HDR on and off, menu, exit). The
  calls the game makes after a successful create are still unseen, so the first session may show more.
- **Commits:** b38d1b8 (routes, formats, cursor scan), 9a04826 (settle reports, analyzers, QA entries), 9f21334 (harness fixes, baseline).

### P4.9 — Final acceptance (game)

Full baseline comparison, the owner's in-game checklist (the QA table in `tests/qa`) on the
final build, counts and tables current, `docs/dev` updated.

**Exit:** owner sign-off.

### Owner decisions still needed

1. **FR-04** accepted and **FR-05** decided (2026-11-09), design points confirmed: the target is the
   *output* rate (real cap = target / multiplier); the default is `0` (uncapped), because games such
   as Cyberpunk 2077 have their own cap; the `auto` value is rounded down (75 Hz gives 73).
2. **P4.8 scope:** decided when the first step of that milestone lists the calls.
