# AGENTS.md

## Project

Sedici is a Windows x64 C++20 proxy DLL named
`amd_fidelityfx_framegeneration_dx12.dll`. It loads the original as
`amd_fidelityfx_framegeneration_dx12.dll.orig`, forwards the five `ffx*`
exports, and on top of that sends one frame-pacing tuning configure that removes
a measured frame-time tail. Behaviour is driven by `sedici.toml` beside the DLL,
with environment overrides.

## Hard rules

- **Forward everything.** Every argument, return value and timing passes through
  unchanged. The only permitted behaviour changes are the two documented ones:
  the frame pacing tuning send (`pacing.mode`) and the async-workload rewrite
  (`async.mode`). Both must be inert when configured off. Do not "fix" anything
  else, including unknown descs, return codes, or swapchain behaviour.
- Only change behaviour when a milestone explicitly says so. Milestone 1 is the
  pacing fix; it is on by default and must remain fully opt-out.
- Never fail a game call because of Sedici's own bookkeeping. Configuration
  errors, a rejected tuning and logging failures are all non-fatal.
- Do not touch `amd_fidelityfx_loader_dx12.dll` or
  `amd_fidelityfx_upscaler_dx12.dll`; the upscaler is owned by m-fsr.
- Keep `third_party/ffx/` and `third_party/tomlplusplus/` unmodified. Do not move
  individual headers without fixing their relative includes, and do not invent
  provenance.
- Never download SDKs or runtime DLLs as part of a build or test.
- Do not launch the game. Deployment is not launch authorization.
- Use Conventional Commits for every commit, for example
  `feat: add present timing stats`.

## Layout

- `src/dllmain.cpp` — the five exports; loads lazily and forwards.
- `src/forward.*` — `LoadLibraryExW` + `GetProcAddress` for the `.orig`.
- `src/config.*` — `sedici.toml` parsing (toml++), defaults, env overrides,
  provenance, once-at-load reporting.
- `src/toggles.*` — the two behaviour switches and per-context pacing state.
- `src/describe.*` — desc-chain walking and per-type logging.
- `src/present_hook.*` — optional `Present`/`Present1` vtable hook.
- `src/stats.*` — rolling prepare/present stats every 300 prepares, plus the
  prepare stall detector.
- `src/log.*` — `sedici.log` beside the DLL, flushed per write,
  `MFSRFG_LOG=0/1/2`.
- `sedici.toml` — shipped defaults; copied on deploy only when absent.
- `tools/` — deploy/restore with `.orig` safety rules, `games.json`.
- `third_party/ffx/` — unmodified FidelityFX/FSR v2.4.0 headers.
- `third_party/tomlplusplus/` — unmodified toml++ v3.4.0 single header.

## Build and verify

```powershell
cmake -B build -A x64
cmake --build build --config Release
dumpbin /exports build\Release\amd_fidelityfx_framegeneration_dx12.dll
```

There must be exactly five exports. Do not add helper exports, including the
proxy marker.

## Deploy safety

`tools/deploy.ps1` and `tools/restore.ps1` must:

- refuse to run while the game is running;
- never overwrite an existing `.orig`;
- refuse to overwrite or remove a DLL that is not Sedici's;
- never overwrite an existing `sedici.toml` on deploy;
- only touch the target DLL, its `.orig`, `sedici.toml` and `sedici.log`.

Never commit logs, build output, or paths containing a username.
