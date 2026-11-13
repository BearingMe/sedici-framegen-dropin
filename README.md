# Sedici

[![CI](https://github.com/BearingMe/sedici-framegen-dropin/actions/workflows/ci.yml/badge.svg)](https://github.com/BearingMe/sedici-framegen-dropin/actions/workflows/ci.yml)

A drop-in replacement for AMD's FSR 3.1 frame generation DLL on DX12. The game loads
`amd_fidelityfx_framegeneration_dx12.dll` as usual, but it is Sedici's: it exports the same five
`ffx*` entry points and makes the extra frames with its own generator, swapchain wrapper, frame
pacing and in-game overlay. AMD's DLL is never loaded.

> **Experimental, noncommercial.** Cyberpunk 2077 is the tested game. Part of the generator is
> adapted from Mob-FGSR (CC BY-NC 4.0), so a built DLL is for noncommercial use. See
> [`NOTICE.md`](NOTICE.md).

## What you get

- **Same interface.** Exactly five exports and the same descriptor types, so a game built for
  FSR 3.1 frame generation cannot tell the difference. A call Sedici does not implement gets a
  defined FFX error code and a log line; it never crashes the game.
- **Its own frame generation.** Stage 1 is analytical (Mob-FGSR: motion-vector warping with a
  lookup table). Stage 2, a small residual network, is designed but postponed.
- **Owned presentation.** `vrr` pacing with a tear guard, a 2x default multiplier and an optional
  frame-rate target.
- **A live overlay.** `Alt+F10` cycles Off, Mini and Full; the Full menu changes the limit, log
  level and scale and can save them to `sedici.toml`.

| Behaviour | Default | How to change it |
|---|---|---|
| Frame generation | Sedici's own generator and swapchain wrapper | always answered by Sedici; the game's own frame generation toggle still turns generation off; `[fg]` selects how frames are shown |
| Output multiplier | `2x` | `fg.multiplier` |
| Presentation | `vrr` with the `scanline` tear guard | `fg.present`, `fg.tear_guard` |
| Target frame rate | **uncapped** (`0`) | `fg.target_fps = 73`, `"auto"`, or `SEDICI_FG_TARGET_FPS` |
| Limiter busy-wait window | `1000` us | `limiter.spin_us` |
| Overlay menu | **off** until `Alt+F10` | `[ui] mode`, `[ui] hotkey` |
| Log level | `1` | `log.level` or `MFSRFG_LOG` |

The target is the output rate the screen receives: `0` is uncapped (no wait is added),
`N` caps at N, and `"auto"` matches the monitor with the low-lag rule
`floor(Hz - Hz^2 / 3600)` (59 at 60 Hz, 73 at 75 Hz, 116 at 120 Hz, 138 at 144 Hz, 224 at 240 Hz),
uncapped if the refresh rate cannot be read. Sedici enforces it by capping real frames at
target / output ratio (73 on a 75 Hz screen at 2x caps the game at 36.5). Some games, such as
Cyberpunk 2077, have their own frame cap, which is why the default is uncapped.

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
| 1. Analytical | Mob-FGSR | 1080p | quadratic trajectory from two motion-vector frames, depth-atomic splat, thin-object hole fill, threshold fusion, LUT (32×32×16) backward warping → `I_base`, `M_input` | ~0.15–0.30 ms estimated; **0.61 ms measured** offline at 1080p (K117, resolved) ([K117](docs/archive/known-issues.md)) |
| 2. Residual | GFFE SCN / STSSNet backbone | 540p | 8 conv layers, 92,992 params (~90.8 KB INT8), predicts `ΔI` and `M̂`; final `clamp(I_base + M̂ · ΔI)` | ~1.10–1.35 ms |

Inputs are only what an FFX FG hook already receives: HUD-less color, depth,
motion vectors. Camera data arrives in the descriptor but stage 1 does not use it yet. No engine
G-buffers, no engine patches.

Budget target: **≤ 2.0 ms at 1080p** on a Radeon RX 7700 XT — 1.30–1.67 ms in FP16
WMMA, 0.95–1.20 ms in INT8 dp4a. These are design estimates from the papers, not
measurements.

Status: **experimental.** The offline suite is accepted (P2.1 to P2.11, with accepted gaps; 41 CTest
entries) and the owner has run an in-game acceptance checklist on the build
(QA-01 to QA-20, [results](tests/qa/README.md)); the findings are open issues. A recognized
create that Sedici cannot satisfy returns an error to the game; there is no fallback. Bugs and
feature requests are tracked as [GitHub issues](https://github.com/BearingMe/sedici-framegen-dropin/issues)
(the retired [K1-K147 list](docs/archive/known-issues.md) and
[feature requests](docs/archive/feature-requests.md) are archived). See also the
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
   With a target fps set, it caps real frames at target / output ratio and targets `previous_deadline + 1/fps`, waits on a
   high-resolution waitable timer until `deadline - spin_us`, then spins on QPC to
   the deadline. A frame already past its deadline does not wait; one more than a
   full interval late reseeds the schedule instead of bursting. Capping the real
   frames puts the output at about twice the cap with 2x generation, which is the target.
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

Every rejection is logged as `rejected: <reason> [code=E_...] [op=<call> ctx=<handle>]`. The codes
are stable and never reused: `E_STALE_HANDLE`, `E_NULL_DESCRIPTOR`, `E_INVALID_DESCRIPTOR`,
`E_UNSUPPORTED`, `E_NAMESPACE`, `E_ALLOCATION`, `E_WRAP_FAILED`, `E_NOT_RETIRED`, `E_SLOT_BUSY`,
`E_RETIREMENT_TIMEOUT`, and `E_REJECTED` for anything else. A router call that keeps the game's
thread for 50 ms or more logs `fg: slow call op=... took N ms`.

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
| `multiplier` | string | `"2x"` | `"2x"`, `"1.75x"`, `"1.5x"`, `"1.25x"` | output frames per real frame; only `vrr` schedules anything but 2x. Keep real fps times the multiplier under the refresh rate. `"adaptive"` was removed on 2026-11-12 (issue #1): it is reported and falls back to `"2x"` |
| `target_fps` | number or `"auto"` | `0` | `0` to `1000`, or `"auto"` | the one frame-rate target, in output frames per second: `0` uncapped, `N` capped, `"auto"` = `floor(Hz - Hz^2/3600)` of the monitor (uncapped if unknown). Replaces `limiter.fps` |
| `tear_guard` | string | `"scanline"` | `"scanline"`, `"timer"`, `"off"` | only with `vrr`; how a flip is kept from landing while the panel draws the previous image |

Sedici owns output pacing completely. A game's request to tune AMD's swapchain
pacing is accepted and ignored, so the game's own call sequence still succeeds.
Measurements of these modes are in
[the current findings](docs/findings/007-fg-baseline.md#vblank-presentation-removes-the-remaining-tearing)
and [finding 008](docs/findings/008-fg-tearing.md); none has been repeated since
the AMD backend was removed.

### `[limiter]`
## Quick start

Requirements: Windows x64, Visual Studio 2022 with the C++ workload, CMake 3.21 or newer.

```powershell
cmake -B build -A x64
cmake --build build --config Release
.\tools\deploy.ps1 -Game cp2077     # backs up the original DLL, installs Sedici, adds sedici.toml
.\tools\restore.ps1 -Game cp2077    # puts the original back
```

The DLL is `build/Release/amd_fidelityfx_framegeneration_dx12.dll`. `deploy.ps1` refuses to run
while the game is running and only touches the DLL, its `.orig` backup, `sedici.toml` and
`sedici.log`. Each tagged version publishes a zip on the [releases page](https://github.com/BearingMe/sedici-framegen-dropin/releases).
Details: [deploy](docs/reference/deploy.md), [build and test](docs/reference/build-and-test.md).

## Configuration

`sedici.toml` sits beside the DLL and is read once. Every key is optional and a bad value falls
back to its default, so a typo never stops the game.

| Setting | Default | Key |
|---|---|---|
| Output multiplier | `2x` | `fg.multiplier` |
| Presentation | `vrr` with the `scanline` tear guard | `fg.present`, `fg.tear_guard` |
| Target output rate | uncapped (`0`); `"auto"` matches the monitor | `fg.target_fps` |
| Overlay | off until `Alt+F10` | `ui.mode`, `ui.hotkey` |
| Log level | `1` | `log.level` or `MFSRFG_LOG` |

Environment variables win over the file. Every key, the error behaviour and the effective-config
log are in [`docs/reference/configuration.md`](docs/reference/configuration.md).

## How it works

```
game.exe
  └── amd_fidelityfx_framegeneration_dx12.dll      <- Sedici, the only implementation
```

1. **Answer every call.** The five exports act only on handles Sedici issued; foreign, stale and
   unknown calls are refused locally with a stable error code.
2. **Wrap the swapchain**, as AMD's did, and present real and generated frames itself.
3. **Generate.** Each prepare dispatch hands depth, motion vectors and render size to Stage 1.
4. **Pace.** The optional limiter caps real frames so the output lands on the target.
5. **Observe.** Calls, cadence and limiter statistics go to `sedici.log`.

The design is in [`references/papers/realtime-fg.md`](references/papers/realtime-fg.md).
Return codes and the DLL lifetime are in [`docs/reference/abi.md`](docs/reference/abi.md).

## Status and limits

- **Tested game:** Cyberpunk 2077. **Alan Wake 2** starts but shows a black scene
  ([#31](https://github.com/BearingMe/sedici-framegen-dropin/issues/31)).
- **Formats:** 8-bit or 10-bit back buffer and HUD-less image; `D32_FLOAT`, `R32_TYPELESS` or
  `R32G8X24_TYPELESS` depth; `R16G16B16A16_FLOAT`, `R16G16_FLOAT` or `R32G32_FLOAT` motion
  vectors. Anything else gets a defined error.
- **Offline suite:** 41 CTest entries pass. The owner's in-game acceptance record is in
  [`tests/qa`](tests/qa/README.md).
- **Known problems** are [GitHub issues](https://github.com/BearingMe/sedici-framegen-dropin/issues);
  those that need a game run are labelled `needs-game`.
- **Stage 1 cost:** 0.61 ms at 1080p, measured offline on an RX 7700 XT. The Stage 2 budget is a
  design estimate, not a measurement.

## Documentation

| Topic | Where |
|---|---|
| Rules and layout for contributors and agents | [`AGENTS.md`](AGENTS.md), [`docs/dev/`](docs/dev/README.md) |
| Plan and acceptance gates | [`MILESTONES.md`](MILESTONES.md) |
| Configuration reference | [`docs/reference/configuration.md`](docs/reference/configuration.md) |
| Return codes, DLL lifetime | [`docs/reference/abi.md`](docs/reference/abi.md) |
| Logging, stats, present timing | [`docs/reference/logging.md`](docs/reference/logging.md) |
| Capture analysis | [`docs/reference/capture-analysis.md`](docs/reference/capture-analysis.md) |
| Dependencies and provenance | [`docs/reference/provenance.md`](docs/reference/provenance.md) |
| Measurements behind the defaults | [`docs/findings/`](docs/findings/) |
| Retired K1-K147 and FR lists | [`docs/archive/`](docs/archive/) |
| Rust port (branch `experiment/rust`) | [`docs/rust/`](docs/rust/) |

## Contributing

Changes reach `main` through a pull request; CI must be green. Bugs and requests are GitHub issues
(Bug and Feature request forms). Read [`AGENTS.md`](AGENTS.md) first; the short version: stay a
drop-in replacement, never fail a game call because of Sedici's own bookkeeping, leave
`third_party/` and AMD's loader and upscaler DLLs alone, and never launch the game from a build or
test. Run `.\tools\check.ps1 -Quick` before you push.

## Licence

Sedici's own code is MIT ([`LICENSE`](LICENSE)). The Stage 1 shaders and lookup table are adapted
from [Mob-FGSR](docs/mobfgsr-attribution.md) (CC BY-NC 4.0), so binaries that include them are
noncommercial. Vendored FidelityFX headers, toml++ and Dear ImGui keep their own licences; see
[`NOTICE.md`](NOTICE.md) and [provenance](docs/reference/provenance.md).
