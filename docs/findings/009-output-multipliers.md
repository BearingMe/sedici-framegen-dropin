# 009: Output multipliers for the vrr presenter

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

Over the refresh rate the game is now held to the panel's pace (about 1.5
refreshes per real frame at 1.5x), like vsync with frame generation, instead
of collapsing. The game-thread log line reports `real_fps`,
`planned_output_fps` and `refresh_hz`, and adds `OVER_REFRESH` with the
`limiter.fps` to use when the plan exceeds the panel.

Offline after the fix: 2x 79/79 and 1.5x 59/59 planned presents at ~35 real
fps with ~20 ms latency; at ~160 requested real fps (1.5x overload) the test
game ran at 15.8 ms per frame against a 20 ms panel pace, not slower.

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

Running with RTSS closed also exposed two checks that fail identically at
`a87d145`, before this change: `generated presentation` on the `"vsync"` path
(2 of 5 midpoints shown, 3 skipped as late) and `session` (the frame 2 midpoint
at 16.02 dB). RTSS's overlay had made the session check fail earlier, at frame 0,
in every previous run. Both are open.
