# Target file structure (version 2, the Phase 3 layout)

**Status (2026-11-09):** adopted as the target of Phase 3 (see
[MILESTONES.md](../../MILESTONES.md#phase-3--organise-the-code-and-prove-nothing-changed)).
Version 1 of this document (written in P2.1) proposed a tree that included an AMD
backend, a bundled binary, a residual-network directory and new build presets. Phase 2
refactored the flat `src/` in place instead, and the AMD backend was removed (d95d454,
tag `pre-amd-removal`). Version 2 keeps version 1's module split and applies it to the
code as it is.

**Rule for this layout:** it is a pure reorganisation. Files are moved with `git mv`, keep
their base names, and change only in their `#include` lines. Nothing is renamed, split or
rewritten, and no behaviour changes. A module boundary that the code already crosses in
both directions is recorded in [docs/dev/layout.md](../dev/layout.md), not repaired here.

## Target tree

```text
sedici/
|-- CMakeLists.txt
|-- exports.def                     # Exactly five public FFX exports
|-- sedici.toml                     # Shipped settings, copied only if absent
|-- src/
|   |-- dllmain.cpp                 # DLL entry and the five exports
|   |-- abi/                        # Descriptor handling and handle routing
|   |   |-- describe.{h,cpp}
|   |   |-- fg_route.{h,cpp}
|   |   `-- fg_token_namespace.h
|   |-- runtime/                    # Session, context and ownership
|   |   |-- fg_integration.{h,cpp}
|   |   |-- fg_context.{h,cpp}
|   |   `-- fg_bookkeeping_faults.{h,cpp}
|   |-- generator/                  # Stage 1 (Mob-FGSR analytical pass)
|   |   |-- fg_stage1.{h,cpp}
|   |   |-- fg_params.{h,cpp}
|   |   |-- fg_packing.h
|   |   |-- fg_shaders.h
|   |   `-- fg_lut_data.h
|   |-- gpu/                        # Pure queue, slot and retirement policies
|   |   |-- fg_queue_policy.h
|   |   |-- fg_slot_policy.h
|   |   `-- fg_retirement_policy.h
|   |-- presentation/               # Game-facing swapchain, pacing, limiter
|   |   |-- fg_swapchain.{h,cpp}
|   |   |-- fg_presenter.{h,cpp}
|   |   |-- fg_pacing.{h,cpp}
|   |   |-- limiter.{h,cpp}
|   |   |-- limiter_schedule.h
|   |   `-- overlay_compose.{h,cpp}
|   |-- ui/                         # Overlay, menu and input
|   |   |-- overlay.{h,cpp}
|   |   |-- ui.{h,cpp}
|   |   |-- ui_input.{h,cpp}
|   |   `-- imgui_config.h
|   |-- settings/                   # sedici.toml, live values, transactions
|   |   |-- config.{h,cpp}
|   |   |-- config_patch.{h,cpp}
|   |   |-- live.{h,cpp}
|   |   |-- settings_io.{h,cpp}
|   |   `-- settings_tx.{h,cpp}
|   `-- diagnostics/                # Log, statistics, timing, diag snapshot
|       |-- log.{h,cpp}
|       |-- diag.{h,cpp}
|       |-- stats.{h,cpp}
|       `-- timing.{h,cpp}
|-- tests/
|   |-- abi/                        # abi.cpp, abi_stub.{h,cpp}
|   |-- integration/                # fgcheck.cpp (production-object GPU host)
|   |-- regression/                 # regression.cpp
|   |-- settings/                   # settings_worker_host.cpp, settings_worker_stub.cpp
|   |-- tools/                      # deploy_check.ps1
|   `-- qa/                         # The owner's in-game evidence (unchanged)
|-- tools/                          # deploy, restore, capture, analyze (unchanged)
|-- third_party/                    # Unmodified (unchanged)
|-- references/                     # Papers and read-only submodules (unchanged)
`-- docs/
    |-- findings/                   # Historical evidence
    |-- refactor/                   # Contracts and decisions
    `-- dev/                        # Agent and contributor guide (new in Phase 3)
```

All 58 files now in `src/` are placed above; none is left over. `exports.def` stays in the
root because the linker and the tests reference it there.

## Not carried over from version 1

- `backends/amd/`, `third_party/amd_fsr3/` and every `.orig` or bundled-runtime item: the AMD
  backend is gone.
- `residual/`, `shaders/`, `assets/`: Stage 2 is postponed, the shaders are embedded in
  `generator/fg_shaders.h` and the LUT in `generator/fg_lut_data.h`. Empty directories are
  not created.
- `cmake/` helper files and `CMakePresets.json`: not needed for a move. The build keeps the
  single `sedici_core` object library so the tests exercise the same compiled objects as the
  DLL.
- Renames such as dropping the `fg_` prefix: deferred. Keeping base names keeps
  `git log --follow` and grep working across the move.

## Dependency direction

Intended, from the ownership rules in [architecture.md](architecture.md):

```text
dllmain -> abi -> runtime -> generator
                    |  \-> presentation -> gpu
                    \----> settings
ui -> settings, presentation (hooks), diagnostics
everything -> diagnostics   settings -> diagnostics
```

The code does not follow this everywhere (for example the swapchain calls back into the
runtime through sinks and hooks). Phase 3 moves files and records the real include edges in
`docs/dev/layout.md`; it does not break an edge to make the picture cleaner.

## Include and build rules

- Includes are written as `#include "module/file.h"` relative to `src/`, with `src/` on the
  include path. No `../` includes.
- `CMakeLists.txt` lists sources by module. Targets are unchanged.
- Tests include production headers the same way.
- The vendored `third_party/` layout is untouched.

## Current-code migration map

| Current file | Target |
| --- | --- |
| `describe.*`, `fg_route.*`, `fg_token_namespace.h` | `src/abi/` |
| `fg_integration.*`, `fg_context.*`, `fg_bookkeeping_faults.*` | `src/runtime/` |
| `fg_stage1.*`, `fg_params.*`, `fg_packing.h`, `fg_shaders.h`, `fg_lut_data.h` | `src/generator/` |
| `fg_queue_policy.h`, `fg_slot_policy.h`, `fg_retirement_policy.h` | `src/gpu/` |
| `fg_swapchain.*`, `fg_presenter.*`, `fg_pacing.*`, `limiter.*`, `limiter_schedule.h`, `overlay_compose.*` | `src/presentation/` |
| `overlay.*`, `ui.*`, `ui_input.*`, `imgui_config.h` | `src/ui/` |
| `config.*`, `config_patch.*`, `live.*`, `settings_io.*`, `settings_tx.*` | `src/settings/` |
| `log.*`, `diag.*`, `stats.*`, `timing.*` | `src/diagnostics/` |
| `dllmain.cpp` | stays in `src/` |
| `tests/abi.cpp`, `abi_stub.*` | `tests/abi/` |
| `tests/fgcheck.cpp` | `tests/integration/` |
| `tests/regression.cpp` | `tests/regression/` |
| `tests/settings_worker_*.cpp` | `tests/settings/` |
| `tests/deploy_check.ps1` | `tests/tools/` |

## Packaging and documentation boundaries

Never download SDKs or runtime DLLs during configure, build or test. Retain dependency
notices, source revisions, LUT provenance and the Mob-FGSR noncommercial obligations; the
root licence does not relicense them. `MILESTONES.md` stays authoritative for status.
