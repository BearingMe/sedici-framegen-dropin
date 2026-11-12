# 009: Output multipliers for the vrr presenter

> **Update (2026-11-12):** the adaptive mode described below was **removed** by the owner after
> QA-18 step 3 (issue #1, commit b405cec). `multiplier = "adaptive"` now logs a note and falls
> back to `"2x"`. The fixed multipliers are still current. The rest of this finding is kept
> unchanged as the record of what adaptive did.

> **Status (2026-11-09): valid, and implemented.** The line below saying nothing has run in
> the game predates the three game sessions recorded further down. The multipliers, the
> adaptive mode and the latency fixes are the current implementation. Finding number 004
> exists only on the branch `exp/fg-1.5x` (commit 84f8060, "why 1.5x frame dropping is
> not supported by the FFX API"); its conclusion is superseded by the scheduler described
> here.

Adds `[fg] multiplier` ("2x", "1.75x", "1.5x", "1.25x", "adaptive") and
`[fg] target_fps` to `present = "vrr"` on `experiment/cobalt-otter`. This file
records the design, the research it rests on, and the offline results. Nothing
here has run in the game yet.

## Basis in the references

- Mob-FGSR interpolates at any phase alpha in (0, 1) between I-frames 0 and 1
  (`references/papers/mob-fgsr.md`, Sec. 3 and Fig. 3); the quadratic
  trajectory takes alpha directly, and Sedici's shader already carries it
  (`delta` = alpha, alpha/2, 3alpha/2, alpha^2/2).
- The upstream renderer schedules several generated frames per rendered frame
  as `delta = j / (n + 1)` (`references/sources/mobfgsr`, commit `df7ed20`,
  `src/offscreen_renderer.cpp:122`); GFFE uses the same `alpha = j / (n + 1)`
  for extrapolated frames (`references/papers/realtime-fg.md`).
- The paper's suggestion of extrapolating the first generated frame for latency
  and interpolating the second is not used: Sedici stays interpolation-only.

## Design

**Even pacing, chosen over showing every real frame.** At ratios below 2 the
output grid does not line up with real frames. Showing every real frame and
inserting midpoints in some intervals gives on-screen durations like
1/2, 1/2, 1 real interval (judder). Instead every output sits on an even grid in
content time (the game's Present clock); a grid point between real frames N-1
and N is generated at its exact alpha, and a point on N is the real frame.

| Multiplier | Pattern | Real frames shown on their own | Smallest alpha | Latency vs 2x |
| --- | --- | --- | ---: | --- |
| 2x | alpha 1/2, real | every one | 1/2 | - |
| 1.75x | 7 outputs / 4 real frames | 1 in 4 | 1/7 | + 0.36 real frame |
| 1.5x | 3 / 2 | 1 in 2 | 1/3 | + 0.17 |
| 1.25x | 5 / 4 | 1 in 4 | 1/5 | + 0.30 |
| adaptive | grid every 1/target s | when within 1/4 slot | any | up to + 0.5 |

Latency follows from interpolation: a point at alpha can only be shown after
real frame N exists, so the schedule runs (1 - smallest alpha) real frames
behind, against 1/2 at 2x.

**Pieces.**

- `OutputSchedule` (`src/fg_pacing.*`): pure planner. Fixed ratio p/q puts
  points at m*q/p real frames, repeating every q frames with p outputs. Adaptive
  steps a 1/target grid, snaps to the real frame within a quarter slot, skips
  points the game has already passed after a hitch, and resets after a 0.25 s
  gap. Never more than two generated frames per real frame.
- `Stage1::generate` takes up to two `GeneratePhase`s (output + alpha) per real
  frame and records them in one command list. Each phase owns its five descriptor
  blocks and writes its alpha into the root constants; history advances once.
- The swapchain generates every planned phase in one submission. A real frame
  with nothing to show (adaptive target below the real rate) still runs one phase
  so the generator's history stays continuous. Generated images come from two
  sets of two so the presenter can still copy the previous frame's.
- The presenter flips each output at `content + latency`. Latency is learned
  from the first output of each frame: how long after its content time it was
  finished on the GPU, rising by half the gap and falling by 2% per frame. At 2x
  this reproduces the previous behaviour (midpoint as soon as ready, real frame
  half an interval later). An output is dropped when a later one is already due.
- `"vsync"` and `"game"` keep presenting 2x from the game thread and log if a
  different multiplier was configured.

## Offline results

`sedici_fgcheck --present`, RTSS closed:

| Check | Result |
| --- | --- |
| Phases 1/3 and 2/3 from one prepare, one command list (PSNR vs true scene; nearest real frame in brackets) | rightward pan 42.95 / 42.92 dB (19.16 / 19.12); leftward pan 40.60 / 38.63 dB (22.18 / 22.19); diagonal thin bar 47.14 / 47.58 dB (26.44 / 26.56) |
| Schedules at 48 real fps | 2x, 1.75x, 1.5x, 1.25x: exact output counts, an even grid to 1e-6 s, at most two generated per real frame; adaptive 48 -> 72: 7190-7210 outputs per 4800 real frames, every output within a quarter slot of the grid |
| Presenter, windowed test chain at ~35 fps | 2x: 79 of 79 planned presents, nothing dropped; 1.5x: 59 of 59 |

## First game session: 1.5x collapsed to ~25 output fps

`multiplier = "1.5x"`, `limiter.fps = 0` (game uncapped), scan-line guard,
75 Hz panel. Log windows of 300 real frames:

| Window | Duration | Game thread blocked on the presenter | Learned latency |
| --- | --- | --- | ---: |
| first | ~10 s (~30 real fps) | 271/300, avg 33 ms | 60 ms |
| later | ~18 s (~17 real fps, ~25 output fps) | 283-297/300, avg 54 ms | 113 ms |

Two causes:

1. **Over the refresh rate.** Uncapped, real fps x 1.5 asked for more frames
   than the panel can show; the guard holds each flip until the scan ends, so
   the presenter needed longer per real frame than the game did.
2. **Latency feedback loop (bug).** Latency was learned from "image ready" as
   measured when the presenter reached a frame, so time spent queued behind
   earlier frames counted as GPU latency. Higher latency moved flips later,
   lengthened the queue, and raised latency again, until the game thread was
   blocked on almost every frame. The stale-output drop never fired, because
   every target moved out with the latency.

Fix: latency is now `structural + GPU`. The structural part is exact, the
distance on the content clock from the game's Present back to the frame's
earliest output (1/2 a real frame at 2x, up to 2/3 at 1.5x). The GPU part is
Present-to-ready, raised only when the presenter actually waited for the GPU;
a frame already finished when picked up only bounds it from above and can only
lower it. An intermediate fix that applied the upper-bound rule to the whole
latency starved it at 0 and dropped every generated frame offline; the split
avoids both.

Over the refresh rate the presenter drops outputs it cannot show instead of
collapsing. The game-thread log line reports `real_fps`,
`planned_output_fps` and `refresh_hz`, and adds `OVER_REFRESH` with the
`limiter.fps` to use when the plan exceeds the panel.

Offline after the fix: 2x 79/79 and 1.5x 59/59 planned presents at ~35 real
fps with ~20 ms latency. (An earlier version of this paragraph said the
overload test game was "held to the panel's pace at 15.8 ms per frame"; that
was Windows' 15.6 ms `Sleep` granularity, not Sedici. The check now measures
time spent inside Sedici instead; see the adaptive section.)

## Second game session: steady at 72, two transient collapses

`multiplier = "1.5x"`, `limiter.fps = 48`, same panel. Capture
`2026-11-06-sedici-vrr-1.5x-48` (log windows of 300 real frames):

| Phase | Real fps | Planned output fps | Generated / real shown | Late | Game thread blocked | Latency |
| --- | ---: | ---: | --- | ---: | --- | ---: |
| steady gameplay (most windows) | 48.0 | 72.0 | 300 / 150, none dropped | 0 | 0/300 | 15.3 ms |
| menus / loading (FG off) | 48 | 48 | 0 / 300 | 0 | 0/300 | ~0.5 ms |
| collapse near frame 3600 | 24 | 36 | 293 / 154 | 333 | 273/300 | 74 ms |
| collapse near frame 6000 | 19 | 29 | 300 / 150 | 331 | 278/300 | 98 ms |

The steady state is what the design predicts: 2/3 of a real frame (13.9 ms)
plus ~1 ms GPU. Both collapses follow GPU stalls of 70-113 ms, and the latency
model still had two ways to amplify one:

1. The structural part is proportional to the real interval and adopted a
   larger value at once. One 150 ms hitch made it ~100 ms; flips then took that
   long, the game blocked on the presenter, its intervals grew, and the
   structural part stayed high.
2. A single GPU spike raised the GPU part by half the spike, decaying at 2% per
   frame.

Fix: the schedule treats an interval over 2.5x the recent one as a
discontinuity and shows only that real frame; the structural part is the
maximum of the last 8 frames (covers every fixed pattern) and the GPU part the
second highest of the last 16 measurements (one spike is ignored). A hitch now
clears within 8-16 frames instead of sustaining itself.

## Adaptive sessions: stutter and input lag at low real fps

`multiplier = "adaptive"`, target 72 on the 75 Hz panel, limiter off, three
captures in one session (`2026-11-06-sedici-vrr-adaptive*`). The user saw
stutter and input lag.

| Run | Real fps | On screen 12.5-14.5 ms | < 2 ms (back-to-back) | > 20 ms | Latency | Game blocked on presenter |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| high input | 71-80 | 80.9% | 2.5% | 3.3% | 19-25 ms | 0-227/300, ~5 ms |
| lower input | 34-44 | 49.5% | 6.3% | 9.9% | 42-60 ms | 205-269/300, 9-16 ms |
| variable (car) | 37-47 | 61.5% | 4.6% | 12.2% | 38-58 ms | 152-249/300 |

Causes and fixes:

1. **Back-to-back flips.** Right after `Present` returns the flip has not
   latched, so the scan line still reports the previous blank and the guard
   opened for a second flip ~1 ms later. The scan-line guard now also holds
   until just before the previous image's scan can have ended
   (`last flip + refresh - lead`); the log adds `back_to_back`.
2. **Game waiting on the presenter.** Jobs read the game's own backbuffers and
   two generated sets, so only one older job could be outstanding; adaptive at
   ~40 fps needs up to a real frame of latency, so the game blocked on most
   frames. Each frame is now copied into a Sedici-owned snapshot right after
   generation (one full-resolution copy on the game queue), generated images
   and generation command lists use a ring of three slots fenced per slot, and
   two older frames may be in flight.
3. **Latency estimate.** The GPU part is now the median of the last 16
   measurements + 1 ms instead of the second highest; a slow frame is late
   rather than raising latency for 16 frames.
4. **Adaptive snap.** A real frame replaces a grid point only within 1/10 of
   an output period (was 1/4, i.e. 3.5 ms off the grid at 72).
5. **Below 20 real fps** (intervals over 50 ms) the schedule shows real frames
   only; latency reached 157 ms at 14 fps during transitions.
6. `OVER_REFRESH` is no longer reported while frame generation is off and no
   longer suggests "adaptive" when already adaptive.

Offline (`sedici_fgcheck --present`, RTSS closed): no back-to-back flips in
any variant; adaptive at ~32 real fps presents 87 outputs for 40 frames with
the test game spending 0.5 ms per frame inside Sedici (latency waitable plus
`Present`). The two-frames-in-flight change first reset the single generation
allocator while the previous frame's generation was still executing; the debug
layer reported it and generation now uses a per-slot list.

Running with RTSS closed also exposed two checks that fail identically at
`a87d145`, before this change: `generated presentation` on the `"vsync"` path
(2 of 5 midpoints shown, 3 skipped as late) and `session` (the frame 2 midpoint
at 16.02 dB). RTSS's overlay had made the session check fail earlier, at frame 0,
in every previous run. Both are open.
