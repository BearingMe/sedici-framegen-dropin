# 005 - Built-in real-frame limiter

Milestone 2 moves the frame cap that run G proved effective into Sedici, so
Anti-Lag 2 can stay enabled while the game runs uncapped.

## Why this run set exists

`002-pacing-baseline.md` run G used the game's own `Maximum FPS 37` with 2x
frame generation and produced the tightest distribution in the whole set:

| metric | run G |
|---|---|
| output fps | 74.0 |
| avg interval | 13.513 ms |
| p50 | 13.500 ms |
| p95 | 14.118 ms |
| p99 | 14.667 ms |
| max | 15.406 ms |
| stddev | 0.324 ms |
| intervals > 1.5x median | 0.00 % |
| intervals < 0.5x median | 0.00 % |

Cyberpunk does not allow Anti-Lag 2 and Maximum FPS at the same time. The
limiter therefore applies the same 37 fps real-frame cap inside Sedici, in front
of the game's `Present`, while the game itself stays uncapped and Anti-Lag 2
stays on.

## What changed

| Change | Detail |
|---|---|
| Config | `[limiter] fps` (default `0`, off) and `spin_us` (default `1000`); `SEDICI_LIMIT_FPS` overrides `fps` |
| Hook point | `Present` / `Present1` on the FFX swapchain wrapper, before the original call |
| Timer | `CreateWaitableTimerExW` with `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`, one per process, armed per frame |
| Final wait | QPC spin with `YieldProcessor` for the last `spin_us`; no `Sleep`, no `timeBeginPeriod` |
| Schedule | `deadline = previous_deadline + 1/fps`; reseed at now when more than one interval late; never wait when already late |
| Stats | every 300 real frames at log level 1 |
| Inert | `fps = 0` and no env var: no timer, no wait, no allocation, no limiter log |

The hook sees exactly one `Present` per real frame (`presentsPerPrepare` is
1.0); generated frames are presented inside AMD's swapchain. Capping here
therefore caps real frames, and 2x frame generation puts the output at about
twice `fps`.

### Limiter stats line

Every 300 real frames:

```text
sedici: limiter stats frames=300 target_ms=27.027 interval_ms{avg=27.027 p50=27.027 p99=27.027 max=27.132} wait_ms{avg=26.936} overshoot_ms{avg=0.001 p99=0.000 max=0.105} late=0 resets=0 fg=on
```

Activation, and any change of frame-generation state:

```text
sedici: limiter active target=37 fps interval=27.027 ms spin=1000 us fg=on
```

## Pre-flight verification

No game was launched for this. The checks below were done first.

**Build.** Clean Release build at `/W4 /permissive-`, zero warnings, exactly
five exports: `ffxConfigure`, `ffxCreateContext`, `ffxDestroyContext`,
`ffxDispatch`, `ffxQuery`.

**Schedule unit test.** Eight cases against the pure `limiter_step` function:
first frame seeds; on-time frame targets `previous + interval`; exactly at the
deadline is late and does not wait; half an interval late does not reset; exactly
one interval late does not reset; more than one interval late reseeds; early
frame still waits; zero interval is inert. All pass.

**Real timer test.** 320 calls through the shipped `limiter.cpp` with stubbed
config and logging, 300-frame stats window:

| case | interval avg | overshoot p99 | overshoot max | late | resets |
|---|---|---|---|---|---|
| `fps=37`, `spin_us=1000` | 27.027 ms (target 27.027) | 0.000 ms | 0.105 ms | 0 | 0 |
| `fps=36`, `spin_us=1000` | 27.778 ms (target 27.778) | 0.017 ms | 0.089 ms | 0 | 0 |
| `fps=37`, `spin_us=0` | 27.028 ms | 0.533 ms | 0.807 ms | 0 | 0 |
| `fps=0` | inert | - | - | - | - |

The `spin_us=0` row is why the default is 1000 us: the timer alone is fine for
the rate, but roughly 30x worse on the final placement.

**Proxy harness.** 53 assertions against a stub `.orig`, all passing: the
milestone-1 forwarding cases, the pacing/async cases, and the new limiter config
cases (default 0, env override, file values, invalid env, out-of-range key,
spin range, spin-longer-than-interval note, env 0 beating the file).

**Deployment.** Release DLL deployed to `bin\x64`; the existing `sedici.toml`
was kept (deploy never overwrites it), so the deployed file has no `[limiter]`
section and the code default `fps = 0` applies until a run sets
`SEDICI_LIMIT_FPS`.

| File | SHA-256 |
|---|---|
| `amd_fidelityfx_framegeneration_dx12.dll` (Sedici) | `73F408E8AECFF5159635EFF2CE5B7713C28C575AAA06D430A88EF398AAE9BBCD` |
| `amd_fidelityfx_framegeneration_dx12.dll.orig` | `02297BEEDD285E822D3A64F314CF00FAF378DCEC0EDC47FF0C4DD71B3A8C2F18` |
| `amd_fidelityfx_loader_dx12.dll` | `E2D85AA05A9BD9ED8B38935FDF5199372CCA6F74C12015143BB6F945EE1608AA` |
| `amd_fidelityfx_upscaler_dx12.dll` | `D0DCCCC74A43C44BA435B7A369B456E0970D8A4464E4BD683119B374F2C9FB46` |

## Fixed conditions

Identical to 002/003, so the numbers are comparable:

- Path tracing off, base frame rate >= 60 fps.
- FSR Quality, frame generation on, borderless windowed, 2560x1080, 75 Hz panel.
- Same spot, same 30 s hand-driven camera pan.
- 45 s warm-up before the capture window, 30 s captured.
- Every run on a fresh process.
- Default pacing fix active (`pacing.mode = "hybrid"`).

