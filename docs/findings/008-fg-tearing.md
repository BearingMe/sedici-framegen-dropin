# 008: Where frame-generation tearing came from

> **Status (2026-11-09): cause analysis valid, resolution superseded by `vrr`.** The shipped
> default is `present = "vrr"` with `tear_guard = "scanline"`; `vblank` and `vsync` are the
> fixed-refresh fallback. The finding stays for its evidence and failure analysis.

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

## Resolution (superseded by `"vrr"` below)

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

## Cause 3: tearing with VRR on, inside the VRR range

The test display supports VRR and tore with it enabled, at 72-73 output fps on
a 75 Hz panel. Re-reading the existing captures (`CPUStartTime + DisplayLatency`
as the on-screen time) shows why:

| Capture | CPU gap between presents | On-screen gap mid / real | Flips < 13.2 ms | GPU latency of the mid present |
| --- | --- | --- | ---: | ---: |
| limiter 36.5, RT off | 13.7 / 13.7 ms | 10.55 / 16.86 ms | 50% | 3.3 ms |
| limiter 36.5, RT on | 13.7 / 13.7 ms | 6.43 / 20.96 ms | 50% | 7.4 ms |
| limiter 30 | 16.7 / 16.7 ms | 16.1 / 17.2 ms | 0.7% | 0.7 ms |

The two `Present` calls are evenly spaced in CPU time, but a flip happens when
the GPU work queued before it finishes. The midpoint's work sits behind the
game's frame, so its flip lands 3-7 ms late and the real frame's flip, already
GPU-ready, follows half an interval after the *call*. The predicted midpoint
duration, 13.7 - 3.3 = 10.4 ms and 13.7 - 7.4 = 6.3 ms, matches the measured
10.55 and 6.43 ms.

A VRR panel still needs one full refresh (13.33 ms at 75 Hz) to scan an image
out. Every second flip arrived sooner than that, and with `ALLOW_TEARING` it was
applied mid-scan: at 10.5 ms that is ~79% down the screen, at 6.4 ms ~48%, which
is the reported "random middle/bottom" tear. At 60 output fps every gap exceeds
13.33 ms, which is why the limiter-30 capture had no short frames.

Root cause: **flips were paced in CPU time while their timing is set by GPU
completion.** Cause 1 made the midpoint ready by the real Present; this is the
same anchor error, still visible once VRR stops hiding it behind vsync.

### `fg.present = "vrr"` and `fg.tear_guard`

- A presenter thread on a dedicated present queue receives each application
  frame. It waits until the midpoint is finished on the GPU, flips it, then
  flips the real frame half an interval after the midpoint's *actual* flip. The
  game thread returns from `Present` immediately; at most one older frame may
  still be presenting.
- The native chain is recreated on the present queue, as AMD's swapchain does
  (the wrap desc hands the game's reference over). Moving it with
  `ResizeBuffers1` instead made RTSS draw its OSD into the chain from the game
  queue, and D3D12 removed the device.
- `tear_guard = "scanline"` reads `D3DKMTGetScanLine` and holds a flip until the
  panel is in its blank or within ~0.25 ms of it; `"timer"` holds it until one
  refresh after the previous flip; `"off"` does neither.
- Logged every 300 frames: `short_gaps` (flips sooner than one refresh),
  `scan_at_flip`, guard holds, GPU wait, and `idle_in_blank` (how often the panel
  was in blank after more than one refresh without a flip: high on VRR, ~5% on a
  fixed refresh).
- **RTSS:** even with the chain created on the present queue, RTSS wrote it from
  another queue in the offline check (device removed, intermittently). `"vrr"`
  therefore falls back to `"vsync"` while `RTSSHooks64.dll` is loaded, unless
  `SEDICI_FG_ALLOW_RTSS=1`.

Offline: `sedici_fgcheck --present` (RTSS closed or allowed) runs 40 frames
through the presenter at ~35 fps with tearing flags, a resize and teardown.

### Game sessions: guard off vs scan-line guard

Authorized sessions on the same test location, 2560x1080 on the 75 Hz FreeSync
panel ("AMD Optimized"), `limiter.fps = 36.5`, RTSS closed, 30 s captures:

| Configuration | presents/s | 12.5-14.5 ms | < 12.5 ms | < 10 ms | median `DisplayedTime` | Result |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| `"game"`, limiter 36.5 (earlier) | 73.0 | 0.5% | 49.5% | 14.4% | 14.6 ms | tears |
| `"vrr"`, `tear_guard = "off"` | 73.0 | 3.8% | 46.9% | 19.3% | 14.3 ms | tears |
| `"vrr"`, `tear_guard = "scanline"` | 72.8 | 85.1% | 0.9% | 0.5% | 13.55 ms | **no tearing, user-confirmed** |
| `"vsync"`, in-game cap ~37.5 (earlier) | 74.9 | 100% | 0% | 0% | 13.35 ms | no tearing, limiter unusable |

- With the scan-line guard, midpoint and real frame are even on screen
  (13.8 / 13.2 ms per parity, previously 10.3 / 16.9). Steady 300-frame
  windows logged `short_gaps=0/600`, `gap_min_ms=13.32`; the guard held
  590-600 of 600 flips for ~2.7 ms each, all at the end of the scan.
- VRR was active, including on "AMD Optimized": 51% of scan-line frames lasted
  13.5-20 ms with none at two refreshes (26.7 ms), which a fixed 75 Hz panel
  cannot show without tearing. Switching FreeSync to "On" changed nothing in
  the log. `idle_in_blank` stayed `0/0` only because at ~73 fps no gap exceeds
  one refresh by 0.5 ms, so it never samples; it is not evidence either way.
- Remaining: a few windows still logged up to 24/600 near back-to-back flips
  (gaps under 1 ms) during 16-35 ms GPU stalls, i.e. hitches rather than steady
  tearing.
- Unexplained: with the guard off, `Present` calls were evenly spaced
  (13.7 / 13.7 ms) yet the screen still showed 10.3 / 16.9 ms, so every second
  flip lands later than its call. The scan-line guard hides this; the cause is
  open.

`[fg] present = "vrr"` with `tear_guard = "scanline"` is now the shipped
default. `"vsync"` remains the fallback and the choice for fixed-refresh
displays.
