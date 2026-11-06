# Target file structure

Status: proposed placement map, not an inventory of existing files. Headers
accompany implementations where needed; the tree is not a requirement to create
empty files. Keep small responsibilities together until a split has a concrete use.

```text
sedici/
|-- CMakeLists.txt
|-- CMakePresets.json
|-- AGENTS.md
|-- README.md
|-- LICENSE                         # Sedici-owned material; third-party terms separate
|-- MILESTONES.md                    # Existing delivery source of truth
|-- exports.def                     # Exactly five public FFX exports
|-- sedici.toml                     # Supported shipped settings, copied only if absent
|-- cmake/
|   |-- dependencies.cmake          # Local dependency checks; no runtime/SDK downloads
|   `-- shaders.cmake               # Shader compilation/embedding rules
|-- third_party/
|   |-- ffx/                        # Existing unmodified API declarations
|   |-- imgui/                      # Existing unmodified UI library/backends
|   |-- tomlplusplus/               # Existing unmodified TOML parser
|   `-- amd_fsr3/                   # Proposed official binary dependency, not AMD source
|       |-- bin/
|       |-- VERSION
|       `-- README.md               # Origin, hashes, dependencies, redistribution notices
|-- src/
|   |-- dllmain.cpp                 # Minimal loader entry point
|   |-- abi/
|   |   |-- exports.cpp             # The five exported FFX functions
|   |   |-- descriptors.cpp         # Descriptor translation/validation boundary
|   |   `-- routing.cpp             # Resolve handle ownership and dispatch
|   |-- runtime/
|   |   |-- session.cpp             # Lifecycle and backend/fallback decisions
|   |   |-- contexts.cpp            # Lifetime-safe context registry
|   |   `-- configuration.cpp       # Apply/acknowledge validated settings revisions
|   |-- backends/
|   |   |-- amd/
|   |   |   |-- backend.cpp         # AMD contexts, callbacks, compatibility bridge
|   |   |   `-- library.cpp         # Private binary loading and export validation
|   |   `-- sedici/
|   |       |-- generator.cpp       # Concrete generation pass sequence
|   |       |-- history.cpp         # Temporal history and discontinuities
|   |       |-- analytical/
|   |       |   |-- projection.cpp
|   |       |   `-- synthesis.cpp
|   |       `-- residual/
|   |           |-- features.cpp    # Stage 1 -> Stage 2 tensor contract
|   |           |-- inference.cpp   # Offline-trained model execution
|   |           `-- reconstruction.cpp
|   |-- gpu/
|   |   |-- resources.cpp
|   |   |-- frame_slots.cpp
|   |   |-- submission.cpp          # Queue ordering and retirement tokens
|   |   `-- retirement.cpp          # Reuse/release only after relevant completion
|   |-- presentation/
|   |   |-- scheduler.cpp           # Pure output/admission decisions where possible
|   |   |-- limiter.cpp             # One admission per real game frame
|   |   |-- presenter.cpp           # Bounded asynchronous output handoff
|   |   |-- swapchain.cpp           # Game-facing DXGI contract
|   |   `-- composition.cpp         # HUD and final-output composition
|   |-- ui/
|   |   |-- overlay.cpp             # View state and metrics display
|   |   |-- controls.cpp            # Settings requests, not subsystem mutations
|   |   |-- renderer.cpp            # ImGui/DX12 rendering resources
|   |   `-- input.cpp               # Window, cursor, clipping, coordinate mapping
|   |-- settings/
|   |   |-- config.cpp              # TOML and supported override ingestion
|   |   |-- validation.cpp          # Candidate/schema validation
|   |   `-- persistence.cpp         # Safe save and legacy migration
|   `-- diagnostics/
|       |-- logging.cpp
|       |-- metrics.cpp
|       `-- health.cpp              # Observations; runtime owns recovery decisions
|-- shaders/
|   |-- analytical/
|   |-- residual/
|   `-- composition/
|-- assets/
|   |-- lut/                        # Analytical LUT and its provenance
|   `-- models/                     # Residual model, manifest, provenance
|-- tests/
|   |-- abi/
|   |-- routing/
|   |-- runtime/
|   |-- sedici/
|   `-- integration/                # One offline DX12 host plus focused scenarios
|-- tools/                          # Existing safe deploy/restore and analysis tools
|-- references/                     # Existing papers and read-only source submodules
`-- docs/
    |-- findings/                   # Historical evidence stays here
    |-- mobfgsr-attribution.md
    `-- refactor/                   # Target decisions and unresolved questions
