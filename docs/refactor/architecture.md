# Target architecture

> **Update: the AMD backend was removed.** The owner decided to drop the AMD
> fallback entirely as long as Sedici stays a drop-in replacement. Text below that
> proposes or describes an AMD backend, a bundled AMD binary, `.orig` forwarding or
> backend fallback is kept as design history and no longer applies. The last commit
> with that code is the tag `pre-amd-removal`; [amd-binary.md](amd-binary.md) keeps
> what was learned about the binary.

Status: recommended refactor baseline; paths below are proposed. See
[decision status](README.md#decision-status) and [file structure](file-structure.md).
The [current-state audit](audit.md) documents where the implementation differs;
these target rules are not claims about today's lock or retirement behavior.

## System shape

One Windows x64 C++20 DLL with exactly five public FFX exports. A
session-owned runtime integrates Sedici generation with settings, presentation,
UI, and diagnostics. (Earlier drafts also integrated an official AMD binary
backend; that was removed.)

## Ownership boundaries

| Module | Owns | Must not own |
| --- | --- | --- |
| `src/abi/` | Five exports, descriptor boundary, handle resolution and dispatch | Backend selection policy, GPU algorithms, UI |
| `src/runtime/` | Logical sessions, context registry, lifecycle, backend selection/fallback policy, settings application | Shader passes, widgets, TOML syntax |
| `src/backends/amd/` | Binary loading/export validation, AMD contexts and callbacks, original-DLL forwarding bridge | Another backend's handles or a second output scheduler |
| `src/backends/sedici/` | Analytical/residual generation, temporal history, model/feature contracts | Window input, TOML reads, final overlay drawing |
| `src/gpu/` | Submission timelines, frame-slot resources, descriptor lifetime, retirement | Backend choice, user-facing defaults |
| `src/presentation/` | Swapchain integration, real-frame admission, output scheduling, final composition | Model inference or persistence |
| `src/ui/` | Input/cursor integration, UI view state and GPU rendering | Backend mutation, direct limiter resets, file writes |
| `src/settings/` | Parse/validate candidates, file persistence and migration | Runtime lifecycle or GPU work |
| `src/diagnostics/` | Structured events, metrics, health observations | Mutating backend state or executing recovery |

The runtime owns watchdog recovery decisions. Diagnostics supplies observations;
it does not become an independent execution controller.

## Dependency rules

- ABI dispatch enters the runtime; it does not repeat backend/fallback decisions.
- Runtime composes concrete backend, GPU, and presentation components. Those
  components do not import the session coordinator to mutate it recursively.
- UI submits settings commands and reads published status/metrics snapshots
  through narrow runtime contracts. Runtime and backends never depend on ImGui.
- Settings parsing and persistence do not depend on UI, backends, or presentation.
- Only settings ingestion reads supported runtime environment overrides. GPU,
  backend, and presentation code never read TOML or environment policy directly.
- GPU infrastructure never depends on settings, UI, or backend selection.
- Diagnostics consumes events and observations; recovery returns through runtime.
- Backend-specific bridges may use shared presentation infrastructure. Shared
  placement does not require AMD and Sedici to share a native presenter.
- Do not introduce a universal `generate()` abstraction that assumes the opaque
  AMD binary and Sedici's compute pipeline have identical contracts.

## Session and context ownership

One logical session records the relationships between FG contexts, swapchain
contexts, backend handles, and the game-facing swapchain. The context registry
provides lifetime-safe access, not a raw pointer that can disappear after unlock.

The registry lock protects lookup/registration only; it is not held across GPU
waits, backend calls, or callbacks. Components document thread ownership, callback
reentrancy, and any lock ordering required by their actual implementation.

The session controls transitions between probing, active/history warmup,
suspended/degraded operation, draining, and stopped/device-lost operation. Each
transition specifies accepted work, outstanding ownership, and observable results.
Do not split authoritative lifecycle and fallback state between several managers.

## Single backend boundary

- Sedici is the only backend. Every export is answered by the router; a handle
  Sedici did not issue, or a descriptor it does not recognise, gets a defined FFX
  error code and a log line, and is never sent to another DLL.
- FG and swapchain contexts can arrive separately: the session must account for
  their relationship and actual creation order, not assume an atomic create call.
- A recoverable generation miss may use real-frame presentation where resources
  remain valid. That is degradation, not a backend handoff.
- Genuine device loss cannot be repaired by selecting another generator on the
  same removed device. Preserve the relevant game-facing error contract.
- Returning success without performing required work is not an acceptable
  substitute for compatibility.

## Frame and composition flow

```text
FFX inputs -> normalized capture/history -> analytical stage -> feature packing
  -> residual stage -> reconstruction -> game HUD -> Sedici overlay -> present
```

The residual stage consumes a versioned HUD-less feature contract: projected
colors, base image, projected depths, motion fields, focus mask, phase, dimensions,
and normalization/color-space conventions. Capture and history ordering are
explicit; the inference implementation does not reach into another stage's
private targets. Training and asset export remain outside the runtime build.

Composition is outside Stage 1. Neither HUD nor overlay pixels enter residual
inference. AMD integration must provide an equivalent final-output overlay
attachment through its supported contract.

## GPU and presentation ownership

- Each submitted workload has an explicit retirement token: timeline plus value.
- Frame slots retain resources, allocator memory, descriptors, inference scratch,
  and rendering data until all relevant consumers retire.
- CPU object scope/COM references do not establish GPU completion. Resource-state
  transitions do not establish cross-queue ordering.
- Borrowed game command lists are used only within their valid recording window;
  do not defer them to a worker without an established ownership contract.
- One rate-policy owner coordinates real-frame admission, output deadlines,
  backpressure, and sync policy. Limiter and output pacing remain distinct actions
  but do not independently accumulate waits for the same elapsed budget.
- Resize and shutdown stop admission and retire relevant work before reuse/release.
  Timeout is not permission to release in-flight resources.
- No heavy initialization, thread joining, file work, or GPU draining in `DllMain`.

Settings transactions are specified in [configuration.md](configuration.md).
Errors, recovery limits, and acceptance are specified in
[reliability.md](reliability.md).

## Constraints from known defects

Status: **notes, not decisions.** Defects recorded in
[known-issues.md](known-issues.md) that the target design has to account for;
each names the entries it comes from. They are deliberately left unfixed in the
current code.

- **One rate-policy owner must see the panel and the GPU** (K1-K4). Output
  planning today ignores the panel's maximum rate and the cost of generation; it
  only warns. The rate-policy owner needs the panel's refresh/VRR range, the
  real-frame cadence and the measured generation cost, and decides the
  multiplier/pattern (for example 2x pattern when the real rate is near half the
  target) instead of scheduling blindly and dropping afterwards.
- **FG off is a different presentation mode, not "real frames through the
  scheduler"** (K4). With no generated outputs the presentation path should not
  throttle the game through the output scheduler and tear guard.
- **Latency is a schedule property plus a measured GPU term** (K1, K6). Keep the
  structural offset exact and derived from the schedule, and the GPU term
  measured only when the presenter genuinely waited; queueing must never feed
  back into either. These are pure, fake-time-testable decisions.
- **Presentation owns the game-facing frame-latency contract** (K8). Once output
  is presented from Sedici's thread, the waitable object and maximum frame
  latency the game sees must be defined by the presentation owner, not forwarded
  from the native chain.
- **Swapchain replacement is an ownership transfer with an explicit failure
  contract** (K11, K12, K14). Decide, per backend, whether the native chain is
  wrapped or recreated; what reference the wrap desc hands over; what state must
  be carried (colour space, HDR metadata, fullscreen, latency); and what the game
  gets when replacement fails after the old chain is gone. Unsupported formats
  must be rejected before ownership changes so the game can be served by the
  fallback.
- **Queue ownership must survive third-party overlays** (K13). Overlays may
  draw from the queue they last observed and reuse it across swapchains. Queue
  lifetime (per device, possibly process-long) and the presentation queue used
  for each chain are part of the presentation contract, as is detection of known
  injected overlays.
- **Frame slots cover every consumer** (K15, K16). A frame slot holds the real
  snapshot, generated outputs, generation command lists and overlay drawing until
  presentation retires it; overlay work for an output that is dropped should not
  be recorded or should be retired with the slot.
- **Status has one source** (K18, K19, K22). FG active/inactive, effective
  multiplier and resolved output target come from the session, updated on
  configure, destroy and display changes, and are what the UI and limiter read.
- **Diagnostics define their metrics** (K10). Counters such as late flips, short
  gaps and VRR evidence need precise definitions and thresholds tied to the
  panel, or they get misread (finding 008 correction).
