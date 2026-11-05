# Sedici

A pure observe-and-forward proxy for AMD's DX12 frame-generation DLL,
`amd_fidelityfx_framegeneration_dx12.dll`.

Sedici takes the original DLL's name. The original is renamed to
`amd_fidelityfx_framegeneration_dx12.dll.orig`, and all five `ffx*` exports are
forwarded with identical arguments, return values and timing. Milestone 0
changes no behavior. It only writes a local log describing what the game asks
frame generation to do.

The long-term goal is to diagnose and then correct the frame pacing that
Cyberpunk 2077 shows with native FSR frame generation, without touching the
loader or the upscaler DLL.

## Status

Milestone 0: observe and forward.

- Five exports: `ffxCreateContext`, `ffxDestroyContext`, `ffxConfigure`,
  `ffxQuery`, `ffxDispatch`.
- All calls forward to the original DLL. Unknown descs are forwarded untouched.
- Desc chains, FG create/configure/dispatch/query fields, and non-OK return
  codes are logged.
- `Present`/`Present1` are vtable-hooked on the swapchain returned by the
  frame-generation swapchain create/wrap descs. QPC timestamps are recorded;
  the call itself is unchanged.
- Every 300 prepare dispatches, a stats line is written.

## Requirements

- Windows x64, Visual Studio 2022 C++ tools, CMake 3.21+, Windows SDK.
- Static CRT, C++20.
- The FidelityFX/FSR headers in `third_party/ffx/` are unmodified SDK v2.4.0
  headers. See `third_party/ffx/README.md` for upstream paths and blob IDs.
- No other dependencies.

## Build

```powershell
cmake -B build -A x64
cmake --build build --config Release
```

Output: `build/Release/amd_fidelityfx_framegeneration_dx12.dll`.

Verify the export table from a Visual Studio developer shell:

```powershell
dumpbin /exports build\Release\amd_fidelityfx_framegeneration_dx12.dll
```

There must be exactly five exports, with the names listed above.

## Deploy and restore

```powershell
.\tools\deploy.ps1 -Game cp2077
.\tools\restore.ps1 -Game cp2077
```

Both scripts:

- resolve the game from `tools/games.json` or an explicit `-GamePath`;
- refuse to run while `Cyberpunk2077.exe` is running;
- never overwrite an existing `.orig` backup;
- refuse to overwrite or remove a DLL that is not Sedici's;
- only touch `amd_fidelityfx_framegeneration_dx12.dll`, its `.orig` backup,
  and `sedici.log`.

The loader and upscaler DLLs are never modified.

## Logging

The log is `sedici.log`, next to the deployed DLL. Every line is flushed
immediately.

```powershell
$env:MFSRFG_LOG = "1"   # 0 = errors only, 1 = normal (default), 2 = verbose
```

Levels:

- **0** — only load/forward failures and internal errors.
- **1** — first 50 calls of each export, then one full description per newly
  seen desc type; every non-OK return; present-hook installation; stats every
  300 prepares.
- **2** — every call, in addition to level 1.

Line shape:

```text
[+123.456 ms] ffxDispatch #7 ctx=0x... result=0 (OK) chain=0x20004 (FrameGenerationPrepare) ...
[+456.789 ms] stats prepares=300 frameTimeDelta_ms{...} prepareQpc_ms{...} presentInterval_ms{...} presentsPerPrepare{...}
```

### What the descriptions include

- **create** — FG context flags (decoded), display size, max render size,
  backbuffer format; HUD-less format; FG version; swapchain wrap/new/for-HWND
  variant, swapchain pointer, queue, DXGI swapchain desc; DX12 backend device.
- **configure** — FG `frameGenerationEnabled`, `allowAsyncWorkloads`, present
  and generation callbacks set/not set, `HUDLessColor` present yes/no and format,
  `generationRect`, `frameID`, flags, `onlyPresentGenerated`; swapchain UI
  resource registered and its format; every key/value configure including
  `FRAMEPACINGTUNING` and its five tuning fields; global debug descs.
- **dispatch** — prepare `frameID`, render size, jitter, MV scale,
  `frameTimeDelta`, reset, camera near/far/FOV, `viewSpaceToMetersFactor`,
  depth/MV formats; the deprecated prepare and V2 prepare are both handled;
  camera-info pNext; the generation desc if the game uses one.
- **query** — type and result, plus known output fields/pointers.
- **any non-OK return** — logged at level 1 regardless of call count.

## Stats

Every 300 prepare calls:

- `frameTimeDelta` (ms): average, minimum, maximum, standard deviation.
- Real interval between prepares from QPC (ms): average, minimum, maximum,
  standard deviation.
- Present interval from QPC (ms): average, p50, p95, p99, maximum.
- Presents per prepare: average, minimum, maximum. About 2.0 is expected with
  2x frame generation.

If the present hook could not be installed, the present-interval fields show
`n/a`; the rest of the log still works.

## Milestones

- **M0 — observe and forward (current).** No behavior changes. Validate the
  desc shapes, pacing tuning, `frameTimeDelta` versus measured intervals, and
  present cadence.
- **M1 — pacing analysis.** Correlate the M0 data with the game's FG flags,
  HUD-less/UI path, and swapchain mode. Identify whether the problem is
  `frameTimeDelta`, swapchain pacing tuning, or both.
- **M2 — experiment.** Only after M1: inject a pacing tuning key/value and/or
  correct `frameTimeDelta`. Any behavior change belongs to a milestone that
  explicitly authorizes it.

## Present-hook risk

The hook patches the shared DXGI swapchain vtable's `Present` and `Present1`
slots. It chains to whatever was there before (D3D runtime or another overlay)
and restores the original page protection. If `VirtualProtect` fails, the hook
is skipped and an error is logged. No other behavior is changed.

## Rules

- Do not change arguments, return values or timing in milestone 0.
- Do not modify the loader or the upscaler DLL.
- Keep `third_party/ffx/` headers unmodified.
- Do not download SDKs or launch the game as part of a build.
