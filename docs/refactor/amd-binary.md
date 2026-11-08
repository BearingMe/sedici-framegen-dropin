# P2.3 evidence: the official AMD frame-generation binary

> **Retained as reference.** The AMD backend, the `.orig` forwarding path and the
> `sedici_amd_probe` tool were removed. This file records what the pinned binary did
> when it was probed; it is kept because it is useful for later work on FSR frame
> generation. Sedici no longer loads, forwards to or ships that binary.

Status: **evidence complete; gate decision recorded below (review pending).**
Technically feasible with the user's installed copy; **bundling was blocked on a
licence review**; the owner has since reviewed AMD's terms and accepted responsibility (`NOTICE.md`). Nothing here is
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
- Redistribution: this inspection does not establish rights. AMD's SDK licence
  (`docs/license.md`) lists the signed DLLs under MIT and the owner has accepted
  responsibility (`NOTICE.md`). Packaging in P2.9 must ship AMD's notice and the
  unmodified, hash-pinned DLL; until P2.9 Sedici uses only the copy already on the
  user's machine.

## Runtime evidence (`sedici_amd_probe`, run on the user's machine)

Run: `sedici_amd_probe <copy of the .orig> --sha256 <hash above>`; 4 runs, last one
all green (`RESULT OK`, five variants exit 0). The probe copies the file into its
own scratch directory, never touches the game folder, and launches no game. The
first three runs found probe mistakes (descriptor chain, configure target, a double
release); those are findings below, not product defects.

### Guarded configure-descriptor probe (2026-11-08)

With owner authorization, the hash-pinned 4.0.1.2740 binary was run in four
isolated child-process variants (256–259). A modern FG configure descriptor was
placed on its own page and changed to `PAGE_NOACCESS` immediately after
`ffxConfigure` returned. Each variant completed 90 Presents and both context
destructions; variants with and without the present callback and both destruction
orders were covered. The descriptor page remained inaccessible throughout.

A later debug-layer run found that the probe’s HUD-less resource state was invalid
(K90), so these observations must be repeated with corrected resource ownership.
They do not establish clean GPU behavior. A corrected, hash-checked legacy
configure attempt (variant 768) returned `ERROR_PARAMETER` (6) before frames
were presented (K94), so no legacy descriptor-lifetime conclusion can be drawn.
Even a corrected successful run demonstrates only no post-return **access** in
the tested workload; it cannot prove the binary never retained its address unused,
nor cover all
configuration shapes, version providers, devices or later paths. The owner-approved
copy policy is therefore used only for source-verified Sedici-owned standalone
descriptors; AMD forwarding remains unchanged pending a stronger lifetime contract
(K87/K88). No game was launched.

### Production Mini callback probe (2026-11-08)

The first authorized production-overlay run (variant 1280) recorded Mini draws
on 90 real and 90 generated callbacks, but reported 268 D3D12 errors. The probe
had transitioned its HUD-less texture back to COPY_DEST before AMD consumed it
(K90). After initializing a static HUD-less texture once and keeping it in
compute-read state, the same hash-checked guarded variant completed 90 Presents,
recorded 90 real and 90 generated Mini draws through Sedici’s production callback,
and reported zero D3D12 debug-layer errors. The descriptor page remained
inaccessible after configure until destruction.

This is command-recording evidence with a static 640×360 synthetic input, not
pixel readback, physical presentation or an in-game test. It does not cover a
caller-provided present callback, separate game UI resource, dynamic HUD-less
updates, or all AMD descriptor-lifetime paths.

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

### Failure, recreation and handoff (probe variants 9, 16, 32/33, 64, 128)

| Question | Result |
| --- | --- |
| Destroy and recreate everything in one process | **Works.** A second full session (new device, window, chain, contexts) after complete destruction runs clean (variants 32, 33). A restart-style recreation is supported. |
| Destroy only the FG context while the wrapped swapchain keeps presenting | **Crashes** (access violation, variant 16). The swapchain cannot outlive its FG context. |
| Same, but quiesced first: generation disabled with `frameGenerationEnabled=false`, GPU idle, 300 ms for AMD's present thread, no Present while the FG context is absent, then recreate and reconfigure | **Works.** Generation resumed on the live swapchain: 30 callbacks over 30 frames, no errors (variant 64). This is the only supported mid-session FG replacement shape found. |
| Wrap with a null `gameQueue` | **Crashes in AMD** (access violation, variant 9). AMD does not validate it. |
| FG create with a 0x0 display next to a live pair | **Process abort** in AMD (fail-fast, variant 128). AMD does not validate sizes. |
| After a successful wrap the native chain | Belongs to AMD (reference consumed). A "fallback to native presentation" after wrap needs a **new native chain**, which needs the old wrapper released first; the consumed chain is not recoverable. |

