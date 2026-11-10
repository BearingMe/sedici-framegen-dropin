# Layout

The Rust port replaced the C++ tree on `experiment/rust`: `src/` is a hexagonal Rust crate and the
layers are enforced by `tests/architecture.rs` (the C++ include-edge check `tools/check_layers.ps1`
was retired with the C++ build). The design and the slice-by-slice port map are in
[`docs/rust/port-design.md`](../rust/port-design.md).

| Layer | Path | Owns | May import |
| --- | --- | --- | --- |
| domain | `src/domain/` | pure rules: limits, enums, pure policies (`config`, `error`, `frame`, `gpu_policy`, `pacing`, `schedule`, `timing`, `token`, `ui_logic`) | std, thiserror |
| application | `src/application/` | use cases and the ports they need: `router`, `session`, `context`, `presentation/{swapchain,presenter,pacing,limiter}`, `settings/{live,tx,io}`, `overlay`, `stats`, `faults`, `ports/` | domain, std, thiserror, tracing |
| adapters | `src/adapters/driving/`, `src/adapters/driven/` | the FFX ABI edge and COM wrapper; D3D12, DXGI, Win32, files, timers, logging, settings worker, Stage 1 (with its `.hlsl` and `lut.bin`), egui overlay | application, domain, `windows`, other crates |
| entry | `src/entry.rs` | the composition root the first `ffx*` call runs | everything |

- Driving adapters call the application (the ffx exports, the COM swapchain wrapper, the window
  procedure). Driven adapters implement ports (D3D12, DXGI, files, timers, logging, overlay).
- No `windows` types in port signatures; ports use small plain types defined next to the trait.
- `unsafe` appears only under `src/adapters`, sits behind a safe type, and carries a
  `// SAFETY:` comment. `src/lib.rs` denies it at the crate root.
- `tests/architecture.rs` asserts that `domain` and `application` import nothing forbidden (no
  `windows`, no upward layer) and contain no `unsafe`.

## Assets

| Asset | Where | Notes |
| --- | --- | --- |
| Stage 1 HLSL | `src/adapters/driven/stage1/shaders/*.hlsl` | embedded with `include_str!`, compiled with the system `D3DCompile` at run time |
| Stage 1 LUT | `src/adapters/driven/stage1/lut.bin` | embedded with `include_bytes!`; Mob-FGSR-derived, CC BY-NC 4.0 (see `LICENSE`, `docs/mobfgsr-attribution.md`) |
| Overlay HLSL | `src/adapters/driven/overlay_ui/hlsl/*.hlsl` | the hand-written egui D3D12 renderer |
| FFX reference headers | `third_party/ffx/` | unmodified AMD SDK 2.4.0 headers; reference declarations only, nothing builds from them |

## Tests

`tests/` holds the Rust integration tests: `abi`, `architecture`, `lifecycle`, `overlay_egui`,
`overlay_host`, `overlay_ui`, `presentation`, `refcount`, `routing`, `stage1`, `swapchain_smoke`,
`tenbit`, with fakes in `tests/support/`. `tests/tools/` holds the two PowerShell check scripts
(`deploy_check.ps1`, `analyze_check.ps1`) and `tests/qa/` the owner's in-game record.