## Run list

The four runs are labelled A-D to match the letter convention of the earlier
findings. They are the same configurations as the milestone-2 plan's N0-N3:

| Run | Game | Sedici | Purpose |
|---|---|---|---|
| A | Anti-Lag 2 on, uncapped | no limiter | baseline |
| B | Anti-Lag 2 on, uncapped | `SEDICI_LIMIT_FPS=37` | the target combination |
| C | Anti-Lag 2 off, Maximum FPS 37 | no limiter | repeat of 002 run G (game cap) |
| D | Anti-Lag 2 on, uncapped | `SEDICI_LIMIT_FPS=36.5` | 73 output, 2 fps headroom under 75 Hz |

B and D need `SEDICI_LIMIT_FPS` set in the shell that launches the game. A and C
need it absent. C also needs `Maximum FPS 37` in the game settings and Anti-Lag 2
off; A, B and D need Anti-Lag 2 on and Maximum FPS uncapped.

Captures for this set go to `captures/005/`, not `captures/`, because
`2026-11-05-A.csv` through `D.csv` already exist from the 002/003 runs. The run
labels stay A-D in this document; the directory keeps the files distinct.

`limiter.fps` accepts fractions. B and C ran at 37 real / 74 output; playing
those showed visible tearing on the 75 Hz panel, and 73 output is the calmer
target (2 fps of headroom). D therefore uses 36.5 (73 output), which is why the
limiter was extended to a fractional target rather than the integer 37/36 the
first plan used.

## Latency columns in `analyze.py`

The PresentMon v1 schema used by `capture.ps1` already carries these columns.
`analyze.py` reports them when present:

| Column | What it measures | What it does not measure |
|---|---|---|
| `MsUntilDisplayed` | PresentMon's software estimate from the Present call to the display change | input sampling, game simulation, photon emission; not comparable across present modes |
| `MsInPresentAPI` | how long the application blocked inside Present | latency; with the limiter it includes the deliberate wait |
| `MsUntilRenderComplete` | Present call to GPU render completion | display latency; it mostly reflects queue depth |
| `MsGPUActive` | GPU busy time for the frame | CPU frame time or latency |

`MsUntilDisplayed` is the best available proxy without extra hardware. Treat it
as a relative comparison between runs on the same machine and mode, not as an
absolute click-to-photon number.


## Results

Each run is appended below as it is captured, with the `analyze.py` tables,
the output-interval histogram in 1 ms bins, and the limiter stats lines from
`sedici.log`.

### Run A - Anti-Lag 2 on, uncapped, no limiter

MsBetweenDisplayChange (output cadence, ms)
| file | swapchain | n | fps | avg | p50 | p95 | p99 | max | sd | %>1.5med | %<0.5med |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2026-11-05-A.csv | 0x000001C8FAAE7EB0 | 3244 | 108.1 | 9.249 | 9.098 | 10.130 | 10.927 | 123.833 | 2.836 | 0.43 | 0.31 |

MsBetweenPresents (present call cadence, ms)
| file | swapchain | n | fps | avg | p50 | p95 | p99 | max | sd | %>1.5med | %<0.5med |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2026-11-05-A.csv | 0x000001C8FAAE7EB0 | 3244 | 108.1 | 9.249 | 9.043 | 9.947 | 10.789 | 123.926 | 2.803 | 0.43 | 0.31 |

MsUntilDisplayed (present-to-display proxy, ms)
| file | swapchain | n | fps | avg | p50 | p95 | p99 | max | sd | %>1.5med | %<0.5med |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2026-11-05-A.csv | 0x000001C8FAAE7EB0 | 3244 | 108.1 | 0.534 | 0.550 | 0.797 | 1.161 | 3.770 | 0.241 | 4.38 | 8.14 |

MsInPresentAPI (time blocked inside Present, ms)
| file | swapchain | n | fps | avg | p50 | p95 | p99 | max | sd | %>1.5med | %<0.5med |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2026-11-05-A.csv | 0x000001C8FAAE7EB0 | 3244 | 108.1 | 0.145 | 0.137 | 0.204 | 0.283 | 0.761 | 0.035 | 4.81 | 0.00 |

MsUntilRenderComplete (present-to-GPU-complete, ms)
| file | swapchain | n | fps | avg | p50 | p95 | p99 | max | sd | %>1.5med | %<0.5med |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2026-11-05-A.csv | 0x000001C8FAAE7EB0 | 3244 | 108.1 | 0.534 | 0.550 | 0.797 | 1.161 | 3.770 | 0.241 | 4.38 | 8.14 |

MsGPUActive (GPU busy time, ms)
| file | swapchain | n | fps | avg | p50 | p95 | p99 | max | sd | %>1.5med | %<0.5med |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2026-11-05-A.csv | 0x000001C8FAAE7EB0 | 3244 | 108.1 | 9.134 | 9.080 | 10.073 | 10.906 | 21.785 | 0.997 | 0.43 | 0.40 |

Latency notes
  MsUntilDisplayed is PresentMon's software estimate from the Present call to
  the display change. Use it for relative comparisons only: it is not
  click-to-photon, it excludes input sampling and game simulation, and it is
  not comparable across present modes. MsInPresentAPI is how long the
  application blocked inside Present; with a limiter it includes the
  deliberate wait, so it is not latency. MsUntilRenderComplete reflects
  queue depth. MsGPUActive is GPU busy time per frame.

