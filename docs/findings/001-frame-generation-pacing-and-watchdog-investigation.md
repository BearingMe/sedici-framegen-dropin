# 001 — Frame-generation pacing and GPU watchdog investigation

Date: 2026-11-05
Status: observation only; no behavior changes

## Summary

Sedici was built, deployed, and exercised in Cyberpunk 2077 with FSR upscaling +
FSR frame generation. The proxy loaded the original DLL, forwarded all five
exports, installed the `Present`/`Present1` hook, and logged no FFX errors.

The investigation found three separate things that should not be conflated:

1. **Long no-prepare gaps** (20–27 s) while the game keeps presenting. These
   correlate with settings/menu use, path-tracing changes, and CPU-heavy
   pipeline/shader work. They are not necessarily driver hangs.
2. **A pre-existing GPU watchdog history** on this machine (`LiveKernelEvent`
   code 141, `WATCHDOG` and `AMD_WATCHDOG` dumps) that predates Sedici.
3. **Uneven frame pacing** that the current outer-swapchain hook cannot measure
   correctly, because generated frames are presented inside the FG swapchain.

No evidence yet shows Sedici causing a crash. Two later runs produced the same
long gaps without any new watchdog or TDR event, and both exited cleanly.

## Proxy status

Observed in every run:

- Original loaded from
  `amd_fidelityfx_framegeneration_dx12.dll.orig` next to the proxy.
- Proxy marker `SEDICI_PROXY_V1` present.
- `Present`/`Present1` hook installed on the FG swapchain vtable.
- No non-OK FFX return codes.
- No missing exports.
- No `VirtualProtect` failure.
- Clean `ffxDestroyContext` on game exit.

## Observed runs

### Run 1 — first FG test

- Game was in settings/menu during part of the run.
- Last stats line showed a **27.138 s** prepare gap and **3,830** presents in
  that interval.
- New watchdog dumps were reported around the same window:
  `WATCHDOG-20261105-0623.dmp` and `WATCHDOG-20261105-0624.dmp` (P1 141).
- The machine already had `WATCHDOG-20261105-0508.dmp` and
  `WATCHDOG-20261105-0529.dmp` **before Sedici was deployed**, plus a long
  October history of 141 / AMD watchdog dumps.
- A Start-menu shutdown was initiated after the run; there was no bugcheck or
  Kernel-Power 41.

Conclusion: the watchdog history is real, but it is not unique to Sedici. The
27 s gap still needs a better explanation than "settings menu".

### Run 2 — settings check

- Total runtime: **129.97 s**.
- Gap 1: **22.35 s** at roughly 28 s into the run, **3,711** presents.
- Gap 2: **11.97 s** at roughly 76 s, **603** presents.
- Process stayed `Responding=True`; CPU remained busy.
- No new watchdog/TDR event.
- Game exited normally.

The user was opening the map and exit menu during the gaps, but did not spend
20 s in those menus. The gaps are therefore not menu idle time alone.

### Run 3 — path tracing

- Total runtime: **131.99 s**.
- One gap: **20.10 s** at roughly 26 s into the run, **2,658** presents.
- Process stayed responsive; no new watchdog/TDR.
- Frame time before the gap: **12–15 ms**.
- Frame time after the gap: **30–41 ms**.
- `RayTracedPathTracing=true` in `UserSettings.json` after the run.
- `GamePipelineLibrary.cache` and `UserSettings.json` were written at exit.
- Clean exit.

The frame-time doubling immediately after the gap and the sustained CPU load
(~8 cores) point to path-tracing pipeline/shader compilation, not 20 s of menu
idle. Cyberpunk exposes this as a no-prepare gap while the loading/menu frame
continues to be presented.

## What the stats actually measure

Stats windows are 300 prepare dispatches. Example clean window:

```text
frameTimeDelta_ms avg=8.447
prepareQpc_ms    avg=8.259
presentInterval_ms avg=3.286 p50=1.575 p95=8.443 p99=17.044 max=212.593
presentsPerPrepare avg=1.000
```

Later clean windows:

```text
frameTimeDelta avg=8.095  prepareQpc avg=8.108  present p50=8.076
frameTimeDelta avg=8.101  prepareQpc avg=8.114  present p50=8.097
frameTimeDelta avg=18.556 prepareQpc avg=18.573 present p50=18.474
frameTimeDelta avg=40.271 prepareQpc avg=40.296 present p50=40.133
```

