# Reliability and integrated acceptance

> **Update: the AMD backend was removed.** The owner decided to drop the AMD
> fallback entirely as long as Sedici stays a drop-in replacement. Text below that
> proposes or describes an AMD backend, a bundled AMD binary, `.orig` forwarding or
> backend fallback is kept as design history and no longer applies. The last commit
> with that code is the tag `pre-amd-removal`; [amd-binary.md](amd-binary.md) keeps
> what was learned about the binary.

> **Status (2026-11-09): target contracts plus implemented scope.** Implemented and
> accepted in P2.4-P2.10: the error contract (reason string plus FFX code, K115),
> bounded retention and parking, the asynchronous log with drop accounting, and the diag
> line. Not built: the watchdog component and the structured event vocabulary. The
> "Current coverage" section is the P2.2 baseline and is historical; the registered
> CTest entries (41 today) are in `CMakeLists.txt` and `MILESTONES.md`.

Status: target contracts for the refactor, not claims that the current build passes.
The architecture's ownership rules are in [architecture.md](architecture.md).

## Error contract

Use a small C++20-compatible, `[[nodiscard]]` result/error representation for
recoverable operations. Do not require C++23 `std::expected` merely for this refactor.
Errors carry stable domain/code, underlying HRESULT/Win32/FFX result where relevant,
session/frame/operation identity, and the selected recovery action.

**Implementation status (owner decision, 2026-11-08):** the typed error representation
below is deferred to the upcoming code refactor and is not part of P2.10. Today a failure
is a literal reason string plus an FFX return code, and a router rejection logs the
operation and context it refused (K115). When the refactor builds the typed contract it
follows ordinary C++20 practice: `enum class` domains and codes rather than strings or
integers, a `[[nodiscard]]` result type, exceptions contained at foreign boundaries,
RAII for ownership, and no macros or global error state.

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

There is no backend fallback. Real-frame degradation and residual-stage
unavailability are different outcomes; report which one happened. Whether an
unavailable model permits analytical-only operation is a product policy still to
be settled.

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
| Failure injection | Failed Sedici initialization, allocation/submission rejection, setting rejection, log/save failure |
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

## Current coverage and gaps (P2.2 baseline)

Status: recorded baseline as of `e745008`, plus coverage added through `f95b54d`;
not freshly rerun during the [P2.1 audit](audit.md). Coverage is listed against the
groups above. "Known failure" means a check
that fails today, tracked as expected-fail with its evidence; it must not be
relabeled green, and an unexpected pass fails the run.