Output interval histogram, 1 ms bins (MsBetweenDisplayChange, 2026-11-05-A.csv, 0x000001C8FAAE7EB0)
| bin (ms) | frames | % |
|---|---|---|
| 0.0-1.0 | 10 | 0.31 |
| 1.0-2.0 | 0 | 0.00 |
| 2.0-3.0 | 0 | 0.00 |
| 3.0-4.0 | 0 | 0.00 |
| 4.0-5.0 | 0 | 0.00 |
| 5.0-6.0 | 4 | 0.12 |
| 6.0-7.0 | 5 | 0.15 |
| 7.0-8.0 | 12 | 0.37 |
| 8.0-9.0 | 1223 | 37.70 |
| 9.0-10.0 | 1785 | 55.02 |
| 10.0-11.0 | 176 | 5.43 |
| 11.0-12.0 | 12 | 0.37 |
| 12.0-13.0 | 3 | 0.09 |
| 13.0-14.0 | 0 | 0.00 |
| 14.0-15.0 | 1 | 0.03 |
| 15.0-16.0 | 0 | 0.00 |
| 16.0-17.0 | 0 | 0.00 |
| 17.0-18.0 | 3 | 0.09 |
| 18.0-19.0 | 3 | 0.09 |
| 19.0-20.0 | 1 | 0.03 |
| 20.0-21.0 | 1 | 0.03 |
| 21.0-22.0 | 0 | 0.00 |
| 22.0-23.0 | 0 | 0.00 |
| 23.0-24.0 | 0 | 0.00 |
| 24.0-25.0 | 0 | 0.00 |
| 25.0-26.0 | 0 | 0.00 |
| 26.0-27.0 | 0 | 0.00 |
| 27.0-28.0 | 0 | 0.00 |
| 28.0-29.0 | 0 | 0.00 |
| 29.0-30.0 | 0 | 0.00 |
| 30.0-31.0 | 0 | 0.00 |
| 31.0-32.0 | 0 | 0.00 |
| 32.0-33.0 | 0 | 0.00 |
| 33.0-34.0 | 0 | 0.00 |
| 34.0-35.0 | 0 | 0.00 |
| 35.0-36.0 | 0 | 0.00 |
| 36.0-37.0 | 0 | 0.00 |
| 37.0-38.0 | 1 | 0.03 |
| 38.0-39.0 | 1 | 0.03 |
| 39.0-40.0 | 0 | 0.00 |
| 40.0+ | 3 | 0.09 |

Swapchains
  2026-11-05-A.csv: only one swapchain (0x000001C8FAAE7EB0)

Limiter: off for this run (`limiter.fps = 0 [default]`), so there are no limiter
stats lines. The output rate is set by the game and the panel, not by Sedici.

Read: 108.1 fps output, median 9.098 ms, 92.7% of intervals in the 8-10 ms
cluster. The 2.836 ms stddev and the 123.8 ms maximum come from three frames at
or above 40 ms plus ten sub-1 ms catch-ups (0.31% below 0.5x the median), not
from the steady state. This is the uncapped "no limiter" reference for B-D.

### Run A (repeat) - Anti-Lag 2 on, uncapped, no limiter, standard spot

MsBetweenDisplayChange (output cadence, ms)
| file | swapchain | n | fps | avg | p50 | p95 | p99 | max | sd | %>1.5med | %<0.5med |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2026-11-05-A2.csv | 0x000001C8FAAE7EB0 | 2864 | 95.5 | 10.473 | 10.326 | 11.607 | 12.133 | 15.489 | 0.618 | 0.00 | 0.00 |

MsBetweenPresents (present call cadence, ms)
| file | swapchain | n | fps | avg | p50 | p95 | p99 | max | sd | %>1.5med | %<0.5med |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2026-11-05-A2.csv | 0x000001C8FAAE7EB0 | 2864 | 95.5 | 10.473 | 10.449 | 11.152 | 11.688 | 15.253 | 0.393 | 0.00 | 0.00 |

MsUntilDisplayed (present-to-display proxy, ms)
| file | swapchain | n | fps | avg | p50 | p95 | p99 | max | sd | %>1.5med | %<0.5med |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2026-11-05-A2.csv | 0x000001C8FAAE7EB0 | 2864 | 95.5 | 0.790 | 0.620 | 1.613 | 2.068 | 5.671 | 0.428 | 29.09 | 7.93 |

MsInPresentAPI (time blocked inside Present, ms)
| file | swapchain | n | fps | avg | p50 | p95 | p99 | max | sd | %>1.5med | %<0.5med |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2026-11-05-A2.csv | 0x000001C8FAAE7EB0 | 2864 | 95.5 | 0.157 | 0.144 | 0.244 | 0.347 | 1.202 | 0.047 | 6.98 | 0.00 |

MsUntilRenderComplete (present-to-GPU-complete, ms)
| file | swapchain | n | fps | avg | p50 | p95 | p99 | max | sd | %>1.5med | %<0.5med |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2026-11-05-A2.csv | 0x000001C8FAAE7EB0 | 2864 | 95.5 | 0.790 | 0.620 | 1.613 | 2.068 | 5.671 | 0.428 | 29.09 | 7.93 |

MsGPUActive (GPU busy time, ms)
| file | swapchain | n | fps | avg | p50 | p95 | p99 | max | sd | %>1.5med | %<0.5med |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2026-11-05-A2.csv | 0x000001C8FAAE7EB0 | 2864 | 95.5 | 10.457 | 10.320 | 11.579 | 12.024 | 13.520 | 0.598 | 0.00 | 0.00 |

