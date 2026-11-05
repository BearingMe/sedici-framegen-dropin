# AGENTS.md

## Project

Sedici is a Windows x64 C++20 proxy DLL named
`amd_fidelityfx_framegeneration_dx12.dll`. It loads the original as
`amd_fidelityfx_framegeneration_dx12.dll.orig` and forwards the five `ffx*`
exports.

## Hard rules

- **Forward everything.** Milestone 0 must pass every argument, return value
  and timing through unchanged. Do not "fix" anything, including unknown
  descs, return codes, or swapchain behavior.
- Only change behavior when a milestone explicitly says so. Milestone 0 is
  observe-only.
- Do not touch `amd_fidelityfx_loader_dx12.dll` or
  `amd_fidelityfx_upscaler_dx12.dll`; the upscaler is owned by m-fsr.
- Keep `third_party/ffx/` unmodified. Do not move individual headers without
  fixing their relative includes, and do not invent provenance.
- Never download SDKs or runtime DLLs as part of a build or test.
- Do not launch the game. Deployment is not launch authorization.
- Use Conventional Commits for every commit, for example
  `feat: add present timing stats`.

## Layout

- `src/dllmain.cpp` — the five exports; loads lazily and forwards.
- `src/forward.*` — `LoadLibraryExW` + `GetProcAddress` for the `.orig`.
- `src/describe.*` — desc-chain walking and per-type logging.
- `src/present_hook.*` — optional `Present`/`Present1` vtable hook.
- `src/stats.*` — rolling prepare/present stats every 300 prepares.
- `src/log.*` — `sedici.log` beside the DLL, flushed per write,
  `MFSRFG_LOG=0/1/2`.
- `tools/` — deploy/restore with `.orig` safety rules, `games.json`.
- `third_party/ffx/` — unmodified FidelityFX/FSR v2.4.0 headers.

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
- only touch the target DLL, its `.orig`, and `sedici.log`.

Never commit logs, build output, or paths containing a username.
