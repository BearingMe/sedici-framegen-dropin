# 008: Where frame-generation tearing came from

Dedicated finding for the tearing reported during the Phase 1 Mob-FGSR game
sessions on `experiment/cobalt-otter`. The chronological integration history is
in [007](007-fg-baseline.md); this file isolates the tearing cause, the evidence
that identifies it, what was ruled out, and the option that removes it.

All measurements below are from authorized Cyberpunk 2077 sessions at one test
location, using `tools/capture.ps1` with the AMD-bundled PresentMon 2.x default
metric set (`DisplayedTime` per frame). Raw CSVs and logs stay in `captures/` and
are not committed.

## Symptom as reported

Three distinct observations, in the order they appeared:

1. Occasional tearing at the **top** of the screen, at 72 output fps with a 36 fps
   in-game cap.
2. After a pacing correction, the FPS regression disappeared but tearing remained.
3. After the midpoint-readiness correction, tearing moved to **random
   middle/bottom positions**, with no frame ever displayed for less than a
   millisecond.

Those are two different faults, not one that moved.

## Cause 1: the midpoint was not rendered when the real frame was presented

The wrapper generated the midpoint inside the game's `Present` call, so its
command list was queued after everything the game had submitted for that frame,
on the same direct queue. Its copy therefore only ran once that frame's GPU work
retired — which is approximately when the *next* application `Present` arrives.

The pacing decision spaced the two `Present` calls by half an interval in **CPU
time** (`realDeadline = entry + interval/2`). CPU spacing says nothing about GPU
completion, so both outputs became presentable together.

Measured with a diagnostic-only build that sampled the midpoint copy's fence at
the real `Present`, without waiting or changing what was presented:

| 300-cycle window | midpoint not GPU-ready | previous cycle still unretired |
| --- | ---: | ---: |
| 3,600-3,900 | 295/300 | 0/300 |
| 4,200-4,500 | 296/298 | 2/300 |
| 4,500-4,800 | 298/300 | 0/300 |
| 4,800-5,100 | 295/300 | 0/300 |
| 5,100-5,400 | 296/300 | 0/300 |

In about 98% of cycles the midpoint did not exist on the GPU when the real frame
was presented. With `SyncInterval=0`, DXGI is documented to "cancel the remaining
time on the previously presented frame and discard this frame if a newer frame is
queued", so both presents were consumed by one refresh: `DisplayedTime` alternated
**27.6 ms / 0.2 ms** and 50.0% of all frames were under 1 ms. At 75 Hz, 0.2 ms is
roughly the top 16 scanlines of a 1080p panel, which matches the reported tear at
the top.

Root cause, precisely: **generation was scheduled at the latest point in the
frame and paced by CPU time, so the generated image never had its own refresh.**

## Cause 2: flips were not aligned to the panel scan

After generation was made ready in time (`mid_not_ready` 0/300, no sub-millisecond
frames), tearing persisted at random positions. The game presents with
`SyncInterval=0` and `DXGI_PRESENT_ALLOW_TEARING`, which permits a flip to take
effect partway through a scan. Nothing in the pipeline aligned a flip with a
vertical blank, and at 72-73 output fps against a 75 Hz panel the phase drifts, so
the tear line lands at a different row each time.

Evidence that this is presentation alignment and not image content:

| Configuration | presents/s | `DisplayedTime` | Result |
| --- | ---: | --- | --- |
| game flags, in-game cap 36 | 72.0 | 27.6 / 0.2 ms | truncated midpoint, tear at top |
| game flags, limiter 36.5 | 73.0 | ~21 / ~6.4 ms | no truncation, random tears |
| `SyncInterval=1`, limiter off, in-game cap ~37.5 | 74.9 | **13.31-13.40 ms, every frame** | no tearing, user-confirmed |

A frame displayed for exactly one refresh, every time, is the signature of a flip
landing at a scan start. Uneven but non-zero durations are the signature of flips
landing wherever the scan happens to be.

## What was ruled out

- **Swapchain buffer count.** The game's chain has 3 buffers. Presenting twice per
  cycle does not exhaust it.
- **The game's frame-latency waitable object.** It is created with
  `FRAME_LATENCY_WAITABLE_OBJECT` and `SetMaximumFrameLatency(1)`, and the wrapper
  never consults it, but `drain()` waits were 0-2 per 300 cycles, so it was not
  the observed blocker.
- **A PresentMon measurement artefact.** `DisplayedTime` and the GPU-side probe
  agreed: no sub-millisecond frames once the midpoint was ready, and a clean
  13.33 ms cadence once presentation was vblank-locked.

## What did not work

**CPU-side vblank arming.** A build that waited until ~1.2 ms before the next
blank reported by `GetFrameStatistics` hit its own deadline (`align_misses=5/600`)
but did not produce refresh-quantised display times (median distance from a
13.33 ms multiple: 3.92 ms). It also cost ~4 ms per frame inside `Present`, taking
the real rate from 36.5 to 31 and making the limiter late on 285-305 of 300 frames.
`SyncQPCTime` describes the last *displayed* flip, not when a newly queued flip
will be scanned, so it cannot be used to predict the target blank from the CPU
thread that is also trying to keep the frame budget.

**`SyncInterval=1` together with the limiter.** Both waits stack: the limiter
sleeps to its deadline and each of the two presents then blocks for a blank. That
configuration produced 20 real fps and a 13.3 / 38.3 ms display pattern. The
working combination is `SyncInterval=1` **without** the limiter, with the game
capping itself at or below half the panel refresh.

## Resolution

`[fg] present = "vblank"` (default): the wrapper presents both outputs with
`SyncInterval=1` and without `DXGI_PRESENT_ALLOW_TEARING`, so each output takes its
own vertical blank. `[fg] present = "game"` keeps the previous pass-through for
lower latency and accepts tearing. `SEDICI_FG_SYNC` overrides either for A/B runs.

Costs, measured:

- Up to one refresh of added latency.
- The real rate must be at or below half the panel refresh (36-37.5 fps on the
  75 Hz test display). Above that, a pair cannot own two blanks and frames are
  skipped.
- At 74.9 output fps against 75 Hz the cadence is 13.33 ms per frame with no
  dropped presents in the capture.

## Reproducing

1. `fg.mode = "sedici"`, `fg.present = "vblank"`, limiter `0`.
2. Set the game's own cap to 36-37 FPS.
3. Capture 30 s in gameplay with `tools/capture.ps1 -PresentMonArgs default`.
4. Expect `DisplayedTime` p10-p90 inside roughly 13.3-13.4 ms, `mid_not_ready`
   0/300 and `drain_waits` near 300/300 (the second present blocks on the blank,
   so the next cycle's drain waits for it).

Open, unrelated to tearing: artifacts during initial game boot, the in-game
shutdown retirement error, and UI composited at half the output rate.
