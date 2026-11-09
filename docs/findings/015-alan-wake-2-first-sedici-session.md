# 015: Alan Wake 2 on the Sedici swapchain, and why no frame is generated

What happened when the `SwapChainNewDX12` route from [finding 014](014-alan-wake-2-swapchain-new.md)
was implemented and run in Alan Wake 2: which blockers were found and removed,
which one remains, and what the game's call sequence looks like. Recorded from
user-run sessions on 2026-11-08. Nothing here was run by an automated check
except where stated.

**Status:** documented only, not fixed. The work lives on the temporary branch
`bearingme/aw2-patch` (worktree `aw2-patch`) and is not merged into `refactor/phase-2`.
On that branch the game runs through Sedici's swapchain without crashing, but frame
generation never starts, so no frame is generated. Resume from the open items below.
Related: [K100](../refactor/known-issues.md), K129-K132.

## What was changed on `bearingme/aw2-patch`

Temporary branch, merged from `refactor/phase-2` (89e3b1b). Commits, oldest first:

| Commit | Change |
| --- | --- |
| `22a8dce` | Route `FrameGenerationSwapChainNewDX12`. The chain is built from the game's factory, desc and queue, wrapped like the wrap form, and returned. Any failure is a local `ERROR`. The back-buffer format (`R8G8B8A8` or `R10G10B10A2`) is threaded through the context, stage 1 and the swapchain checks. |
| `e7732e1` | Accept a chain whose device object differs from the queue's device when both are on the same adapter. |
| `a27af74` | Answer the swapchain interpolation command list and texture queries with wrapper-owned scratch objects, and acknowledge the game's own `FrameGeneration` dispatch as a no-op. |
| `4b02acd` | Fix a dangling message pointer in the unsupported-flags error path, which printed garbage. |

Offline: the 34-test suite passes, and one check creates a chain through the `New`
form with a 10-bit back buffer plus a 10-bit session. The K70 test now uses
`R16G16B16A16_FLOAT` as its unsupported format, because `R10G10B10A2` is supported.

## Findings, in the order they were found

1. **Wrong base.** The patch branch was cut before the AMD backend was removed.
   The first deployed build still loaded `AMD's .orig`, defaulted `fg.mode` to
   `"amd"` and forwarded Alan Wake 2's `SwapChainNewDX12` create to AMD, which
   succeeded. Routing never ran. The work was redone on `refactor/phase-2`.
2. **Device mismatch (`E_INVALIDARG`).** With routing live, `Swapchain::wrap`
   refused the chain: `chain device …C1760 differs from queue device …B6580`.
   Alan Wake 2 appears to run D3D12 through a layer that makes the chain and
   queue report different device objects. That is an inference from the pointers;
   the layer itself was not identified. The refusal returned an error from
   `ffxCreateContext`, and the game crashed at launch. With the same-adapter rule
   the wrap succeeds and the game runs.
3. **Interpolation queries.** Every frame the game queries
   `SwapChainInterpolationCommandList` (`0x30003`) and
   `SwapChainInterpolationTexture` (`0x30004`) on the swapchain context. They
   were refused; after `a27af74` they are answered with scratch objects. The
   scratch texture reports `R10G10B10A2_UNORM`, 2560x1080, UAV state.
4. **HDR create flag (open, the blocker).** The game's `FrameGenerationCreate`
   always fails with `flags=AsyncWorkloads|HDR`, backbuffer and HUD-less format
   `R10G10B10A2_UNORM`, display 2560x1080, max render 1706x720. The context
   accepts only inverted, infinite and async-workload flags, so
   `FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE` (bit 5) is rejected and no
   generation context exists. The flag was present in a run where the user had
   HDR off, so it does not appear to depend on the game's HDR setting. That is
   one observation, not a tested rule.

The owner chose not to add HDR support on this branch. Result: the game gets a
working `New` swapchain and then a failed `FrameGenerationCreate`.

## Evidence from the last session

- `fg.multiplier = "1.5x"` (ratio 3/2), `present = "vrr"`, `ui.mode = "off"`,
  `ui.scale = "auto"` in the game's `sedici.toml`.
- Swapchain: `buffers=2 size=2560x1080 format=R10G10B10A2_UNORM flags=0x802
  swapEffect=4 windowed=true`, version `0xc01007` (3.1.7).
- Every periodic report in the whole run reads `generated{shown=0 dropped=0}`,
  in menus and in gameplay. Real frames are presented, many dropped by the
  presenter's schedule (`real{shown≈140 dropped≈160}` out of 300), so the
  1.5x schedule is dropping real frames while producing no generated ones. This
  is expected without a generator, but it means this build shows fewer real
  frames than the game rendered, not more.
- During gameplay `latency_ms` rose from about 2 to 40 and `gpu_wait_ms` averaged
  about 21 ms: the game was GPU-bound at the time. Not attributed to Sedici.
- Calls seen: `ffxCreateContext` (New, OK), `ffxQuery` `SwapChainGpuMemoryUsage`
  (refused, the game continues), `ffxCreateContext` (FrameGenerationCreate,
  refused on HDR), then the interpolation queries every frame.
- No `ffxConfigure`, `ffxDispatch` prepare or FG dispatch was ever logged,
  because the generation context was never created. The calls the game makes after
  a successful create are therefore **unknown**.

## What is not established

- That the same-adapter rule is safe. The game queue and the chain's own buffers
  may come from different device objects; a copy or resize across them was not
  exercised past steady presentation.
- That scratch objects are enough for the interpolation queries. The game never
  got to use them with a live context.
- Whether Alan Wake 2 calls `FrameGenerationPrepare` and the present callback in
  the way Cyberpunk does, or relies on the swapchain invoking its dispatch
  callback, which Sedici's design does not do.
- Whether stage 1 gives acceptable output on HDR-encoded input if the flag is
  accepted. Untested.
- `ForHwndDX12` (`0x30006`).
- Full-menu overlay input: `Full menu unavailable: the executable's cursor calls
  could not be intercepted` appeared in the earlier session, and the overlay
  stayed Mini or off.

## Open items for after the refactor

1. Decide HDR: accept `ENABLE_HIGH_DYNAMIC_RANGE` (and then check stage 1 on
   PQ/linear HDR input and the thresholds), or leave Alan Wake 2 without frame
   generation. This gates everything below.
2. Record the call sequence after a successful `FrameGenerationCreate`: configure,
   prepare, callbacks, dispatch.
3. Decide how Sedici honours the game's interpolation list and texture: keep the
   scratch shim, or generate into the texture the game dispatches against.
4. Review the same-adapter device rule against the refactored wrapper and queue
   ownership (P2.4/P2.5 retirement and device checks).
5. Intercept the executable's cursor calls for Full-menu input in this game.
6. Port or drop the branch's commits: the `New` route and the 10-bit plumbing
   belong in the refactor (K14, K100); the scratch-query shim and the device rule
   are provisional.
