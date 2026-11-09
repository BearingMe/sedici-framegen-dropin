# AGENTS.md

## Project

Sedici is a Windows x64 C++20 DLL named
`amd_fidelityfx_framegeneration_dx12.dll`. It is a drop-in replacement for AMD's
FSR 3.1 frame generation: the game loads it instead of AMD's DLL and Sedici's own
frame generation answers every call. There is no AMD backend and the original DLL
is never loaded. Behaviour is driven by `sedici.toml` beside the DLL, with
environment overrides. The real-frame limiter is optional.

## Hard rules

- **Stay a drop-in replacement.** The DLL name, the five `ffx*` exports (exactly
  five, no helper exports), the descriptor types and fields, the swapchain
  behaviour and the return codes must match what a game built against FSR 3.1
  frame generation expects. A call Sedici does not implement gets a defined FFX
  error code and a log line; it never crashes the host and is never sent to
  another DLL. Record the code chosen for each such case in the README and do not
  change one without saying why.
- Never fail a game call because of Sedici's own bookkeeping. Configuration
  errors, a malformed `sedici.toml` and logging failures are all non-fatal.
  The exceptions below are the only bookkeeping failures that may return an error.
- The real-frame limiter (`limiter.fps`) is off by default and fully opt-out.
- **Owner-approved context-identity exception:** Sedici FG context handles use a
  finite, process-lifetime reserved token namespace. Reserve the namespace before
  backend creation; if it cannot be reserved or is exhausted, reject that create
  before consuming game resources. Assume foreign FFX handles do not use values in
  the reserved namespace. Never reuse issued tokens. This exception does not
  authorize other bookkeeping failures.
- **Owner-approved create-allocation exception:** if routing-owner,
  routing-registry, or wrapper-registry bookkeeping allocation fails before any
  native swapchain ownership transfer, Sedici may return a non-fatal local create
  error. This does not authorize a failure after consuming the native chain.
- **Owner-approved Present1 metadata exception:** if Sedici cannot allocate its
  owned copy of variable-size Present1 dirty-rectangle/scroll metadata, it may
  return a local error before limiter admission, schedule advancement or GPU
  submission. The game-facing frame must remain unconsumed so the caller can
  retry. This exception does not authorize other presentation bookkeeping errors.
- Do not touch `amd_fidelityfx_loader_dx12.dll` or
  `amd_fidelityfx_upscaler_dx12.dll`; the upscaler is owned by m-fsr.
- Keep `third_party/ffx/`, `third_party/tomlplusplus/` and
  `third_party/imgui/` unmodified. Do not move individual headers without
  fixing their relative includes, and do not invent provenance.
- Never download SDKs or runtime DLLs as part of a build or test.
- Do not launch the game. Deployment is not launch authorization.
- **Document every bug you find. Always.** A bug that is only fixed, only mentioned in
  a chat, or only left in a code comment is lost. Whenever you find a defect (in
  production code, tests, build, tools, docs, or an external binary's behaviour that
  Sedici must design around), whether or not you fix it, you must record it in
  `docs/refactor/known-issues.md` with a new `K` number and these fields: the problem
  and its impact; the evidence and how it was found, marked measured, code or
  suspected; the status (fixed with the commit hash, open, or hazard); and the owning
  milestone. Record it in the same commit as the fix, or before moving on if it is not
  fixed. A fix with no entry is incomplete. Never delete or rewrite an entry to hide a
  bug: mark it resolved with the commit. Report new bugs to the user in the same turn
  you find them.
- **Keep the Phase 2 progress table current.** The "Phase 2 progress" table and bug
  counts in `MILESTONES.md` are what the owner reads to see where the work stands.
  Update them in the same commit as any change to a milestone's status, evidence or
  open items, or to a `known-issues.md` entry, and refresh the "Last updated" date.
- Use Conventional Commits for every commit, for example
  `feat: add present timing stats`.

### History: the AMD backend was removed

