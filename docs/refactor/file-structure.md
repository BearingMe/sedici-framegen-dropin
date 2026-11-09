# Target file structure (version 2, the Phase 3 layout)

**Status (2026-11-09):** adopted as the target of Phase 3 (see
[MILESTONES.md](../../MILESTONES.md#phase-3--organise-the-code-and-prove-nothing-changed)).
Version 1 of this document (written in P2.1) proposed a tree that included an AMD
backend, a bundled binary, a residual-network directory and new build presets. Phase 2
refactored the flat `src/` in place instead, and the AMD backend was removed (d95d454,
tag `pre-amd-removal`). Version 2 keeps version 1's module split and applies it to the
code as it is.

**Rule for this layout:** it is a pure reorganisation. Files are moved with `git mv` and lose their `fg_` prefix (the folder now says what
`fg_` said). They change only in their `#include` lines. Nothing else is renamed, split or
rewritten, and no behaviour changes. A module boundary that the code already crosses in
both directions is recorded in `docs/dev/layout.md` (written in P3.5), not repaired here.

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
|   |   |-- route.{h,cpp}
|   |   `-- token_namespace.h
|   |-- runtime/                    # Session, context and ownership
|   |   |-- integration.{h,cpp}
|   |   |-- context.{h,cpp}
|   |   `-- bookkeeping_faults.{h,cpp}
|   |-- generator/                  # Stage 1 (Mob-FGSR analytical pass)
|   |   |-- stage1.{h,cpp}
|   |   |-- params.{h,cpp}
|   |   |-- packing.h
|   |   |-- shaders.h
|   |   `-- lut_data.h
|   |-- gpu/                        # Pure queue, slot and retirement policies
|   |   |-- queue_policy.h
|   |   |-- slot_policy.h
|   |   `-- retirement_policy.h
|   |-- presentation/               # Game-facing swapchain, pacing, limiter
|   |   |-- swapchain.{h,cpp}
|   |   |-- presenter.{h,cpp}
|   |   |-- pacing.{h,cpp}
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
  `generator/shaders.h` and the LUT in `generator/lut_data.h`. Empty directories are
  not created.
- `cmake/` helper files and `CMakePresets.json`: not needed for a move. The build keeps the
  single `sedici_core` object library so the tests exercise the same compiled objects as the
  DLL.
- Symbol renames and any file rename other than dropping the `fg_` prefix. `git mv` records
  each prefix drop as a rename, so `git log --follow` still works; `docs/dev/moves.md`
  lists old and new names for grep.

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

New names are the old names without the `fg_` prefix. Files that never had it keep their
name.

| Current file | Target |
| --- | --- |
| `describe.*`, `fg_route.*`, `fg_token_namespace.h` | `src/abi/describe.*`, `route.*`, `token_namespace.h` |
| `fg_integration.*`, `fg_context.*`, `fg_bookkeeping_faults.*` | `src/runtime/integration.*`, `context.*`, `bookkeeping_faults.*` |
| `fg_stage1.*`, `fg_params.*`, `fg_packing.h`, `fg_shaders.h`, `fg_lut_data.h` | `src/generator/stage1.*`, `params.*`, `packing.h`, `shaders.h`, `lut_data.h` |
| `fg_queue_policy.h`, `fg_slot_policy.h`, `fg_retirement_policy.h` | `src/gpu/queue_policy.h`, `slot_policy.h`, `retirement_policy.h` |
| `fg_swapchain.*`, `fg_presenter.*`, `fg_pacing.*`, `limiter.*`, `limiter_schedule.h`, `overlay_compose.*` | `src/presentation/swapchain.*`, `presenter.*`, `pacing.*`, `limiter.*`, `limiter_schedule.h`, `overlay_compose.*` |
| `overlay.*`, `ui.*`, `ui_input.*`, `imgui_config.h` | `src/ui/` (same names) |
| `config.*`, `config_patch.*`, `live.*`, `settings_io.*`, `settings_tx.*` | `src/settings/` (same names) |
| `log.*`, `diag.*`, `stats.*`, `timing.*` | `src/diagnostics/` (same names) |
| `dllmain.cpp` | stays in `src/` |
| `tests/abi.cpp`, `abi_stub.*` | `tests/abi/` |
| `tests/fgcheck.cpp` | `tests/integration/fgcheck.cpp` (the executable keeps its name) |
| `tests/regression.cpp` | `tests/regression/` |
| `tests/settings_worker_*.cpp` | `tests/settings/` |
| `tests/deploy_check.ps1` | `tests/tools/` |

## Packaging and documentation boundaries

Never download SDKs or runtime DLLs during configure, build or test. Retain dependency
notices, source revisions, LUT provenance and the Mob-FGSR noncommercial obligations; the
root licence does not relicense them. `MILESTONES.md` stays authoritative for status.
