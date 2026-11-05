# Sedici

A proxy for AMD's DX12 frame-generation DLL, `amd_fidelityfx_framegeneration_dx12.dll`,
that fixes the frame pacing Cyberpunk 2077 shows with native FSR frame
generation.

Sedici takes the original DLL's name. The original is renamed to
`amd_fidelityfx_framegeneration_dx12.dll.orig` and all five `ffx*` exports are
forwarded with identical arguments, return values and timing. On top of that
forwarding, it sends one optional configure call that removes a measured
frame-time tail, and it writes a local log of what the game asks frame
generation to do.

Nothing else changes. The loader and upscaler DLLs are never touched.

## What it does now

**Milestone 1: the pacing fix is on by default.**

| Behaviour | Default | How to change it |
|---|---|---|
| Frame pacing tuning sent after each FG swapchain is created | **on** (`hybrid`) | `pacing.mode = "off"` or `SEDICI_PACING=off` |
| `allowAsyncWorkloads` rewritten to `false` | **off** (`game`) | `async.mode = "off"` or `SEDICI_ASYNC=0` |
| Log level | `1` | `log.level` or `MFSRFG_LOG` |

With `pacing.mode = "off"` and `async.mode = "game"`, Sedici forwards exactly
what it forwarded in milestone 0: no extra calls, no desc rewrites.

### Why this specific fix

`docs/findings/002-pacing-baseline.md` measured nine configurations on a 75 Hz
panel. The only thing that separated a good run from a bad one was the tail, not
the median:

| | worst interval | stddev |
|---|---|---|
| no fix | 50-67 ms | 1.1-1.4 ms |
| `pacing.mode = "hybrid"` | **9.6 ms** | **0.45 ms** |

Throughput was unchanged, a fresh-process control ruled out a restart artefact,
and a 37 fps frame cap reproduced the same clean tail (so the cap and the fix
address the same contention). The async toggle removes the same tail and does
not stack with it, which is why it stays off by default and is kept for A/B
testing.

## How it works

```
Cyberpunk2077.exe
  └── amd_fidelityfx_framegeneration_dx12.dll      <- Sedici
        └── amd_fidelityfx_framegeneration_dx12.dll.orig   <- AMD's DLL
```

1. **Forward.** Every export is passed through unchanged: same arguments, same
   return code, same ordering. Unknown descs are forwarded untouched.
2. **Tune once per swapchain.** After a successful `ffxCreateContext` whose
   desc chain contains a frame generation swapchain create/wrap desc, Sedici
   sends

   ```
   FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_KEYVALUE_DX12  (0x30008)
     key = FFX_API_CONFIGURE_FG_SWAPCHAIN_KEY_FRAMEPACINGTUNING (2)
     ptr = FfxApiSwapchainFramePacingTuning{...}
   ```

   to that context. The state is tracked per context, so when the game recreates
   its swapchain (resolution change, frame generation toggled) the new context
   gets its own send rather than inheriting the old one's "already applied" flag.
   A new swapchain pointer on the same context also triggers a resend.
3. **Retry, bounded.** If the swapchain path rejects the tuning, Sedici retries
   through the next successful `ffxConfigure` on that context, at most five
   attempts. A rejected tuning is logged and never changes what the game sees.
4. **Observe.** Desc chains, return codes, cadence statistics and detected
   prepare stalls are written to `sedici.log`.

The send happens on Sedici's side of the call. It is an extra `ffxConfigure`
call into AMD's DLL made *after* the game's own call has already completed and
returned, so the game's call itself is untouched.

## Configuration

`sedici.toml` sits beside the deployed DLL. It is read **once**, on the first
`ffx*` call of the process, and never again.

Reading it there rather than in `DllMain` is deliberate: `DllMain` runs under the
loader lock, where file and allocation work is a deadlock risk. The first `ffx*`
call happens roughly a second into process start and long before the game
creates a frame generation context, so it is a load-time read in every sense
that matters.

Every key is optional. A missing key uses its built-in default. A missing file
means every default applies.

### `[pacing]`

