# 016: Alan Wake 2 on the routed swapchain: it boots, then the scene is black

The first run of the build that routes `SwapChainNewDX12` (P4.8, QA-03 second run, 2026-11-09). Evidence:
[tests/qa/qa-03-alan-wake-2-swapchain/qa-03-sedici-2.log](../../tests/qa/qa-03-alan-wake-2-swapchain/qa-03-sedici-2.log)
(excerpt of a 250-line log). Reported by the owner: "game booted, black screen when rendering the scene
(Cyberpunk is working normally)".

**Status:** the cause is not established. One defect that fits was found and fixed (K138); whether it is
the cause needs the next run (QA-17).

## What worked

| Step | Log |
| --- | --- |
| `New` create | `created swapchain (new)`, `FrameGenerationSwapChainNewDX12 result=0`, R10G10B10A2, 2560x1080, flags 0x802 |
| Device objects | `chain device ... differs from queue device ...; same adapter, using the queue's device` (K130 rule used) |
| Menu, uncapped | about 150 real fps for 40 s: `vrr present jobs ... real{shown~150 dropped~150}`, `refresh_hz=75`. The game is uncapped in its menu, so half of the real frames are dropped by the presenter (the K4 shape) |
| Generation context | at 48.0 s `FrameGenerationCreate` with `flags=AsyncWorkloads\|HDR`, R10G10B10A2: result 0 (K129 accepted). It took 427 ms (`slow call op=create`) |
| Frame sequence | per frame: `Prepare` (ours), `Configure` (names the swapchain), query interpolation list, query interpolation texture, `FrameGenerationDispatch` into that list with `numGeneratedFrames=1` and the interpolation texture as `output[0]`. The game's own dispatch is acknowledged (K131) |

## What went wrong

- Frames 5660 to 5666: prepare, configure, both queries and the game dispatch all succeed. At frame 5667
  the first `Prepare` is refused with `prepare slot is still recorded or in flight` (`E_SLOT_BUSY`), and every
  `Prepare` after it is refused too (61 rejections logged, the rest suppressed), until the run ends at 75 s.
- The last interpolation query in the log is at 48.97 s. `Configure` is still answered at 49.2 s. No
  query and no game dispatch follows, so the thread that makes them stopped, while another thread keeps calling
  `Prepare` about 62 times a second.
- No periodic `vrr game thread` or `overlay present` line follows 45.6 s, in 30 seconds. At 150 fps one is
  written every two seconds, so the wrapper stopped receiving presents (or was blocked in one) from about the
  moment the scene started. That is the black screen.
- A call that never returns writes no `slow call` line, so a blocked call looks like silence.

## The defect found (K138)

`Swapchain::present` holds the swapchain mutex for the whole present, including the limiter sleep, the
drain wait and the presenter's back-pressure wait. `interpolation_list` and `interpolation_texture` took the
same mutex, so the game's submitting thread, asking for them every frame, waited behind a Present on
another thread. The new check in `sedici_fgcheck --routing` reproduces it: with a Present sleeping 600 ms, the
queries took 499 ms on the old code and 1 ms after the fix. A permanent stop needs Present to wait for work
that the blocked thread would have submitted (the prepare list), which is what the slot rejections after
frame 5667 look like. That chain is a hypothesis; the log cannot show it.

## What the next run will show (added with the fix)

- `fg: slow present took N ms (waited M ms for the swapchain lock)` when a Present runs 250 ms or more.
- The queries no longer share that lock.
- If the screen is still black, the log should now say whether Present returns at all.

## Still open

- `Prepare` for frame N+1 arrives before the present of frame N has released the single prepare slot in
  this game's pipelining. Sedici answers that with an error to the game each time (K139).
- Whether the game expects the swapchain itself to present the interpolation texture it recorded into: Sedici
  discards what the game records there and generates its own frame.

## Third run, review against `aw2-patch`, and what the SDK documentation says (2026-11-09)

**Third run (QA-03, `qa-03-sedici-3.full.log`).** Still black. The queries no longer block (they continue
past 35.9 s), so K138 is not the whole story. The generation context is created at 34.9 s; the first
`Prepare` (frame 4510) succeeds, the next one is refused as `E_SLOT_BUSY` and so is every one after it, until
the run ends at 51.6 s. The last periodic present line is at 34.3 s, so the wrapper stops receiving
presents (or never finishes one) from the moment generation starts. The `build` stamp still reads 09:35:20: it
comes from `config.cpp`'s compile time, so it does not say which DLL ran (K101). A generation that fails
inside Present was invisible at log level 1; `Context::fail` now logs at level 1, so the next log carries the
reason.

**Review against the `aw2-patch` worktree.** The worktree is clean at `fffa3e6`, the tip that was ported. The
port is faithful: the `New` route, 10-bit formats, the same-adapter rule, the scratch list and texture, and
the dispatch acknowledgement match. Differences are deliberate: the `ForHwnd` route, `GetVersions` for both
forms, error codes, the scan of every module for cursor imports, and K138's separate lock. The branch never
ran with a generation context (the HDR flag was refused there, K129), so the black screen is new ground, not
a regression from the branch.

**What the SDK documentation says (context7, `/websites/gpuopen_manuals`).**

- The swapchain hands the game a command list (`ffxGetInterpolationCommandlist`) and a texture that
  represents the back buffer (`ffxGetFrameinterpolationTexture`); the game records its interpolation
  dispatch into them, and the *swapchain* submits that list and presents the result. So the game never
  executes the list; our scratch shim leaves a list that is never submitted, which is consistent, but it means
  the frame the game believes it generated is not shown, and Sedici's own generation has to cover for it.
- "When using frame generation, a new preparation dispatch must be called every frame", at the place of the
  upscaling dispatch. AMD's swapchain therefore keeps per-frame state for each prepare; Sedici keeps one slot
  (K139). A game that prepares frame N+1 before frame N has been presented is normal for AMD's design and is
  refused by ours.
- The present callback runs before presentation (UI composition); Alan Wake 2 sets `presentCallback=none`.

**What follows.** K139 is now the lead: a single prepare slot against a per-frame prepare. If the next log
shows the first generation failing for a reason of its own (the level-1 `rejected:` line), that is fixed
first. If the refusals are only the result of the first generation never completing, the slot needs to be a
short ring of prepared frames (a change to `runtime/context`, for the owner to approve).