Sedici's own wrapper already refuses all three hostile cases without crashing
(`sedici_abi invalid-calls`: null queue, null swapchain slot, empty display size,
destroyed handle, double destroy all return an error and leave the caller's chain
intact). Because AMD does not validate, **Sedici must keep validating these before
forwarding anything to AMD**: no foreign or zero-sized argument may reach the binary.

## Integration contract (testable; for P2.4/P2.9)

1. **Ownership.** One backend owns a given FG+swapchain pair for its whole life. A
   handle belongs to the binary instance that created it; Sedici never passes a Sedici
   handle to AMD, or an AMD handle to another instance.
2. **Load.** AMD is loaded privately by full path under a name Sedici chooses; no
   sidecar file is needed. Load failure, missing file or a hash that is not the
   pinned one means the AMD backend is unavailable (never an error to the game).
3. **Creation order, pre-ownership fallback.** Everything that can fail is probed
   before ownership commits: argument validation (queue, swapchain slot, non-zero
   sizes, device), provider-version query, and AMD wrap on a *copy-safe* path. A wrap
   that succeeds consumes the caller's native chain; there is no fallback after that
   except recreation. A refusal before consumption leaves the chain with the caller.
4. **Descriptor shapes.** Swapchain wrap: bare descriptor only (no version or backend
   descriptor linked). FG configure goes to the FG context. Version descriptors from
   the 4.0.2 headers are not part of this binary's contract (providers are 3.1.6 and
   3.1.7).
5. **Callbacks.** AMD invokes the generation callback once per game Present and the
   present callback twice per frame, on AMD's own thread. Callbacks must be safe after
   `ffxDestroyContext` returns and must tolerate a destroyed FG context.
6. **Presentation.** AMD owns scheduling and flips. Sedici's vrr presenter and its
   `OutputSchedule` are never attached to an AMD swapchain; limiter, overlay and
   metrics attach through the documented callbacks only.
7. **Mid-session replacement.** Only quiesce-destroy-recreate (variant 64) or full
   destruction and recreation (variants 32/33) are supported; swapping the generator
   under a presenting swapchain is not. "Seamless" handoff is therefore not promised:
   a backend change takes effect at the next swapchain recreation, or after an explicit
   quiesce.
8. **Destruction.** Either order works; the swapchain context may be destroyed first
   or the FG context may be destroyed first only when the swapchain is no longer
   presenting.

Each point maps to a probe variant or a `sedici_abi` scenario, so it can be
re-checked against a different AMD binary by re-running the probe.

## Gate decision

Evidence record and open gaps: [finding 011](../findings/011-p23-amd-binary-gaps.md).

| Question | Decision |
| --- | --- |
| Is an AMD backend technically feasible? | **GO.** Private load, coexistence, create order, callbacks, destruction order, recreation and quiesced replacement are all demonstrated on the real binary. |
| Can AMD feed Sedici's presenter? | **NO.** AMD flips on its own thread. Sedici keeps two backends and never stacks two schedulers. |
| Seamless mid-session handoff? | **NO** (not promised). Next swapchain recreation, or explicit quiesce. |
| Bundling AMD's DLL with Sedici (P2.9) | **UNBLOCKED by owner decision.** AMD's SDK licence (`docs/license.md`) lists the signed DLLs under MIT; the owner reviewed it and accepted responsibility (see `NOTICE.md`). Packaging must ship AMD's notice and keep the DLL unmodified and hash-pinned. Before that decision the AMD backend may only use the copy already on the user's machine (the game's `.orig`), pinned by hash. |
| Pin | SHA-256 `02297BEE...2F18` (FG 4.0.1.2740, providers 3.1.6/3.1.7). Any other hash needs the probe re-run before the backend is enabled. |

**Re-estimates.** P2.4: unchanged, but it must include the argument-validation
boundary (AMD aborts on bad input). P2.8: simpler than feared (AMD keeps composition,
Sedici attaches overlay through the present callback it already uses). P2.9: shrinks
to "load the installed copy by hash" plus a separate, blocked "ship the DLL" item.

Not covered here: behaviour with game inputs at production size and cadence (the probe
uses a 640x360 synthetic scene), and the overlay composed through AMD's present
callback (it only copies). Those belong to P2.8/P2.9 acceptance, not this gate.
