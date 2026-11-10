# Rust port (experiment/rust): design and conventions

Branch `experiment/rust` is never merged to `main`. Goal: every C++ source under `src/` and every
offline test under `tests/` has a Rust equivalent, with the same behaviour and the same coverage
(not more tests per file). The C++ tree stays until each piece is ported and verified, then the
C++ build is retired in the last step.

## Layers (enforced by `tests/architecture.rs`)

| Layer | Path | May import | `unsafe` |
|---|---|---|---|
| domain | `src/domain` | std, thiserror | no |
| application | `src/application` | domain, std, thiserror, tracing | no |
| adapters | `src/adapters/{driving,driven}` | application, domain, `windows`, etc. | yes, contained |
| entry | `src/entry.rs` | everything | no |

- `application` talks to the outside only through traits in `src/application/ports/`.
- Driving adapters call into the application (the ffx exports, the COM swapchain wrapper, the window
  procedure). Driven adapters implement ports (D3D12, DXGI, files, timers, logging, overlay).
- Every `unsafe` block sits in an adapter and carries a `// SAFETY:` comment. An adapter exposes a
  safe type (owning wrappers over COM pointers and handles); callers never see a raw pointer except
  at the ffx ABI edge, where it is converted into a validated Rust value on entry.
- No `windows` types in port signatures; ports use small plain types defined next to the trait.

## Rules
- Tests: `rstest` (cases, not dozens of tests per file); port the scenario of the C++ test, not each line.
- Errors: `thiserror` enums per module; only `adapters/driving/ffx_abi` maps them to FFX return codes.
  The stable `ErrorCode` table in `domain/error.rs` is part of the log contract.
- Logs: `tracing` macros only (`error!/warn!/info!/debug!/trace!`). Mapping: C++ level 0 = error,
  1 = info, 2 = debug/trace. The sink is `adapters/driven/logging.rs` and writes `sedici.log` beside
  the DLL with the same line shape as the C++ log so `tools/analyze_log.ps1` keeps working.
- Dependencies: Rust crates only. toml++ becomes `toml_edit`; the log thread becomes
  `tracing-subscriber` + `tracing-appender`; D3D12/DXGI/Win32 come from the `windows` crate (raw
  bindings, not a wrapper); Dear ImGui becomes `egui` with a hand-written D3D12 renderer. HLSL stays
  HLSL, compiled with the system `D3DCompile` at runtime exactly as the C++ does; keep shaders as
  `.hlsl` files under `src/adapters/driven/stage1/shaders/`.
- C++ that must stay (none planned) goes under `native/` behind one safe trait; document it in this file.
- Exports: exactly five, listed in `exports.def` (kept) and checked by a test.
- Hard rules in `AGENTS.md` still apply (drop-in behaviour, never fail a game call on bookkeeping,
  never launch the game, no downloads of SDKs or DLLs, `third_party/` unmodified, bugs documented as K entries).
- Line endings: the repo uses CRLF for existing files; new Rust files may be LF (`.gitattributes` decides).

## Port map

| C++ | Rust |
|---|---|
| `diagnostics/error_code.h`, `abi/token_namespace.h`, `gpu/*.h`, `presentation/limiter_schedule.h`, `settings/target_rate.h`, `ui/ui_logic.h` | `domain/{error,token,gpu_policy,schedule,ui_logic}.rs` (**done**) |
| `settings/config.*`, `config_patch.*` | `domain/config.rs`, `adapters/driven/config_toml.rs`, `application/ports/config_store.rs` |
| `settings/live.*`, `settings_tx.*`, `settings_io.*`, `target_rate.h` | `application/settings/{live,tx,io}.rs`, `adapters/driven/settings_worker.rs` |
| `diagnostics/{log,stats,timing,bookkeeping_faults}.*`, `status/diag.*`, `presentation/pacing.*` | `adapters/driven/logging.rs`, `application/{stats,faults}.rs`, `domain/{timing,pacing}.rs` (**done**) |
| `abi/describe.*`, `dllmain.cpp` | `adapters/driving/ffx_abi/{types,desc_walk,exports}.rs`, `entry.rs` |
| `abi/route.*`, `runtime/{integration,context}.*` | `application/{router,session,context}.rs` |
| `generator/{params,packing,stage1,shaders,lut_data}.*` | `domain/frame.rs`, `application/ports/generator.rs`, `adapters/driven/stage1/*` (+ `.hlsl`, `lut.bin`) |
| GPU helpers inside presenter/swapchain/stage1 | `application/ports/gpu.rs`, `adapters/driven/d3d12/*` |
| `presentation/{swapchain,presenter,limiter}.*` | `application/presentation/*`, `adapters/driving/dxgi_wrapper.rs`, `adapters/driven/dxgi_native.rs`, `adapters/driven/win32/timer.rs` |
| `presentation/overlay_compose.*`, `ui/{overlay,ui}.*` | `adapters/driven/{overlay_compose,overlay_ui}.rs`, `application/ports/overlay.rs` |
| `ui/ui_input.*` | `adapters/driving/window_input.rs`, `adapters/driven/win32/{iat,modules}.rs`, `application/ports/input.rs` |
| `tests/abi`, `tests/integration`, `tests/regression`, `tests/settings` | `tests/{abi,integration,regression,settings}.rs` with fakes in `tests/support/` |

## Order
1. Domain policies (done). 2. Leaves in parallel: config, diagnostics, D3D12 helpers + Stage 1, Win32
interception. 3. Application core (router, session, context, presentation, settings) against the ports
the leaves defined. 4. Driving adapters (ffx ABI, COM wrapper, window input) and the composition root.
5. egui overlay. 6. Tests, architecture test, baseline comparison. 7. Retire the C++ build.
