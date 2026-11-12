# Build and test

> Reference moved out of the [README](../../README.md); the text is unchanged.

## Build

```powershell
cmake -B build -A x64
cmake --build build --config Release
```

Output: `build/Release/amd_fidelityfx_framegeneration_dx12.dll`.

Verify the export table from a Visual Studio developer shell:

```powershell
dumpbin /exports build\Release\amd_fidelityfx_framegeneration_dx12.dll
```

There must be exactly five exports: `ffxCreateContext`, `ffxDestroyContext`,
`ffxConfigure`, `ffxQuery`, `ffxDispatch`. No helper exports. The DLL carries a
non-exported `SEDICI_PROXY_V1` marker that `deploy.ps1` uses to recognise it.

Optional offline regression checks (do not launch the game):

```powershell
cmake -B build -A x64 -DSEDICI_BUILD_TESTS=ON -DSEDICI_BUILD_FGCHECK=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

These cover config-save safety, settings transactions, token and retirement
policies, window and input handling, the live limiter, failure handling, and
Mini/Full DX12 rendering with window replacement. GPU tests use the installed debug layer when available; they never
download one. Their explicit GPU waits do not prove FFX's in-game queue bounds.

`sedici_abi_*` load the built DLL through its five exports. A stub beside it,
named like the original DLL, is a tripwire: Sedici must never load it, and
`sedici_abi_contract` checks that every call, including version queries and the
error codes for foreign, null and unknown inputs, is answered locally. Other
scenarios cover a malformed config, handle rejection, and a combined session with
the Mini overlay and limiter (`sedici_abi_session`). Each run writes a new
`build/tests/<config>/abi/<scenario>-<time>-<pid>` directory and never deletes
anything. `sedici_fgcheck*` need `SEDICI_BUILD_FGCHECK`; known failures print
`XFAIL` and are listed in `docs/refactor/reliability.md`. Close RTSS first:
its overlay is injected into test windows (vrr checks are then skipped).
