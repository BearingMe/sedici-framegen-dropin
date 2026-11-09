# QA-17 Alan Wake 2: generation, HDR and the Full menu

**Result:** **Not run.** Needs the game; prepared in P4.8.

Back to the [test table](../README.md). Bugs are in [known-issues.md](../../../docs/refactor/known-issues.md); requests in [feature-requests.md](../../../docs/refactor/feature-requests.md). Verdict rules: [phase4-game-checklist.md](../../../docs/refactor/phase4-game-checklist.md).

Prepared for the build that routes `FrameGenerationSwapChainNewDX12`, accepts the 10-bit back buffer
and the HDR create flag, and answers the swapchain's interpolation queries (K14, K100, K129-K132).
QA-03 only asks whether the create is accepted; this test asks whether frames are generated and
whether the menu works.

## Before every run

1. Close the game. `tools\deploy.ps1` for the game (QA-01 rules), note the DLL hash it prints.
2. **Delete `sedici.log`** beside the game (K101). The first lines of the new log carry the build date.
3. `sedici.toml`: `[log] level = 1` (level 2 only where a step says so). Keep a copy of the file you used.
4. Note the display model, refresh rate, VRR on or off in the driver, and the game's resolution and window mode.

## Steps

1. Start Alan Wake 2 (HDR off in the game's options). Reach the main menu and then gameplay.
   Expected in the log: `created swapchain (new)`, then `ffxCreateContext ... FrameGenerationCreate ... result=0`.
2. Play five minutes in a scene with camera movement. Generated frames must be visible (`generated`
   above zero in the `overlay present frames=...` lines).
3. Take two screenshots of a slow camera pan, one with frame generation off (Alt+F10 cycles the
   overlay; the Full menu has the generation switch) and one on, same spot.
4. Open the Mini menu and then the Full menu. Click every control once, drag a slider, close it.
   The log line `overlay cursor interception installed/unavailable` says what was found.
5. Turn the game's HDR on (restart if the game asks) and repeat steps 1 to 3. Compare the screenshots:
   generated frames must not be darker, brighter or tinted differently from the real ones.
6. Change the resolution and the window mode once each (the game's options). Frames must keep
   presenting; the log must not show `device removed` or `DX12 error`.
7. Quit from the game's menu. The log must end with both `ffxDestroyContext` calls `result=0`.

## What to keep in this folder

`qa-17-sedici.log` (an excerpt, see the tests/qa README; the full log stays local), the output of
`tools\analyze_log.ps1 -Log <full log>` pasted below, the screenshots, the game build version, the
display and driver notes, and the `sedici.toml` used.

## Pass

Steps 1, 2, 4 and 7 as stated; step 3 and 5 show no colour or brightness shift; step 6 survives. If the
Full menu is unavailable the pass still holds for generation, and the interception line is the finding.

## Results

| Step | Result | Notes |
| --- | --- | --- |
| 1 create accepted | not run | |
| 2 generation visible | not run | |
| 3 FG on/off screenshots | not run | |
| 4 Mini and Full menu | not run | |
| 5 HDR on | not run | |
| 6 resolution and window mode | not run | |
| 7 clean exit | not run | |

## Analyzer output

(paste `tools\analyze_log.ps1` output here)
