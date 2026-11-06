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

## Generated presentation, HUD and the session path

The wrapper now presents two frames per game Present: the midpoint for the
*previous* pair, then the real frame the game just finished. That one-cycle lag is
inherent, because a midpoint between frames N-1 and N cannot precede N; the
alternative is an uneven output cadence. `plan_present` (pure, in `fg_pacing.*`)
caps every wait at half an interval so a slow cycle cannot stretch the game's own
frame time, and the wrapper drops a midpoint whose slot has passed instead of
queueing it.

Pacing is owned by the wrapper alone. The limiter still caps real frames, and the
overlay callback is untouched.

The generated image is HUD-less by construction, so the composite pass adds back
the HUD layer as `currentBackbuffer - HUDLessColor`. This preserves a static HUD
exactly (100% of a known block survived in all three scenes) but, over fast
motion, a semi-transparent HUD leaves background change behind. A registered UI
resource would avoid that; Cyberpunk registers none.

Presentation uses three separate command allocators: two alternating ones for the
per-cycle copies and one for generation. Sharing one is wrong, not merely slow:
the copies of a cycle execute before either allocator may be reset, and generation
is recorded after them, which the debug layer reports as a device removal.

`fg::Session` binds the FFX-shaped context to a wrapped swapchain found by
identity, records generation on the wrapper's list, and reports the wrapper's
fence so retirement stays with the object that owns the list. The offline host
runs describe, prepare, generate, present, FG off and clean destroy through it.

## Export routing and what is still unverified

`fg.mode` selects the path once, at the first create. `"amd"` is the default and
forwards everything, so every earlier release's behaviour is intact. `"sedici"`
creates the generator context and the swapchain wrapper and answers the five
exports for those handles only; a handle Sedici did not create is still forwarded,
so an AMD-owned context keeps working even in that mode.

The real-frame limiter attaches through `route_set_present_hook` rather than
through the swapchain directly. That keeps the wrapper free of limiter and
configuration symbols, and gives the limiter exactly one call per application
frame — the same rule the old vtable hook followed.

Two things cannot be established from the recorded contract, and both are left
deliberately conservative:

- **Queries.** No capture records which `ffxQuery` descs Cyberpunk issues on a
  frame generation context. Rather than guess an answer, a query on a Sedici-owned
  context is refused and logged. The first game session will name the descs that
  need implementing.
- **Unsupported calls.** Once Sedici owns a context there is no per-call fallback,
  because a context cannot be handed to AMD mid-life. Anything unsupported is
  refused and logged instead.

Neither affects `"amd"`, and neither is exercised by a game yet.

## Deployment record

Deployed to the Cyberpunk `bin\x64` directory through `tools/deploy.ps1` on
2026-11-06, with the game stopped and no launch performed.

- Deployed DLL SHA256 `89A6987B2DEDB5AFC9F75BE88CF7589BBC4D7B9EDD3AFCA419E63E2290659B98`,
  identical to the Release build from commit `3fddbe4`.
- Original `.orig` untouched at `02297BEEDD285E822D3A64F314CF00FAF378DCEC0EDC47FF0C4DD71B3A8C2F18`.
- The existing `sedici.toml` was kept, not overwritten: it is the user's, and
  deploy never replaces it.
- That kept file has no `[fg]` section, so the deployed build runs with
  `fg.mode = "amd"` and behaves exactly like the previous deployment. The new
  code is present but inert until the mode is switched.

To exercise Sedici's own generator, add to the deployed `sedici.toml`:

```toml
[fg]
mode = "sedici"
```

That is a deliberate, reversible change to the user's configuration, not
something deployment does. Roll back with `tools/restore.ps1 -Game cp2077`, which
restores the `.orig` and removes `sedici.toml`.

Metrics use linear UNORM RGB (alpha excluded) for PSNR and pooled
8x8 uniform luma SSIM windows with stride 4, not Gaussian-window SSIM.

This is a real-frame-only wrapper, not the finished FG scheduler, and is not
routed from the five exports. Remaining: generation callbacks and routing,
generated-frame/HUD composition, bounded output
scheduling, version/query semantics and coordinated fallback. No deployment
or game launch has occurred for the new path.
