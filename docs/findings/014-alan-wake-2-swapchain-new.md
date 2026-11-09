# 014: Alan Wake 2 asks for `SwapChainNewDX12`, which Sedici refuses

Why a deployed Sedici build cannot produce a single frame in Alan Wake 2, what
the game's log and descriptors show, and what it would take to support the game.
Recorded from a user-run session; nothing here was run by an automated check.

**Update:** the `New` route was built and run on a temporary branch (`bearingme/aw2-patch`, not merged); see
[finding 015](015-alan-wake-2-first-sedici-session.md) for the result and the
blocker that remains (the HDR create flag, K129).

**Status:** open. The game's only FFX call is answered with
`FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE`, so frame generation is off and the game
runs on its own chain. The return code is the documented one and the host did not
crash (AGENTS.md hard rule satisfied), but the drop-in contract is not met for
this game. Tracked as [K100](../refactor/known-issues.md).

## Evidence

### Deployment state

| Item | Value |
| --- | --- |
| Install | `C:\Program Files\Epic Games\AlanWake2` (Epic, no `bin\x64`; `tools/games.json` id `aw2`) |
| Deployed `amd_fidelityfx_framegeneration_dx12.dll` | 1,527,296 bytes, SHA-256 `D34D38FE…EE54A`, Sedici marker present, written 2026-11-08 03:00:58, byte-identical to Cyberpunk's deployed copy |
| Original DLL (`.orig`) | 40,085,776 bytes, SHA-256 `02297BEE…2F18`, AMD **4.0.1.2740** |
| Original's exports | exactly the five `ffx*` functions |

The `.orig` hash is the same binary P2.3 pinned
([finding 011](011-p23-amd-binary-gaps.md), [amd-binary.md](../refactor/amd-binary.md)),
and it is byte-identical to Cyberpunk's `.orig`. AW2 therefore ships the same AMD
frame generation build as the game Sedici was first developed against.

The deployed DLL is the same build Cyberpunk ran (identical hash), so this is
not a per-game stale-binary effect. Its provenance is not established from the
log: every session **before** the AW2 ones begins `loaded original …`, a line
only the deleted AMD-forwarding build wrote (`src/forward.cpp` at tag
`pre-amd-removal`; no source file on `refactor/phase-2` contains that string),
and those sessions print `pacing.*`/`async.mode`/`fg.mode` keys that the
current `config.cpp` no longer has. The three AW2 sessions print none of that:
they start directly with `sedici: read …`, use the current
`present/tear_guard/multiplier/target_fps/limiter/ui/log` key set, and still
report `ui.scale = 1 [file]` and `fg.target_fps = 73 [file]`, which the current
reader could not produce from the file that is there (`scale = 1`, `target_fps = 73`;
the current parser yields `[default]` for a rejected value, `[file]` only for a
value it accepted). So the log is a mixed record: an older forwarding build's
Cyberpunk sessions followed by an AW2 build that behaves like the current
routing. The refusal is confirmed independently by reading `route_create` in the
tree. The repository's `build\Release` DLL (SHA-256 `7CD1A1F1…A597`,
2026-11-08 04:03) is not what ran.

### The log

`sedici.log` beside the game is three AW2 process starts appended to a verbatim
copy of Cyberpunk's log (bytes 0–4,797,998 identical to
`Cyberpunk 2077\bin\x64\sedici.log`; 95 of its sessions read the Cyberpunk
`sedici.toml` and run 2560×1080 Cyberpunk workloads, one for 301 s). Only the last
three entries are AW2. The copy is itself misleading as a record, and the log
cannot say which build wrote it (recorded as [K101](../refactor/known-issues.md)).

Each of the three AW2 sessions makes exactly one FFX call:

```
[7.126 ms] ffxCreateContext #1 ctx=null result=2 (UNKNOWN_DESCTYPE)
  chain=0x30005 (FrameGenerationSwapChainNewDX12)
       -> 0x3000b (FrameGenerationSwapChainVersionDX12) version=0xc01007 (3.1.7)
  variant=new swapchainPtr=0xc09f4feec8 swapchain=0x57ff8043bd0
  desc{buffers=2 size=2560x1080 format=R10G10B10A2_UNORM(0x18)
       usage=0x20 swapEffect=4 flags=0x802 windowed=true outputWindow=0x4f0c16}
  factory=0x29320902be0 gameQueue=0x293207ff5f0
```

