# Workflow

## Build and test

```powershell
cargo build --release                                          # target\release\amd_fidelityfx_framegeneration_dx12.dll
cargo test -- --test-threads=1                                 # the offline suite; serialise the GPU tests
cargo clippy --all-targets                                     # no errors; warnings are noted
pwsh tools/baseline.ps1 -Against docs\dev\baseline.txt          # behaviour record (docs/dev/baseline.md)
pwsh tests\tools\deploy_check.ps1 -Dll target\release\amd_fidelityfx_framegeneration_dx12.dll -Repo .
pwsh tests\tools\analyze_check.ps1 -Repo .
```

There must be exactly five exports. `dumpbin /exports target\release\amd_fidelityfx_framegeneration_dx12.dll`
shows them from a Visual Studio developer shell; `tests/abi.rs` reads the same PE export table and
asserts the marker as well.

Never launch the game, never download SDKs or DLLs (see `AGENTS.md`).

The export check in `tests/abi.rs` builds the DLL itself when the test profile has no image, so
`cargo test` run alone still checks it; `cargo build` first keeps that rebuild out of the test time.

The two PowerShell check scripts are plain scripts, not Rust tests: they need a real `powershell.exe`
and write throwaway folders under the system temp directory. The Rust suite does not call them (the
parity note in `docs/rust/test-parity.md` records why).

## Adding or moving a file

1. Put the file in the layer that owns it ([layout.md](layout.md)); `tests/architecture.rs` fails an
   import that goes sideways or up.
2. Settings, errors and other pure rules go in `src/domain/`; use cases in `src/application/`
   behind a port; anything touching Windows, COM, the GPU, files or the ABI in `src/adapters/`.
3. Any `unsafe` block sits in an adapter, carries a `// SAFETY:` comment, and is behind a safe type.
4. Update the layout section of `AGENTS.md` in the same commit.

## Commit convention

Conventional Commits. A structural commit is `refactor(<module>): ...` with a body listing the
moves and the baseline result, and states whether behaviour changed.

## Bugs

Every defect found gets a `K` entry in `docs/refactor/known-issues.md` in the same commit or
before moving on, and the counts in `MILESTONES.md` are updated (see `AGENTS.md`).