| Key | Type | Default | Valid values | Effect |
|---|---|---|---|---|
| `mode` | string | `"hybrid"` | `"hybrid"`, `"off"` | `hybrid` sends the pacing tuning configure; `off` sends nothing |
| `safety_margin_ms` | float | `0.1` | finite, `>= 0`, `<= 1000` | `FfxApiSwapchainFramePacingTuning::safetyMarginInMs` |
| `variance_factor` | float | `0.1` | `0.0` to `1.0` | `varianceFactor` |
| `hybrid_spin_time` | integer | `2` | `0` to `1000` | `hybridSpinTime`, in timer resolution units |
| `wait_for_single_object_on_fence` | boolean | `false` | `true`, `false` | `allowWaitForSingleObjectOnFence` |

`allowHybridSpin` is always `true` when `mode = "hybrid"`; it is the switch the
whole fix hinges on, so it is not exposed. The SDK documents `hybrid_spin_time`
values below 2 as prone to overshoot; Sedici applies them anyway but logs a note.

### `[async]`

| Key | Type | Default | Valid values | Effect |
|---|---|---|---|---|
| `mode` | string | `"game"` | `"game"`, `"off"` | `game` forwards `allowAsyncWorkloads` untouched; `off` clears `AsyncWorkloads` from the FG create flags and forces `allowAsyncWorkloads = false` in every FG configure desc |

The rewrite is scoped: Sedici changes the caller's desc in place for the duration
of one call and restores every byte before returning, so the game never observes
a modified desc.

### `[log]`

| Key | Type | Default | Valid values | Effect |
|---|---|---|---|---|
| `level` | integer | `1` | `0`, `1`, `2` | `0` errors only, `1` normal, `2` verbose |

### Environment overrides

Environment variables win over the file. They are for A/B runs and for testing
one configuration without editing anything.

| Variable | Overrides | Values |
|---|---|---|
| `SEDICI_PACING` | `pacing.mode` | `hybrid`, `off` |
| `SEDICI_ASYNC` | `async.mode` | `0`, `game` |
| `MFSRFG_LOG` | `log.level` | `0`, `1`, `2` |

String values are case-insensitive.

### Errors never fail the game

Configuration is best-effort. Every problem is written to `sedici.log`, the
affected key falls back to its default, and the call proceeds.

| Problem | Result |
|---|---|
| file missing | all defaults |
| syntax error | all defaults; logged with line and column |
| one key has the wrong type | that key's default; logged with line and column |
| one key has an unknown value | that key's default; logged with the value and the allowed set |
| one key is out of range | that key's default; logged with the constraint |
| a section is not a table | the whole section ignored; logged once |
| environment value unknown | override ignored, file/default stays; logged |
| anything unexpected | defaults for keys not yet read; logged |

A syntax error looks like this, and the game still runs:

```text
sedici.toml:5:1: parse error: expected a value - using built-in defaults for every key
```

Per-key errors name the key and what was allowed:

```text
sedici.toml:2:8: pacing.mode: unknown value "turbo" (expected "hybrid" or "off") - using default "hybrid"
sedici.toml:3:1: pacing.variance_factor: must be within [0.0, 1.0] - using default 0.1
```

### Effective configuration log

The first call writes the effective settings once, with provenance:

```text
sedici: read C:\...\bin\x64\sedici.toml (once, on the first ffx call)
sedici:   pacing.mode                             = "hybrid" [file]
sedici:   pacing.safety_margin_ms                 = 0.1 [default]
sedici:   pacing.variance_factor                  = 0.1 [default]
sedici:   pacing.hybrid_spin_time                 = 2 [default]
sedici:   pacing.wait_for_single_object_on_fence  = false [default]
sedici:   async.mode                              = "game" [default]
sedici:   log.level                               = 1 [default]
sedici: effective: frame pacing tuning ON (hybrid spin), async workloads as the game sets them
```

`[default]`, `[file]` and `[env]` say where each value came from. A missing file
prints `sedici: no sedici.toml beside the DLL; using built-in defaults` instead
of the path line.

## Logging

The log is `sedici.log`, beside the deployed DLL, flushed on every line.

