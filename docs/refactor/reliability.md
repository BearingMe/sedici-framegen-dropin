# Reliability and integrated acceptance

Status: target contracts for the refactor, not claims that the current build passes.
The architecture's ownership rules are in [architecture.md](architecture.md).

## Error contract

Use a small C++20-compatible, `[[nodiscard]]` result/error representation for
recoverable operations. Do not require C++23 `std::expected` merely for this refactor.
Errors carry stable domain/code, underlying HRESULT/Win32/FFX result where relevant,
session/frame/operation identity, and the selected recovery action.

Distinguish unsupported capabilities, invalid settings, allocation failures,
submission failures, retirement timeouts, device removal, and contract violations.
A late generated frame being dropped is a scheduling outcome, not necessarily an
exceptional error. Accepted, recorded, submitted, and retired work are distinct.

- Contain C++ exceptions at FFX/COM/callback boundaries. `noexcept` is not a catch;
  allocations inside it must have an intentional failure policy.
- Never silently discard a failed presenter submission and advance ownership as
  though it succeeded. Return an actionable accepted/rejected outcome.
- Do not replace genuine backend errors with success, invent compatibility answers,
  or expose Sedici bookkeeping failures as game failures when a safe path exists.
- Logging and persistence failures are non-fatal; preserve runtime state and report
  their unavailable/degraded status where possible.
- Recoverable draining failures retain resources still in use. Registry removal,
  error returns, or C++ destruction do not prove GPU retirement.
- Catching an access violation does not establish that a corrupted process is safe
  to continue. Do not make blanket crash-containment promises.

## Watchdog meaning and limits

The watchdog is runtime invariant monitoring and bounded recovery, not a new
external supervisor process. Observe submission/retirement progress, queue depth,
generation failures, missed deadlines, settings acknowledgements, overlay health,
and device state. Runtime executes recovery from those observations.

Drop stale generated work instead of accumulating a presentation backlog. Reset
history at defined cuts, discontinuities, FG toggles, and geometry changes. Disable
a failing optional component independently where its failure does not invalidate
the frame/session. Avoid uncontrolled per-frame initialization retries or rapid
backend/quality oscillation; thresholds and retry limits must be explicit/tested.

The observer must not require the same lock as the operation being monitored.
Timeout handling must not CPU-signal fictitious GPU completion, release in-flight
memory, or terminate a worker thread forcibly. Preserve real device-loss results.
No watchdog can promise to recover arbitrary driver hangs or process corruption.

Backend fallback scope is specified in architecture.md. Real-frame degradation,
residual-stage unavailability, and AMD takeover are different outcomes; report which
one happened. Whether an unavailable model permits analytical-only operation or
selects AMD is an explicit product policy still to be settled.

## Diagnostics contract

Capture bounded structured events on latency-sensitive paths; format/write them
outside output recording and presentation-critical locks. Use stable event codes,
monotonic timestamps, session/frame IDs, settings revisions, underlying results,
and recovery reasons. Rate-limit repetition and count lost diagnostic events.

```text
event=settings_applied session=3 revision=18 real_fps_limit=36.5
event=output_dropped session=3 frame=904 phase=0.5 reason=deadline
event=backend_selected requested=sedici active=fsr3 reason=unsupported_format
```

Keep a bounded recent-event history and report both requested and active backend,
presentation policy, and settings. A slow/unwritable log file must not stall game
rendering. Diagnostic shutdown is bounded and outside `DllMain`.

Publish coherent metrics snapshots to UI. Name measurement points honestly:
real-frame entry/admission, output Present calls, generated outputs accepted/dropped,
GPU pass times, waits, and retirement/backpressure. CPU Present timestamps do not
measure physical scanout, displayed FPS, or input-to-photon latency. Unknown data
is unavailable, not a misleading zero or a multiplied real-frame estimate.

## Focused acceptance coverage

Reuse production targets and the existing small offline DX12 host. Prefer a few
composed scenarios and deterministic policy tests over a broad GPU option matrix.

