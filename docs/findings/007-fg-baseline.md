# 007: Frame generation baseline and observed contract

Baseline for the Mob-FGSR work on `experiment/cobalt-otter`. Everything here is
derived from existing captures, logs and code. No new game run was made; the
next run happens after the first deploy of the new path.

## Existing implementation

Sedici is currently a forwarding proxy, and AMD still owns frame generation:

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
- 2x interpolation means one generated midpoint per prepare, presented before
  its real frame, with a one-frame input delay.

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
- Depth is non-linear (reversed-Z, infinite far), so the paper's `0.004` depth
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

Metrics use linear UNORM RGB (alpha excluded) for PSNR and pooled
8x8 uniform luma SSIM windows with stride 4, not Gaussian-window SSIM.

This is a real-frame-only wrapper, not the finished FG scheduler, and is not
routed from the five exports. Remaining: generation callbacks and routing,
generated-frame/HUD composition, bounded output
scheduling, version/query semantics and coordinated fallback. No deployment
or game launch has occurred for the new path.