Sedici began as a pass-through proxy with a "forward everything" rule, then gained
its own frame generation behind an opt-in `fg.mode`. The owner then decided to
remove the AMD backend entirely, provided Sedici stays a drop-in replacement. The
forwarding rule, the pacing tuning send, the async rewrite, descriptor-copy
staging and the AMD probe are gone. The tag `pre-amd-removal` is the last commit
that has them. Findings and `docs/refactor/amd-binary.md` stay as reference for
what AMD's binary did; do not delete them.

## Experiment: two-stage frame generation

**Objective.** Ship a drop-in replacement for AMD's FSR 3.1 frame generation
(`amd_fidelityfx_framegeneration_dx12.dll`) that generates frames with Sedici's own
two-stage pipeline, using only the inputs an FFX FG hook is given: HUD-less color,
depth, motion vectors and camera matrices. No engine G-buffers, no engine patches.

**Design source.** `references/papers/realtime-fg.md` is the architecture note:
paper screening, the chosen papers, per-layer network specification, GPU budget
and training plan. `references/papers/mob-fgsr.md` and
`references/papers/gffe.md` are transcriptions of the two source papers.

**Stage 1 — analytical, 1080p (Mob-FGSR).** Quadratic per-pixel trajectory from
two consecutive motion-vector frames, depth-atomic splatting, thin-object hole
fill, depth/brightness threshold fusion separating disocclusion from shading
change, and backward warping through a pre-trained 32×32×16 LUT (~16 KB) instead
of a bicubic filter. Produces the base frame `I_base` and the input focus mask
`M_input`. Budget ~0.15–0.30 ms.

**Stage 2 — residual network, 540p (GFFE SCN, STSSNet backbone).** Eight 3×3
convolutional layers, 92,992 parameters (~90.8 KB INT8), 32.10 GFLOPs, 16
downsampled input channels, output `PixelShuffle(2)` producing 3 channels of
`ΔI` and one attention mask `M̂`; final frame is
`clamp(I_base + M̂ · ΔI)`. Uses only 3×3 conv, ReLU, 2×2 pooling/bilinear and
PixelShuffle, so it quantizes to INT8 dp4a or FP16 WMMA. Budget ~1.10–1.35 ms.

**Combined budget.** ≤ 2.0 ms at 1080p on a Radeon RX 7700 XT, ~20–25 TFLOPS
effective: 1.30–1.67 ms FP16, 0.95–1.20 ms INT8. This is a design estimate, not a
measurement; nothing on this branch has been profiled in the game.

**Upstream references.** `references/sources/` holds shallow, blobless, sparsely
checked-out submodules: `mobfgsr`, `stssnet`, `tbfe`, `lmv`, `moflow`. They are
read-only reference code. Do not vendor files out of them into `src/` without
recording the origin commit, and do not build them into the DLL.

**Scope boundaries.**

- Training and LUT export happen offline, outside this repository's build. The
  shipped DLL never trains.
- The upscaler DLL belongs to m-fsr and is not touched; this work is frame
  generation only.
- Generated frames must come out of Sedici's own swapchain wrapper so that
  `Present`, pacing and overlay keep a single, well-defined place to attach.
- When the generator fails to initialize, gets an unsupported desc/format, or the
  GPU rejects a resource, Sedici returns a defined error for that call and logs
  it. There is no fallback implementation. A failure must never crash the host.

**Execution plan.** Follow [MILESTONES.md](MILESTONES.md), the source of truth for
experiment delivery. Phase 1 ships Mob-FGSR at 1080p with custom scheduling and
2× interpolation in Cyberpunk, before adding GFFE.

- Build the baseline from the existing working implementation and findings
  002/005/006. Do not require another contract-capture run or launch the game
  before the first deploy. Deployment still does not authorize a launch.
- Keep validation small: one offline DX12 executable using the production path,
  PSNR/SSIM comparisons, GPU timing and a brief presentation check, then deploy
  with authorization. Do not add a broad test matrix or dozens of new tests.
- Treat vendored FFX SDK 2.4.0 / FG 4.0.2 / swapchain 4.0.0 headers as reference
  declarations, not proof of the installed FSR 3.1 binary's implementation.
- Record source revisions and license obligations for adapted material;
  MobFGSR is CC BY-NC 4.0. Keep submodules unmodified.

