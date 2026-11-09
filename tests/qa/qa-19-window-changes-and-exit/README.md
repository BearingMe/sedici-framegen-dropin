# QA-19 Window changes, settle time, exit and menu clicks (retest)

**Result:** **Not run.** Needs the game; prepared in P4.7.

Back to the [test table](../README.md). Bugs are in [known-issues.md](../../../docs/refactor/known-issues.md); requests in [feature-requests.md](../../../docs/refactor/feature-requests.md). Verdict rules: [phase4-game-checklist.md](../../../docs/refactor/phase4-game-checklist.md).

Retests QA-11, QA-13 and QA-07 on the build with the longer waits (K98: 250 ms, K127: 1000 ms), the
early settle reports (K123), the slow-call log (K121, K122), Alt+F4 passing through the Full menu (K126)
and the target-fps menu.

## Before every run

1. Close the game. `tools\deploy.ps1` for the game (QA-01 rules), note the DLL hash it prints.
2. **Delete `sedici.log`** beside the game (K101). The first lines of the new log carry the build date.
3. `sedici.toml`: `[log] level = 1` (level 2 only where a step says so). Keep a copy of the file you used.
4. Note the display model, refresh rate, VRR on or off in the driver, and the game's resolution and window mode.

Cyberpunk 2077, RTSS **off** (the VRR presenter) for the first pass, and RTSS **on** (the vsync
fallback) for the second, as in QA-11.

## Steps

1. Start, reach gameplay, wait 30 seconds. Then, one change every 30 seconds: fullscreen, windowed,
   1920x1080, 1280x720, back to the desktop resolution, borderless. Write the order down.
2. Alt-tab away and back twice.
3. Open the Full menu, press Alt+F4. The game must close. (K126)
4. Start again, set the game to 1920x1080 on the 2560x1080 desktop, open the Full menu, click each
   control, drag the target-fps slider and press Apply, move the cursor to every screen edge, close the
   menu. Clicks must land under the cursor and the cursor must be released afterwards. (K36)
5. Quit once from the game's menu with the Mini menu open, once with the Full menu open.
6. Repeat steps 1 to 5 with RTSS on.

## Read from the logs (`tools\analyze_log.ps1`)

- `prepareSlotRejections` must be 0 (K98).
- `destroyRefusals` must be 0 and every `ffxDestroyContext` `result=0` (K127).
- Each gap of a second or more is listed; a `slow call` line at the same time means Sedici held the game's
  thread (K122). The early `frames=60` lines after each change give the settle time of the real rate:
  3 seconds or less (K123). If it is longer, the first and steady real fps in the output are the finding.
- `overlayDisabled` must be 0 (K124); if not, the log line now has the exception text.

## Results

| Pass | prepare-slot rejections | destroy refusals | longest gap | settle after a change | Alt+F4 with Full | clicks at 1080p |
| --- | --- | --- | --- | --- | --- | --- |
| VRR | not run | not run | not run | not run | not run | not run |
| RTSS (vsync) | not run | not run | not run | not run | not run | not run |