`frameTimeDelta` tracks the measured prepare interval closely, including under
path tracing. The 66.000 ms clamp is visible during hitches:

```text
frameTimeDelta max=66.000  prepareQpc max=81.279
frameTimeDelta max=66.000  prepareQpc max=90.616
frameTimeDelta max=66.000  prepareQpc max=177.583
frameTimeDelta max=66.000  prepareQpc max=27138.463 (hang/compile gap)
```

`presentsPerPrepare` is 1.0 in normal windows and only spikes during
no-prepare gaps. The outer swapchain `Present` hook sees the game's real-frame
presents; generated frames are presented internally by the FG swapchain and do
not pass through this vtable. Therefore:

- The present intervals above are effectively **real-frame cadence**, not
  output cadence.
- The 3,711 / 2,658 / 603 present counts during gaps are menu/loading presents,
  not a frame-generation ratio.
- The task's original expectation of ~2 presents per prepare does not hold for
  this hook location.

This is the main measurement limitation to fix before drawing pacing
conclusions.

## Game API shape observed

From the first 50 calls of each export:

- Create:
  - `FrameGenerationCreate`: flags `AsyncWorkloads|DepthInverted`
  - display `2560x1080`, max render `2560x1080`
  - backbuffer `R8G8B8A8_UNORM`
  - `FrameGenerationCreateVersion` version `4.0.1`
  - `OverrideVersion` present
  - Swapchain variant: `FrameGenerationSwapChainWrapDX12`
- Configure:
  - `frameGenerationEnabled=true`
  - `allowAsyncWorkloads=true`
  - `presentCallback=none`
  - `generationCallback=set`
  - `HUDLessColor=yes`, `R8G8B8A8_UNORM`, `2560x1080`
  - `flags=none`, `onlyPresentGenerated=false`
  - no frame-pacing tuning key observed
- Dispatch:
  - Uses the legacy `FrameGenerationPrepare` desc (`0x20004`), not the V2 desc.
  - Also dispatches `FrameGenerationDispatch` (`0x20003`) every frame with
    `numGeneratedFrames=1`.
  - render size `1707x720`
  - jitter pattern is the usual 8-tap subpixel sequence
  - `motionVectorScale=(1707,720)`
  - depth `R32_FLOAT`, motion vectors `R16G16B16A16_FLOAT`
  - `frameTimeDelta` values match measured prepare intervals in clean windows

## Settings snapshot

```text
RayTracedPathTracing        true
RayTracedLighting           Ultra
RayTracedReflections        true
RayTracedSunShadows         true
RayTracedLocalShadows       true

Resolution                 2560x1080
ResolutionScaling          FSR4 (Quality)
FSR3_FrameGeneration       true
FSR4_FrameGeneration       false
FrameGeneration            FSR3
FSR_AntiLag2Mode           Enabled
ReflexMode                 Disabled
VSync                      Off
WindowMode                 BorderlessWindowed
DynamicResolutionScaling   false
MaximumFPS                 off
```

## Working hypotheses

1. **Pacing is in the FG swapchain present scheduling.** The current hook cannot
   see generated-frame presents, so we need output-level timestamps before
   changing anything. Candidate tools: PresentMon/ETW, or hooking the inner
   swapchain used by the FG wrapper.
2. **`frameTimeDelta` is clamped at 66 ms.** It tracks normal frames accurately,
   but long frames are under-reported. This is a plausible M2 correction target,
   especially after hitches/compiles; it is not the main steady-state cause.
3. **Pacing is affected by presentation settings.** `AntiLag2=Enabled`,
   `VSync=Off`, and borderless windowed are all variables. Controlled A/B tests
   are needed; do not bundle them with a behavior change.

The watchdog history should be tracked separately. Runs 2 and 3 produced long
gaps without new watchdog events, so a gap alone is not proof of a driver hang.

## Next steps

- Add gap instrumentation at level 1: if the prepare interval exceeds ~1 s,
  log `frameID`, `reset`, presents since the last prepare, foreground process/
  window, and CPU time. This distinguishes menu, compile/loading, and a true
  render stall.
- Measure real output cadence with PresentMon/ETW or an inner-swapchain hook.
- Run controlled pacing tests without changing settings mid-run:
  AntiLag2 on/off, VSync on/off, fullscreen vs borderless.
- Keep milestone 0 behavior unchanged until the pacing measurement is sound.