**Open questions.** Whether the FG swapchain wrapper can be reproduced without
AMD's private swapchain implementation; how the game's `HUDLessColor` and UI
resource registration map onto the two stages; whether INT8 quantization holds
quality on moving shadows; and how camera cuts are detected from the FFX descs
alone.

## Layout

- `MILESTONES.md` — experiment phases, acceptance gates and implementation references.
- `src/dllmain.cpp` — the five exports; each call is answered by the router.
- `src/fg_route.*`, `src/fg_integration.*`, `src/fg_context.*` — handle tokens and
  ownership, the session that binds a context to a wrapped swapchain, and the
  frame generation context.
- `src/settings/config.*` — `sedici.toml` parsing (toml++), defaults, env overrides,
  provenance, once-at-load reporting.
- `src/describe.*` — desc-chain walking and per-type logging.
- `src/overlay.*` — output cadence recording and the overlay draw hooks the
  swapchain wrapper calls for each presented frame.
- `src/overlay_compose.*` — the DX12 copy and resource-state restoration used by
  that callback.
- `src/ui.*` — Dear ImGui DX12 overlay: input hook, Mini/Full menu, live
  controls, snapshot/cancel.
- `src/ui_input.*` — Full-menu cursor ownership: executable import interception
  for `SetCursorPos`/`ClipCursor`, clip save/restore, client-to-output mapping.
- `src/settings/live.*` — mutable live values shared by the overlay, limiter, pacing,
  async and logging.
- `src/diagnostics/timing.*` — 360-sample real/output interval rings and percentiles.
- `src/settings/config_patch.*` — comment-preserving `sedici.toml` value patcher.
- `src/limiter.*` — opt-in real-frame limiter: high-resolution waitable timer
  plus QPC spin, scheduling, and per-300-frame accuracy stats.
- `src/limiter_schedule.h` — the pure deadline/deadline-reset decision, kept
  separate so it can be tested without a clock.
- `src/diagnostics/stats.*` — rolling prepare/present stats every 300 prepares, plus the
  prepare stall detector.
- `src/diagnostics/log.*` — `sedici.log` beside the DLL, written by a background thread from a
  bounded queue with per-message rate limiting and drop/loss counters,
  `MFSRFG_LOG=0/1/2`.
- `src/diagnostics/diag.*` — read-only snapshot of the log counters and parked objects, and the
  "diag:" line reported with the periodic stats.
- `sedici.toml` — shipped defaults; copied on deploy only when absent.
- `tools/` — deploy/restore with `.orig` safety rules, `games.json`.
- `tests/qa/` — the owner's in-game acceptance tests (QA-01 to QA-16): a table of tests
  and one folder per test with notes, screenshots and log excerpts.
- `third_party/ffx/` — unmodified FidelityFX/FSR v2.4.0 headers.
- `third_party/tomlplusplus/` — unmodified toml++ v3.4.0 single header.
- `third_party/imgui/` — unmodified Dear ImGui v1.93.0 plus DX12/Win32 backends.
- `references/papers/` — Mob-FGSR and GFFE transcriptions plus
  `realtime-fg.md`, the architecture note this experiment implements.
- `references/sources/` — sparse submodules of the upstream reference code
  (`mobfgsr`, `stssnet`, `tbfe`, `lmv`, `moflow`); read-only, never compiled.

## Build and verify

```powershell
cmake -B build -A x64
cmake --build build --config Release
dumpbin /exports build\Release\amd_fidelityfx_framegeneration_dx12.dll
```

There must be exactly five exports. Do not add helper exports, including the
proxy marker.

## Deploy safety

`tools/deploy.ps1` and `tools/restore.ps1` must:

- refuse to run while the game is running;
- never overwrite an existing `.orig`;
- refuse to overwrite or remove a DLL that is not Sedici's;
- never overwrite an existing `sedici.toml` on deploy;
- only touch the target DLL, its `.orig`, `sedici.toml` and `sedici.log`.

Never commit logs, build output, or paths containing a username. Owner-approved
exception (2026-11-08): `tests/qa/` holds the in-game QA evidence (folder READMEs,
tidied notes, game screenshots and log **excerpts**). Full logs stay local
(`*.full.log` is ignored), and any evidence is checked for usernames and home paths first.
