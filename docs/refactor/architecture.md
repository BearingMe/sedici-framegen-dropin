# Target architecture

Status: recommended refactor baseline; paths below are proposed. See
[decision status](README.md#decision-status) and [file structure](file-structure.md).

## System shape

One Windows x64 C++20 proxy DLL with exactly five public FFX exports. A
session-owned runtime integrates either Sedici generation or an official AMD
binary backend with settings, presentation, UI, and diagnostics.

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

## Backend and fallback boundary

- The bundled AMD binary is a pinned implementation dependency. `.orig` is the
  game's original compatibility provider, not an interchangeable handle owner.
- Identify which binary owns every external context; never forward a Sedici handle
  to AMD or a context from one AMD binary instance to another.
- Select AMD for an unavailable Sedici path before ownership commits. Probe and
  validate before publishing replacement handles or consuming swapchain ownership.
- FG and swapchain contexts can arrive separately: selection must account for
  their relationship and actual creation order, not assume an atomic create call.
- A recoverable generation miss may use real-frame presentation where resources
  remain valid. That is degradation, not an AMD backend handoff.
- Switching an owned session to AMD requires coordinated draining and recreation,
  or a documented next-recreation/restart boundary. Seamless takeover is unresolved.
- Genuine device loss cannot be repaired by selecting another generator on the
  same removed device. Preserve the relevant game-facing error contract.
- Unsupported calls must respect the real FFX contract and ownership. Neither
  forwarding a foreign handle nor returning success without performing required
  work is an acceptable substitute for compatibility.

Whether the selected AMD binary can feed Sedici's presenter through supported
dispatch/callback interfaces must be established. Otherwise AMD retains its own
presentation integration; do not stack Sedici's output scheduler over AMD's.

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