```powershell
$env:MFSRFG_LOG = "1"   # 0 = errors only, 1 = normal, 2 = verbose
```

Levels:

- **0** - only load/forward failures and internal errors.
- **1** - the effective configuration; first 50 calls of each export; one full
  description per newly seen desc type; every non-OK return; present-hook
  installation; pacing tuning sends; stats every 300 prepares; one line per
  detected prepare stall.
- **2** - every call, in addition to level 1.

The log level controls logging only. Turning it down does not turn the pacing
fix off.

### What the descriptions include

- **create** - FG context flags (decoded), display size, max render size,
  backbuffer format; HUD-less format; FG version; swapchain wrap/new/for-HWND
  variant, swapchain pointer, queue, DXGI swapchain desc; DX12 backend device.
- **configure** - FG `frameGenerationEnabled`, `allowAsyncWorkloads`, present and
  generation callbacks set/not set, `HUDLessColor` present yes/no and format,
  `generationRect`, `frameID`, flags, `onlyPresentGenerated`; swapchain UI
  resource registered and its format; every key/value configure including
  `FRAMEPACINGTUNING` and its five tuning fields; global debug descs.
- **dispatch** - prepare `frameID`, render size, jitter, MV scale,
  `frameTimeDelta`, reset, camera near/far/FOV, `viewSpaceToMetersFactor`,
  depth/MV formats; the deprecated prepare and V2 prepare are both handled;
  camera-info pNext; the generation desc if the game uses one.
- **query** - type and result, plus known output fields/pointers.
- **any non-OK return** - logged at level 1 regardless of call count.

### Pacing tuning sends

Every send is logged with the values used and AMD's return code:

```text
sedici: FramePacingTuning sent after swapchain create ctx=0x... key=2 safetyMarginInMs=0.100 varianceFactor=0.100 allowHybridSpin=true hybridSpinTime=2 allowWaitForSingleObjectOnFence=false result=0 (OK)
```

A rejection keeps forwarding intact and logs a second line:

```text
sedici: frame pacing tuning was rejected (result=...); forwarding continues unchanged and the retry path through ffxConfigure stays armed
```

### Stats

Every 300 prepare calls:

- `frameTimeDelta` (ms): average, minimum, maximum, standard deviation.
- Real interval between prepares from QPC (ms): average, minimum, maximum,
  standard deviation.
- Present interval from QPC (ms): average, p50, p95, p99, maximum.
- Presents per prepare: average, minimum, maximum. About 2.0 is expected with
  2x frame generation.

If the present hook could not be installed, the present-interval fields show
`n/a`; the rest of the log still works.

### Stall instrumentation

Run I in `docs/findings/002-pacing-baseline.md` contained five episodic stalls: a
prepare interval of 24-72 ms followed immediately by a sub-2 ms one. Nothing
else in the log explained them, so Sedici now reports each one as it happens, at
log level 1.

A stall is a real-frame prepare interval that is **both** more than 3x the
rolling median of the last 61 prepare intervals **and** at least 25 ms. The
median is taken before the new sample is added, so a stall cannot hide in its
own baseline. The log is rate-limited to one line per second.

```text
sedici: stall prepare interval=72.406 ms median=13.402 ms ratio=5.402x frameID=4242 reset=n/a presentsSincePrepare=0 swapchainEvents1s{count=1 last=configure 812.4 ms ago} qpc=78860918227
```

| Field | Meaning |
|---|---|
| `interval` | measured gap between prepares, ms |
| `median` | rolling median the comparison used, ms |
| `ratio` | `interval / median` |
| `frameID` | the prepare's frame id |
| `reset` | the prepare's reset flag, or `n/a` for the legacy prepare desc, whose reset field is unused |
| `presentsSincePrepare` | presents seen since the previous prepare |
| `swapchainEvents1s` | swapchain creates and successful configures in the last second, with the age of the most recent |
| `qpc` | raw QPC timestamp, for correlating with external tools |

If a stall line appears while playing, the `swapchainEvents1s` field is the first
thing to look at: a non-zero count means the hitch coincided with the game
rebuilding its swapchain or reconfiguring frame generation.