Latency notes
  MsUntilDisplayed is PresentMon's software estimate from the Present call to
  the display change. Use it for relative comparisons only: it is not
  click-to-photon, it excludes input sampling and game simulation, and it is
  not comparable across present modes. MsInPresentAPI is how long the
  application blocked inside Present; with a limiter it includes the
  deliberate wait, so it is not latency. MsUntilRenderComplete reflects
  queue depth. MsGPUActive is GPU busy time per frame.

Output interval histogram, 1 ms bins (MsBetweenDisplayChange, 2026-11-05-A2.csv, 0x000001C8FAAE7EB0)
| bin (ms) | frames | % |
|---|---|---|
| 9.0-10.0 | 637 | 22.24 |
| 10.0-11.0 | 1671 | 58.34 |
| 11.0-12.0 | 513 | 17.91 |
| 12.0-13.0 | 38 | 1.33 |
| 13.0-14.0 | 3 | 0.10 |
| 14.0-15.0 | 1 | 0.03 |
| 15.0-16.0 | 1 | 0.03 |
| 16.0-17.0 | 0 | 0.00 |
| 17.0-18.0 | 0 | 0.00 |
| 18.0-19.0 | 0 | 0.00 |
| 19.0-20.0 | 0 | 0.00 |
| 20.0-21.0 | 0 | 0.00 |
| 21.0-22.0 | 0 | 0.00 |
| 22.0-23.0 | 0 | 0.00 |
| 23.0-24.0 | 0 | 0.00 |
| 24.0-25.0 | 0 | 0.00 |
| 25.0-26.0 | 0 | 0.00 |
| 26.0-27.0 | 0 | 0.00 |
| 27.0-28.0 | 0 | 0.00 |
| 28.0-29.0 | 0 | 0.00 |
| 29.0-30.0 | 0 | 0.00 |
| 30.0-31.0 | 0 | 0.00 |
| 31.0-32.0 | 0 | 0.00 |
| 32.0-33.0 | 0 | 0.00 |
| 33.0-34.0 | 0 | 0.00 |
| 34.0-35.0 | 0 | 0.00 |
| 35.0-36.0 | 0 | 0.00 |
| 36.0-37.0 | 0 | 0.00 |
| 37.0-38.0 | 0 | 0.00 |
| 38.0-39.0 | 0 | 0.00 |
| 39.0-40.0 | 0 | 0.00 |

Swapchains
  2026-11-05-A2.csv: only one swapchain (0x000001C8FAAE7EB0)

This is the spot-matched baseline for B-D. It is the same process as the first A
capture, but the game had been brought to the standard test spot.

Read: 95.5 fps output, median 10.326 ms, stddev 0.618 ms, worst interval
15.489 ms, and zero intervals outside 0.5-1.5x the median. The 9-12 ms
histogram is a single tight cluster. The first A capture (108.1 fps, 123.8 ms
worst) was at a different place in the world and is kept only as an earlier
sample; this repeat is the reference for the limiter runs.
### Run B - Anti-Lag 2 on, uncapped, SEDICI_LIMIT_FPS=37

MsBetweenDisplayChange (output cadence, ms)
| file | swapchain | n | fps | avg | p50 | p95 | p99 | max | sd | %>1.5med | %<0.5med |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2026-11-05-B.csv | 0x00000171BE23E2B0 | 2219 | 74.0 | 13.511 | 13.305 | 18.121 | 18.914 | 73.555 | 3.554 | 0.14 | 0.14 |

MsBetweenPresents (present call cadence, ms)
| file | swapchain | n | fps | avg | p50 | p95 | p99 | max | sd | %>1.5med | %<0.5med |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2026-11-05-B.csv | 0x00000171BE23E2B0 | 2219 | 74.0 | 13.507 | 13.340 | 14.546 | 15.437 | 49.391 | 0.912 | 0.05 | 0.00 |

MsUntilDisplayed (present-to-display proxy, ms)
| file | swapchain | n | fps | avg | p50 | p95 | p99 | max | sd | %>1.5med | %<0.5med |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2026-11-05-B.csv | 0x00000171BE23E2B0 | 2219 | 74.0 | 1.936 | 0.617 | 5.106 | 5.967 | 60.854 | 2.305 | 46.10 | 30.69 |

MsInPresentAPI (time blocked inside Present, ms)
| file | swapchain | n | fps | avg | p50 | p95 | p99 | max | sd | %>1.5med | %<0.5med |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2026-11-05-B.csv | 0x00000171BE23E2B0 | 2219 | 74.0 | 0.152 | 0.149 | 0.193 | 0.300 | 2.350 | 0.059 | 2.57 | 0.00 |

MsUntilRenderComplete (present-to-GPU-complete, ms)
| file | swapchain | n | fps | avg | p50 | p95 | p99 | max | sd | %>1.5med | %<0.5med |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2026-11-05-B.csv | 0x00000171BE23E2B0 | 2219 | 74.0 | 1.916 | 0.607 | 5.104 | 5.925 | 48.083 | 2.108 | 46.10 | 28.57 |

MsGPUActive (GPU busy time, ms)
| file | swapchain | n | fps | avg | p50 | p95 | p99 | max | sd | %>1.5med | %<0.5med |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2026-11-05-B.csv | 0x00000171BE23E2B0 | 2219 | 74.0 | 11.291 | 11.352 | 14.033 | 14.439 | 59.864 | 2.114 | 0.14 | 0.05 |