```

## Placement decisions

- `runtime/session.cpp` is the integration owner. Do not create parallel
  `routing/fallback`, `runtime/lifecycle`, and session state machines by default.
- AMD is a backend, not just a directory of forwarding functions. The loader may
  share mechanics for bundled and `.orig` binaries while preserving their distinct
  policies, identities, and handle ownership.
- GPU and presentation infrastructure live outside the Sedici generator so UI and
  AMD bridges do not depend on algorithm internals. Backend-specific bridges stay
  with their backend. Shared location does not guarantee shared presentation.
- The real-frame limiter has an explicit home under presentation; it is not an
  incidental effect of an overlay callback or export wrapper.
- Keep input/cursor/window lifetime integration explicit. It must survive renderer
  disablement and restore game input independently of GPU rendering success.
- Preserve the vendored header layout. Moving shaders/LUT assets requires updating
  embedding, attribution, and packaging together, not modifying upstream files.

## Current-code migration map

These are responsibility extractions, not instructions to move whole files.

| Current files | Target responsibilities |
| --- | --- |
| `src/dllmain.cpp` | Minimal `dllmain.cpp` plus ABI exports |
| `src/forward.*` | AMD library loader and compatibility bridge |
| `src/describe.*`, `src/fg_route.*` | ABI descriptor handling, routing, diagnostics, runtime registry |
| `src/fg_integration.*`, `src/fg_context.*` | Session/backend integration, normalized inputs, history, GPU ownership |
| `src/fg_stage1.*`, `src/fg_params.*`, `src/fg_shaders.h` | Analytical stage, shared GPU resources, shaders, separate composition |
| `src/fg_pacing.*`, `src/fg_presenter.*`, `src/fg_swapchain.*` | Presentation policy, presenter, swapchain, submission/retirement |
| `src/present_hook.*`, `src/limiter.*`, `src/toggles.*` | Backend-specific interception, real-frame admission, runtime policy/status |
| `src/overlay.*`, `src/overlay_compose.*` | AMD callback bridge, final composition, metrics emission |
| `src/ui.*`, `src/ui_input.*` | UI view/controls/renderer and input ownership |
| `src/config.*`, `src/config_patch.*`, `src/live.*` | Settings ingestion/persistence and acknowledged runtime configuration |
| `src/log.*`, `src/timing.*`, `src/stats.*` | Diagnostics events, metrics, health observations |

## Build boundaries

Use a small number of production library targets with explicit target-scoped
dependencies; do not create a library for every folder. Separate pure policy,
GPU/backend implementation, UI integration, and the final ABI boundary only where
that separation enforces a dependency or enables a real test.

Tests link the same production implementation targets. A focused boundary check
also loads the built DLL and verifies its five exports; tests must not only
recompile a slightly different source list that omits the integration being tested.
Presets distinguish release, validation, and host-side sanitizer runs.

## Packaging and documentation boundaries

The AMD runtime's deployed filename, directory, dependency search policy, hashes,
and license approval are unresolved. This tree reserves a dependency location;
it does not authorize adding binaries or changing the deploy allowlist. Never
download SDKs/runtime DLLs during configure, build, or test.

Retain dependency notices, source revisions, LUT/model provenance, and the
Mob-FGSR noncommercial obligations; a root license does not relicense them.

Keep `MILESTONES.md` authoritative. If later moved to `docs/milestones.md`, move it
once and update links/instructions rather than maintaining both. Broader
`docs/architecture.md`, `compatibility.md`, or `fallback.md` can be introduced or
extracted when approved; do not duplicate these target notes as current facts.
