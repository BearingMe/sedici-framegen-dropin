# Layout

Phase 3 organised `src/` into modules without changing behaviour. The target is
[file-structure.md](../refactor/file-structure.md) (version 2); this page records what is on disk.

| Module | Owns | Files |
| --- | --- | --- |
| `src/` | DLL entry and the five exports | `dllmain.cpp` |
| `abi/` | desc-chain walking, handle routing, the reserved token namespace | `describe`, `route`, `token_namespace.h` |
| `runtime/` | the frame generation context and the session that binds it to a swapchain | `integration`, `context`, `bookkeeping_faults` |
| `generator/` | Stage 1 (Mob-FGSR analytical pass), parameters, packing, embedded shaders and LUT | `stage1`, `params`, `packing.h`, `shaders.h`, `lut_data.h` |
| `gpu/` | pure queue, slot and retirement policies | `queue_policy.h`, `slot_policy.h`, `retirement_policy.h` |
| `presentation/` | swapchain wrapper, presenter, pacing, real-frame limiter, overlay copy | `swapchain`, `presenter`, `pacing`, `limiter`, `limiter_schedule.h`, `overlay_compose` |
| `ui/` | overlay hooks, Dear ImGui menu, cursor ownership, ImGui config | `overlay`, `ui`, `ui_input`, `imgui_config.h`, `ui_logic.h` |
| `settings/` | `sedici.toml` parsing, live values, patching, transactions | `config`, `config_patch`, `live`, `settings_io`, `settings_tx`, `target_rate.h` |
| `diagnostics/` | log, rolling stats, interval rings, bookkeeping fault injection, error codes | `log`, `stats`, `timing`, `bookkeeping_faults`, `error_code.h` |
| `status/` | the read-only snapshot of parked objects and log counters (the `diag:` line) | `diag` |

All sources except `dllmain.cpp` are built once into the `sedici_core` object library, which the
DLL and every test host share. Tests live in `tests/abi`, `tests/integration` (`sedici_fgcheck`),
`tests/regression`, `tests/settings`, `tests/tools` and `tests/qa` (the owner's in-game record).

## Layers and include edges (P4.5)

Every module include goes to a strictly lower layer, so there is no cycle. The CTest entry
`sedici_layers` (`tools/check_layers.ps1`) fails the build of any change that includes a header of the
same or a higher layer.

| Layer | Modules |
| --- | --- |
| 0 | `diagnostics` (log, stats, timing, fault injection), `gpu` |
| 1 | `settings`, `generator` |
| 2 | `presentation` |
| 3 | `runtime` |
| 4 | `abi` |
| 5 | `status`, `ui` |
| 6 | `src/dllmain.cpp` |

Measured include edges (module on the left includes headers of the modules on the right):

```text
abi          -> diagnostics, presentation, runtime, settings
generator    -> diagnostics
presentation -> diagnostics, generator, gpu, settings
root         -> abi, diagnostics, presentation, settings, status, ui
runtime      -> diagnostics, generator, presentation
settings     -> diagnostics
status       -> abi, diagnostics, presentation, settings
ui           -> abi, diagnostics, presentation, settings
```

## How the Phase 3 cycles were removed

Phase 3 recorded seven cycles. P4.5 removed them without changing behaviour:

- `bookkeeping_faults` (test fault injection) moved from `runtime` to `diagnostics`, which every
  module may use.
- `diag` (the snapshot of parked objects and log counters) moved to the new module `status`, above
  `abi`, because it reads the router, the swapchain and the settings worker. `stats` no longer calls it;
  the DLL entry registers it (`stats_set_extra_line`).
- `log` no longer reads `settings`: the settings layer publishes the live log level (`log_publish_level`).
- `settings` no longer calls the limiter: the limiter registers a cap listener (`live_set_cap_listener`).
- The limiter no longer asks the router whether frame generation is on: the DLL entry registers a probe
  (`limiter_set_fg_probe`).
