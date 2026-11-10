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

## GPU and Stage 1 slice (done)

Landed on `experiment/rust`: `domain/frame.rs` (params, constants, packing, colour formats),
`application/ports/{gpu,generator}.rs`, `adapters/driven/d3d12/{device,fence,queue,resources}.rs`,
`adapters/driven/stage1/{mod,pipelines}.rs` with `shaders/*.hlsl` and `lut.bin`, and the GPU test
`tests/stage1.rs`. The C++ build and the five exports are unaffected.

**Ports other slices program against.** The later context, swapchain and presenter slices call these;
changing one changes every caller.

- `gpu::GpuDevice` — associated types `Texture`, `Buffer`, `CommandAllocator`, `CommandList`, `Fence`,
  `Queue`; `create_texture`, `create_upload_buffer`, `create_readback_buffer`, `write_buffer`,
  `read_buffer`, `create_command_allocator`, `create_command_list`, `create_fence`, `create_queue`,
  `removed_reason`.
- `gpu::GpuQueue<L, F>` — `execute(&[&L])`, `signal(&F, u64)`, `wait(&F, u64)` (a queue-side
  dependency, never a CPU wait).
- `gpu::GpuFence` — `completed() -> u64` and `wait(value, Duration) -> Result<(), GpuError>`; every
  wait is bounded, `u64::MAX` is a removed device and is never success.
- `gpu::GpuCommandList<T>` — `transition`, `uav_barrier`, `copy_resource`, `dispatch`.
- `gpu::GpuTexture` — `info()`, `identity()` (for the aliasing checks); `GpuBuffer` — `size()`.
- `GpuError` — `Failed { what, code }`, `Timeout`, `DeviceRemoved { reason }`, `Policy(GpuFailure)`.
  `GpuError::code()` gives the HRESULT-shaped value the log prints.
- `generator::FrameGenerator<D>` — `create`, `configure`, `prepare`, `discard_prepare`, `generate`,
  `params`, `ready`, `release_targets`, `pending`; `InputFrame`, `GeneratePhase`, `HudSource`,
  `Stage1Pass`, `PassMark`, `MAX_PHASES = 2`. `wait_retired(fence, value, timeout, device_removed)`
  is the one bounded-retirement rule shared by the context and the swapchain.
- The adapter's concrete types are `D3d12Device`, `D3d12Texture`, `D3d12Buffer`,
  `D3d12CommandList`, `D3d12CommandAllocator`, `D3d12Fence`, `D3d12Queue`, and
  `stage1::Stage1` (which implements `FrameGenerator<D3d12Device>`).

**GPU test.** `tests/stage1.rs` ports the `fgcheck_quality` scenario: four synthetic scenes through
the production `Stage1`, checking the midpoint against the scene at the midpoint, the two phases in
one list, HUD preservation and the K53 reset seed. It skips cleanly when no D3D12 adapter exists. The
scores reproduce the C++ baseline: 42.92 / 40.60 / 45.99 dB and the fast bar at 42.87 dB, SSIM
0.9985 / 0.9975 / 0.9984, phases 42.95 / 42.92 dB, HUD 100%. The C++ `Context::prepare` negates the
FFX motion scale before Stage 1 sees it; the test does the same, and that sign is load-bearing.

## Presentation and swapchain slice (done)

Landed on `experiment/rust`: `application/ports/{swapchain,overlay}.rs`,
`application/presentation/{swapchain,presenter}.rs`, `adapters/driving/dxgi_wrapper.rs`,
`adapters/driven/{dxgi_native,overlay_compose}.rs`, and the offline suite `tests/presentation.rs`.
The C++ build and the five exports are unaffected.

**Ports other slices program against.**

- `swapchain::NativeSwapchain` — associated `Buffer: GpuTexture`; `back_buffer(index)`,
  `buffer_reference_count(index)` (`None` when the adapter cannot count, which is why resize keeps
  its drain guard), `desc`, `current_back_buffer_index`, `present`, `present1`, `resize_buffers`,
  `resize_buffers1`, `frame_statistics`, `last_present_count`, `maximum_frame_latency`,
  `set_maximum_frame_latency`, `has_waitable_object`, `containing_output`, `set_fullscreen_state`,
  `fullscreen`, `is_temporary_mono_supported`, `wait_for_retirement`. Plain types: `PresentFlags`,
  `PresentParams`/`Rect`/`Offset`, `SwapChainDesc`, `FrameStatistics`, `OutputDesc`, `Scanline`.
  `SwapChainDesc::supported_format` is the C++ `supported_color_format`: RGBA8 and RGB10A2 only.
- `overlay::OverlayHook<T>` — `draw(&dyn GpuCommandList<T>, &T, generated)` and
  `presented(generated)`, called from whichever thread presents; `NoOverlay` is the default.