| Group | Required evidence |
| --- | --- |
| Policy | Fake-time rate changes, hitch/reset behavior, complete settings validation, selection/fallback decisions, lifecycle transitions |
| ABI | Actual five-export DLL boundary, unchanged compatibility forwarding, descriptor restoration, binary/context ownership and return codes |
| Combined session | FG, Mini/Full UI, live limiter changes, acknowledged settings, save/cancel, and truthful metrics working together |
| Lifecycle | FG off/on, resize/window replacement, outstanding GPU work, input restoration, clean shutdown |
| Failure injection | Unavailable/incompatible AMD dependency, failed Sedici initialization, allocation/submission rejection, setting rejection, log/save failure |
| GPU correctness | Multiple in-flight frames, descriptor/resource lifetime, chronological outputs, HUD and overlay preservation, image comparisons |
| Performance | Production-resolution GPU pass times, total generation cost, and bounded backlog, without validation instrumentation skew |

The combined scenario must enter through production integration, not manually call
the overlay renderer and separately exercise the generator. Exercise both backend
paths under their documented capabilities. Backend stubs enable deterministic
failure tests but do not prove the official binary's ABI or presentation behavior.

Keep PSNR/SSIM and visual inspection for the analytical stage. For Stage 2, validate
the feature layout, normalization, reset behavior, reconstruction, model provenance,
and temporal behavior. Do not invent universal quality thresholds from toy scenes.

The paper's small weight count does not bound activation memory: the 960x540x16
FP16 input alone is about 15.8 MiB and a 24-channel stem about 23.7 MiB. Include
intermediates, skip connections, scratch, and frame-slot multiplicity in the resource
budget. Measure feature packing, inference, and reconstruction as well as analytical
dispatches; report composition/copy/wait costs separately rather than hiding them.

## Tooling and release evidence

- Register relevant offline checks with CTest and keep fixtures isolated from game
  configuration. Tests must remain meaningful in optimized builds.
- Use installed DX12 debug/GBV facilities in the offline host, configured before
  creating its device. Do not enable the debug layer late on a game-owned device.
- Use DRED when available; enabling it after device creation is not retroactive.
- Keep validation-heavy runs separate from GPU performance acceptance. Record
  injected overlays and validation availability rather than filtering faults away.
- Use host-side MSVC ASan and focused static analysis for memory lifetime, unchecked
  results, and exception/concurrency mistakes. They do not prove GPU correctness.
- Automatically verify exactly five DLL exports and record binary/model revisions.
- No SDK/runtime downloads or game launches as a side effect of build/test.
  Deployment and any agent-driven game launch still require separate authorization.

## Research references

These are design references, not proof of the installed game's implementation or
authorization to adopt a new dependency. Pin reviewed revisions before adapting
upstream material into production.

- [C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines.html): ownership, RAII, errors, shared state, callbacks and locks.
- [AMD frame-generation swapchain](https://gpuopen.com/manuals/fsr_sdk/techniques/frame-interpolation-swap-chain/): generation/composition/pacing handoffs.
- [DX12 multi-engine synchronization](https://learn.microsoft.com/en-us/windows/win32/direct3d12/user-mode-heap-synchronization): queue ordering and fence timelines.
- [Microsoft MiniEngine allocator pool](https://github.com/microsoft/DirectX-Graphics-Samples/blob/master/MiniEngine/Core/CommandAllocatorPool.cpp): fence-qualified reuse, not an engine to vendor wholesale.
- [ImGui integration](https://github.com/ocornut/imgui/wiki/Getting-Started): application-owned backend/queue integration.
- [DirectML resource lifetime](https://learn.microsoft.com/en-us/windows/ai/directml/dml-resource-lifetime): inference does not hide synchronization; DirectML is not selected here.
- [GPU-based validation](https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-d3d12-debug-layer-gpu-based-validation) and [DRED](https://learn.microsoft.com/en-us/windows/win32/direct3d12/use-dred): validation and device-fault evidence.
- [CMake buildsystem](https://cmake.org/cmake/help/latest/manual/cmake-buildsystem.7.html): target-scoped implementation sharing.
- [MSVC ASan](https://learn.microsoft.com/en-us/cpp/sanitizers/asan?view=msvc-170): host memory diagnostics, not a production runtime dependency.

Existing local evidence remains in [findings](../findings/) and
[Mob-FGSR attribution](../mobfgsr-attribution.md). Do not reclassify historical
component checks as successful combined-product acceptance.