| Group | Covered by | Gap |
| --- | --- | --- |
| Policy | `sedici_fgcheck`: QPC conversion, 2x pacing closed loop, output schedule (2x/1.75x/1.5x/1.25x, adaptive grid, hitch, < 20 fps, pause). `sedici_regression`: TOML parsing and limiter schedule. | Settings transactions (P2.6 does not exist yet); presenter latency/guard decisions are only exercised through windows, not as pure policy. |
| ABI | `sedici_abi exports`: exactly the five named exports in the built DLL. `sedici_abi contract`: with a tripwire stub beside the DLL that must never be called, version queries return the recorded providers and every foreign, null or unknown input gets its defined error code. (The `forward`, `pacing`, `async-off`, `missing-orig` and read-only AMD scenarios were removed with the AMD backend.) | Game-side behaviour; the contract is checked only against the headers and recorded AMD answers. |
| Combined session | `sedici_abi session` (and the scenarios above, which share its rig): fg.mode = "sedici" through the five exports with a real swapchain: FG create/wrap/configure, prepare + Present for 200 frames with the Mini overlay and a 30 fps limiter, FG off/on, resize, destroy. Evidence is the DLL's own log (vrr presenter and 2x schedule, overlay initialized, overlay metrics of ~30 real / ~60 output fps, the limiter's FG on/off label, no setup failures) plus return codes and the D3D12 debug layer. | Live limiter changes and settings save/cancel: see accepted gaps below. |
| Lifecycle | `sedici_fgcheck --present`: wrapper resize, teardown, vrr recreation, 2x/1.5x/adaptive/overload presentation. `sedici_abi session`: FG off/on and resize through the exports. `sedici_abi window-replace`: three full sessions in a row, one on the same window, one on a replaced window, with per-session contexts and presenters counted in the log. `sedici_abi destroy-inflight`: three rounds that destroy the contexts, and in one round resize, with the game's and the presenter's GPU work still outstanding (nothing waits between frames). `sedici_abi overlay-hotkey`: Alt+F10 through the hooked window procedure, Off/Mini/Full cycle, session ended while Full, window replaced, cursor clip restored. `sedici_regression`: window/input capture and restoration. | None open for P2.2; see accepted gaps below. |
| Failure injection | Malformed TOML (`sedici_abi bad-config`); RTSS-injected fallback to "vsync"; generator refusal (fgcheck contract checks). `sedici_abi log-failure`: `sedici.log` blocked by a directory, a full session still runs clean. `sedici_abi invalid-calls`: no backend desc, 0x0 display, null game queue, null swapchain slot (the caller's chain stays usable), prepare with no list/resources, unknown dispatch type on an owned context, destroyed handle, double destroy; a hostile-call burst leaves a running session working. | Accepted gaps below. |
| GPU correctness | `sedici_fgcheck`: Stage 1 PSNR/SSIM at 1/2, 1/3, 2/3 from one prepare; HUD preservation; two frames in flight with per-slot fences (debug layer); no back-to-back flips. `sedici_regression gpu-*`: overlay composition. | Chronological readback through the vrr presenter (the FLIP_SEQUENTIAL readback check only exists for the old path). |
| Performance | Stage 1 time per scene in `sedici_fgcheck`. | Production-resolution timings; generation and presenter cost in game. |

### P2.2 accepted gaps

Evidence record: [finding 010](../findings/010-p22-coverage-gaps.md).

This is the historical P2.2 baseline, not a claim that later work has made no
progress. The gaps were **accepted, not forgotten**. Current status is in the
[Phase 2 progress table](../../MILESTONES.md#phase-2-progress) and
[known issues](../archive/known-issues.md) (now GitHub issues). The uncommitted P2.5 work now injects selected
bookkeeping, queue-signal, command-list Close and allocator Reset failures in the
production-object host; it does not yet establish bounded live-device recovery.
The P2.6 prototype tests settings-model save/cancel and some failure cases without
clicks. Full-menu Begin/Save now use a bounded worker in the working tree, with a
paused-I/O callback test, an actual-DLL baseline-completion check, and an isolated
shared-core test-module unload check; actual proxy-DLL unload verification and the
external-edit replacement race remain open. Full foreground input and integrated UI interactions remain unaccepted.

| Gap | Why it is not covered | Where it is closed |
| --- | --- | --- |
| Click-driven cap change, Save, Cancel and the settings round trip in the Full menu | The Full menu is immediate-mode ImGui laid out at runtime; its controls have no stable coordinates and there is no UI model to drive. Full input also needs the host to be the foreground window, which an unattended CTest run does not get (`sedici_abi overlay-hotkey` records this as an INFO line). | P2.6 (settings transactions) and P2.8 introduce a settings/UI model that a test can drive without pixels. The hotkey and clip restoration are exercised; availability of actual Full input depends on foreground access, and click-driven Save/Cancel remain uncovered. |
| Allocation and submission failures (device removed, `CreateCommittedResource`/`ExecuteCommandLists` failing) | D3D12 offers no portable fault injection on a real device; it needs a device/queue abstraction in production code. | P2.5 (GPU submission and retirement) defines that boundary; its tests inject here. |
| Save-time failures (read-only or externally edited `sedici.toml`) | Reachable only through the Full menu's Save button (see first row). `config_save` itself is covered in `sedici_regression`. | P2.6. |
| Chronological readback through the vrr presenter | The presenter hands images to DXGI, not to a readable queue; the FLIP_SEQUENTIAL readback exists only for the old path. | P2.7 (output pacing) when the presenter gets a testable output sink. |
| Production-resolution performance | Needs a measured run on the target GPU, not a pass/fail test. | P2.11 acceptance. |

Overlapping the creation of a second wrapped swapchain while the first is alive is
not exercised; the game destroys the first session before creating the next, and no
requirement says otherwise.

Known failures (expected-fail, `sedici_fgcheck --present`):

- `generated presentation` — `"vsync"` path at `vsync` pacing in the test
  window: 3 of 5 midpoints skipped as late. Fails identically at `a87d145`;
  hidden until RTSS was closed (finding 009).
- `session` — `"vsync"` path through `fg::Session`: the frame 2 midpoint reads
  back at 16.02 dB. Same history.

Documentation discrepancies found while writing the checks (for P2.1
reconciliation, not fixed here):

- The shipped `sedici.toml` says `async.mode = "off"` clears
  `FFX_FRAMEGENERATION_ENABLE_ASYNC_WORKLOAD_SUPPORT` from the create flags.
  The code stopped doing that in `6c2c180`; README describes the current
  behaviour (only `allowAsyncWorkloads` in configure), and `sedici_abi async-off`
  checks the README behaviour.

Build: the DLL, `sedici_regression` and `sedici_fgcheck` share one `sedici_core` object library, so tests exercise the same compiled production objects (37 CTest entries are registered; the count at the P2.2 baseline was 13).

Third-party interference: with RTSS running, vrr checks are reported as skipped
(the wrapper refuses "vrr" under RTSS) and RTSS has drawn its overlay into test
back buffers, which made the session check fail at frame 0 in every earlier run.

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