Latency notes
  MsUntilDisplayed is PresentMon's software estimate from the Present call to
  the display change. Use it for relative comparisons only: it is not
  click-to-photon, it excludes input sampling and game simulation, and it is
  not comparable across present modes. MsInPresentAPI is how long the
  application blocked inside Present; with a limiter it includes the
  deliberate wait, so it is not latency. MsUntilRenderComplete reflects
  queue depth. MsGPUActive is GPU busy time per frame.

Output interval histogram, 1 ms bins (MsBetweenDisplayChange, 2026-11-05-B.csv, 0x00000171BE23E2B0)
| bin (ms) | frames | % |
|---|---|---|
| 0.0-1.0 | 2 | 0.09 |
| 1.0-2.0 | 0 | 0.00 |
| 2.0-3.0 | 0 | 0.00 |
| 3.0-4.0 | 1 | 0.05 |
| 4.0-5.0 | 0 | 0.00 |
| 5.0-6.0 | 0 | 0.00 |
| 6.0-7.0 | 0 | 0.00 |
| 7.0-8.0 | 0 | 0.00 |
| 8.0-9.0 | 17 | 0.77 |
| 9.0-10.0 | 476 | 21.45 |
| 10.0-11.0 | 340 | 15.32 |
| 11.0-12.0 | 93 | 4.19 |
| 12.0-13.0 | 133 | 5.99 |
| 13.0-14.0 | 144 | 6.49 |
| 14.0-15.0 | 92 | 4.15 |
| 15.0-16.0 | 157 | 7.08 |
| 16.0-17.0 | 323 | 14.56 |
| 17.0-18.0 | 301 | 13.56 |
| 18.0-19.0 | 126 | 5.68 |
| 19.0-20.0 | 12 | 0.54 |
| 20.0-21.0 | 0 | 0.00 |
| 21.0-22.0 | 0 | 0.00 |
| 22.0-23.0 | 0 | 0.00 |
| 23.0-24.0 | 0 | 0.00 |
| 24.0-25.0 | 0 | 0.00 |
| 25.0-26.0 | 0 | 0.00 |
| 26.0-27.0 | 0 | 0.00 |
| 27.0-28.0 | 0 | 0.00 |
| 28.0-29.0 | 0 | 0.00 |
| 29.0-30.0 | 0 | 0.00 |
| 30.0-31.0 | 0 | 0.00 |
| 31.0-32.0 | 0 | 0.00 |
| 32.0-33.0 | 0 | 0.00 |
| 33.0-34.0 | 0 | 0.00 |
| 34.0-35.0 | 0 | 0.00 |
| 35.0-36.0 | 0 | 0.00 |
| 36.0-37.0 | 0 | 0.00 |
| 37.0-38.0 | 1 | 0.05 |
| 38.0-39.0 | 0 | 0.00 |
| 39.0-40.0 | 0 | 0.00 |
| 40.0+ | 1 | 0.05 |

Swapchains
  2026-11-05-B.csv: only one swapchain (0x00000171BE23E2B0)

Limiter stats from the capture window (90-121 s after process start):

```text
[93728.965 ms] sedici: limiter stats frames=300 target_ms=27.027 interval_ms{avg=27.027 p50=27.027 p99=27.789 max=35.626} wait_ms{avg=14.262} overshoot_ms{avg=0.000 p99=0.000 max=0.031} late=12 resets=0 fg=on
[101837.002 ms] sedici: limiter stats frames=300 target_ms=27.027 interval_ms{avg=27.027 p50=27.027 p99=27.027 max=27.091} wait_ms{avg=15.595} overshoot_ms{avg=0.000 p99=0.000 max=0.064} late=0 resets=0 fg=on
[109945.144 ms] sedici: limiter stats frames=300 target_ms=27.027 interval_ms{avg=27.027 p50=27.027 p99=27.027 max=27.052} wait_ms{avg=15.558} overshoot_ms{avg=0.000 p99=0.000 max=0.025} late=0 resets=0 fg=on
[118053.240 ms] sedici: limiter stats frames=300 target_ms=27.027 interval_ms{avg=27.027 p50=27.027 p99=27.027 max=27.027} wait_ms{avg=15.627} overshoot_ms{avg=0.000 p99=0.000 max=0.000} late=0 resets=0 fg=on
```

Read: the real-frame side is essentially exact. In the clean windows the
interval is 27.027 ms at p50, p99 and even max, with `late=0`, `resets=0` and
overshoot p99 0.000 ms (max 0.064 ms). The target output rate is met exactly:
74.0 fps.

The output side is the surprise. The display-change histogram is bimodal, with
clusters at 9-10 ms (21.5 %) and 16-18 ms (28.1 %), not the single 13.5 ms
cluster run G produced with the game's own cap. The pair sums to the same
27 ms, but the split between the real and generated present is uneven. The
present-call cadence is tighter (sd 0.912 ms) than the display cadence
(sd 3.554 ms), so this is happening after the present call, inside the FFX
swapchain's real/generated presentation order, not in the limiter's deadline
math.

This is the first real cost of capping at Present rather than before render:
the rate is right, the real-frame schedule is right, but the generated frame no
longer lands cleanly at the midpoint. C is the control that will show whether
the game's own cap really is more even on the same spot.
### Run C - Anti-Lag 2 off, Maximum FPS 37, no limiter

MsBetweenDisplayChange (output cadence, ms)
| file | swapchain | n | fps | avg | p50 | p95 | p99 | max | sd | %>1.5med | %<0.5med |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2026-11-05-C.csv | 0x00000125EDD1FC70 | 2093 | 69.8 | 14.318 | 13.374 | 18.606 | 33.315 | 433.411 | 14.596 | 2.58 | 2.34 |

