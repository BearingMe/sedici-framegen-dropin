# P2.3 evidence: the official AMD frame-generation binary

Status: **static and runtime evidence recorded; gate decision pending review.**
Technically feasible; redistribution rights and mid-session handoff are the open
blockers (see the end of this file). Nothing here is
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

## Runtime evidence (`sedici_amd_probe`, run on the user's machine)

Run: `sedici_amd_probe <copy of the .orig> --sha256 <hash above>`; 4 runs, last one
all green (`RESULT OK`, five variants exit 0). The probe copies the file into its
own scratch directory, never touches the game folder, and launches no game. The
first three runs found probe mistakes (descriptor chain, configure target, a double
release); those are findings below, not product defects.

| Question | Result |
| --- | --- |
| Loads privately under another file name; dependencies resolve | **Yes.** `sedici_amd_a.dll` loads, five exports resolve. No file beside the DLL is needed; `dxgi`, `d3d12`, `winmm`, `version` and the VC runtime are mapped. |
| Loads under the `.orig` name, as the proxy does today | **Yes.** |
| Two instances coexist | **Yes.** Two copies are two modules with distinct function addresses. Freeing one leaves the other usable. The same path loaded twice returns the same instance. |
| Provider versions | `GetVersions`: FrameGeneration **3.1.6** (id `0xf600000000c01006`), swapchain wrap **3.1.7** (id `0xf65cdd1200c01007`), one each. The file is labelled 4.0.1, but the providers inside report 3.1.x, matching AGENTS.md's "FSR 3.1". The vendored 4.0.2 headers are therefore not the binary's contract. |
| Swapchain wrap create | Succeeds with a **bare** wrap descriptor (`swapchain` in/out + `gameQueue`). With the swapchain-version and backend descriptors linked it returned `ERROR_PARAMETER` (6) in the first run; the later shape loop found the bare form accepted, so those descriptors must not be added to a wrap create. |
| Create order | Wrap first, then FG context: both succeed. |
| **Wrap consumes the caller's native chain reference** | **Yes.** The caller must not release its reference after a successful wrap. With a release the process segfaults at teardown (control run, exit 139); without it all variants exit 0. The returned pointer can even equal the old address (the native object is freed and the wrapper reuses the slot), so identity checks cannot detect the handover. Sedici's own wrapper keeps this contract (`handedRefs`). |
| **Configure target** | `ffxConfigure(FG)` with the FG configure desc is accepted by the **FG context** (rc 0). Sent to the swapchain context it returns `ERROR_PARAMETER` (6). The configure desc carries the swapchain pointer. |
| Generation callback | AMD calls it **once per game Present, 90 of 90**, from its own thread, and the host forwards the descriptor to `ffxDispatch(FG)` (rc 0). With no generation callback AMD called nothing and presented without error. |
| Present callback | When installed it is called **twice per game frame, 180 of 90**, 90 with `isGeneratedFrame`. Gaps between calls were bunched: median 2.0-2.8 ms, p10 0.2-0.45 ms, p90 31 ms (probe frames were a 20 ms sleep loop at about 50 fps, so the 31 ms gap is AMD queueing a pair, not a pacing measurement). |
| Present cost on the game thread | Median 0.61 ms, max 1.2-1.5 ms with generation; 0.32 ms without (AMD presents on its own thread). |
| Destruction order | Swapchain-then-FG and FG-then-swapchain both return OK and exit cleanly. |
| Behaviour without a generation callback | Present succeeds, no callback, no crash. |

Consequences for the refactor:

- **Presentation model.** AMD's swapchain schedules its own presents on its own
  thread and calls the host's callbacks from there. The present callback does expose
  each output (real and generated) with `outputSwapChainBuffer`, but flipping is AMD's.
  Sedici cannot feed its own presenter from AMD's wrapper without stacking schedulers,
  which the architecture note forbids. **Decision input for P2.4/P2.9:** the AMD
  backend keeps AMD-owned scheduling and composition; Sedici's vrr presenter is the
  Sedici backend only. The two never share a swapchain.
- **Contracts the Sedici wrapper must keep** (also verifiable offline against the stub):
  wrap/new consumes the input reference; the FG configure is addressed to the FG
  context; the generation callback runs on a foreign thread and must be safe after
  `ffxDestroyContext` returns.
- **Provider versions are 3.1.x.** Anything keyed to 4.0.x descriptor fields
  (e.g. V2 prepare, camera info) must be probed before use, not assumed.
- **A bundled binary is feasible technically** (private load, coexistence, no
  sidecar files); **legal redistribution is not established** (see above).

### Open questions the probe cannot answer

- Whether AMD's output can be consumed by Sedici's presenter. The probe only shows
  whether the present callback exposes generated frames and at what cadence; if AMD
  keeps its own present thread the answer is the architecture note's second branch:
  AMD retains its scheduling and Sedici must not stack a second scheduler.
- Mid-session handoff between the two generators.
- Failure after a native chain has been consumed. The stub tests cover Sedici's side
  only (see [reliability.md](reliability.md)).