## Requirements

- Windows x64, Visual Studio 2022 C++ tools, CMake 3.21+, Windows SDK.
- Static CRT, C++20.
- The FidelityFX/FSR headers in `third_party/ffx/` are unmodified SDK v2.4.0
  headers. See `third_party/ffx/README.md` for upstream paths and blob IDs.
- toml++ v3.4.0, single-header flavour, unmodified, in
  `third_party/tomlplusplus/toml.hpp`. MIT.

No other dependencies.

### toml++ provenance

| | |
|---|---|
| Project | [marzer/tomlplusplus](https://github.com/marzer/tomlplusplus) |
| Version | **v3.4.0** (tag `v3.4.0`, released 2023-10-13) |
| File | single-header `toml.hpp`, 485,931 bytes |
| SHA-256 | `6B5172AD4DD6519AEC67B919181FA7A38A2234131E5B2AFA232DFE444819783E` |
| Licence | MIT (`SPDX-License-Identifier: MIT` in the header preamble) |
| Source | `https://raw.githubusercontent.com/marzer/tomlplusplus/v3.4.0/toml.hpp` |

The file is vendored byte-for-byte; nothing in it is edited. Two documented
compile-time switches are set in `src/config.cpp` before the include, not in the
header:

- `TOML_EXCEPTIONS=0` so `toml::parse` returns a `parse_result` instead of
  throwing. Nothing about configuration parsing may throw across an `ffx*`
  export boundary.
- `TOML_ENABLE_FORMATTERS=0` because Sedici only parses; this drops the
  serialisers.

`CMakeLists.txt` fails the configure step with an explanatory message if the
header is missing.

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

There must be exactly five exports: `ffxCreateContext`, `ffxDestroyContext`,
`ffxConfigure`, `ffxQuery`, `ffxDispatch`. No helper exports, including the
proxy marker.

## Deploy and restore

```powershell
.\tools\deploy.ps1 -Game cp2077
.\tools\restore.ps1 -Game cp2077
```

`deploy.ps1`:

- resolves the game from `tools/games.json` or an explicit `-GamePath`;
- refuses to run while `Cyberpunk2077.exe` is running;
- never overwrites an existing `.orig` backup;
- refuses to overwrite a DLL that is not Sedici's;
- copies `sedici.toml` only if one is not already there, so a tuned file is kept.

`restore.ps1` puts the original DLL back and removes `sedici.log` and
`sedici.toml`. Restore is an uninstall, not a merge: **copy your `sedici.toml`
out first if you tuned it.** It refuses to touch a DLL that is not Sedici's.

Both scripts only touch `amd_fidelityfx_framegeneration_dx12.dll`, its `.orig`
backup, `sedici.toml` and `sedici.log`. The loader and upscaler DLLs are never
modified.

## Milestones

- **M0 - observe and forward.** No behavior changes. Validate desc shapes,
  pacing tuning, `frameTimeDelta` versus measured intervals, present cadence.
  Shipped.
- **M1 - pacing fix on by default (current).** `sedici.toml` configuration,
  per-context pacing send, stall instrumentation, confirmation capture runs.
- **M2 - further tuning.** Only if M1's confirmation runs point somewhere:
  correcting `frameTimeDelta`, or extending the tuning to latency-driven cases.

## Present-hook risk

The hook patches the shared DXGI swapchain vtable's `Present` and `Present1`
slots. It chains to whatever was there before (D3D runtime or another overlay)
and restores the original page protection. If `VirtualProtect` fails, the hook
is skipped and an error is logged. No other behavior is changed. The hook is only
used for timing measurements; removing it does not affect the pacing fix.

## Rules

- Forward everything. Only the documented pacing send and the opt-in async
  rewrite may change behavior, and both are inert when configured off.
- Never fail a game call because of Sedici's own bookkeeping.
- Do not modify the loader or the upscaler DLL.
- Keep `third_party/ffx/` and `third_party/tomlplusplus/` unmodified.
- Do not download SDKs or launch the game as part of a build.
