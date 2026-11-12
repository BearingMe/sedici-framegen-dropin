# QA-18 Present-interval capture: cadence, FG cost, FG-off throttling

**Result:** **Not run.** Needs the game and a capture tool; prepared in P4.6.

Back to the [test table](../README.md). Bugs are in [known-issues.md](../../../docs/refactor/known-issues.md); requests in [feature-requests.md](../../../docs/refactor/feature-requests.md). Verdict rules: [phase4-game-checklist.md](../../../docs/refactor/phase4-game-checklist.md).

QA-04, QA-05 and QA-10 asked for present-interval captures that were not taken. This test takes them
once, with the analyzer that reads the file, and answers K1, K2, K4, K5, K7, K8 and K15.

## Before every run

1. Close the game. `tools\deploy.ps1` for the game (QA-01 rules), note the DLL hash it prints.
2. **Delete `sedici.log`** beside the game (K101). The first lines of the new log carry the build date.
3. `sedici.toml`: `[log] level = 1` (level 2 only where a step says so). Keep a copy of the file you used.
4. Note the display model, refresh rate, VRR on or off in the driver, and the game's resolution and window mode.

Capture tool: PresentMon or CapFrameX writing a CSV with a `MsBetweenPresents` (or `FrameTime`) column.
Use the same scene, the same camera path and the same in-game settings in every step.

## Steps

Each step is 2 minutes after the scene has settled. Run
`tools\analyze_presents.ps1 -Csv <file> -TargetFps <output rate you expect>` on each CSV and
`tools\analyze_log.ps1 -Log <log>` on each log.

1. **Baseline, no Sedici.** `tools\restore.ps1`, play the scene, capture. This is the reference for steps 2 to 4.
2. **2x, uncapped** (`fg.multiplier = "2x"`, `fg.target_fps = 0`, `fg.present = "vrr"`). Capture. Compare the real
   fps with step 1 (K2) and read `dropped` in the periodic lines (K7). Repeat once with `fg.present = "vsync"`
   and compare the GPU time (K15).
3. **Adaptive at about 32 real fps** (`fg.multiplier = "adaptive"`, `[log] level = 2`, cap the game or raise
   the settings until it runs near 32). Capture and run the analyzer with `-TargetFps` set to the adaptive
   target (73 at 75 Hz). Read the `vrr present` lines for late flips by cause (`late=N{gpu carry guard}`) and
   `planned_output_fps` in the `vrr game thread` lines, and keep the `vrr trace` lines (issue #1, K1).
4. **Frame generation idle.** Open a menu in which Cyberpunk stops generating, game uncapped, capture
   30 seconds; repeat the same menu with `tools\restore.ps1` (K4).
5. **Tear guard off** (`fg.tear_guard = "off"`, VRR on, 2x). Capture 1 minute and keep the `vrr present`
   lines (K5).
6. **Latency waitable.** 2x, VRR, 1 minute. The game's own frame-time stability is the measure: the
   analyzer's 99th percentile against the median (K8). Repeat with `fg.present = "vsync"` for comparison.
7. **Target fps.** `fg.target_fps = "auto"`, 2x, 2 minutes. The analyzer's cadence share must be 95% or
   more within 10% of the target (the `auto` value for your display is in the log's `effective:` lines).
   Then `fg.target_fps = 60`, same check at 60.

## Pass

The numbers in the verdict table of the checklist; every step has its CSV and log in this folder. A
step that misses its number is a finding, not a failed test: write it down with the figures.

## Results

| Step | Entry | Result | Numbers |
| --- | --- | --- | --- |
| 1 baseline | reference | not run | |
| 2 2x uncapped, vrr and vsync | K2, K7, K15 | not run | |
| 3 adaptive at 32 fps | K1 | not run | |
| 4 FG idle against baseline | K4 | not run | |
| 5 tear guard off | K5 | not run | |
| 6 latency waitable | K8 | not run | |
| 7 target fps auto and 60 | K3, FR-05 | not run | |