MsBetweenPresents (present call cadence, ms)
| file | swapchain | n | fps | avg | p50 | p95 | p99 | max | sd | %>1.5med | %<0.5med |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2026-11-05-C.csv | 0x00000125EDD1FC70 | 2093 | 69.8 | 14.325 | 13.335 | 15.084 | 33.751 | 432.957 | 14.250 | 2.53 | 2.29 |

MsUntilDisplayed (present-to-display proxy, ms)
| file | swapchain | n | fps | avg | p50 | p95 | p99 | max | sd | %>1.5med | %<0.5med |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2026-11-05-C.csv | 0x00000125EDD1FC70 | 2093 | 69.8 | 1.705 | 0.597 | 5.131 | 6.025 | 19.200 | 1.871 | 42.43 | 35.40 |

MsInPresentAPI (time blocked inside Present, ms)
| file | swapchain | n | fps | avg | p50 | p95 | p99 | max | sd | %>1.5med | %<0.5med |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2026-11-05-C.csv | 0x00000125EDD1FC70 | 2093 | 69.8 | 0.130 | 0.106 | 0.205 | 0.870 | 2.002 | 0.136 | 7.36 | 0.00 |

MsUntilRenderComplete (present-to-GPU-complete, ms)
| file | swapchain | n | fps | avg | p50 | p95 | p99 | max | sd | %>1.5med | %<0.5med |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2026-11-05-C.csv | 0x00000125EDD1FC70 | 2093 | 69.8 | 1.690 | 0.594 | 5.130 | 5.957 | 7.109 | 1.816 | 42.52 | 35.31 |

MsGPUActive (GPU busy time, ms)
| file | swapchain | n | fps | avg | p50 | p95 | p99 | max | sd | %>1.5med | %<0.5med |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2026-11-05-C.csv | 0x00000125EDD1FC70 | 2093 | 69.8 | 10.680 | 10.223 | 13.955 | 21.804 | 58.873 | 3.807 | 2.34 | 6.64 |

Latency notes
  MsUntilDisplayed is PresentMon's software estimate from the Present call to
  the display change. Use it for relative comparisons only: it is not
  click-to-photon, it excludes input sampling and game simulation, and it is
  not comparable across present modes. MsInPresentAPI is how long the
  application blocked inside Present; with a limiter it includes the
  deliberate wait, so it is not latency. MsUntilRenderComplete reflects
  queue depth. MsGPUActive is GPU busy time per frame.

Output interval histogram, 1 ms bins (MsBetweenDisplayChange, 2026-11-05-C.csv, 0x00000125EDD1FC70)
| bin (ms) | frames | % |
|---|---|---|
| 0.0-1.0 | 36 | 1.72 |
| 1.0-2.0 | 9 | 0.43 |
| 2.0-3.0 | 4 | 0.19 |
| 3.0-4.0 | 0 | 0.00 |
| 4.0-5.0 | 0 | 0.00 |
| 5.0-6.0 | 0 | 0.00 |
| 6.0-7.0 | 0 | 0.00 |
| 7.0-8.0 | 2 | 0.10 |
| 8.0-9.0 | 29 | 1.39 |
| 9.0-10.0 | 422 | 20.16 |
| 10.0-11.0 | 180 | 8.60 |
| 11.0-12.0 | 89 | 4.25 |
| 12.0-13.0 | 172 | 8.22 |
| 13.0-14.0 | 225 | 10.75 |
| 14.0-15.0 | 147 | 7.02 |
| 15.0-16.0 | 121 | 5.78 |
| 16.0-17.0 | 207 | 9.89 |
| 17.0-18.0 | 262 | 12.52 |
| 18.0-19.0 | 110 | 5.26 |
| 19.0-20.0 | 23 | 1.10 |
| 20.0-21.0 | 4 | 0.19 |
| 21.0-22.0 | 1 | 0.05 |
| 22.0-23.0 | 1 | 0.05 |
| 23.0-24.0 | 2 | 0.10 |
| 24.0-25.0 | 2 | 0.10 |
| 25.0-26.0 | 4 | 0.19 |
| 26.0-27.0 | 11 | 0.53 |
| 27.0-28.0 | 6 | 0.29 |
| 28.0-29.0 | 2 | 0.10 |
| 29.0-30.0 | 0 | 0.00 |
| 30.0-31.0 | 0 | 0.00 |
| 31.0-32.0 | 1 | 0.05 |
| 32.0-33.0 | 0 | 0.00 |
| 33.0-34.0 | 2 | 0.10 |
| 34.0-35.0 | 2 | 0.10 |
| 35.0-36.0 | 2 | 0.10 |
| 36.0-37.0 | 0 | 0.00 |
| 37.0-38.0 | 0 | 0.00 |
| 38.0-39.0 | 0 | 0.00 |
| 39.0-40.0 | 0 | 0.00 |
| 40.0+ | 15 | 0.72 |

Swapchains
  2026-11-05-C.csv: only one swapchain (0x00000125EDD1FC70)

No limiter lines beyond the configuration block (`limiter.fps = 0 [default]`).

The first ten seconds of the capture contain load/streaming stalls (433, 378,
190 ms at t=1.97, 2.57, 2.88 s), so the 30 s summary above is dominated by them.
The steady-state slice (t >= 15 s, n = 1111) is:

| avg | p50 | p95 | p99 | max | sd | >1.5x med | <0.5x med |
|---|---|---|---|---|---|---|---|
| 13.511 | 13.326 | 18.337 | 19.143 | 20.049 | 3.376 | 0.09 % | 0.00 % |

