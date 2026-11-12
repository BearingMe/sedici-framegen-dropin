# QA-18 Present-interval capture: cadence, FG cost, FG-off throttling

**Result:** **Partly run** (2026-11-12, Cyberpunk 2077, 75 Hz VRR panel, borderless, PresentMon 2.6): steps 1 and 2,
and step 3 as it was then (adaptive at 32 real fps). That run led to the removal of `multiplier = "adaptive"`
(issue #1). Step 3 is now defined as 2x at about 32 real fps and **has not been run yet**; steps 4 to 7 have not
been run.
Figures, analyzer output and setup are in [qa-18-notes.md](qa-18-notes.md). The log excerpts are
[step 2](qa-18-step2-sedici.log), [step 3 adaptive](qa-18-step3-adaptive-sedici.log) and
[step 3 adaptive after the #35 fix](qa-18-step3-adaptive-fix-sedici.log). The CSVs and full logs stay on the owner's
machine.

Back to the [test table](../README.md). Bugs are [GitHub issues](https://github.com/BearingMe/sedici-framegen-dropin/issues) (the old K list is archived in [known-issues.md](../../../docs/archive/known-issues.md)). Verdict rules: [phase4-game-checklist.md](../../../docs/refactor/phase4-game-checklist.md).

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
3. **2x at about 32 real fps** (`fg.multiplier = "2x"`, `[log] level = 2`, cap the game at 32). Capture and run
   the analyzer with `-TargetFps 64`. Read the `vrr present` lines for late flips by cause
   (`late=N{gpu carry guard}`) and `latency_ms`, and keep the `vrr trace` lines (issue #1, K1). Until
   2026-11-12 this step measured `multiplier = "adaptive"`, which was removed after it.
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
| 1 baseline | reference | run 2026-11-12 | FG off, uncapped: 53.5 fps average, median 18.58 ms, 99th 21.09 ms, 1% low 37.8 fps, cadence 96.8% |
| 2 2x uncapped, vrr and vsync | K2, K7, K15 | vrr only, 2026-11-12 | 47.2 real fps (FG-off 53.5: 12% cost, K2 misses 10% by 2 points); 2x over the 75 Hz refresh (OVER_REFRESH): 130-150 of 300 real frames dropped, so K7 needs a run below refresh; latency ~40 ms. vsync not run |
| 3 adaptive at 32 fps (now: 2x at 32 fps) | K1 | adaptive run twice 2026-11-12, then removed | First build: 66 fps shown of 70 planned, real dropped 22-40 per 300, late ~99%, 50.7 ms. After #35 fix: 72.8 planned, no drops, 49 ms, but ~73% of flips held 5-10 ms past target by the tear guard (#39). 2x at 32 fps not run yet |
| 4 FG idle against baseline | K4 | not run | |
| 5 tear guard off | K5 | not run | |
| 6 latency waitable | K8 | not run | |
| 7 target fps auto and 60 | K3, FR-05 | not run | |
