# Move ledger

Old path to new path for every file moved in Phase 3, relative to the repository root. Old paths are
those of tag `phase2-baseline`. `git log --follow <new path>` shows the history across the move.
The `fg_` prefix was dropped; nothing else was renamed.

| Old path | New path |
| --- | --- |
| `src/describe.cpp` | `src/abi/describe.cpp` |
| `src/describe.h` | `src/abi/describe.h` |
| `src/fg_route.cpp` | `src/abi/route.cpp` |
| `src/fg_route.h` | `src/abi/route.h` |
| `src/fg_token_namespace.h` | `src/abi/token_namespace.h` |
| `src/diag.cpp` | `src/diagnostics/diag.cpp` |
| `src/diag.h` | `src/diagnostics/diag.h` |
| `src/log.cpp` | `src/diagnostics/log.cpp` |
| `src/log.h` | `src/diagnostics/log.h` |
| `src/stats.cpp` | `src/diagnostics/stats.cpp` |
| `src/stats.h` | `src/diagnostics/stats.h` |
| `src/timing.cpp` | `src/diagnostics/timing.cpp` |
| `src/timing.h` | `src/diagnostics/timing.h` |
| `src/fg_lut_data.h` | `src/generator/lut_data.h` |
| `src/fg_packing.h` | `src/generator/packing.h` |
| `src/fg_params.cpp` | `src/generator/params.cpp` |
| `src/fg_params.h` | `src/generator/params.h` |
| `src/fg_shaders.h` | `src/generator/shaders.h` |
| `src/fg_stage1.cpp` | `src/generator/stage1.cpp` |
| `src/fg_stage1.h` | `src/generator/stage1.h` |
| `src/fg_queue_policy.h` | `src/gpu/queue_policy.h` |
| `src/fg_retirement_policy.h` | `src/gpu/retirement_policy.h` |
| `src/fg_slot_policy.h` | `src/gpu/slot_policy.h` |
| `src/limiter.cpp` | `src/presentation/limiter.cpp` |
| `src/limiter.h` | `src/presentation/limiter.h` |
| `src/limiter_schedule.h` | `src/presentation/limiter_schedule.h` |
| `src/overlay_compose.cpp` | `src/presentation/overlay_compose.cpp` |
| `src/overlay_compose.h` | `src/presentation/overlay_compose.h` |
| `src/fg_pacing.cpp` | `src/presentation/pacing.cpp` |
| `src/fg_pacing.h` | `src/presentation/pacing.h` |
| `src/fg_presenter.cpp` | `src/presentation/presenter.cpp` |
| `src/fg_presenter.h` | `src/presentation/presenter.h` |
| `src/fg_swapchain.cpp` | `src/presentation/swapchain.cpp` |
| `src/fg_swapchain.h` | `src/presentation/swapchain.h` |
| `src/fg_bookkeeping_faults.cpp` | `src/runtime/bookkeeping_faults.cpp` |
| `src/fg_bookkeeping_faults.h` | `src/runtime/bookkeeping_faults.h` |
| `src/fg_context.cpp` | `src/runtime/context.cpp` |
| `src/fg_context.h` | `src/runtime/context.h` |
| `src/fg_integration.cpp` | `src/runtime/integration.cpp` |
| `src/fg_integration.h` | `src/runtime/integration.h` |
| `src/config.cpp` | `src/settings/config.cpp` |
| `src/config.h` | `src/settings/config.h` |
| `src/config_patch.cpp` | `src/settings/config_patch.cpp` |
| `src/config_patch.h` | `src/settings/config_patch.h` |
| `src/live.cpp` | `src/settings/live.cpp` |
| `src/live.h` | `src/settings/live.h` |
| `src/settings_io.cpp` | `src/settings/settings_io.cpp` |
| `src/settings_io.h` | `src/settings/settings_io.h` |
| `src/settings_tx.cpp` | `src/settings/settings_tx.cpp` |
| `src/settings_tx.h` | `src/settings/settings_tx.h` |
| `src/imgui_config.h` | `src/ui/imgui_config.h` |
| `src/overlay.cpp` | `src/ui/overlay.cpp` |
| `src/overlay.h` | `src/ui/overlay.h` |
| `src/ui.cpp` | `src/ui/ui.cpp` |
| `src/ui.h` | `src/ui/ui.h` |
| `src/ui_input.cpp` | `src/ui/ui_input.cpp` |
| `src/ui_input.h` | `src/ui/ui_input.h` |
| `tests/abi.cpp` | `tests/abi/abi.cpp` |
| `tests/abi_stub.cpp` | `tests/abi/abi_stub.cpp` |
| `tests/abi_stub.h` | `tests/abi/abi_stub.h` |
| `tests/fgcheck.cpp` | `tests/integration/fgcheck.cpp` |
| `tests/regression.cpp` | `tests/regression/regression.cpp` |
| `tests/settings_worker_host.cpp` | `tests/settings/settings_worker_host.cpp` |
| `tests/settings_worker_stub.cpp` | `tests/settings/settings_worker_stub.cpp` |
| `tests/deploy_check.ps1` | `tests/tools/deploy_check.ps1` |

`src/dllmain.cpp` and `exports.def` did not move. Comments inside moved files still use the old
names; they were left alone because Phase 3 changes only `#include` lines.