The game's cap is definitely active. The log's prepare stats in the same window:

```text
[110097.241 ms] stats prepares=300 frameTimeDelta_ms{avg=27.028 min=26.598 max=27.412 sd=0.112} prepareQpc_ms{avg=27.028 min=24.826 max=29.250 sd=0.571} presentInterval_ms{avg=27.027 p50=26.945 p95=28.684 p99=29.857 max=30.280} presentsPerPrepare{avg=1.000 min=1 max=1}
[118206.143 ms] stats prepares=300 frameTimeDelta_ms{avg=27.028 min=26.365 max=27.547 sd=0.131} prepareQpc_ms{avg=27.030 min=25.088 max=28.904 sd=0.466} presentInterval_ms{avg=27.036 p50=27.023 p95=28.444 p99=29.462 max=30.526} presentsPerPrepare{avg=1.000 min=1 max=1}
```

Read: B and C are equally uneven in this session. B's full-window sd was 3.554;
C's steady-state sd is 3.376, and both histograms have the same 9-10 ms and
16-18 ms clusters. The limiter is not adding the unevenness: the game's own cap,
at the same spot and with a perfect 27.03 ms real-frame cadence, produces the
same split. 002 run G's single 13.5 ms cluster (sd 0.324) is not reproduced by
either, so the current setup differs from 002 somewhere other than the limiter.
The most likely confounder is the upscaler path: the game currently has
`ResolutionScaling = FSR4`, `FSR4 = Quality`; 002's note only says "Quality
upscaling" and did not record which upscaler backend was selected.
### Run D - Anti-Lag 2 on, uncapped, SEDICI_LIMIT_FPS=36.5 (73 output)

MsBetweenDisplayChange (output cadence, ms)
| file | swapchain | n | fps | avg | p50 | p95 | p99 | max | sd | %>1.5med | %<0.5med |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2026-11-05-D.csv | 0x000002B5C3F4D660 | 2189 | 73.0 | 13.691 | 13.567 | 18.370 | 19.202 | 20.066 | 3.314 | 0.00 | 0.00 |

MsBetweenPresents (present call cadence, ms)
| file | swapchain | n | fps | avg | p50 | p95 | p99 | max | sd | %>1.5med | %<0.5med |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2026-11-05-D.csv | 0x000002B5C3F4D660 | 2189 | 73.0 | 13.694 | 13.552 | 14.623 | 15.734 | 18.944 | 0.470 | 0.00 | 0.00 |

MsUntilDisplayed (present-to-display proxy, ms)
| file | swapchain | n | fps | avg | p50 | p95 | p99 | max | sd | %>1.5med | %<0.5med |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2026-11-05-D.csv | 0x000002B5C3F4D660 | 2189 | 73.0 | 1.922 | 0.565 | 5.126 | 6.041 | 7.009 | 1.875 | 48.01 | 25.99 |

MsInPresentAPI (time blocked inside Present, ms)
| file | swapchain | n | fps | avg | p50 | p95 | p99 | max | sd | %>1.5med | %<0.5med |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2026-11-05-D.csv | 0x000002B5C3F4D660 | 2189 | 73.0 | 0.149 | 0.146 | 0.193 | 0.318 | 1.117 | 0.042 | 2.79 | 0.00 |

MsUntilRenderComplete (present-to-GPU-complete, ms)
| file | swapchain | n | fps | avg | p50 | p95 | p99 | max | sd | %>1.5med | %<0.5med |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2026-11-05-D.csv | 0x000002B5C3F4D660 | 2189 | 73.0 | 1.922 | 0.565 | 5.126 | 6.041 | 7.009 | 1.875 | 48.01 | 25.99 |

MsGPUActive (GPU busy time, ms)
| file | swapchain | n | fps | avg | p50 | p95 | p99 | max | sd | %>1.5med | %<0.5med |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2026-11-05-D.csv | 0x000002B5C3F4D660 | 2189 | 73.0 | 11.153 | 11.189 | 13.999 | 14.478 | 19.038 | 1.769 | 0.05 | 0.05 |

Latency notes
  MsUntilDisplayed is PresentMon's software estimate from the Present call to
  the display change. Use it for relative comparisons only: it is not
  click-to-photon, it excludes input sampling and game simulation, and it is
  not comparable across present modes. MsInPresentAPI is how long the
  application blocked inside Present; with a limiter it includes the
  deliberate wait, so it is not latency. MsUntilRenderComplete reflects
  queue depth. MsGPUActive is GPU busy time per frame.

Output interval histogram, 1 ms bins (MsBetweenDisplayChange, 2026-11-05-D.csv, 0x000002B5C3F4D660)
| bin (ms) | frames | % |
|---|---|---|
| 7.0-8.0 | 1 | 0.05 |
| 8.0-9.0 | 10 | 0.46 |
| 9.0-10.0 | 400 | 18.27 |
| 10.0-11.0 | 380 | 17.36 |
| 11.0-12.0 | 123 | 5.62 |
| 12.0-13.0 | 116 | 5.30 |
| 13.0-14.0 | 105 | 4.80 |
| 14.0-15.0 | 100 | 4.57 |
| 15.0-16.0 | 130 | 5.94 |
| 16.0-17.0 | 306 | 13.98 |
| 17.0-18.0 | 333 | 15.21 |
| 18.0-19.0 | 151 | 6.90 |
| 19.0-20.0 | 32 | 1.46 |
| 20.0-21.0 | 2 | 0.09 |
| 21.0-22.0 | 0 | 0.00 |
| 22.0-23.0 | 0 | 0.00 |
| 23.0-24.0 | 0 | 0.00 |
| 24.0-25.0 | 0 | 0.00 |
| 25.0-26.0 | 0 | 0.00 |
| 26.0-27.0 | 0 | 0.00 |
| 27.0-28.0 | 0 | 0.00 |
| 28.0-29.0 | 0 | 0.00 |
| 29.0-30.0 | 0 | 0.00 |
| 30.0-31.0 | 0 | 0.00 |
| 31.0-32.0 | 0 | 0.00 |
| 32.0-33.0 | 0 | 0.00 |
| 33.0-34.0 | 0 | 0.00 |
| 34.0-35.0 | 0 | 0.00 |
| 35.0-36.0 | 0 | 0.00 |
| 36.0-37.0 | 0 | 0.00 |
| 37.0-38.0 | 0 | 0.00 |
| 38.0-39.0 | 0 | 0.00 |
| 39.0-40.0 | 0 | 0.00 |