Start times 7.1 ms, 4.3 ms and 3.9 ms after load; the three runs are about 2.5
minutes apart and nothing follows the refused create — no `ffxQuery`, no
`FrameGenerationCreate`, no wrap, no configure, no dispatch, no present. The game
falls back to its own swapchain. `UNKNOWN_DESCTYPE` appears 3 times in the whole
file, all from these calls; the 3,193 generic `result=1 (ERROR)` lines belong to
the older Cyberpunk sessions and none occurs in the AW2 sessions.

### What the descriptor says

| Field | Value | Meaning |
| --- | --- | --- |
| `format` | `R10G10B10A2_UNORM` (24) | 10-bit back buffer |
| `buffers` | 2 | double buffered |
| `swapEffect` | 4 | `FLIP_DISCARD` |
| `usage` | `0x20` | `DXGI_USAGE_RENDER_TARGET_OUTPUT` |
| `flags` | `0x802` | `ALLOW_MODE_SWITCH` (0x2) \| `ALLOW_TEARING` (0x800) |
| `windowed` | true | windowed at create; exclusive fullscreen is not requested |
| size | 2560×1080 | same display mode as the Cyberpunk runs in the copied log |
| version sub-desc | `0xc01007` | swapchain 3.1.7 |

The chain is created by the game through FFX rather than handed over for
wrapping, and the game asks for tearing — the same combination the wrap form
gives Cyberpunk after `fg.present = "vrr"` recreates the chain.

## Why it happens

`route_create` owns only two descriptor types and returns `false` for anything
else; `dllmain.cpp` turns that into `FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE`:

- `FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WRAP_DX12` (`0x30001`)
- `FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION` (`0x20001`)

The recogniser in `describe.cpp` already prints the
`FRAMEGENERATIONSWAPCHAIN_NEW_DX12` (`0x30005`) and
`FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12` (`0x30006`) chains, so they were known
descriptors that were never routed. The vendored SDK declares both.

## Second blocker if `New` were routed

`Swapchain::initialize_buffers` (`fg_swapchain.cpp:331` today) and `resize`
(`fg_swapchain.cpp:1095` today) accept only `DXGI_FORMAT_R8G8B8A8_UNORM`; anything else
returns `DXGI_ERROR_UNSUPPORTED`. This is [K14](../refactor/known-issues.md), and
AW2's request lands on it exactly: a routed `New` create would build the chain and
then refuse it at buffer creation, i.e. after Sedici has consumed the factory and
queue and created a chain for the game. Fixing it needs 10-bit paths through the
wrapper's copies, the overlay compose, the generator's input handling, and a
decision on what the generator consumes from an `R10G10B10A2` back buffer.

## What supporting the game requires

| Item | Detail | Owner |
| --- | --- | --- |
| Route `SwapChainNewDX12` | Create the chain from `desc`/`dxgiFactory`/`gameQueue` and hand back a Sedici-owned `IDXGISwapChain4`. The existing presenter `adopt` is wrap-shaped (it reads and destroys a native chain); a `New` path creates the chain directly, so it is new code, not a routing tweak | P2.4 |
| Archive the unmodified `.orig` | `deploy.ps1` performs the move on the first deploy. The target already has a manual `.orig` with the correct hash, so this is satisfied here | P2.9 |
| 10-bit back buffers | K14. Required before the routed create can succeed | P2.4, P2.7 |
| Decide the version reported for `New` | `3.1.7` is the swapchain provider id Sedici already answers for the wrap type | P2.4 |
| Test without the game | An ABI scenario creating through the `New` form with a null/foreign factory and an unsupported format must get defined errors; a supported-format happy path needs a DX12 host | P2.2, P2.4 |
| Confirm the game's remaining sequence | The log stops at the refused create, so the queries and the `FrameGenerationCreate` shape AW2 would issue after a successful swapchain create are unknown. Capture again after `New` is routed | P2.4 |

## Things this does not establish

- That AW2 would work with `New` routed: the 10-bit requirement is unmet, and the
  game has not been run past one call.
- That the repository build (`build\Release`, SHA-256 `7CD1A1F1…A597`) would behave
  differently: its routing code refuses the same descriptor type in the same
  place, so the outcome is expected to be identical, but that build has not run.
- Anything about `ForHwndDX12` (`0x30006`): AW2 did not use it, and nothing in the
  repository routes or logs a decision for it.
