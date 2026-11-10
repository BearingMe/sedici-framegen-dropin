# Rust port (experiment/rust): design and conventions

> **Branch note.** This document describes `experiment/rust`, which is not merged into `main`. The
> paths, `cargo` commands and K entries it names exist only on that branch.

Branch `experiment/rust` is never merged to `main`. Goal: every C++ source under `src/` and every
offline test under `tests/` has a Rust equivalent, with the same behaviour and the same coverage
(not more tests per file). The C++ tree stayed until each piece was ported and verified; the last
step (done, 2026-11-10) retired it.

## Status (2026-11-10): every slice ported; the C++ build is retired

The port order below is complete. `src/` is Rust only, `tests/` is Rust plus the two PowerShell
check scripts, `CMakeLists.txt` and the C++ sources/tests are deleted, `third_party/imgui/` and
`third_party/tomlplusplus/` are gone (replaced by `egui` and `toml_edit`), and `third_party/ffx/`
stays as reference declarations. `cargo build --release` produces the DLL in `target/release`,
`cargo test -- --test-threads=1` is green, and the release image exports exactly the five `ffx*`
names (checked by `tests/abi.rs` against the PE export table).

**Deliberate gaps that remain.** These are mechanisms the C++ had and the port does not, recorded in
`docs/rust/test-parity.md` and as K entries; none is a missing scenario that could be tested today:

- the real-frame limiter is not wired to the wrapper's present hook (K-wiring gap, parity note);
- `Router::set_present_hook` is not propagated to wrappers (so K60 has no Rust case);
- the wrapper's failure/retirement hooks (`Faults::run_wrapper_lookup_hook`,
  `run_context_record_hook`, `run_queue_recovery_hook`) have no production run site;
- swapchain parking and the reaper are not ported (the wrapper is an `Arc`);
- a used DLL does not pin itself (no `DllMain`; K148);
- the overlay disables itself on a non-RGBA8 back buffer, so a 10-bit chain loses it (K164);
- the present-hook Present1 allocation fault is not injectable (parity note);
- `Stats::on_dispatch` has no production caller, so the Full menu's prepare figures are unset (K166);
- a fullscreen `DXGI_SWAP_CHAIN_FULLSCREEN_DESC` on an HWND create is refused (K160).

The remaining `GAP` rows in `docs/rust/test-parity.md` are either these mechanisms or checks that
need the game, a real display, RTSS or the owner's eyes (K23, K24, K36, K54, K112); they are not
port work.

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
- Dependencies: Rust crates only. toml++ became `toml_edit`; the log thread became
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
5. egui overlay. 6. Tests, architecture test, baseline comparison. 7. Retire the C++ build (done,
   2026-11-10: `tools/deploy.ps1` reads `target\release`, `tools/baseline.ps1` records `cargo test`,
   `tools/check_layers.ps1` and `tools/check_moves.ps1` deleted with the C++ tree).

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

**Build note (superseded: the exports exist).** The slice landed while the crate was still
`crate-type = ["rlib"]`, because a `cdylib` in the same unit made `cargo test` link the DLL through
`exports.def` and fail on the five then-unimplemented symbols (K141, K146). The ffx ABI slice added
the exports, so `Cargo.toml` now declares `["cdylib", "rlib"]`, `build.rs` passes
`/DEF:exports.def` on MSVC, and `cargo build --release` writes the DLL to `target/release`. The
export check in `tests/abi.rs` builds the image when the test profile has none (K171).

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
is `create_wrapper(&CreateContextDesc, WrapOrigin, owner) -> (OpaqueHandle, Arc<dyn Wrapper>)`,
building or adopting the chain and returning the game's chain handle. Until they land, `backend.rs`
installs `UnsupportedSwapchains` (a defined local error, K149) and the split-recording queue wait returns an
error; a frame generation create, configure, prepare, query and destroy all work end to end.
## Overlay and menu slice (done)

Landed on `experiment/rust`: `application/overlay.rs` (output-cadence recorder and `overlay present`
lines, `OverlayState`, the `MiniView`/`FullView` view-model and `OverlayAction`),
`application/ports/ui_renderer.rs`, and `adapters/driven/overlay_ui/{mod,device,menu,input}.rs`
with `hlsl/overlay_vs.hlsl` and `hlsl/overlay_ps.hlsl`. `egui` (0.33, `default-features = off`,
`default_fonts`) is the only new dependency; no egui renderer backend is used, the D3D12 renderer is
hand-written. The C++ build is unaffected.

**Ports the swapchain slice programs against.**

- `ui_renderer::OverlayRenderer` — `draw(&mut self, target: &RenderTarget, view: &OverlayView,
  input: &[UiInput], frame: UiFrameInfo) -> Result<Vec<OverlayAction>, OverlayDrawError>`,
  `disabled`, `disabled_reason`, `disable(reason)`, `draw_counts` (ports `ui_on_present`,
  `ui_disable`, `ui_render_status`). Call it once per presented output, real or generated, after
  generation and before Present, on the list the frame is being recorded on. A view of
  `OverlayView::Off` records nothing and costs nothing.
