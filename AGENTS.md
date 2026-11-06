# AGENTS.md

## Project

Sedici is a Windows x64 C++20 proxy DLL named
`amd_fidelityfx_framegeneration_dx12.dll`. It loads the original as
`amd_fidelityfx_framegeneration_dx12.dll.orig`, forwards the five `ffx*`
exports, and on top of that sends one frame-pacing tuning configure that removes
a measured frame-time tail, plus an optional real-frame limiter. Behaviour is
driven by `sedici.toml` beside the DLL, with environment overrides.

## Hard rules

- **Forward everything.** Every argument, return value and timing passes through
  unchanged. The only permitted behaviour changes are the three documented ones:
  the frame pacing tuning send (`pacing.mode`), the async-workload rewrite
  (`async.mode`) and the real-frame limiter (`limiter.fps`, which deliberately
  delays Present). All must be inert when configured off. Do not "fix" anything
  else, including unknown descs, return codes, or swapchain behaviour.
- Only change behaviour when a milestone explicitly says so. Milestone 1 is the
  pacing fix; milestone 2 adds the real-frame limiter. The pacing fix is on by
  default; the limiter is off by default. Both must remain fully opt-out.
- Never fail a game call because of Sedici's own bookkeeping. Configuration
  errors, a rejected tuning and logging failures are all non-fatal.
- Do not touch `amd_fidelityfx_loader_dx12.dll` or
  `amd_fidelityfx_upscaler_dx12.dll`; the upscaler is owned by m-fsr.
- Keep `third_party/ffx/`, `third_party/tomlplusplus/` and
  `third_party/imgui/` unmodified. Do not move individual headers without
  fixing their relative includes, and do not invent provenance.
- Never download SDKs or runtime DLLs as part of a build or test.
- Do not launch the game. Deployment is not launch authorization.
- Use Conventional Commits for every commit, for example
  `feat: add present timing stats`.

### Branch override: `experiment/cobalt-otter`

This branch replaces AMD's frame generation with Sedici's own, so
`Forward everything` does not apply to the frame-generation path. The override is
narrow:

- Everything Sedici does not implement is still forwarded byte-for-byte.
- The FG path must reproduce AMD's observable contract: same DLL name, the same
  five exports, the same desc types and fields, the same swapchain behaviour and
  return codes. A game built against FSR 3.1 frame generation must not be able to
  tell which implementation it is talking to.
- Pacing, limiter and overlay must keep working on top of the new path.
- Every other rule on this page still applies unchanged.

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
- When the new path is disabled, fails to initialize, gets an unsupported
  desc/format, or the GPU rejects a resource, Sedici falls back to the existing
  forward-to-`.orig` behaviour and logs it. A game call must never fail because
  the new path is unavailable.

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
- `src/dllmain.cpp` — the five exports; loads lazily and forwards.
- `src/forward.*` — `LoadLibraryExW` + `GetProcAddress` for the `.orig`.
- `src/config.*` — `sedici.toml` parsing (toml++), defaults, env overrides,
  provenance, once-at-load reporting.
- `src/toggles.*` — the pacing and async behaviour switches, per-context pacing
  state, and the frame-generation-active label used by the limiter.
- `src/describe.*` — desc-chain walking and per-type logging.
- `src/present_hook.*` — optional `Present`/`Present1` vtable hook.
- `src/overlay.*` — FFX frame-generation present callback: records output
  cadence and performs AMD's back-buffer copy when no separate UI is registered.
- `src/overlay_compose.*` — the DX12 copy and resource-state restoration used by
  that callback.
- `src/ui.*` — Dear ImGui DX12 overlay: input hook, Mini/Full menu, live
  controls, snapshot/cancel.
- `src/ui_input.*` — Full-menu cursor ownership: executable import interception
  for `SetCursorPos`/`ClipCursor`, clip save/restore, client-to-output mapping.
- `src/live.*` — mutable live values shared by the overlay, limiter, pacing,
  async and logging.
- `src/timing.*` — 360-sample real/output interval rings and percentiles.
- `src/config_patch.*` — comment-preserving `sedici.toml` value patcher.
- `src/limiter.*` — opt-in real-frame limiter: high-resolution waitable timer
  plus QPC spin, scheduling, and per-300-frame accuracy stats.
- `src/limiter_schedule.h` — the pure deadline/deadline-reset decision, kept
  separate so it can be tested without a clock.
- `src/stats.*` — rolling prepare/present stats every 300 prepares, plus the
  prepare stall detector.
- `src/log.*` — `sedici.log` beside the DLL, flushed per write,
  `MFSRFG_LOG=0/1/2`.
- `sedici.toml` — shipped defaults; copied on deploy only when absent.
- `tools/` — deploy/restore with `.orig` safety rules, `games.json`.
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

Never commit logs, build output, or paths containing a username.
