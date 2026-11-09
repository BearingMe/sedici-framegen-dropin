# 003 - Default pacing fix and confirmation runs

> **Status (2026-11-09): superseded, closed history.** This records the milestone when
> Sedici sent AMD's pacing tuning by default. That path and the `[pacing]` and `[async]`
> keys were removed (d95d454). Today's defaults are `fg.present = "vrr"`,
> `tear_guard = "scanline"` and `limiter.fps = 0` (`src/config.h`). The run set J-M was
> never completed.

Milestone 1 turns the finding from
[002-pacing-baseline.md](002-pacing-baseline.md) into Sedici's default behaviour
and then re-measures it on fresh processes.

## Why this run set exists

002 established three things:

- `SEDICI_PACING=hybrid` removes the frame-time tail: worst interval 50-67 ms
  down to 9.6 ms, stddev 1.1-1.4 ms down to 0.45 ms, no throughput cost.
- `SEDICI_ASYNC=0` does the same thing and does not stack with it.
- A 37 fps frame cap reproduces the same clean tail, so the cap and the toggles
  are addressing the same contention.

002 was a one-capture-per-configuration screening run driven by environment
variables. It did not answer the question that matters for shipping: does the
fix still work when it is the *default*, read from a config file, on a process
that was started fresh? Run I also left five unexplained stalls.

This run set answers exactly that, and nothing else. It is not a new
investigation; it is confirmation.

## What changed in Sedici

| Change | Detail |
|---|---|
| Configuration | `sedici.toml` beside the DLL, parsed with toml++ v3.4.0, read once on the first `ffx*` call |
| Defaults | `pacing.mode = "hybrid"`, `async.mode = "game"`, `log.level = 1` |
| Precedence | built-in default < `sedici.toml` < environment (`SEDICI_PACING`, `SEDICI_ASYNC`, `MFSRFG_LOG`) |
| Failure mode | every config problem is logged with line and column, that key falls back to its default, the game call is never failed |
| Pacing send | once per frame generation context; a recreated swapchain gets its own send; bounded retry through `ffxConfigure` unchanged |
| Stall instrumentation | log level 1: prepare interval > 3x rolling median and >= 25 ms, at most one line per second |
| Opt-out | `pacing.mode = "off"` + `async.mode = "game"` forwards exactly what milestone 0 forwarded |

The tuning values sent are the ones 002 used: `safetyMarginInMs = 0.1`,
`varianceFactor = 0.1`, `allowHybridSpin = true`, `hybridSpinTime = 2`,
`allowWaitForSingleObjectOnFence = false`.

## Pre-flight verification

No game was launched for this. Three checks were done first.

**Export table.** Release build, `dumpbin /exports`, exactly five:
`ffxConfigure`, `ffxCreateContext`, `ffxDestroyContext`, `ffxDispatch`,
`ffxQuery`.

**Proxy behaviour**, against a stub `.orig` and a host that drives
create + configure + destroy (35 assertions, all passing):

| Case | Result |
|---|---|
| no `sedici.toml`, no env | defaults, pacing sent once, create flags unchanged, log says "no sedici.toml" |
| `pacing.mode = "off"`, `async.mode = "game"` | no pacing configure at all, flags unchanged (0x9), async untouched |
| env `SEDICI_PACING=off` over file `hybrid` | override wins, provenance says `[env]` |
| syntax error in file | `sedici.toml:<line>:<col>: parse error`, defaults used, tuning still sent |
| unknown value `pacing.mode = "turbo"` | logged with position, default `hybrid` used, sibling key still honoured |
| `async.mode = "off"` | create flags 0x8 (`AsyncWorkloads` cleared), forwarded `allowAsyncWorkloads=0`, caller's desc restored to its original bytes afterwards |
| custom tuning values | forwarded verbatim (0.25 / 0.4 / spin 5 / wait true) |
| swapchain recreate on the same context | second send, recreate logged |
| stall probe | exactly one line despite two stalls inside a second (rate limit works) |

**Deployment.** Release DLL deployed to `bin\x64`; `sedici.toml` written from the
repository copy; `.orig` untouched. A second deploy kept the existing
`sedici.toml` rather than overwriting it, which is the rule the script is
supposed to follow.

| File | Bytes | SHA-256 |
|---|---|---|
| `amd_fidelityfx_framegeneration_dx12.dll` (Sedici) | 518,656 | `C96F215C5F78AC5BD8273926EBCE88916AB21870BDC41930292A6229E2011705` |
| `amd_fidelityfx_framegeneration_dx12.dll.orig` | 40,085,776 | `02297BEEDD285E822D3A64F314CF00FAF378DCEC0EDC47FF0C4DD71B3A8C2F18` |
| `sedici.toml` (shipped defaults) | 2,588 | `A3709E0DDDB4D3E6E3393D137A5001FCA1EC8C13DC323A87037A83627C84F8C9` |
| `amd_fidelityfx_loader_dx12.dll` | 26,376 | `E2D85AA05A9BD9ED8B38935FDF5199372CCA6F74C12015143BB6F945EE1608AA` |
| `amd_fidelityfx_upscaler_dx12.dll` | 28,761,864 | `D0DCCCC74A43C44BA435B7A369B456E0970D8A4464E4BD683119B374F2C9FB46` |

## Fixed conditions

Identical to 002, so the numbers are comparable:

- Path tracing off, base frame rate >= 60 fps.
- FSR Quality, frame generation on, Anti-Lag 2 on, VSync off (it cannot be
  enabled with FG).
- Borderless windowed, 2560x1080, 75 Hz panel.
- Same spot, same 30 s hand-driven camera pan.
- 45 s warm-up before the capture window, 30 s captured.
- Every run on a **fresh process**, launched directly from `bin\x64`, so the
  environment variables are the ones the launching shell had.

## Run list

| Run | Override | Purpose |
|---|---|---|
| J | `SEDICI_PACING=off` | no-fix control; should reproduce the A/F tail |
| K | none (file defaults) | the default fix |
| L | none (file defaults) | repeat of K on another fresh process |
| M | none, Maximum FPS 37 | repeat of I with hybrid only; does the run-I stall come back |

`SEDICI_PACING` is set in the launching shell for J only. K, L and M use the
deployed `sedici.toml` with `pacing.mode = "hybrid"`.

## Results

Pending. Each run is appended below as it is captured, with the
`analyze.py` per-swapchain table, the run's `sedici.log` configuration block, and
any stall-instrumentation lines.

## Final read

Pending: does K/L reproduce 002's run E tail (worst <= ~15 ms, stddev <= ~0.5 ms)
on two fresh processes while J reproduces the bad tail, and does M show the
run-I stalls again - and if so, what does the stall log say they coincide with.
