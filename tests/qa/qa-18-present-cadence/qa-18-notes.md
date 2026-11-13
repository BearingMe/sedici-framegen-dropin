# QA-18 notes, 2026-11-12

Back to the [QA-18 README](README.md).

## Setup

- Game: Cyberpunk 2077, same spot and in-game settings in every step, game frame generation on for
  steps 2 and 3, no Anti-Lag.
- Display: 75 Hz, VRR. The game is borderless (`fullscreen=no`). PresentMon reports
  `Hardware: Independent Flip`, tearing allowed, sync interval 0. The display model was not recorded.
- Capture: PresentMon 2.6.0 console (Intel-signed release), 120 s per step after a 5 s delay.
- Sedici: `present = "vrr"`, `tear_guard = "scanline"`, `target_fps = 0`. The full logs and CSVs are kept on
  the owner's machine; this folder has excerpts.

## Step 1: baseline, no Sedici (`tools\restore.ps1`)

FG off, no frame cap. `analyze_presents.ps1`:

```
6418 frames over 120 s, average 53,5 fps (column MsBetweenPresents)
Interval: median 18,58 ms, 95th 20,021, 99th 21,091, 99.9th 31,75, max 124,118; 1 percent low 37,78 fps
Stalls of 1.000 ms or more: 0
Cadence: 96,82% of intervals within 10% of the target 18,58 ms (53,8 fps)
```

## Step 2: 2x uncapped, vrr ([log excerpt](qa-18-step2-sedici.log))

`analyze_presents.ps1`:

```
8223 frames over 120 s, average 68,54 fps (column MsBetweenPresents)
Interval: median 13,826 ms, 95th 17,749, 99th 20,229, 99.9th 22,452, max 49,539; 1 percent low 46,24 fps
Stalls of 1.000 ms or more: 0
Cadence: 77,72% of intervals within 10% of the target 13,826 ms (72,3 fps)
```

- The game thread ran at ~47 real fps, so 2x planned ~94 output fps on a 75 Hz panel. The log reports
  `OVER_REFRESH` itself.
- Per 300 jobs, 130-150 real frames were dropped and 0-13 generated frames. `late` was about 100% of
  flips. Latency was ~40 ms.
- **K2** (FG cost): 47.2 real fps against 53.5 with FG off is a 12% cost, missing the 10% criterion by
  2 points. The two runs were not captured at the same moment.
- **K7**: not judged. Output was above refresh, so the drops are the over-refresh behaviour; K7 needs a
  run below refresh.
- The vsync repeat (K15) was not run.

## Step 3: adaptive at 32 real fps (in-game cap)

`multiplier = "adaptive"` was measured twice and then removed (issue #1, commit b405cec). From now on the
step runs 2x at about 32 real fps.

**First build** ([log excerpt](qa-18-step3-adaptive-sedici.log)). This build had an experimental rule that
dropped generated frames later; it was reverted. `analyze_presents.ps1 -TargetFps 73`:

```
7940 frames over 120 s, average 66,19 fps (column MsBetweenPresents)
Interval: median 13,459 ms, 95th 18,55, 99th 19,232, 99.9th 21,462, max 40,897; 1 percent low 49,35 fps
Stalls of 1.000 ms or more: 0
Cadence: 60,67% of intervals within 10% of the target 13,699 ms (73 fps)
```

- `planned_output_fps` was 69.4-70.5 against the target of 73 (#35).
- Per 300 jobs: real frames dropped 22-40, generated 0-6, `late` ~99%, latency ~50.7 ms.
- `MsBetweenDisplayChange` was bimodal: about 4,240 intervals at 12-13 ms and 3,100 at 16-19 ms.

**After the #35 fix** ([log excerpt](qa-18-step3-adaptive-fix-sedici.log), log level 2):

- `planned_output_fps` was 72.75-72.86. Per 300 jobs: ~683 flips (~73 per second), 0 dropped, latency
  ~49 ms.
- `late` was ~500 of 683 flips: about 485 `guard`, 0-1 `gpu`, 7-18 `carry`.
- Trace (flip minus target, about 40 windows of 48 outputs): median 2-7 ms, p90 8-11 ms, occasionally
  ~20 ms. This is #39.
- A 20 s PresentMon spot check is not usable: the game was composed (owner in another window) for the
  first ~7 s.
- No full 120 s capture was taken. The owner reported ~66 fps on screen, then stable.

**Decision:** the owner removed adaptive. Issue #1 is closed by the removal; #36 and #39 stay open.