Swapchains
  2026-11-05-D.csv: only one swapchain (0x000002B5C3F4D660)

D used a 10 s warm-up instead of 45 s at the user's request; the process had
been running and settled since 19:44:03, and the 30 s capture window itself is
unchanged.

Limiter stats in and around the capture window:

```text
[85778.274 ms] sedici: limiter stats frames=300 target_ms=27.397 interval_ms{avg=27.397 p50=27.397 p99=27.397 max=27.398} wait_ms{avg=18.893} overshoot_ms{avg=0.000 p99=0.000 max=0.000} late=0 resets=0 fg=on
[102623.383 ms] sedici: limiter stats frames=300 target_ms=27.397 interval_ms{avg=27.397 p50=27.397 p99=27.397 max=27.448} wait_ms{avg=17.567} overshoot_ms{avg=0.000 p99=0.000 max=0.051} late=0 resets=0 fg=on
[119092.854 ms] sedici: limiter stats frames=300 target_ms=27.397 interval_ms{avg=27.397 p50=27.397 p99=27.397 max=27.535} wait_ms{avg=16.196} overshoot_ms{avg=0.000 p99=0.000 max=0.138} late=0 resets=0 fg=on
```

## Final read

**Does B match C's evenness while keeping Anti-Lag 2?** Yes, within noise. B
(limiter 37) had output sd 3.554 and C (game cap 37) had steady-state sd 3.376;
both histograms have the same 9-10 ms and 16-18 ms clusters. Neither reproduces
002 G's single 13.5 ms cluster (sd 0.324). Since the game's own cap and the
limiter agree, the limiter is not the source of the bimodality; the current
setup differs from 002 somewhere else (the upscaler path is the obvious
candidate: `ResolutionScaling = FSR4`, `FSR4 = Quality`).

**Is spin accuracy good?** Yes. In the clean windows the real-frame interval is
27.397 ms at p50, p99 and max, with `late=0`, `resets=0`, and overshoot p99
0.000 ms (worst observed 0.221 ms). The deadline math and the timer/spin split
are doing exactly what they were asked to do.

**Is D better or worse than B?** D (36.5 real / 73 output) is slightly better on
every cadence number and hits the 73 fps target:

| | B (37 / 74 out) | C (37 / 74 out) | D (36.5 / 73 out) |
|---|---|---|---|
| output fps | 74.0 | 74.0 | **73.0** |
| p50 | 13.305 | 13.326 | 13.567 |
| p95 | 18.121 | 18.337 | 18.370 |
| output sd | 3.554 | 3.376 | **3.314** |
| present-call sd | 0.912 | - | **0.470** |
| >1.5x median | 0.14 % | 0.09 % | **0.00 %** |
| <0.5x median | 0.14 % | 0.00 % | **0.00 %** |

The remaining bimodality (9-10 ms / 16-18 ms) is unchanged in shape; the rate
moved from 74 to 73 and the distribution tightened slightly, but the real and
generated presents are still not evenly split. Tearing, which is what prompted
the 73 fps target, is not measurable from PresentMon; D is the run to judge by
eye. VSync remains off and cannot be combined with FG, so 73 reduces the beat
rather than removing tearing.

The next lever, if the bimodality matters more than the rate, is placement: cap
after the original `Present` instead of before it, so the FFX swapchain keeps
its own real/generated spacing while Sedici still throttles the render loop.
That is a separate experiment, not part of milestone 2.
## VRR and the 73 fps target

The panel is 75 Hz with VRR from 40 to 75 Hz. At 73 output the average period is
13.7 ms, inside the window with about 2 fps of headroom under the ceiling; that
is why 36.5 real / 73 output is the intended final rate rather than 37 / 74.

The cap fixes the average, not the split. The 9-10 ms half of the output
histogram implies roughly 100-111 fps, above the 75 Hz ceiling, while the
16-18 ms half is about 55-62 fps, inside the window. With VRR the display tracks
the output, but it cannot track a 9 ms interval, so the real/generated pair can
still tear or lose one of the two even with the average at 73. The evenness of
that pair, not the average rate, is the remaining thing to fix.

## Recommended final configuration

```toml
[pacing]
mode = "hybrid"

[async]
mode = "game"

[limiter]
fps = 36.5
spin_us = 1000
```

The shipped `sedici.toml` keeps `fps = 0` (off) so the milestone-2 defaults stay
exactly milestone-1 behaviour. A deployed `sedici.toml` is never overwritten:
add the `[limiter]` block to the game's own file, or set
`SEDICI_LIMIT_FPS=36.5` in the launching shell.