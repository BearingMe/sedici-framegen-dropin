# Logging and present timing

> Reference moved out of the [README](../../README.md); the text is unchanged.

## Logging

The log is `sedici.log`, beside the deployed DLL. A background thread writes it, so a
slow or failing disk can never stall a game call. Lines wait in a fixed 256-line queue;
when it is full the extra lines are dropped, counted, and reported in the log once the
disk keeps up. A message repeated in a tight loop is rate limited (a burst of eight,
then two a second) and the next line that gets through says how many were suppressed.
If the file cannot be written, the lines are discarded and counted, and the log says how
many were lost when it becomes writable again. The last lines before a hard crash may
not have reached the file.

```powershell
$env:MFSRFG_LOG = "1"   # 0 = errors only, 1 = normal, 2 = verbose
```

Levels:

- **0** - only internal errors.
- **1** - the effective configuration; first 50 calls of each export; one full
  description per newly seen desc type; every non-OK return; limiter activation
  and stats; overlay and presentation statistics.
- **2** - every call, in addition to level 1.

The log level controls logging only.

### What the descriptions include

- **create** - FG context flags (decoded), display size, max render size,
  backbuffer format; HUD-less format; FG version; swapchain wrap/new/for-HWND
  variant, swapchain pointer, queue, DXGI swapchain desc; DX12 backend device.
- **configure** - FG `frameGenerationEnabled`, `allowAsyncWorkloads`, present and
  generation callbacks set/not set, `HUDLessColor` present yes/no and format,
  `generationRect`, `frameID`, flags, `onlyPresentGenerated`; swapchain UI
  resource registered and its format; every key/value configure including
  `FRAMEPACINGTUNING` and its five tuning fields (accepted and ignored); global
  debug descs.
- **dispatch** - prepare `frameID`, render size, jitter, MV scale,
  `frameTimeDelta`, reset, camera near/far/FOV, `viewSpaceToMetersFactor`,
  depth/MV formats; the deprecated prepare and V2 prepare are both handled;
  camera-info pNext; the generation desc if the game uses one.
- **query** - type and result, plus known output fields/pointers.
- **any non-OK return** - logged at level 1 regardless of call count.

### Stats

The periodic report also carries a `diag:` line (log queue drops, lost lines, parked
objects) and, when the planned output rate would exceed the display refresh, an
`OVER_REFRESH` note with the real-frame cap that would fit.

Every 300 prepare calls:

- `frameTimeDelta` (ms): average, minimum, maximum, standard deviation.
- Real interval between prepares from QPC (ms): average, minimum, maximum,
  standard deviation.

The present-interval and presents-per-prepare statistics of the old proxy are not
produced any more; nothing feeds them from Sedici's wrapper yet (K99).

### Limiter

Activation is logged once, and again whenever the frame-generation state
changes:

```text
sedici: limiter active target=37 fps interval=27.027 ms spin=1000 us fg=on
```

Every 300 real frames it reports what the schedule actually achieved:

```text
sedici: limiter stats frames=300 target_ms=27.027 interval_ms{avg=27.027 p50=27.027 p99=27.027 max=27.132} wait_ms{avg=26.936} overshoot_ms{avg=0.001 p99=0.000 max=0.105} late=0 resets=0 fg=on
```

| Field | Meaning |
|---|---|
| `frames` | real frames in this window |
| `target_ms` | `1000 / limiter.fps` |
| `interval_ms` | measured interval between real presents after the wait: avg, p50, p99, max |
| `wait_ms` | average time spent waiting (timer + spin) before the original Present |
| `overshoot_ms` | how far past the deadline the spin ended, for frames that waited; this is spin accuracy, not lateness |
| `late` | frames that arrived at or after their deadline and were not waited on |
| `resets` | times the schedule reseeded because a frame arrived more than one interval late |
| `fg` | frame generation state the game last requested |

`late` and `resets` are the two numbers to watch for a hitch: a long stall
produces one reset and no burst, rather than a queue of back-to-back presents.

### Stall instrumentation

Run I in `docs/findings/002-pacing-baseline.md` contained five episodic stalls: a
prepare interval of 22-72 ms followed immediately by a sub-2 ms one. Nothing
else in the log explained them, so Sedici now reports each one as it happens, at
log level 1.

A stall is a real-frame prepare interval that is **both** more than 3x the
rolling median of the last 61 prepare intervals **and** at least 25 ms. The
median is taken before the new sample is added, so a stall cannot hide in its
own baseline. The log is rate-limited to one line per second.

```text
sedici: stall prepare interval=72.406 ms median=13.402 ms ratio=5.402x frameID=4242 reset=n/a presentsSincePrepare=0 swapchainEvents1s{count=0} qpc=78860918227
```

| Field | Meaning |
|---|---|
| `interval` | measured gap between prepares, ms |
| `median` | rolling median the comparison used, ms |
| `ratio` | `interval / median` |
| `frameID` | the prepare's frame id |
| `reset` | the prepare's reset flag, or `n/a` for the legacy prepare desc, whose reset field is unused |
| `presentsSincePrepare` | always 0 at present: nothing counts presents for the detector yet (K99) |
| `swapchainEvents1s` | always empty at present: nothing records swapchain events on the Sedici path yet (K99) |
| `qpc` | raw QPC timestamp, for correlating with external tools |

A stall line means the game's own prepare calls arrived late. Whether that
coincided with a swapchain rebuild cannot be told from this line yet.

## Present timing

The wrapper presents on its own thread and queue in `vrr` mode and runs the real
frame limiter from the game's thread, before the real `Present`. `DXGI_PRESENT_TEST`
calls never reach the limiter. If the limiter's high-resolution timer cannot be
created it falls back to a standard waitable timer, and if no timer can be created the
limiter disables itself and the game runs uncapped; the error is logged and nothing fails
the game's call. The timer is created lazily on the first capped present, never in
`DllMain`, and is not closed at process exit.