- `ui_renderer::RenderTarget` — `command_list`, `resource`, `device` (raw pointer identities the
  adapter downcasts; it refuses a device it was not created for), `width`, `height`,
  `format: OutputFormat(u32)` (currently only `DXGI_FORMAT_R8G8B8A8_UNORM`), `state`
  (`gpu::ResourceState`, the buffer's state before the draw), `generated`. The renderer transitions
  into `RENDER_TARGET` and back out.
- `ui_renderer::UiFrameInfo` — `output_width`, `output_height`, `client_width`, `client_height`,
  `delta_seconds`. `ui_renderer::UiInput` — one consumed Win32 message in client pixels
  (`msg`, `wparam`, `lparam`); Alt+F4 is filtered out (K126).
- `ui_renderer::OverlayDrawError` — `Failed`, `NoDevice`, `DeviceChanged`, `UnsupportedFormat`,
  `Disabled`; `is_permanent()` and `reason()` for the one `sedici: overlay disabled` line pair.
  The adapter must call `disable` and give input back on the first error.
- `application/overlay::OverlayState` — the menu's state machine: `enter_full`, `take_begin_request`,
  `on_io_completion`, `mark_begin_failed`, `apply_mode_transition`, `full_view`, `mini_view`,
  `effective_scale`, `set_applied_scale`, `edits_enabled`, `apply(action, tx)`. The swapchain slice
  calls these and `OverlayUi::initialize`; the `OverlayUi` concrete type is the renderer.
- `adapters/driven/overlay_ui::OverlayUi` — `new`, `initialize(&D3d12Device, OutputFormat)`,
  `run_pass` (the egui pass, for an integration that drives egui itself), `draw_with_raw` (the
  test-friendly path, taking the adapter's own list and texture), and the `OverlayRenderer` impl.

**Renderer test.** `tests/overlay_ui.rs` ports the `gpu-mini`/`gpu-full` scenarios as one
parameterised rstest: it renders a Mini and a Full frame to an offscreen 640x360 R8G8B8A8 texture,
checks that the menu area is painted and the far corner is not, and that a disabled renderer records
nothing and leaves the device healthy. It reads the DX12 info queue when the debug layer exists. It
skips cleanly when no D3D12 adapter is present. `tests/overlay_egui.rs` proves an egui pass produces
triangles without any D3D12 (it guards K152).

**In-flight assumption.** The C++ ImGui DX12 backend rotated four vertex/index buffers with no
fence of its own, safe only because draws never overlap; this renderer keeps that assumption and
states it (`device::INFLIGHT_NOTE`). A change that lets two draws overlap must add a fence first.

**Resolved by the swapchain reconciliation slice.** `backend.rs::Dx12Swapchains` implements the real
factory: a Wrap origin adopts the reference the FFX wrap desc hands over, a New origin calls
`CreateSwapChain` on the game's factory and queue, and an HWND origin calls
`CreateSwapChainForHwnd` (a non-null `fullscreenDesc` is refused, K155). Both build `DxgiNative`,
`Swapchain::wrap`, the `DxgiWrapper` COM object, and return the COM handle the game receives. The
same-adapter rule (K130) runs in `DxgiNative::attach`; the window for the overlay is resolved from
`OutputWindow`/`hwnd`/`GetHwnd` (K141) and handed to `window_input::capture_window_global`. The
split-recording queue dependency stays a defined local error (`D3d12QueueDependency`), because the
production path records on the game's own queue.

**The reconciliation adapter.** `adapters/driven/wrapper_handle.rs` is the one place that knows
both seams: `WrapperHandle` implements `session::Wrapper` (installing the session's `ContextHandle`
as the wrapper's generation sink, and answering the two interpolation queries), and
`SessionDriven<D>` implements `PresentedContext` over that handle. The wrapper is instantiated as
`Swapchain<D3d12Device, DxgiNative, SessionDriven<D3d12Device>, Overlay>`. Retirement is the
wrapper's own queue fence, wrapped in a `context::Retirement` probe by
`Swapchain::install_retirement`; without it, admission stays closed (K37). The wrapper texture and
list handles the adapter passes through are K154.

**Overlay wiring (done).** `adapters/driven/overlay_ui/host.rs` replaces the interim counting hook:
`OverlayHost` is the production `OverlayHook<D3d12Texture>` the swapchain factory installs. `draw`
runs the `OverlayState::frame` pass (I/O completions, mode transition, begin-edit request, the
Mini/Full view), lazily initializes the egui/D3D12 objects on the game's device and back-buffer
format, records the view on the frame's own list, applies the returned `OverlayAction`s through
`OverlayState::act` (settings transaction, `SettingsIo` save job) and mirrors the renderer's draw
counters. `presented` feeds the real/output timing rings and the `overlay present` cadence, exactly
once per output including after a disable, and prints the one-off `overlay present callback active`
line. Failures disable the overlay with the one line pair `tools/analyze_log.ps1` counts; nothing
ever fails the Present. `OverlayServices` (one per process) owns the `Live` values, the `SettingsTx`,
the `SettingsIo` worker, the `Stats` and the `OverlayTiming`, and the factory also installs its
`on_real_present` as the wrapper's per-application-Present hook.

`adapters/driving/window_input.rs::capture_window_global` now installs a real subclass
(`production_proc`) once per process and chains every captured window; the host receives the hotkey
cycle, the queued Full-menu input, `WM_SETFOCUS` and the `kHotkeyMessage` capture decision
(`OverlayInputHost`). Full capture enters through the real `CursorGuard`
(`win32/iat.rs` `CursorInterception`) on the window thread and falls back to Mini when interception is
unavailable (K132); a render-path retry is rate limited to half a second. `tests/overlay_host.rs`
drives a hidden window, an offscreen back buffer and the production host: Mini and Full outputs draw,
the counters and the cadence line prove it, the hotkey cycles, and a disabled host draws nothing.
K163-K165 record the remaining gaps (a non-D3D12 adapter, 10-bit formats, the null-procedure
ambiguity). The `Stats` prepare/stall window is no longer fed: the C++ fed it from `ffxDispatch` and
the overlay's own present hook; `Stats::on_dispatch` has no production caller until the dispatch path
carries `PrepareInfo` (K166).

**GPU smoke test.** `tests/swapchain_smoke.rs` creates a hidden 64x64 window, a D3D12 device, a
direct queue and a DXGI factory, then drives the production router's `create` with a
swapchain-NEW desc, queries the interpolation list and texture, presents three times through the
COM `IDXGISwapChain4`, and destroys. It skips cleanly when there is no D3D12 adapter or the factory
refuses a hidden-window chain (K157 records that this session's `D3D12CreateDevice` returns a
device that does not QI to `IDXGIDevice`, so the test builds the factory with `CreateDXGIFactory1`
instead of from the device's parent).


## Test parity with the C++ suite (audit)

`docs/rust/test-parity.md` maps every C++ offline scenario group (`tests/abi`, `tests/integration`,
`tests/regression`, `tests/settings`, `tests/tools`) to the Rust test that covers it, or records a
`GAP` with the reason. That file also holds the baseline procedure: it is now implemented by
`tools/baseline.ps1`, which records the Rust suite (per-binary results, verdicts, figures, skips)
and diffs it against `docs/dev/baseline.txt`; the C++ build it used to read is retired (K175).

Scenarios ported by this slice, not previously covered in Rust:

- `tests/tenbit.rs` — `fgcheck_tenbit.cpp`: a scene through an 8-bit and a 10-bit `Stage1`, the C++
  lift/reduce packing, and the same thresholds (`between > 42.0` dB, 10-bit not worse than 8-bit by
  more than 0.5 dB). One rstest case per C++ scene.
- `tests/lifecycle.rs` — `fgcheck_routing.cpp`'s AW2 block and the repeated-lifecycle memory check,
  plus the `fgcheck.cpp` present-queue-count 0→1→0 check, through the production router, factory,
  wrapper and Stage 1: the New and HWND 10-bit chains, the interpolation queries, the HDR-flagged
  session with the game dispatch acknowledged, the null factory local error, 40 cycles of private and
  video memory (4 MiB / 16 MiB bounds), and one presenter's shared queue count.
- `tests/refcount.rs` — the lifetime `fgcheck_contract` relied on: the adapter borrows the game's
  texture and command list and does not change the game's reference count (K169).
- `tests/routing.rs` gained a case per missing input of the New and HWND create forms, matching the
  C++ `run_contract` checks.

`tests/presentation.rs` gained a fake that models the D3D12 rule "an allocator cannot be reset while
its list is open", so the interpolation-list re-ask (K168) is now covered without a GPU.

Three defects were found and fixed by this slice, each with a regression test: K167 (a stopped
presenter's Present answered `E_INVALIDARG` instead of `DXGI_ERROR_INVALID_CALL`), K168 (the second
interpolation-list ask reset an open list's allocator and failed every time), and K169 (the FFX
resource seam adopted the game's handles and then AddRef'd them again, which leaked the game's
reference and dereferenced freed memory on the second borrow).

## Deployment decision (owner, 2026-11-10)

The owner decided to deploy the Rust build (`target
eleasemd_fidelityfx_framegeneration_dx12.dll`,
branch `experiment/rust`) to **Cyberpunk 2077** with `tools/deploy.ps1 -Game cp2077`, before any in-game
validation of the Rust port. This is an owner decision, not a result of the offline tests: the port
has only run through the offline suite (522 tests) and a hidden-window swapchain smoke test, and never
inside a game. Deployment is not launch authorization: the game is not launched by the agent, and
`restore.ps1` puts the original back.

Status: **not deployed yet.** The agent's attempt was refused by the auto-mode classifier (no reason
given), so the owner runs the deploy themselves or adds a permission rule. The first in-game run, once
the owner launches the game, should be read with `tools/analyze_log.ps1` and recorded under `tests/qa/`.
