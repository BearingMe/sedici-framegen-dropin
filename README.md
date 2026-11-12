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
