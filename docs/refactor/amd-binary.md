# P2.3 evidence: the official AMD frame-generation binary

Status: **static facts recorded; runtime probe written but not yet run.** The
go/no-go gate in [MILESTONES.md](../../MILESTONES.md) (P2.3) stays open until the
runtime section below is filled from a real `sedici_amd_probe` run. Nothing here is
inferred from the vendored 2.4.0 headers.

## Artifact (static, read-only inspection)

Source: the game's own `amd_fidelityfx_framegeneration_dx12.dll.orig`, which is the
file Sedici's deploy renamed from AMD's DLL. It was inspected in place and never
modified.

| Property | Value |
| --- | --- |
| Size | 40,085,776 bytes |
| SHA-256 | `02297BEEDD285E822D3A64F314CF00FAF378DCEC0EDC47FF0C4DD71B3A8C2F18` |
| File / product version | 4.0.1.2740 / 4.0.1.0 |
| Product | AMD FidelityFX Frame Generation, copyright 2026 AMD |
| Signature | Authenticode valid, signer Advanced Micro Devices (certificate expires 2027-04-26) |
| Machine / linker | x64, PE32+, MSVC linker 14.44, built 2026-06-11 |
| Exports | exactly five, by name: `ffxConfigure`, `ffxCreateContext`, `ffxDestroyContext`, `ffxDispatch`, `ffxQuery` |
| Static dependencies | `dxgi`, `MSVCP140`, `WINMM`, `VERSION`, `VCRUNTIME140`, `VCRUNTIME140_1`, `KERNEL32`, `USER32`, UCRT API sets. No `d3d12.dll` import, so D3D12 is bound at runtime through the device the host passes. |
| Delay imports | none found |

Notes for the contracts:

- The binary is FG **4.0.1**; the vendored headers are 4.0.2. The game's create
  descriptor already advertises 4.0.1 (finding 007), so the ABI the game uses is
  the older one. The probe sends the 4.0.2 version constant on purpose, to find out
  whether a newer-built host is accepted.
- `MSVCP140`/`VCRUNTIME140*` are redistributable runtimes. The game already ships
  them, but a bundled copy elsewhere would need them resolvable from its own path.
- The 25 MB `.rdata` suggests embedded shader blobs, so no external shader files are
  expected next to the DLL. The probe's dependency check will confirm this.
- Redistribution rights are **not established** by this inspection. The licence for
  shipping AMD's binary inside Sedici's package must be read from AMD's FidelityFX
  SDK terms before P2.9; until then Sedici must only ever use the copy already on the
  user's machine.

## Runtime evidence (to be filled from `sedici_amd_probe`)

Run `sedici_amd_probe <copy of the .orig> --sha256 <hash above>`. It copies the file
into its own scratch directory, never touches the game folder, and launches no game.

| Question | Probe check | Result |
| --- | --- | --- |
| Loads privately under another file name, dependencies resolve | `private load under a renamed file name` | pending |
| Loads under the `.orig` name as the proxy does | `load under the '.orig' name` | pending |
| Two instances coexist with separate state | `two copies are two independent module instances` | pending |
| Provider versions enumerable | `GetVersions(...)` INFO lines | pending |
| Swapchain wrap then FG create succeeds, in the game's order | `swapchain wrap context created`, `frame-generation context created after the swapchain context` | pending |
| Configure goes to the swapchain context, not the FG context | `FG configure accepted`, misrouted INFO line | pending |
| AMD calls the host's generation callback | `AMD invokes the host's generation callback` | pending |
| Present callback cadence with AMD's own pacing | variants 1 and 3 INFO lines | pending |
| Destruction order, either way round | variants 2 and 3 | pending |
| Behaviour without a generation callback | variant 4 | pending |

### Open questions the probe cannot answer

- Whether AMD's output can be consumed by Sedici's presenter. The probe only shows
  whether the present callback exposes generated frames and at what cadence; if AMD
  keeps its own present thread the answer is the architecture note's second branch:
  AMD retains its scheduling and Sedici must not stack a second scheduler.
- Mid-session handoff between the two generators.
- Failure after a native chain has been consumed. The stub tests cover Sedici's side
  only (see [reliability.md](reliability.md)).
