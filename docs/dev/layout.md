# Layout

Phase 3 organised `src/` into modules without changing behaviour. The target is
[file-structure.md](../refactor/file-structure.md) (version 2); this page records what is on disk.

| Module | Owns | Files |
| --- | --- | --- |
| `src/` | DLL entry and the five exports | `dllmain.cpp` |
| `abi/` | desc-chain walking, handle routing, the reserved token namespace | `describe`, `route`, `token_namespace.h` |
| `runtime/` | the frame generation context, the session that binds it to a swapchain, bookkeeping fault injection | `integration`, `context`, `bookkeeping_faults` |
| `generator/` | Stage 1 (Mob-FGSR analytical pass), parameters, packing, embedded shaders and LUT | `stage1`, `params`, `packing.h`, `shaders.h`, `lut_data.h` |
| `gpu/` | pure queue, slot and retirement policies | `queue_policy.h`, `slot_policy.h`, `retirement_policy.h` |
| `presentation/` | swapchain wrapper, presenter, pacing, real-frame limiter, overlay copy | `swapchain`, `presenter`, `pacing`, `limiter`, `limiter_schedule.h`, `overlay_compose` |
| `ui/` | overlay hooks, Dear ImGui menu, cursor ownership, ImGui config | `overlay`, `ui`, `ui_input`, `imgui_config.h` |
| `settings/` | `sedici.toml` parsing, live values, patching, transactions | `config`, `config_patch`, `live`, `settings_io`, `settings_tx` |
| `diagnostics/` | log, snapshot, rolling stats, interval rings | `log`, `diag`, `stats`, `timing` |

All sources except `dllmain.cpp` are built once into the `sedici_core` object library, which the
DLL and every test host share. Tests live in `tests/abi`, `tests/integration` (`sedici_fgcheck`),
`tests/regression`, `tests/settings`, `tests/tools` and `tests/qa` (the owner's in-game record).

## Real include edges between modules

Measured from the `#include "module/..."` lines under `src/` after the moves (module on the left
includes headers of the modules on the right):

```text
(root) dllmain -> abi, diagnostics, presentation, settings, ui
abi            -> diagnostics, presentation, runtime, settings
diagnostics    -> abi, presentation, runtime, settings
generator      -> diagnostics
presentation   -> abi, diagnostics, generator, gpu, runtime, settings
runtime        -> diagnostics, generator, presentation
settings       -> diagnostics, presentation
ui             -> abi, diagnostics, presentation, runtime, settings
```

## Known cycles (recorded, not removed)

These module pairs include each other. They existed before the move (the flat `src/` hid them)
and Phase 3 does not break them:

- `abi` and `diagnostics`; `abi` and `presentation`
- `diagnostics` and `presentation`; `diagnostics` and `runtime`; `diagnostics` and `settings`
- `presentation` and `runtime`; `presentation` and `settings`

`diagnostics` is meant to be a leaf but `diag` reaches into `runtime`, `abi` and `presentation`
to read parked objects and counters, and `log` and `diag` read `settings`. Untangling this is
Phase 4 work. `gpu` and `generator` have no inbound cycle.
