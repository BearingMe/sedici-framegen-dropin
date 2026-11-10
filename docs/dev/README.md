# Developer and agent guide

Start here. Sedici is a Windows x64 Rust `cdylib` named
`amd_fidelityfx_framegeneration_dx12.dll`, a drop-in replacement for AMD FSR 3.1 frame generation
with its own two-stage generator.
The rules are in [`AGENTS.md`](../../AGENTS.md) (read them first); the plan and status are in
[`MILESTONES.md`](../../MILESTONES.md); the known defects are in
[`known-issues.md`](../refactor/known-issues.md); the port's design and conventions are in
[`docs/rust/port-design.md`](../rust/port-design.md).

| File | Content |
| --- | --- |
| [layout.md](layout.md) | The Rust layers under `src/`, what each owns, and the import rules |
| [workflow.md](workflow.md) | Build, test, make a change, commit convention, what to do when a check fails |
| [baseline.md](baseline.md) | The behaviour baseline and how to reproduce it |
| [moves.md](moves.md) | Phase 3 history: old C++ path to new path for every moved file |

Quick map: `src/entry.rs` is the composition root the five exports start from;
`src/adapters/driving/ffx_abi/` receives the calls; `src/application/` owns routing, contexts,
sessions, presentation and settings; `src/domain/` is the pure rules;
`src/adapters/driven/stage1/` is Stage 1 with its `.hlsl` shaders and `lut.bin`;
`src/adapters/driven/` holds D3D12, DXGI, Win32, logging, settings I/O and the egui overlay.
Tests live in `tests/*.rs`; `tests/architecture.rs` enforces the layering.
