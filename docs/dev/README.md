# Developer and agent guide

Start here. Sedici is a Windows x64 C++20 DLL named `amd_fidelityfx_framegeneration_dx12.dll`,
a drop-in replacement for AMD FSR 3.1 frame generation with its own two-stage generator.
The rules are in [`AGENTS.md`](../../AGENTS.md) (read them first); the plan and status are in
[`MILESTONES.md`](../../MILESTONES.md); the known defects are in
[`known-issues.md`](../refactor/known-issues.md).

| File | Content |
| --- | --- |
| [layout.md](layout.md) | The modules under `src/`, what each owns, the real include edges and the known cycles |
| [workflow.md](workflow.md) | Build, test, make a change, commit convention, what to do when a check fails |
| [baseline.md](baseline.md) | The Phase 3 baseline and how to reproduce it |
| [moves.md](moves.md) | Old path to new path for every moved file |

Quick map: `src/dllmain.cpp` is the five exports; `src/abi/` receives the calls;
`src/runtime/` owns contexts and sessions; `src/generator/` is Stage 1; `src/presentation/` owns
the swapchain wrapper, pacing and limiter; `src/ui/` is the overlay and menu; `src/settings/`
is `sedici.toml`; `src/diagnostics/` is log, stats, timing and fault injection; `src/status/` is the diagnostic snapshot; `src/gpu/` holds pure GPU policies.
Includes are written relative to `src/`, for example `#include "presentation/swapchain.h"`.
