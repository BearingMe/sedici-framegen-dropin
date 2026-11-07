# P2.1 current-state audit and P2.2 handoff

Audit baseline: commit `f95b54d` (`test: add export-level harness and P2.2
coverage baseline`), clean working tree when this audit began. This is a static
source review, not a new game run or a fresh test result. The coverage evidence
in [reliability.md](reliability.md#current-coverage-and-gaps-p22-baseline) records
earlier runs; results can depend on GPU, display, and injected overlays. Code
observations below are not proof of safety under all thread interleavings.

## Call paths and current owners

| Boundary | Current behavior and owner | Evidence / limitation |
| --- | --- | --- |
| Startup / forwarding | `settings()` initializes on first export; `forward.cpp` loads the adjacent `.orig` once and resolves all five functions. Missing library/exports leave forwarding unavailable. | `src/dllmain.cpp`, `src/forward.cpp`; ABI stub is not AMD compatibility evidence. |
| Create | In `fg.mode = "sedici"`, recognized FG/wrap descriptors enter `route_create`; success publishes an owned handle. Other creates go to `.orig`. Recognized creation failure returns an error, not automatic `.orig` fallback. | `src/fg_route.cpp`, `src/dllmain.cpp`; partial native-chain replacement can complicate recovery. |
| Configure / dispatch / query | Registry-owned handles enter the Sedici implementation; unrecognized handles go to `.orig`. Sedici FG dispatch accepts prepare types, owned queries are rejected, and wrapper configure currently returns success without applying the descriptor. | `src/fg_route.cpp`; test compatibility per descriptor, not by assuming success means support. |
| Integration | A `WrapHandle` owns a COM wrapper; `Session` owns `Context` and attaches a generator sink to a wrapper found through a separate raw-pointer registry. FG configure associates them. | `src/fg_integration.cpp`; lookup returns a raw pointer after its mutex is released, before `AddRef`: concurrent removal/lifetime safety needs proof. |
| Generation | Prepare records input/history through `Context`; wrapper invokes `Session::record` on a supplied command list. Context retains raw input pointers and a Stage1 instance; caller ordering and input lifetime matter. HUD composition is currently part of generation. | `src/fg_context.h`, `src/fg_integration.cpp`, `src/fg_stage1.cpp`; do not infer GPU completion from COM lifetime. |
| Present and UI | Sedici installs a real-present hook for timing/limiter and output hooks for overlay. Wrapper handles game-thread presentation or hands jobs to the VRR presenter thread; output hooks can execute on that thread. `.orig` uses the native hook and FFX present callback path. | `src/dllmain.cpp`, `src/fg_swapchain.cpp`, `src/fg_presenter.h`, `src/overlay.cpp`; callbacks and input restoration across replacement need coverage. |
| Destroy | Registry entry is moved out and erased under its mutex, then destroyed outside it. `Session::destroy` detaches, waits up to 200 ms and calls `Context::destroy`; its caller does not retain the session after a failed destroy. Wrapper destructor calls `drain` but ignores its result. | `src/fg_route.cpp`, `src/fg_integration.cpp`, `src/fg_swapchain.cpp`; inspect failure and resource lifetime under actual in-flight work before asserting safe teardown. A later stale handle is no longer recognized and may be forwarded. |

`g_owned_mutex` protects routing operations, but is held during create, configure,
query and dispatch into owned components, not merely lookup. Wrapper callbacks
and lock ordering must be checked against the actual execution path before changing
that lock. The wrapper registry uses a different mutex. `Swapchain` has its own
mutex; the VRR `Presenter` has a job mutex, worker, queue and condition variables.
Do not assume registry removal alone synchronizes presenter callbacks or GPU work.

## Lifecycle and GPU evidence map

| Event | Code-observed behavior | Open evidence / target owner |
| --- | --- | --- |
| Partial creation | Wrapper may create resources and attempt native chain recreation before publication. Presenter adoption documents failure both before and after native destruction. | P2.3/P2.4: distinguish safe fallback before and after ownership changes; do not promise untouched chain on every failure. |
| FG off/on | Configure updates context and attaches/detaches sink according to enabled state. | P2.2: combined toggle with outstanding work; P2.4: explicit callback lifetime. |
| Resize | Wrapper waits for presenter outstanding jobs, drains, and resizes; errors propagate. | P2.2/P2.5: GPU-in-flight, error and window replacement scenarios, not just successful resize. |
| Submission | Wrapper signals a game-queue fence and calls `submitted`; `Session::submitted` ignores `track_submission`'s return. VRR jobs retain COM references and a ready fence; presenter owns a separate queue/fence. | P2.5: injected signal/tracking/submit failure, queue ordering, per-consumer retirement; a reference is not retirement. |
| Shutdown / device loss | Several waits are bounded, but bounded wait is not itself safe retirement; destructor paths must be analyzed on failed waits. | P2.2/P2.5: timeout and device-loss injection; P2.10: bounded reporting and recovery. |

## Configuration and diagnostic observations

`src/config.cpp` ingests documented environment overrides; `src/live.*` holds
individual atomic values. `src/ui.cpp` edits these directly, resets limiter or
pacing state, saves via `config_save` from the UI path, and restores a captured
snapshot on Cancel. Save checks a file stamp and avoids writing values sourced from
environment overrides; this is not revisioned acknowledgement of a coherent
multi-setting transaction. UI's 36.5/37 FPS presets are existing behavior, not
approved future defaults.

Separate direct environment reads exist: `SEDICI_FG_ALLOW_RTSS`, `SEDICI_FG_SYNC`,
`SEDICI_FG_ALIGN` in `src/fg_swapchain.cpp` and `SEDICI_FG_SCANLINE_LEAD_US` in
`src/fg_presenter.cpp`. Their full validation and precedence must be characterized
before migration. `sedici.toml` and `src/config.h` say async-off clears create
flags, while the current implementation and ABI baseline say configure-only; the
shipped comment is stale. This audit does not edit the shipped configuration.

Logging and error paths are not yet the proposed bounded watchdog. `src/log.cpp`
opens, writes and flushes the log synchronously under a mutex; calls on critical
threads can therefore do file I/O. In particular, the wrapper accepts any owned
swapchain configure, `Presenter::submit` catches exceptions without reporting them,
and retirement tracking results can be ignored. Their exact
failure implications need injection, not speculative assertions of observed game
failure. See `src/fg_route.cpp`, `src/fg_presenter.h`, `src/fg_integration.cpp`.

## P2.2 prioritized handoff

The existing executable-level coverage and expected failures are detailed in
[reliability.md](reliability.md#current-coverage-and-gaps-p22-baseline). CMake
now builds one `sedici_core` object library shared by the DLL, `sedici_regression`
and `sedici_fgcheck` (P2.2). `sedici_abi` loads the actual DLL plus a stub `.orig`.
Only `sedici_amd_probe` touches the real AMD ABI, see [amd-binary.md](amd-binary.md).

| Priority | Scenario and production entry point | Expected evidence / destination |
| --- | --- | --- |
| 1 | Export-level owned handle create/configure/destroy, stale handle and concurrent wrapper detach/lookup, using injectable synchronization where needed. | No foreign-handle forwarding or callback after owner destruction; characterize current failures explicitly. P2.4. |
| 1 | Present/resize/destroy while genuinely submitted work is outstanding; inject rejected submission, signal/retirement failure and timeout. | No premature reuse/release or fictitious completion; record the current failure without treating a timeout as a pass. P2.5. |
| 2 | Extend `sedici_abi session` through actual input to change cap, open Full UI, Save/Cancel, toggle FG and replace window. | Observe effective behavior, input restoration, file outcome and metrics together; distinguish future acknowledgement semantics from today's implementation. P2.6–P2.8. |
| 2 | Stub `.orig` create failure and owned creation refusal, including native-chain replacement failure. | Record exact return/ownership; no assertion of automatic fallback. P2.3/P2.4. |
| 3 | Inject log and persistence failures; exercise malformed settings and external edits. | Game call and old file remain safe; errors observable. P2.6/P2.10. |
| 3 | Retain PSNR/SSIM, expected-fail presentation cases and five-export DLL test; compare production-resolution performance separately from validation. | Detect regression without laundering known failures into green acceptance. P2.2/P2.11. |

Use focused host-side seams for controlled failure and scheduling, not a broad GPU
matrix. Add no seam that changes the public ABI. The official binary and its
composition/redistribution feasibility remain P2.3 evidence, not a stub result.

## Review and policy blockers

- `AGENTS.md` requires fallback when the new path is unavailable and limits deploy
  files to the proxy, `.orig`, TOML and log. Current recognized creation failures
  do not all fall back; the proposed bundled binary requires a separate approved
  change to loading/deployment policy. Do not silently rewrite those rules.
- Current shipped defaults and legacy overrides remain authoritative; schema,
  numeric bounds, save conflicts and recreation semantics need P2.6 decisions.
- Binary provenance, distribution rights, ABI and presenter interoperability are
  P2.3 blockers. Seamless takeover and Stage 2 analytical-only fallback are open.
- P2.1 review can proceed on this documented evidence and unresolved risks; P2.2
  must establish the tests above before P2.4 production migration. No new tests
  were run as part of this static audit.