**The wrapper's contract for the context slice.** `application::presentation::swapchain` defines
`PresentedContext` (`Texture = D::Texture`, `List`), so `application::{context,session}` implement it
without importing the wrapper: `prepared`, `generate_phases(list, real, phases, reset)`,
`note_ordered_submission`, `track_submission(fence_value)`, `abandon_generation`,
`has_untracked_work`, `wait_for_retirement(timeout)`, `output_state`. `GeneratorSink<C>` pairs a
context with its owner key and an optional lease; `set_generator`/`clear_generator_if_owner`/
`close_generator` mirror the C++ sink lifecycle, including the untracked holder that keeps an owner
with unregistered work alive until a queue fence is accepted (K103).

**Entry point.**
`Swapchain::wrap(native: Arc<N>, device: Arc<D>, queue: Arc<D::Queue>, clock: Arc<dyn Clock>,
choice: PresentChoice, queues: Arc<SharedPresentQueues<D>>, overlay: Arc<O>, environment:
WrapEnvironment, handed_refs: u32, report: &mut WrapReport) -> Result<Self, SwapchainError>`. It
takes no `make_list`: the command lists come from the `GpuDevice`, and `RecordingList`'s reset/close
is the `ListLifecycle` trait the D3D12 adapter implements. `WrapEnvironment` carries what the layer
may not read itself (`scanline`, `rtss_loaded`, `lead_seconds`, `force_sync_override`,
`align_vblank`); the composition root reads the environment variables.

**Test.** `tests/presentation.rs` ports `fgcheck_present`/`fgcheck_retention`/`fgcheck_pacing`
against fakes: one midpoint per cycle and the buffer-index advance, the three injected generation
faults stopping admission, the Present1 metadata retry, removed-device retirement, resize refusal
while a buffer is taken, the format and mask rules, the "vrr" fallback, the scratch texture/list,
and the pacing/schedule figures. Nothing skips.

**Build note.** `Cargo.toml` declares `crate-type = ["rlib"]` so `cargo test` links the library
directly; with `cdylib` in the list the test binary links the DLL through `exports.def` and fails on
the five not-yet-exported symbols (K141). The `cdylib` entry and the `exports.def` link argument come
back with the ffx ABI slice (order step 4). `cargo build --release` today produces only an rlib, and
that is expected until the exports exist.

## Application core and composition root (done)

Landed on `experiment/rust`: `application/{context,session,router}.rs`, `entry.rs`,
`adapters/driven/{backend,win32/modules}.rs` (the module-load probe), and `tests/routing.rs`. The DLL
again exports exactly the five `ffx*` entry points (a Rust `cdylib` links the CRT entry, so the port
has no `DllMain` of its own; K148).

**Context** (`application/context.rs`). The FFX create/configure/prepare/generate state machine over
`FrameGenerator` and `GpuDevice`, porting `runtime/context.cpp` exactly: the single prepare slot with
the K139 K98 wait, the HUD-less format check against `m_color_format`, the HDR flag accepted, the
split-recording ordering rules, `track_submission`, `wait_prepare`, `abandon_generation`, the bounded
`wait_for_retirement` and `Context::fail` logging one info line. Three pointer-shaped ports stand in
for the C++ raw pointers: `Retirement` (an `Arc<dyn Retirement>` instead of `ID3D12Fence*`),
`QueueDependency` (instead of `ID3D12CommandQueue*`), and `DeviceLookup`/`FrameObjects` (instead of
casting the game's `ID3D12Resource*`). `ContextSink` is the object-safe `ContextHandle` a swapchain
installs as its generation sink.

**Session** (`application/session.rs`). Binds a context to a wrapper through the `Wrapper` trait:
`set_generator`, `clear_generator_if_owner`, `close_generator`, `interpolation_list`,
`interpolation_texture` — the exact operations `presentation/swapchain.h` exposes. The New/ForHwnd
`WrapOrigin` and rollback live here; `SessionState` is the C++ lifecycle enum.

**Router** (`application/router.rs`). Handle tokens (`domain/token.rs` behind a `TokenSource`, with
`ProcessTokens` reserving the range through an `AddressReservation` the adapter supplies, and
`MemoryTokens` for tests), ownership leases (`Arc<Owned>` with a shared-call mutex and `dead` flag),
create/configure/query/dispatch/destroy against the context and session, the `PROVIDER_VERSIONS`
table (including the New/HWND ids), and the exact `route.cpp` rejection codes. It implements
`ffx_model::FfxService`; the parked-owner reaping (K41) and the injected bookkeeping faults (K29,
K63, K102) are covered by `tests/routing.rs`.

**Expected from the swapchain slice.** The session and router are built against two traits the
presentation slice supplies. `session::Wrapper` (above) is the generator half; `router::SwapchainFactory`
is `create_wrapper(&CreateContextDesc, WrapOrigin) -> (OpaqueHandle, Arc<dyn Wrapper>)`, building or
adopting the chain and returning the game's chain handle. Until they land, `backend.rs` installs
`UnsupportedSwapchains` (a defined local error, K149) and the split-recording queue wait returns an
error; a frame generation create, configure, prepare, query and destroy all work end to end.

