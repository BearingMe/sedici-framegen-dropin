# 002 — Frame pacing baseline (PresentMon) and first A/B toggles

Date: 2026-11-05
Status: measurement work in progress; Sedici behavior unchanged unless a
`SEDICI_*` variable is set.

Follows `docs/001-frame-generation-pacing-and-watchdog-investigation.md`.

## Why this run set exists

`001` established that:

- `frameTimeDelta` tracks real prepare intervals closely and is only clamped at
  66 ms on long frames.
- The game provides HUD-less color and asks for **no** frame-pacing tuning.
- Frame generation is created with **AsyncWorkloads**.
- The outer `Present` hook only sees real frames, so the generated-frame output
  cadence has never been measured.
- Earlier runs mixed in path tracing (base fps ~25–33) and possibly m-fsr's
  upscaler, which are confounders.

This document covers a clean baseline: AMD's original upscaler, moderate
settings so the real frame rate stays at or above 60 fps, and output cadence
measured with PresentMon instead of a `Present` hook.

## Environment changes for this baseline

- m-fsr removed from `bin\x64` with its own `restore.ps1` (removed the m-fsr
  upscaler DLL, the ownership manifest, `m-fsr.log`, and `model\`). AMD's
  original upscaler is back.
- Sedici stays deployed as the frame-generation proxy, with the original
  preserved as `.orig`.
- `bin\x64.bkp` was not touched.

Final state, verified by SHA256:

| File | Role | SHA256 |
| --- | --- | --- |
| `amd_fidelityfx_loader_dx12.dll` | backup (untouched) | `E2D85AA0…1608AA` |
| `amd_fidelityfx_upscaler_dx12.dll` | backup (AMD original, m-fsr removed) | `D0DCCCC7…C9FB46` |
| `amd_fidelityfx_framegeneration_dx12.dll` | Sedici proxy | `67A50DD2…83F8A8` |
| `amd_fidelityfx_framegeneration_dx12.dll.orig` | backup | `02297BEE…8C2F18` |

## Measurement tooling

`tools/capture.ps1 -Label <name> -Seconds 30`

- Waits for `Cyberpunk2077.exe`, then captures that process only, so every row
  belongs to the game.
- Uses `--v1_metrics` for stable column names: `msBetweenPresents`,
  `msBetweenDisplayChange`, `SwapChainAddress`.
- `-WarmupSeconds` (default 45) lets the game reach steady state before the
  30 s window opens; the script prints a countdown.
- Writes `captures/<date>-<label>.csv`, next to it a copy of `sedici.log` and a
  small `*.meta.txt` with the process id, capture time and the toggle variables.

`tools/analyze.py <csv...> [--markdown] [--append <file>]`

- Groups by swapchain inside each file.
- Reports frame count, fps, and `msBetweenDisplayChange` avg / p50 / p95 / p99 /
  max / stddev, plus the share of intervals above 1.5× the median (stutter) and
  below 0.5× the median (bunching), and the same for `msBetweenPresents`.
- Identifies the frame-generation output swapchain as the one with the higher
  present rate (expected ≈ 2× the real-frame swapchain at FSR FG 1×).

PresentMon on this machine is the AMD Adrenalin build:
`C:\Program Files\AMD\CNext\CNext\PresentMon-x64.exe`
(`PresentMon HEAD a9c55ce77e11bb962ab1ce58f62cddca16fc89ab`, CLI 2.x). No
download was needed.

## Toggles added to Sedici

Both are inert unless their environment variable is set; with neither set the
DLL forwards exactly as in milestone 0.

| Variable | Effect |
| --- | --- |
| `SEDICI_ASYNC=0` | Clears `FFX_FRAMEGENERATION_ENABLE_ASYNC_WORKLOAD_SUPPORT` (0x1) from `ffxCreateContextDescFrameGeneration::flags`, and forces `allowAsyncWorkloads=false` in `ffxConfigureDescFrameGeneration` and `FfxFrameGenerationConfig`. |
| `SEDICI_PACING=hybrid` | After a create chain containing a frame-generation swapchain desc, sends `ffxConfigureDescFrameGenerationSwapChainKeyValueDX12` with `key = FFX_API_CONFIGURE_FG_SWAPCHAIN_KEY_FRAMEPACINGTUNING` (2) and `FfxApiSwapchainFramePacingTuning{safetyMarginInMs=0.1, varianceFactor=0.1, allowHybridSpin=true, hybridSpinTime=2, allowWaitForSingleObjectOnFence=false}`. |

Notes:

- The desc rewrite is scoped: the caller's desc chain is patched for the
  forwarded call and restored byte for byte afterwards, so the game still sees
  its own values.
- The pacing desc is sent from `ffxCreateContext` on the swapchain context. If
  that context rejects it, the proxy retries on the next successful
  `ffxConfigure` whose chain carries a swapchain pointer (bounded to 5
  attempts). The return code and the exact values sent are logged either way.
- `ffx_framegeneration_api_types.h` documents `safetyMarginInMs=0.1`,
  `varianceFactor=0.1`, `hybridSpinTime=2` and
  `allowWaitForSingleObjectOnFence=false` as defaults; only
  `allowHybridSpin` is changed to `true`.

Verified end to end against a stub `.orig` (no game, no GPU): with both
variables unset the stub receives flags `0x9` and `allowAsyncWorkloads=1`; with
`SEDICI_ASYNC=0` it receives `0x8` and `0`; with `SEDICI_PACING=hybrid` it
receives the `0x30008` configure with the values above and `result=0 (OK)`. The
caller's desc is unchanged after each call in all cases.

## Fixed conditions

Every run uses: path tracing **off**, RT moderate or off, base fps ≥ 60, FSR
Quality upscaling, FSR frame generation on, borderless windowed, the same save,
the same spot, the same slow camera pan, 30 s capture.

## Run list

| Run | Environment | Game setting |
| --- | --- | --- |
| A | none | Anti-Lag 2 on, VSync off |
| B | none | Anti-Lag 2 off, VSync off |
| C | none | Anti-Lag 2 off, VSync off, Maximum FPS 75 |
| D | `SEDICI_ASYNC=0` | as A |
| E | `SEDICI_PACING=hybrid` | as A |

C was originally Anti-Lag 2 on + VSync on. Cyberpunk cannot run VSync with
frame generation enabled, so the third condition is the other pacing mechanism
the game exposes: a locked frame rate. Read C as "Anti-Lag off, frames locked"
against A "Anti-Lag on, uncapped".

### Display and present mode

The only active display is `\\.\DISPLAY1`: 2560x1080, supported modes 50/60/75 Hz,
currently **75 Hz**.

The game presents with `SyncInterval=0` and `PresentFlags=0x200`
(`DXGI_PRESENT_ALLOW_TEARING`); PresentMon reports `PresentMode` as
`Hardware: Independent Flip`. Nothing throttles frame generation output to the
panel, which is why the captures show 169-173 fps on a 75 Hz screen.

Consequence for interpretation: in tearing independent-flip mode
`msBetweenDisplayChange` closely tracks `msBetweenPresents` (5.768 vs 5.768 ms in
run A), so it measures the present pipeline, not what the panel actually latches.
It is still the right tool for comparing runs against each other, but it cannot
by itself prove smoothness at the eye. Output above 75 fps also means most
generated frames never become a distinct displayed frame.


### How the game is launched

The game is started **directly**, not through `steam://`, for all five runs:

```powershell
cd "C:\Program Files (x86)\Steam\steamapps\common\Cyberpunk 2077\bin\x64"
# run A/B/C: no SEDICI_* variables
Remove-Item Env:\SEDICI_ASYNC, Env:\SEDICI_PACING -ErrorAction SilentlyContinue
# run D
$env:SEDICI_ASYNC  = '0'
# run E
$env:SEDICI_PACING = 'hybrid'
.\Cyberpunk2077.exe
```

Then, from the same shell:

```powershell
powershell -ExecutionPolicy Bypass -File tools\capture.ps1 -Label A -Seconds 30
```

Why direct launch:

- The game inherits the variables from the launching shell, so no `setx` and no
  Steam restart between runs.
- Steam's overlay is only injected into processes Steam spawns, so a direct
  launch keeps the present path free of an extra `Present` hook.

Constraints:

- `Cyberpunk2077.exe` is a Steam-DRM-wrapped binary with no `steam_appid.txt`,
  so the Steam client must be running and logged in, even for a direct launch.
- The launch path must be identical for all five runs. The runs in `001` went
  through Steam, so they are context, not a baseline to diff against. An
  optional sixth run (A again, via `steam://rungameid/1091500`, after
  `setx SEDICI_*` + a full Steam restart) bridges the two paths if needed.

## Results

Each run lists both cadence series and a short read. `%>1.5med` is stutter
share, `%<0.5med` is bunching share.

### Run A - Anti-Lag 2 on, VSync off (no Sedici toggles)

MsBetweenDisplayChange (output cadence, ms)
| file | swapchain | n | fps | avg | p50 | p95 | p99 | max | sd | %>1.5med | %<0.5med |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2026-11-05-A.csv | 0x0000021271C62EE0 | 5197 | 173.4 | 5.768 | 5.620 | 6.620 | 7.986 | 50.194 | 1.127 | 0.81 | 0.04 |

MsBetweenPresents (present call cadence, ms)
| file | swapchain | n | fps | avg | p50 | p95 | p99 | max | sd | %>1.5med | %<0.5med |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2026-11-05-A.csv | 0x0000021271C62EE0 | 5197 | 173.4 | 5.768 | 5.591 | 6.550 | 7.841 | 37.318 | 0.990 | 0.79 | 0.00 |

Notes:

- Only **one** swapchain is visible to PresentMon: the frame-generation
  wrapper's swapchain, at **173.4 fps** — exactly 2× the ~86.7 fps real-frame
  rate Sedici logs. The game's inner swapchain is not a DXGI object that the
  ETW provider reports, so output cadence is measurable directly and there is
  no second swapchain to compare against.
- This also explains the earlier `Present`-hook mismatch: Sedici patched the
  wrapper's vtable, which only sees the game's real presents, while PresentMon
  sees the underlying presents of both real and generated frames.
- Cadence is tight for a baseline: median 5.620 ms, stddev 1.127 ms, 0.81 % of
  intervals above 1.5× the median, no bunching.
- The single 50.19 ms maximum is one isolated hitch, not a recurring pattern.

### Run B - Anti-Lag 2 off, VSync off (no Sedici toggles)

MsBetweenDisplayChange (output cadence, ms)
| file | swapchain | n | fps | avg | p50 | p95 | p99 | max | sd | %>1.5med | %<0.5med |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2026-11-05-B.csv | 0x0000021271C62EE0 | 5079 | 169.4 | 5.904 | 5.722 | 7.141 | 8.804 | 51.299 | 1.184 | 1.24 | 0.08 |

MsBetweenPresents (present call cadence, ms)
| file | swapchain | n | fps | avg | p50 | p95 | p99 | max | sd | %>1.5med | %<0.5med |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2026-11-05-B.csv | 0x0000021271C62EE0 | 5079 | 169.4 | 5.904 | 5.742 | 6.928 | 8.862 | 33.494 | 0.960 | 1.12 | 0.00 |

Notes:

- A is ahead of B on every cadence metric: median 5.620 vs 5.722 ms, p95 6.620
  vs 7.141 ms, p99 7.986 vs 8.804 ms, stddev 1.127 vs 1.184 ms, stutter 0.81 vs
  1.24 %. Throughput 173.4 vs 169.4 fps.
- The direction is consistent across the whole distribution rather than one
  metric, which is more than a throughput coincidence, but 2.3 % is still
  inside what a single pair of 30 s captures can resolve. Run-to-run noise has
  not been characterised yet; an A repeat would establish the floor.
- Same single swapchain, same 2× relationship to the real-frame rate, so the
  capture itself is behaving identically between runs.


### Run C - Anti-Lag 2 off, Maximum FPS 73 (no Sedici toggles)

MsBetweenDisplayChange (output cadence, ms)
| file | swapchain | n | fps | avg | p50 | p95 | p99 | max | sd | %>1.5med | %<0.5med |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2026-11-05-C.csv | 0x0000021271C62EE0 | 4325 | 144.2 | 6.934 | 6.814 | 8.401 | 9.618 | 39.199 | 1.157 | 0.55 | 0.05 |

MsBetweenPresents (present call cadence, ms)
| file | swapchain | n | fps | avg | p50 | p95 | p99 | max | sd | %>1.5med | %<0.5med |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2026-11-05-C.csv | 0x0000021271C62EE0 | 4325 | 144.2 | 6.934 | 6.689 | 8.302 | 9.629 | 33.450 | 0.856 | 0.58 | 0.00 |

Swapchains
  2026-11-05-C.csv: only one swapchain (0x0000021271C62EE0)

Notes:

- Output settles at **144.2 fps** with the in-game cap at 73, i.e. 2 x 72.1. This
  answers the open question: **CP2077's Maximum FPS caps the game's real
  frames, not the frame-generation output.** Every later run has to be read
  with that in mind.
- Relative jitter is the lowest of the three runs: stddev / mean is 16.7 %
  (C) against 19.5 % (A) and 20.1 % (B), and only 0.55 % of intervals exceed
  1.5x the median.
- Throughput is 17 % below A, and the cap does not change the underlying
  situation: 144 fps is still 1.9x the 75 Hz panel. To land output at or below
  75 Hz with FG at 2x, the real-frame cap would have to be about 37.
- Present mode is unchanged (`SyncInterval=0`, `PresentFlags=0x200`,
  `Hardware: Independent Flip`), so the cap regularises the cadence without
  changing how frames reach the display.

### Run D - SEDICI_ASYNC=0, Anti-Lag 2 on, uncapped

MsBetweenDisplayChange (output cadence, ms)
| file | swapchain | n | fps | avg | p50 | p95 | p99 | max | sd | %>1.5med | %<0.5med |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2026-11-05-D.csv | 0x000002274008D950 | 5179 | 172.7 | 5.790 | 5.759 | 6.440 | 7.297 | 14.299 | 0.489 | 0.19 | 0.00 |

MsBetweenPresents (present call cadence, ms)
| file | swapchain | n | fps | avg | p50 | p95 | p99 | max | sd | %>1.5med | %<0.5med |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2026-11-05-D.csv | 0x000002274008D950 | 5179 | 172.7 | 5.790 | 5.704 | 6.352 | 7.061 | 14.280 | 0.389 | 0.17 | 0.00 |

Swapchains
  2026-11-05-D.csv: only one swapchain (0x000002274008D950)

Notes:

- `SEDICI_ASYNC=0` is the best run so far, and by a wide margin on every
  dispersion metric: stddev 0.489 ms against 1.127 ms in A, p99 7.297 against
  7.986 ms, worst interval **14.299 ms against 50.194 ms**, stutter share 0.19 %
  against 0.81 %.
- Throughput is unchanged (172.7 fps against 173.4 fps), so this is not a
  speed trade: moving frame generation off the async compute queue costs
  nothing here and removes almost all of the tail.
- The worst frame collapsing from ~50 ms to ~14 ms is the most meaningful number
  in the set so far: 50 ms is a visible hitch, 14 ms is one frame at 75 Hz.
- Caveat: run-to-run noise is still uncharacterised, so a repeat of A would be
  needed to prove the effect is larger than variance. The size of the
  difference (2.3x on stddev, 3.5x on max) makes a pure-noise explanation
  unlikely, but it is not yet excluded.

### Run E - SEDICI_PACING=hybrid, Anti-Lag 2 on, uncapped

MsBetweenDisplayChange (output cadence, ms)
| file | swapchain | n | fps | avg | p50 | p95 | p99 | max | sd | %>1.5med | %<0.5med |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2026-11-05-E.csv | 0x00000292674F1DB0 | 5368 | 178.9 | 5.588 | 5.532 | 6.364 | 6.940 | 9.610 | 0.450 | 0.11 | 0.00 |

MsBetweenPresents (present call cadence, ms)
| file | swapchain | n | fps | avg | p50 | p95 | p99 | max | sd | %>1.5med | %<0.5med |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2026-11-05-E.csv | 0x00000292674F1DB0 | 5368 | 178.9 | 5.589 | 5.493 | 6.219 | 7.110 | 9.976 | 0.387 | 0.11 | 0.00 |

Swapchains
  2026-11-05-E.csv: only one swapchain (0x00000292674F1DB0)

Notes:

- Best run of the set on every single metric: highest throughput (178.9 fps),
  lowest median (5.532 ms), p95 (6.364 ms), p99 (6.940 ms), worst interval
  (9.610 ms), stddev (0.450 ms) and stutter share (0.11 %).
- The tuning was accepted by the swapchain context on the first attempt
  (`result=0`), so the fallback retry through `ffxConfigure` never fired.
- E edges D (0.450 against 0.489 ms stddev, 9.610 against 14.299 ms max,
  178.9 against 172.7 fps), but both sit far from the no-toggle runs and the
  gap between them is small enough to be noise.

### Run F - control: no toggles, fresh process

MsBetweenDisplayChange (output cadence, ms)
| file | swapchain | n | fps | avg | p50 | p95 | p99 | max | sd | %>1.5med | %<0.5med |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2026-11-05-F.csv | 0x000001CA897F1370 | 5709 | 190.5 | 5.252 | 5.171 | 6.547 | 7.727 | 66.949 | 1.357 | 0.96 | 0.09 |

MsBetweenPresents (present call cadence, ms)
| file | swapchain | n | fps | avg | p50 | p95 | p99 | max | sd | %>1.5med | %<0.5med |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2026-11-05-F.csv | 0x000001CA897F1370 | 5709 | 190.5 | 5.250 | 5.232 | 6.494 | 7.600 | 49.258 | 1.083 | 0.67 | 0.00 |

Notes:

- Deliberate control for the process-restart confounder: fresh launch, no
  `SEDICI_*` variables, otherwise identical to A. `sedici.log` shows no toggle
  line and the create flags are `AsyncWorkloads|DepthInverted`, as in A.
- Its tail is the **worst of all six runs**: 66.949 ms worst interval and
  stddev 1.357 ms. The restart hypothesis is therefore dead — a fresh process
  without toggles does not reproduce the good tail that D and E showed.
- It is also the fastest on the median (5.171 ms, 190.5 fps), which makes the
  point sharper: throughput and smoothness are separate axes here, and the
  median never distinguished any of these configurations. Only the tail did.

### Run G - control at refresh: no toggles, cap 37

MsBetweenDisplayChange (output cadence, ms)
| file | swapchain | n | fps | avg | p50 | p95 | p99 | max | sd | %>1.5med | %<0.5med |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2026-11-05-G.csv | 0x000001CA897F1370 | 2217 | 74.0 | 13.513 | 13.500 | 14.118 | 14.667 | 15.406 | 0.324 | 0.00 | 0.00 |

MsBetweenPresents (present call cadence, ms)
| file | swapchain | n | fps | avg | p50 | p95 | p99 | max | sd | %>1.5med | %<0.5med |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2026-11-05-G.csv | 0x000001CA897F1370 | 2217 | 74.0 | 13.513 | 13.376 | 14.285 | 14.882 | 15.508 | 0.338 | 0.00 | 0.00 |

Notes:

- Capping the real frames at 37 lands output at exactly **74.0 fps**, so the cap
  is 2x as expected and now sits just under the 75 Hz panel (13.500 ms against a
  13.333 ms refresh).
- This is the tightest distribution in the whole set by a wide margin: stddev
  0.324 ms, **zero** intervals above 1.5x the median, **zero** below 0.5x, and a
  worst interval of 15.406 ms. Every frame lands between 13.0 and 14.5 ms.
- The paired comparison with F is clean: same process (pid 8056), same session,
  same scene, no toggles. Only the cap changed, and the worst interval went from
  66.949 ms to 15.406 ms while stdout stayed at 74 fps.
- It is the lowest output rate of the set, so it is also the only run where the
  measured cadence cannot exceed what the panel latches.

### Run H - both toggles: SEDICI_ASYNC=0 + SEDICI_PACING=hybrid

MsBetweenDisplayChange (output cadence, ms)
| file | swapchain | n | fps | avg | p50 | p95 | p99 | max | sd | %>1.5med | %<0.5med |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2026-11-05-H.csv | 0x0000024C792B40C0 | 5178 | 172.7 | 5.792 | 5.753 | 6.428 | 7.155 | 10.684 | 0.422 | 0.10 | 0.00 |

MsBetweenPresents (present call cadence, ms)
| file | swapchain | n | fps | avg | p50 | p95 | p99 | max | sd | %>1.5med | %<0.5med |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2026-11-05-H.csv | 0x0000024C792B40C0 | 5178 | 172.7 | 5.792 | 5.719 | 6.325 | 7.147 | 10.832 | 0.337 | 0.08 | 0.00 |

Notes:

- Both toggles applied to one process for the first time; `sedici.log` shows the
  async clear, the tuning configure and `result=0` for the tuning.
- Lowest relative jitter of the uncapped runs: stddev / mean 7.3 %, against
  8.0 % for E and 8.5 % for D.
- It does **not** stack. E alone has the better absolute worst interval
  (9.610 against 10.684 ms) and more throughput (178.9 against 172.7 fps). D, E
  and H all sit inside each other's noise, so the honest reading is that the
  two toggles are not additive: each one already removes the tail on its own.

### Run I - both toggles + Maximum FPS 37

MsBetweenDisplayChange (output cadence, ms)
| file | swapchain | n | fps | avg | p50 | p95 | p99 | max | sd | %>1.5med | %<0.5med |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2026-11-05-I.csv | 0x0000024C792B40C0 | 2216 | 73.9 | 13.534 | 13.453 | 14.207 | 14.899 | 72.406 | 2.279 | 0.36 | 0.32 |

MsBetweenPresents (present call cadence, ms)
| file | swapchain | n | fps | avg | p50 | p95 | p99 | max | sd | %>1.5med | %<0.5med |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2026-11-05-I.csv | 0x0000024C792B40C0 | 2216 | 73.9 | 13.530 | 13.370 | 14.269 | 14.874 | 60.377 | 1.487 | 0.23 | 0.00 |

Notes:

- Same output rate as G (73.9 against 74.0 fps) and the same median (13.453
  against 13.500 ms), so the cap is what sets pacing; the toggles add nothing
  measurable to the steady state.
- The distribution is not as clean as G, though. Five stall-and-catch-up events
  appear mid-capture, each a long interval followed immediately by a
  sub-2 ms one:
  `t=17.4 s 24.12/51.86 -> 1.26`, `t=17.6 s 22.15/26.85`,
  `t=18.9 s 68.54 -> 1.97`, `t=28.0 s 51.98 -> 0.74`,
  `t=29.6 s 72.41 -> 1.30`.
- Those events are what push stddev to 2.279 ms and the max to 72.4 ms. The
  first 1.5 s of the capture, before the stalls start, is clean and matches G
  exactly (p50 13.360, sd 0.378, max 14.731), so the steady state is fine and
  the tail is episodic.
- Do not read this as "the toggles cause stalls": I ran on a different process
  (pid 13596) than G (pid 8056), and a single 30 s window cannot separate a
  toggle effect from an unrelated hitch. It is recorded as an unresolved
  observation, not a conclusion.

## Comparison

`MsBetweenDisplayChange`, 30 s captures, single swapchain each.

| Run | Sedici | Game | fps | p50 | p95 | p99 | max | sd | sd/mean | >1.5x med | <0.5x med |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| A | none | Anti-Lag on, uncapped | 173.4 | 5.620 | 6.620 | 7.986 | 50.194 | 1.127 | 19.5 % | 0.81 % | 0.04 % |
| B | none | Anti-Lag off, uncapped | 169.4 | 5.722 | 7.141 | 8.804 | 51.299 | 1.184 | 20.1 % | 1.24 % | 0.08 % |
| C | none | Anti-Lag off, cap 73 | 144.2 | 6.814 | 8.401 | 9.618 | 39.199 | 1.157 | 16.7 % | 0.55 % | 0.05 % |
| D | `SEDICI_ASYNC=0` | Anti-Lag on, uncapped | 172.7 | 5.759 | 6.440 | 7.297 | 14.299 | 0.489 | 8.4 % | 0.19 % | 0.00 % |
| E | `SEDICI_PACING=hybrid` | Anti-Lag on, uncapped | 178.9 | 5.532 | 6.364 | 6.940 | 9.610 | 0.450 | 8.0 % | 0.11 % | 0.00 % |
| F | none, fresh process | Anti-Lag on, uncapped | 190.5 | 5.171 | 6.547 | 7.727 | 66.949 | 1.357 | 25.8 % | 0.96 % | 0.09 % |
| G | none | Anti-Lag on, cap 37 | 74.0 | 13.500 | 14.118 | 14.667 | 15.406 | 0.324 | **2.4 %** | **0.00 %** | **0.00 %** |
| H | `ASYNC=0` + `PACING=hybrid` | Anti-Lag on, uncapped | 172.7 | 5.753 | 6.428 | 7.155 | 10.684 | 0.422 | 7.3 % | 0.10 % | 0.00 % |
| I | `ASYNC=0` + `PACING=hybrid` | Anti-Lag on, cap 37 | 73.9 | 13.453 | 14.207 | 14.899 | 72.406 | 2.279 | 16.8 % | 0.36 % | 0.32 % |

Interpretation, against the three questions the run set was meant to answer:

**Smoothest: G**, at 2.4 % relative jitter with zero stutter and zero bunching.
**Best uncapped: E** on absolute worst case and throughput, with H marginal on
relative jitter. **Worst tail: F** (66.9 ms), which is also the control that
removed the process-restart confounder. Worst average smoothness remains B.
**I shows the cap is the binding constraint**: at 74 fps the toggles change
nothing in the steady state, but the run contains five episodic stalls that G
did not have, on a different process, so that part stays open.

**Does D or E beat A beyond run-to-run noise? Yes.** The control run settles it.

- Three no-toggle captures now exist, two on the original process (A, B) and one
  on a fresh process (F). All three show a worst interval between 50.2 and
  66.9 ms and a stddev of 1.13-1.36 ms.
- Two toggle captures, each on its own fresh process, show 14.3 and 9.6 ms worst
  interval and stddev 0.489 and 0.450 ms.
- The toggle family and the no-toggle family do not overlap on either metric, and
  the process-restart explanation is excluded by F: a fresh process with no
  toggles produced the worst tail in the whole set.
- Throughput does not explain it either. F is the fastest run on the median
  (5.171 ms) and still has the worst tail, so the effect is not a warm-up or a
  performance difference but a change in the tail distribution.

**Where does G fit?** Capping to 74 fps also removes the tail (15.4 ms worst
case, 2.4 % relative jitter), so a hard frame limit is a third independent way
to get a clean distribution. But it is not strictly better than the toggles:
uncapped D and E reached 14.3 and 9.6 ms worst case at 173-179 fps, i.e. as good
or better in absolute terms, without giving up throughput. G buys the tightest
*relative* cadence at the cost of 74 fps and a 37 fps real-frame rate. In other
words, the toggles fix the same tail that a frame cap fixes, but without the
throughput cost.

Remaining caveats:

- One capture per configuration. The family separation is convincing, but
  D against E (3.6 % throughput, 0.489 against 0.450 ms stddev) is not resolvable
  from one sample each; treat them as tied at best.
- The camera pan is hand-driven and cannot be bit-identical between runs.
- On a 75 Hz panel with tearing independent flip, only G (74.0 fps) is at or
  below the refresh rate; every other run sits between 1.9x and 2.5x refresh, so
  their `msBetweenDisplayChange` describes the present pipeline rather than what
  the display latches. G is the only capture where the measured cadence is
  bounded by the panel.
- The median never separated these runs (5.17-6.93 ms everywhere except the
  capped runs). Whatever the toggles fix, it is the tail, not average frame rate.

## Conclusions

1. **Output cadence is measurable, and it is the FG wrapper's swapchain.**
   PresentMon reports exactly one swapchain per process at 2x the real-frame
   rate, so the earlier "the hook only sees real frames" limitation is solved by
   measurement rather than by hooking anything deeper.
2. **The panel is the hard constraint.** The only active display is 75 Hz, and
   every uncapped run output 169-190 fps. Only G and I, capped to ~74, sat at or
   under refresh.
3. **Median frame time never distinguished these setups** (5.17-6.93 ms
   uncapped, 13.45-13.50 ms capped). The entire difference between good and bad
   runs lives in the tail.
4. **`SEDICI_ASYNC=0` and `SEDICI_PACING=hybrid` both remove the tail**, at no
   throughput cost: worst interval 9.6-14.3 ms against 50.2-66.9 ms without
   them. The fresh-process control (F) rules out a process-restart explanation.
5. **They do not stack** (H is inside the noise of either alone), which suggests
   they act on the same underlying scheduling contention.
6. **A 37 fps cap dominates them when it is on** (I): at 74 fps output the
   steady state is identical with or without the toggles, so the cap is the
   binding constraint, not the toggles.
7. Practical reading: for uncapped play, `SEDICI_PACING=hybrid` gave the best
   absolute tail and the most throughput; for a locked 74 fps, no toggle is
   needed. Choosing between the two is a latency-versus-steadiness judgement
   these numbers cannot make.

Limitations that still apply: one capture per configuration, a hand-driven
camera pan, and a 75 Hz panel within a tearing independent-flip present path, so
the numbers describe the present pipeline and not necessarily what the eye sees.

## Next

- ~~Measure `SEDICI_ASYNC=0` + `SEDICI_PACING=hybrid` together at the uncapped
  frame rate.~~ Done, run H: no gain over E alone.
- ~~Measure a toggle with the frame cap in place.~~ Done, run I: the cap
  dominates; the toggles add nothing at 74 fps. I also showed five episodic
  stalls that G did not have, on a different process — unexplained, and the one
  thing worth a repeat if the investigation continues.
- Optional: A again through `steam://rungameid/1091500` with `setx` plus a full
  Steam restart, to confirm the result survives the real launch path.
- The open question is now comfort, not cadence: G is the only configuration
  that matches the panel, but it runs the game loop at 37 fps. Deciding between
  "74 fps locked" and "175 fps with a fixed tail" is a latency and feel
  judgement that these numbers cannot settle.
